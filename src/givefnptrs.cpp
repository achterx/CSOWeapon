// givefnptrs.cpp  v10
//
// gpGlobals fix: the ptr is stored at engfuncs_RVA + 218*4 in mp.dll's .data.
// This is the standard GoldSrc layout — gpGlobals is declared right after the
// enginefuncs_t block. RVA 0x1E51878 + 0x368 = 0x1E51BE0.
//
// We store a pointer-to-pointer so every RefreshGlobals() call re-reads the
// actual current value (it changes when the map reloads).

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include "givefnptrs.h"
#include "logger.h"

enginefuncs_t g_engfuncs = {};
globalvars_t* g_pGlobals  = nullptr;

// Pointer to the mp.dll variable that holds gpGlobals
static uint32_t* g_ppGlobals = nullptr;

void GiveFnptrs_RefreshGlobals()
{
    if (!g_ppGlobals) return;
    uint32_t val = 0;
    __try { val = *g_ppGlobals; } __except(EXCEPTION_EXECUTE_HANDLER) { return; }
    if (val && !(val & 3))
        g_pGlobals = reinterpret_cast<globalvars_t*>((uintptr_t)val);
}

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

    // Engfuncs scan
    struct EngCand { size_t offset; int run; };
    std::vector<EngCand> cands;
    int curRun = 0; size_t runStart = 0;
    for (size_t off = 0; off + 4 <= mpSize; off += 4) {
        uint32_t v = 0;
        bool ok = SafeRead32(mpData + off, v);
        if (ok && v >= (uint32_t)hwBase && v < (uint32_t)hwEnd) {
            if (!curRun) runStart = off;
            ++curRun;
        } else {
            if (curRun >= 20) { EngCand c; c.offset=runStart; c.run=curRun; cands.push_back(c); }
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
    if (foundOff == (size_t)-1) { Log("[GiveFnptrs] No valid engfuncs found\n"); return false; }

    memcpy(&g_engfuncs, mpData + foundOff, sizeof(g_engfuncs));
    auto* ptrs = reinterpret_cast<uint32_t*>(&g_engfuncs);
    Log("[GiveFnptrs] pfnPrecacheModel[0]  = 0x%08X\n", ptrs[0]);
    Log("[GiveFnptrs] pfnPrecacheSound[1]  = 0x%08X\n", ptrs[1]);
    Log("[GiveFnptrs] pfnSetOrigin   [20]  = 0x%08X\n", ptrs[20]);
    Log("[GiveFnptrs] engfuncs OK\n");

    // --- gpGlobals: try 3 methods in order of reliability ---

    // Method 1: fixed RVA (engfuncs + 218*4 = engfuncs + 0x368)
    // Standard GoldSrc layout: globalvars_t* gpGlobals immediately follows enginefuncs_t
    {
        size_t globalsOff = foundOff + 218 * 4;
        if (globalsOff + 4 <= mpSize) {
            uint32_t val = 0;
            SafeRead32(mpData + globalsOff, val);
            Log("[GiveFnptrs] Method1: mp.dll+0x%zX = 0x%08X\n", globalsOff, val);
            if (val && !(val & 3)) {
                __try {
                    auto* g = reinterpret_cast<globalvars_t*>((uintptr_t)val);
                    if (g->maxClients >= 1 && g->maxClients <= 64 &&
                        g->frametime >= 0.0f && g->frametime < 1.0f) {
                        g_ppGlobals = reinterpret_cast<uint32_t*>(mpData + globalsOff);
                        g_pGlobals  = g;
                        Log("[GiveFnptrs] g_globals(M1) = 0x%08X  t=%.3f mc=%d ft=%.4f\n",
                            val, g->time, g->maxClients, g->frametime);
                        goto done;
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
    }

    // Method 2: scan next 64 dwords after engfuncs for a valid globals ptr
    {
        for (int d = 218; d < 218+64; d++) {
            size_t off = foundOff + (size_t)d * 4;
            if (off + 4 > mpSize) break;
            uint32_t val = 0;
            if (!SafeRead32(mpData + off, val)) continue;
            if (!val || (val & 3)) continue;
            __try {
                auto* g = reinterpret_cast<globalvars_t*>((uintptr_t)val);
                if (g->maxClients >= 1 && g->maxClients <= 64 &&
                    g->frametime >= 0.0f && g->frametime < 1.0f &&
                    g->time >= 0.0f) {
                    g_ppGlobals = reinterpret_cast<uint32_t*>(mpData + off);
                    g_pGlobals  = g;
                    Log("[GiveFnptrs] g_globals(M2,d=%d) = 0x%08X  t=%.3f mc=%d ft=%.4f\n",
                        d, val, g->time, g->maxClients, g->frametime);
                    goto done;
                }
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    // Method 3: GiveFnptrsToDll export disassembly
    {
        auto* fn = reinterpret_cast<uint8_t*>(GetProcAddress(hMp, "GiveFnptrsToDll"));
        if (fn) {
            Log("[GiveFnptrs] GiveFnptrsToDll @ 0x%08zX — scanning for MOV stores\n", (uintptr_t)fn);
            int storeCount = 0;
            for (int i = 0; i < 512 && storeCount < 4; i++) {
                uint8_t b0=0, b1=0; uint32_t imm=0;
                __try { b0=fn[i]; } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
                uintptr_t dest=0; int skip=0;
                if (b0==0xA3) {
                    __try { memcpy(&imm,fn+i+1,4); } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
                    dest=imm; skip=5;
                } else if (b0==0x89) {
                    __try { b1=fn[i+1]; } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
                    if ((b1&0xC7)==0x05) {
                        __try { memcpy(&imm,fn+i+2,4); } __except(EXCEPTION_EXECUTE_HANDLER) { break; }
                        dest=imm; skip=6;
                    }
                }
                if (dest>=mpBase && dest<mpBase+mpSize) {
                    Log("[GiveFnptrs] Store[%d] fn+%d -> [0x%08zX]\n", storeCount, i, dest);
                    uint32_t val=0;
                    if (SafeRead32(reinterpret_cast<void*>(dest), val) && val && !(val&3)) {
                        __try {
                            auto* g=reinterpret_cast<globalvars_t*>((uintptr_t)val);
                            if (g->maxClients>=1 && g->maxClients<=64 &&
                                g->frametime>=0.0f && g->frametime<1.0f) {
                                g_ppGlobals=reinterpret_cast<uint32_t*>(dest);
                                g_pGlobals=g;
                                Log("[GiveFnptrs] g_globals(M3) = 0x%08X  t=%.3f mc=%d\n",
                                    val, g->time, g->maxClients);
                                goto done;
                            }
                        } __except(EXCEPTION_EXECUTE_HANDLER) {}
                    }
                    storeCount++;
                }
                if (skip>1) i+=skip-1;
            }
        } else {
            Log("[GiveFnptrs] GiveFnptrsToDll export not found\n");
        }
    }

    Log("[GiveFnptrs] g_globals not found — time will be wrong!\n");
    goto done;  // still return true, engfuncs is what matters

done:
    return true;
}
