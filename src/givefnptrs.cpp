// givefnptrs.cpp  v9
// 
// Fix for gpGlobals: instead of heuristic scan (wrong!), find the
// GiveFnptrsToDll export in mp.dll and disassemble it to find where
// it stores the globalvars_t* parameter (EDX/[ESP+8] -> MOV [addr], reg).
//
// GiveFnptrsToDll signature: void GiveFnptrsToDll(enginefuncs_t*, globalvars_t*)
// The function stores both pointers to static variables in mp.dll's .data.
// We just need to find the MOV [imm32], reg instruction that stores the globals ptr.

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include "givefnptrs.h"
#include "logger.h"

enginefuncs_t g_engfuncs = {};
globalvars_t* g_pGlobals  = nullptr;

static bool SafeRead32(const void* addr, uint32_t& out)
{
    __try {
        out = *static_cast<const uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static IMAGE_NT_HEADERS* GetNtHeaders(HMODULE hMod)
{
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
                        reinterpret_cast<uint8_t*>(hMod) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        return nt;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static bool ValidateEngfuncs(const uint8_t* mpData, size_t offset,
                              uintptr_t hwBase, uintptr_t hwEnd)
{
    auto* ptrs = reinterpret_cast<const uint32_t*>(mpData + offset);
    int ok = 0;
    for (int i = 0; i < 5; ++i) {
        uint32_t v = ptrs[i];
        if (v >= (uint32_t)hwBase && v < (uint32_t)hwEnd && (v & 3) == 0) ++ok;
    }
    if (ok < 5) return false;
    uint32_t v20 = ptrs[20];
    return v20 >= (uint32_t)hwBase && v20 < (uint32_t)hwEnd && (v20 & 3) == 0;
}

// ---------------------------------------------------------------------------
// Find gpGlobals via GiveFnptrsToDll export disassembly
//
// GiveFnptrsToDll(enginefuncs_t* pFnTable, globalvars_t* pGlobals)
// Compiled as __cdecl, args at [esp+4] and [esp+8].
// The function body does (roughly):
//   MOV EAX, [ESP+8]        ; load pGlobals
//   MOV [g_pGlobals], EAX   ; store to static — opcode: A3 <imm32>
//                           ; or: MOV [imm32], EAX = 89 05 <imm32>
//   MOV ECX, [ESP+4]        ; load pFnTable  
//   ...memcpy of engfuncs...
//
// We scan the first 256 bytes of GiveFnptrsToDll for MOV [imm32], reg
// patterns that store into mp.dll's .data section, then use the second
// such store as the globals pointer location.
// ---------------------------------------------------------------------------
static uintptr_t* FindGlobalsPtr(HMODULE hMp)
{
    // Get GiveFnptrsToDll export
    auto* fn = reinterpret_cast<uint8_t*>(GetProcAddress(hMp, "GiveFnptrsToDll"));
    if (!fn) {
        Log("[GiveFnptrs] GiveFnptrsToDll export not found\n");
        return nullptr;
    }
    Log("[GiveFnptrs] GiveFnptrsToDll @ 0x%08zX\n", (uintptr_t)fn);

    uintptr_t mpBase = (uintptr_t)hMp;
    auto* mpNt = GetNtHeaders(hMp);
    if (!mpNt) return nullptr;
    uintptr_t mpEnd = mpBase + mpNt->OptionalHeader.SizeOfImage;

    // Scan up to 512 bytes of function body for MOV [imm32], reg patterns
    // that write into mp.dll address space:
    //   A3 xx xx xx xx       = MOV [imm32], EAX
    //   89 05 xx xx xx xx    = MOV [imm32], EAX (alternate encoding)
    //   89 0D xx xx xx xx    = MOV [imm32], ECX
    //   89 15 xx xx xx xx    = MOV [imm32], EDX
    //   89 1D xx xx xx xx    = MOV [imm32], EBX
    //   89 35 xx xx xx xx    = MOV [imm32], ESI
    //   89 3D xx xx xx xx    = MOV [imm32], EDI
    
    uintptr_t stores[8];
    int nStores = 0;

    for (int i = 0; i < 512 && nStores < 8; i++)
    {
        uint8_t b0 = 0, b1 = 0;
        uint32_t imm = 0;
        __try { b0 = fn[i]; } __except(EXCEPTION_EXECUTE_HANDLER) { break; }

        uintptr_t dest = 0;
        int skip = 0;

        if (b0 == 0xA3) {
            // MOV [imm32], EAX  (5 bytes)
            __try { memcpy(&imm, fn + i + 1, 4); } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
            dest = imm; skip = 5;
        } else if (b0 == 0x89) {
            __try { b1 = fn[i+1]; } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
            // ModRM: mod=00, r/m=101 means [disp32]
            // 89 /r with ModRM mod=00,rm=101: bytes are 89 XX XX XX XX XX (6 bytes)
            if ((b1 & 0xC7) == 0x05) {
                __try { memcpy(&imm, fn + i + 2, 4); } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
                dest = imm; skip = 6;
            }
        } else if (b0 == 0xC7) {
            // MOV DWORD PTR [imm32], imm32 — skip
            __try { b1 = fn[i+1]; } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
            if ((b1 & 0xC7) == 0x05) i += 9; // skip 10 bytes total
            continue;
        }

        if (dest && dest >= mpBase && dest < mpEnd) {
            Log("[GiveFnptrs] Store[%d] @ fn+%d -> [0x%08zX]\n", nStores, i, dest);
            stores[nStores++] = dest;
        }
        if (skip > 1) i += skip - 1;
    }

    if (nStores < 2) {
        Log("[GiveFnptrs] Not enough stores found (%d)\n", nStores);
        return nullptr;
    }

    // The second store in GiveFnptrsToDll is typically the globals pointer.
    // The first is the engfuncs memcpy destination or the engfuncs ptr itself.
    // Validate: the stored value should be a valid pointer (non-null, aligned)
    // Try each store to find globalvars_t*
    for (int i = 0; i < nStores; i++) {
        uint32_t val = 0;
        if (!SafeRead32(reinterpret_cast<void*>(stores[i]), val)) continue;
        if (!val || (val & 3)) continue;
        // Check if it looks like a globalvars_t (time >= 0, frametime in range, maxClients sane)
        __try {
            auto* g = reinterpret_cast<globalvars_t*>((uintptr_t)val);
            if (g->maxClients >= 1 && g->maxClients <= 64 &&
                g->frametime >= 0.0f && g->frametime < 1.0f) {
                Log("[GiveFnptrs] globals ptr @ store[%d]=0x%08zX -> g=0x%08X t=%.3f mc=%d\n",
                    i, stores[i], val, g->time, g->maxClients);
                // Save the POINTER TO THE POINTER so we always read fresh
                return reinterpret_cast<uintptr_t*>(stores[i]);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    Log("[GiveFnptrs] Could not identify globals store from %d candidates\n", nStores);
    return nullptr;
}

// Pointer to the location in mp.dll that holds gpGlobals
static uintptr_t* g_pGlobalsPtr = nullptr;

// Call this each frame to refresh gpGlobals
void GiveFnptrs_RefreshGlobals()
{
    if (g_pGlobalsPtr) {
        uint32_t val = 0;
        if (SafeRead32(g_pGlobalsPtr, val) && val)
            g_pGlobals = reinterpret_cast<globalvars_t*>((uintptr_t)val);
    }
}

bool GiveFnptrs_Init(HMODULE hMp)
{
    Log("[GiveFnptrs] GiveFnptrs_Init mp.dll=0x%08X\n", (uintptr_t)hMp);

    HMODULE hHw = GetModuleHandleA("hw.dll");
    if (!hHw) { Log("[GiveFnptrs] hw.dll not found\n"); return false; }
    auto* hwNt = GetNtHeaders(hHw);
    if (!hwNt) { Log("[GiveFnptrs] hw.dll bad headers\n"); return false; }
    uintptr_t hwBase = reinterpret_cast<uintptr_t>(hHw);
    uintptr_t hwEnd  = hwBase + hwNt->OptionalHeader.SizeOfImage;
    Log("[GiveFnptrs] hw.dll @ 0x%08zX  size=0x%X\n", hwBase, hwNt->OptionalHeader.SizeOfImage);

    auto* mpNt = GetNtHeaders(hMp);
    if (!mpNt) { Log("[GiveFnptrs] mp.dll bad headers\n"); return false; }
    uintptr_t mpBase = reinterpret_cast<uintptr_t>(hMp);
    DWORD     mpSize = mpNt->OptionalHeader.SizeOfImage;
    auto*     mpData = reinterpret_cast<uint8_t*>(mpBase);

    // Engfuncs scan (unchanged)
    struct EngCand { size_t offset; int run; };
    std::vector<EngCand> cands;
    int    curRun   = 0;
    size_t runStart = 0;

    for (size_t off = 0; off + 4 <= mpSize; off += 4) {
        uint32_t v = 0;
        bool ok = SafeRead32(mpData + off, v);
        if (ok && v >= (uint32_t)hwBase && v < (uint32_t)hwEnd) {
            if (!curRun) runStart = off;
            ++curRun;
        } else {
            if (curRun >= 20) {
                EngCand c; c.offset = runStart; c.run = curRun;
                cands.push_back(c);
            }
            curRun = 0;
        }
    }

    std::sort(cands.begin(), cands.end(),
        [](const EngCand& a, const EngCand& b){ return a.run > b.run; });

    Log("[GiveFnptrs] %zu engfuncs candidates\n", cands.size());

    size_t foundOff = (size_t)-1;
    for (size_t i = 0; i < cands.size(); ++i) {
        auto* ptrs = reinterpret_cast<uint32_t*>(mpData + cands[i].offset);
        Log("[GiveFnptrs] Cand mp.dll+0x%zX run=%d [0]=%08X [20]=%08X\n",
            cands[i].offset, cands[i].run, ptrs[0], ptrs[20]);
        if (ValidateEngfuncs(mpData, cands[i].offset, hwBase, hwEnd)) {
            Log("[GiveFnptrs] VALIDATED @ mp.dll+0x%zX\n", cands[i].offset);
            foundOff = cands[i].offset;
            break;
        }
    }

    if (foundOff == (size_t)-1) {
        Log("[GiveFnptrs] No valid engfuncs found\n");
        return false;
    }

    memcpy(&g_engfuncs, mpData + foundOff, sizeof(g_engfuncs));
    auto* ptrs = reinterpret_cast<uint32_t*>(&g_engfuncs);
    Log("[GiveFnptrs] pfnPrecacheModel[0]  = 0x%08X\n", ptrs[0]);
    Log("[GiveFnptrs] pfnPrecacheSound[1]  = 0x%08X\n", ptrs[1]);
    Log("[GiveFnptrs] pfnSetOrigin   [20]  = 0x%08X\n", ptrs[20]);
    Log("[GiveFnptrs] engfuncs OK\n");

    // Find globals via GiveFnptrsToDll disassembly
    g_pGlobalsPtr = FindGlobalsPtr(hMp);
    if (g_pGlobalsPtr) {
        GiveFnptrs_RefreshGlobals();
        if (g_pGlobals)
            Log("[GiveFnptrs] g_globals = 0x%08X  time=%.3f maxClients=%d\n",
                (uint32_t)(uintptr_t)g_pGlobals, g_pGlobals->time, g_pGlobals->maxClients);
    } else {
        Log("[GiveFnptrs] g_globals not found (non-fatal)\n");
    }

    return true;
}
