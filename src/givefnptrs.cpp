// givefnptrs.cpp  v7
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include "givefnptrs.h"
#include "logger.h"

enginefuncs_t g_engfuncs = {};
globalvars_t* g_pGlobals  = nullptr;

// ---------------------------------------------------------------------------
// Plain helpers — may use __try  (no C++ objects in scope)
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

// Probe a potential globalvars_t* with SEH — separate fn so __try is valid
static bool ProbeGlobals(uint32_t ptrVal, globalvars_t** out)
{
    __try {
        auto* g = reinterpret_cast<globalvars_t*>((uintptr_t)ptrVal);
        if (g->maxClients >= 1 && g->maxClients <= 64 &&
            g->time      >= 0.0f &&
            g->frametime >= 0.0f && g->frametime < 1.0f)
        {
            *out = g;
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// RTTI vtable search  (uses std::vector — no __try)
// ---------------------------------------------------------------------------

static uintptr_t RttiSearch(const uint8_t* data, size_t mpSize,
                             uintptr_t base, uintptr_t textLo, uintptr_t textHi)
{
    static const char* names[] = {
        "?AVCWeaponFrostbite",
        "?AVCWeaponFrostBite",
        "?AVCCSWeaponFrostbite",
        "?AVCCSWeaponFrostBite",
        "?AVWeaponFrostbite",
        nullptr
    };

    for (int ni = 0; names[ni]; ++ni) {
        const char* needle = names[ni];
        size_t      nlen   = strlen(needle);
        Log("[FB-RTTI] Searching '%s'...\n", needle);

        for (size_t off = 0; off + nlen + 4 < mpSize; ++off) {
            if (memcmp(data + off, needle, nlen) != 0) continue;

            uintptr_t typeDesc = (base + off) - 8;
            Log("[FB-RTTI] TypeDesc candidate @ mp.dll+0x%zX\n", off - 8);

            for (size_t off2 = 0; off2 + 20 < mpSize; off2 += 4) {
                uint32_t v = 0;
                if (!SafeRead32(data + off2, v)) continue;
                if (v != (uint32_t)typeDesc) continue;

                uintptr_t colAddr = base + off2 - 12;
                uint32_t  sig     = 0;
                SafeRead32(reinterpret_cast<void*>(colAddr), sig);
                if (sig != 0) continue;

                for (size_t off3 = 0; off3 + 4 < mpSize; off3 += 4) {
                    uint32_t pCol = 0;
                    if (!SafeRead32(data + off3, pCol)) continue;
                    if (pCol != (uint32_t)colAddr)      continue;

                    uintptr_t vt    = base + off3 + 4;
                    uint32_t  slot0 = 0;
                    SafeRead32(reinterpret_cast<void*>(vt), slot0);
                    if (slot0 >= (uint32_t)textLo && slot0 < (uint32_t)textHi) {
                        Log("[FB-RTTI] vtable @ 0x%08zX  slot[0]=0x%08X\n", vt, slot0);
                        return vt;
                    }
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Dense-pointer vtable search  (no __try)
// ---------------------------------------------------------------------------

struct VtableCandidate { uintptr_t addr; int run; bool dtorPair; };

static uintptr_t DenseSearch(const uint8_t* data, size_t mpSize,
                              uintptr_t base, uintptr_t textLo, uintptr_t textHi)
{
    std::vector<VtableCandidate> cands;
    int    curRun   = 0;
    size_t runStart = 0;

    for (size_t off = 0; off + 4 <= mpSize; off += 4) {
        uint32_t v = 0;
        bool ok = SafeRead32(data + off, v);
        if (ok && v >= (uint32_t)textLo && v < (uint32_t)textHi) {
            if (!curRun) runStart = off;
            ++curRun;
        } else {
            if (curRun >= 50) {
                uint32_t s0 = 0, s1 = 0;
                SafeRead32(data + runStart,     s0);
                SafeRead32(data + runStart + 4, s1);
                VtableCandidate c;
                c.addr     = base + runStart;
                c.run      = curRun;
                c.dtorPair = (s0 == s1);
                cands.push_back(c);
            }
            curRun = 0;
        }
    }

    std::sort(cands.begin(), cands.end(), [](const VtableCandidate& a, const VtableCandidate& b) {
        if (a.dtorPair != b.dtorPair) return (int)a.dtorPair > (int)b.dtorPair;
        return a.run > b.run;
    });

    Log("[FB-Dense] %zu candidates (run>=50)\n", cands.size());
    for (size_t i = 0; i < cands.size() && i < 8; ++i) {
        uint32_t s0=0, s1=0, s2=0;
        SafeRead32(reinterpret_cast<void*>(cands[i].addr),   s0);
        SafeRead32(reinterpret_cast<void*>(cands[i].addr+4), s1);
        SafeRead32(reinterpret_cast<void*>(cands[i].addr+8), s2);
        Log("[FB-Dense]  [%zu] mp.dll+0x%zX run=%d dtor=%d  %08X %08X %08X\n",
            i, cands[i].addr - base, cands[i].run, (int)cands[i].dtorPair, s0, s1, s2);
    }

    size_t strOff = 0;
    for (size_t off = 0; off + 9 < mpSize; ++off) {
        if (_strnicmp(reinterpret_cast<const char*>(data + off), "frostbite", 9) == 0) {
            strOff = off; break;
        }
    }
    Log("[FB-Dense] frostbite string @ mp.dll+0x%zX\n", strOff);

    for (size_t i = 0; i < cands.size(); ++i) {
        if (!cands[i].dtorPair) continue;
        size_t vtOff = cands[i].addr - base;
        if (strOff) {
            size_t dist = vtOff > strOff ? vtOff - strOff : strOff - vtOff;
            if (dist > 0x600000) continue;
        }
        Log("[FB-Dense] BEST vtable @ 0x%08zX (mp.dll+0x%zX) run=%d\n",
            cands[i].addr, vtOff, cands[i].run);
        return cands[i].addr;
    }

    if (!cands.empty()) {
        Log("[FB-Dense] fallback: longest run @ 0x%08zX\n", cands[0].addr);
        return cands[0].addr;
    }
    Log("[FB-Dense] no candidate found\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Public: find frostbite vtable
// ---------------------------------------------------------------------------

uintptr_t FindWeaponFrostbiteVtable(HMODULE hMp)
{
    auto* nt = GetNtHeaders(hMp);
    if (!nt) { Log("[FB] GetNtHeaders failed\n"); return 0; }

    uintptr_t base   = reinterpret_cast<uintptr_t>(hMp);
    DWORD     mpSize = nt->OptionalHeader.SizeOfImage;
    auto*     data   = reinterpret_cast<uint8_t*>(base);
    uintptr_t textLo = base + 0x1000;
    uintptr_t textHi = base + (mpSize * 7 / 10);

    uintptr_t vt = RttiSearch(data, mpSize, base, textLo, textHi);
    if (!vt)    vt = DenseSearch(data, mpSize, base, textLo, textHi);
    return vt;
}

// ---------------------------------------------------------------------------
// Engfuncs validation  (no __try — plain range checks)
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
// GiveFnptrs_Init
// Has std::vector → __try must NOT appear in this function.
// All SEH probes go through SafeRead32 / ProbeGlobals.
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
    Log("[GiveFnptrs] hw.dll @ 0x%08zX  size=0x%X\n",
        hwBase, hwNt->OptionalHeader.SizeOfImage);

    auto* mpNt = GetNtHeaders(hMp);
    if (!mpNt) { Log("[GiveFnptrs] mp.dll bad headers\n"); return false; }
    uintptr_t mpBase = reinterpret_cast<uintptr_t>(hMp);
    DWORD     mpSize = mpNt->OptionalHeader.SizeOfImage;
    auto*     mpData = reinterpret_cast<uint8_t*>(mpBase);

    Log("[GiveFnptrs] Scanning mp.dll for engfuncs...\n");

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
                EngCand c;
                c.offset = runStart;
                c.run    = curRun;
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
            for (int j = 0; j < 30 && j < cands[i].run; ++j)
                Log("[GiveFnptrs]   [%2d] 0x%08X\n", j, ptrs[j]);
            foundOff = cands[i].offset;
            break;
        }
    }

    if (foundOff == (size_t)-1) {
        Log("[GiveFnptrs] No valid engfuncs found\n");
        return false;
    }

    memcpy(&g_engfuncs, mpData + foundOff, sizeof(g_engfuncs));
    Log("[GiveFnptrs] pfnSetOrigin[20] = 0x%08X\n",
        reinterpret_cast<uint32_t*>(&g_engfuncs)[20]);
    Log("[GiveFnptrs] engfuncs OK\n");

    // g_globals — use ProbeGlobals (separate fn with __try)
    Log("[GiveFnptrs] Scanning for g_globals...\n");
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
