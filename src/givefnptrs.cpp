// givefnptrs.cpp  v11
// All __try blocks are in plain functions (no C++ objects in scope).

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include "givefnptrs.h"
#include "logger.h"

enginefuncs_t g_engfuncs = {};
globalvars_t* g_pGlobals  = nullptr;
static uint32_t* g_ppGlobals = nullptr;

void GiveFnptrs_RefreshGlobals()
{
    if (!g_ppGlobals) return;
    uint32_t val = 0;
    __try { val = *g_ppGlobals; } __except(EXCEPTION_EXECUTE_HANDLER) { return; }
    if (val && !(val & 3))
        g_pGlobals = reinterpret_cast<globalvars_t*>((uintptr_t)val);
}

// ---------------------------------------------------------------------------
// Plain helpers — __try valid here (no C++ objects)
// ---------------------------------------------------------------------------
static bool SafeRead32(const void* addr, uint32_t& out)
{
    __try { out = *static_cast<const uint32_t*>(addr); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static IMAGE_NT_HEADERS* GetNtHeaders(HMODULE hMod)
{
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<uint8_t*>(hMod) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        return nt;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
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

// Check if a raw pointer value looks like a valid globalvars_t
static bool ProbeGlobals(uint32_t val, globalvars_t** out)
{
    __try {
        auto* g = reinterpret_cast<globalvars_t*>((uintptr_t)val);
        if (g->maxClients >= 1 && g->maxClients <= 64 &&
            g->frametime >= 0.0f && g->frametime < 1.0f &&
            g->time >= 0.0f) {
            *out = g;
            return true;
        }
        return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Scan GiveFnptrsToDll body for MOV [mp_addr], reg stores
// Returns address of the mp.dll variable that holds gpGlobals, or 0
static uint32_t* FindGlobalsPtrViaExport(HMODULE hMp,
                                          uintptr_t mpBase, uintptr_t mpEnd)
{
    auto* fn = reinterpret_cast<uint8_t*>(GetProcAddress(hMp, "GiveFnptrsToDll"));
    if (!fn) return nullptr;
    Log("[GiveFnptrs] GiveFnptrsToDll @ 0x%08zX\n", (uintptr_t)fn);

    // Collect up to 8 stores into mp.dll address space
    uint32_t* stores[8]; int n = 0;
    for (int i = 0; i < 512 && n < 8; i++) {
        uint8_t b0 = 0, b1 = 0; uint32_t imm = 0;
        __try { b0 = fn[i]; } __except(EXCEPTION_EXECUTE_HANDLER) { break; }

        uintptr_t dest = 0; int skip = 0;
        if (b0 == 0xA3) {
            __try { memcpy(&imm, fn+i+1, 4); } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
            dest = imm; skip = 5;
        } else if (b0 == 0x89) {
            __try { b1 = fn[i+1]; } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
            if ((b1 & 0xC7) == 0x05) {
                __try { memcpy(&imm, fn+i+2, 4); } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
                dest = imm; skip = 6;
            }
        }
        if (dest >= mpBase && dest < mpEnd) {
            Log("[GiveFnptrs] Export store[%d] -> [0x%08zX]\n", n, dest);
            stores[n++] = reinterpret_cast<uint32_t*>(dest);
        }
        if (skip > 1) i += skip - 1;
    }

    // Try each store to find one pointing to globalvars_t
    for (int i = 0; i < n; i++) {
        uint32_t val = 0;
        if (!SafeRead32(stores[i], val)) continue;
        globalvars_t* g = nullptr;
        if (ProbeGlobals(val, &g)) {
            Log("[GiveFnptrs] Globals via export store[%d]=0x%08X t=%.3f mc=%d\n",
                i, val, g->time, g->maxClients);
            return stores[i];
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Engfuncs scan — lives in its own function so __try is valid
// Returns offset within mpData, or (size_t)-1 on failure
// ---------------------------------------------------------------------------
static size_t FindEngfuncs(const uint8_t* mpData, DWORD mpSize,
                            uintptr_t hwBase, uintptr_t hwEnd)
{
    // Build candidate list without std::vector — use fixed array
    struct Cand { size_t offset; int run; };
    static Cand cands[256]; int nCands = 0;

    int curRun = 0; size_t runStart = 0;
    for (size_t off = 0; off + 4 <= mpSize; off += 4) {
        uint32_t v = 0;
        bool ok = SafeRead32(mpData + off, v);
        if (ok && v >= (uint32_t)hwBase && v < (uint32_t)hwEnd) {
            if (!curRun) runStart = off;
            ++curRun;
        } else {
            if (curRun >= 20 && nCands < 256) {
                cands[nCands].offset = runStart;
                cands[nCands].run = curRun;
                nCands++;
            }
            curRun = 0;
        }
    }

    // Sort descending by run length (bubble sort — small n)
    for (int i = 0; i < nCands-1; i++)
        for (int j = i+1; j < nCands; j++)
            if (cands[j].run > cands[i].run) {
                Cand tmp = cands[i]; cands[i] = cands[j]; cands[j] = tmp;
            }

    Log("[GiveFnptrs] %d engfuncs candidates\n", nCands);
    for (int i = 0; i < nCands; i++) {
        auto* ptrs = reinterpret_cast<const uint32_t*>(mpData + cands[i].offset);
        Log("[GiveFnptrs] Cand mp.dll+0x%zX run=%d [0]=%08X [20]=%08X\n",
            cands[i].offset, cands[i].run, ptrs[0], ptrs[20]);
        if (ValidateEngfuncs(mpData, cands[i].offset, hwBase, hwEnd))
            return cands[i].offset;
    }
    return (size_t)-1;
}

// ---------------------------------------------------------------------------
// GiveFnptrs_Init
// No __try here (has no C++ objects either, but keep it clean)
// ---------------------------------------------------------------------------
bool GiveFnptrs_Init(HMODULE hMp)
{
    Log("[GiveFnptrs] GiveFnptrs_Init mp.dll=0x%08X\n", (uintptr_t)hMp);

    HMODULE hHw = GetModuleHandleA("hw.dll");
    if (!hHw) { Log("[GiveFnptrs] hw.dll not found\n"); return false; }
    IMAGE_NT_HEADERS* hwNt = GetNtHeaders(hHw);
    if (!hwNt) { Log("[GiveFnptrs] hw.dll bad headers\n"); return false; }
    uintptr_t hwBase = reinterpret_cast<uintptr_t>(hHw);
    uintptr_t hwEnd  = hwBase + hwNt->OptionalHeader.SizeOfImage;
    Log("[GiveFnptrs] hw.dll @ 0x%08zX  size=0x%X\n", hwBase, hwNt->OptionalHeader.SizeOfImage);

    IMAGE_NT_HEADERS* mpNt = GetNtHeaders(hMp);
    if (!mpNt) { Log("[GiveFnptrs] mp.dll bad headers\n"); return false; }
    uintptr_t mpBase = reinterpret_cast<uintptr_t>(hMp);
    DWORD     mpSize = mpNt->OptionalHeader.SizeOfImage;
    uint8_t*  mpData = reinterpret_cast<uint8_t*>(mpBase);

    size_t foundOff = FindEngfuncs(mpData, mpSize, hwBase, hwEnd);
    if (foundOff == (size_t)-1) { Log("[GiveFnptrs] No valid engfuncs\n"); return false; }

    Log("[GiveFnptrs] VALIDATED @ mp.dll+0x%zX\n", foundOff);
    memcpy(&g_engfuncs, mpData + foundOff, sizeof(g_engfuncs));
    uint32_t* ptrs = reinterpret_cast<uint32_t*>(&g_engfuncs);
    Log("[GiveFnptrs] pfnPrecacheModel[0]  = 0x%08X\n", ptrs[0]);
    Log("[GiveFnptrs] pfnPrecacheSound[1]  = 0x%08X\n", ptrs[1]);
    Log("[GiveFnptrs] pfnSetOrigin   [20]  = 0x%08X\n", ptrs[20]);
    Log("[GiveFnptrs] engfuncs OK\n");

    // --- Find gpGlobals, 3 methods ---

    // Method 1: fixed offset — gpGlobals ptr is stored right after engfuncs block
    // Standard GoldSrc layout: globalvars_t* gpGlobals declared after enginefuncs_t
    {
        size_t off = foundOff + 218 * 4;  // 218 fn ptrs * 4 bytes each
        if (off + 4 <= mpSize) {
            uint32_t val = 0;
            SafeRead32(mpData + off, val);
            Log("[GiveFnptrs] M1: [mp+0x%zX] = 0x%08X\n", off, val);
            globalvars_t* g = nullptr;
            if (val && !(val & 3) && ProbeGlobals(val, &g)) {
                g_ppGlobals = reinterpret_cast<uint32_t*>(mpData + off);
                g_pGlobals  = g;
                Log("[GiveFnptrs] g_globals(M1)=0x%08X t=%.3f mc=%d ft=%.5f\n",
                    val, g->time, g->maxClients, g->frametime);
                return true;
            }
        }
    }

    // Method 2: scan next 64 dwords after engfuncs
    {
        for (int d = 218; d < 282; d++) {
            size_t off = foundOff + (size_t)d * 4;
            if (off + 4 > mpSize) break;
            uint32_t val = 0;
            if (!SafeRead32(mpData + off, val) || !val || (val & 3)) continue;
            globalvars_t* g = nullptr;
            if (ProbeGlobals(val, &g)) {
                g_ppGlobals = reinterpret_cast<uint32_t*>(mpData + off);
                g_pGlobals  = g;
                Log("[GiveFnptrs] g_globals(M2,d=%d)=0x%08X t=%.3f mc=%d ft=%.5f\n",
                    d, val, g->time, g->maxClients, g->frametime);
                return true;
            }
        }
    }

    // Method 3: GiveFnptrsToDll export disassembly
    {
        uint32_t* p = FindGlobalsPtrViaExport(hMp, mpBase, mpBase + mpSize);
        if (p) {
            uint32_t val = 0; SafeRead32(p, val);
            globalvars_t* g = nullptr;
            if (ProbeGlobals(val, &g)) {
                g_ppGlobals = p;
                g_pGlobals  = g;
                Log("[GiveFnptrs] g_globals(M3)=0x%08X t=%.3f mc=%d\n",
                    val, g->time, g->maxClients);
                return true;
            }
        }
    }

    Log("[GiveFnptrs] g_globals NOT FOUND — time=0 everywhere\n");
    return true;  // engfuncs is what matters for safety
}