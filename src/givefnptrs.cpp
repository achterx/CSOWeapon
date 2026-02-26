// givefnptrs.cpp  v8
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include "givefnptrs.h"
#include "logger.h"

enginefuncs_t g_engfuncs = {};
globalvars_t* g_pGlobals  = nullptr;

// ---------------------------------------------------------------------------
// Plain helpers — may use __try (no C++ objects in scope)
// ---------------------------------------------------------------------------

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

static bool ProbeGlobals(uint32_t ptrVal, globalvars_t** out)
{
    __try {
        auto* g = reinterpret_cast<globalvars_t*>((uintptr_t)ptrVal);
        if (g->maxClients >= 1 && g->maxClients <= 64 &&
            g->time      >= 0.0f &&
            g->frametime >= 0.0f && g->frametime < 1.0f) {
            *out = g;
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// Engfuncs validation — no __try
// ---------------------------------------------------------------------------

static bool ValidateEngfuncs(const uint8_t* mpData, size_t offset,
                              uintptr_t hwBase, uintptr_t hwEnd)
{
    auto* ptrs = reinterpret_cast<const uint32_t*>(mpData + offset);
    int ok = 0;
    for (int i = 0; i < 5; ++i)
        if (ptrs[i] >= (uint32_t)hwBase && ptrs[i] < (uint32_t)hwEnd) ++ok;
    if (ok < 4) return false;
    return ptrs[20] >= (uint32_t)hwBase && ptrs[20] < (uint32_t)hwEnd;
}

// ---------------------------------------------------------------------------
// GiveFnptrs_Init — has std::vector, so NO __try directly here
// ---------------------------------------------------------------------------

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

    // Scan for engfuncs: longest run of 20+ consecutive hw.dll pointers
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

    // g_globals scan — ProbeGlobals is a separate plain fn so __try is valid
    g_pGlobals = nullptr;
    for (size_t off = 0; off + 4 <= mpSize; off += 4) {
        uint32_t ptrVal = 0;
        if (!SafeRead32(mpData + off, ptrVal)) continue;
        if (ptrVal < 0x10000 || ptrVal > 0x7FFFFFFF) continue;
        if (ProbeGlobals(ptrVal, &g_pGlobals)) {
            Log("[GiveFnptrs] g_globals = 0x%08X  time=%.3f maxClients=%d\n",
                ptrVal, g_pGlobals->time, g_pGlobals->maxClients);
            break;
        }
    }
    if (!g_pGlobals)
        Log("[GiveFnptrs] g_globals not found (non-fatal)\n");

    return true;
}
