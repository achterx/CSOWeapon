// givefnptrs.cpp — CSNZ frostbite fix, v7
#define _CRT_SECURE_NO_WARNINGS
// Fixes:
//  1. Correct vtable detection (was finding entity-registry struct, not vtable)
//  2. Log all engfuncs candidates with their index contents for manual validation
//  3. Robust fallback: try all candidate engfuncs tables until SetOrigin looks valid

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <algorithm>
#include "givefnptrs.h"
#include "logger.h"

enginefuncs_t g_engfuncs = {};
globalvars_t* g_pGlobals = nullptr;

// ─── PE helpers ─────────────────────────────────────────────────────────────

static bool SafeRead32(const void* addr, uint32_t& out) {
    __try {
        out = *reinterpret_cast<const uint32_t*>(addr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static IMAGE_NT_HEADERS* GetNtHeaders(HMODULE hMod) {
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<uint8_t*>(hMod) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        return nt;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// ─── Find a vtable for CWeaponFrostbite using two strategies ────────────────
//
// Strategy A: RTTI scan
//   MSVC embeds ".?AVCWeaponFrostbite@@" (or similar) as a TypeDescriptor name.
//   The Complete Object Locator that references it points to the vftable.
//
// Strategy B: Dense pointer block scan
//   The vtable is a contiguous array of 50+ pointers, all within mp.dll .text,
//   residing in mp.dll .rdata or .data.  We pick the LONGEST run that also has
//   slot[0] == slot[1] (MSVC dtor pair) or slot[0] close to slot[1].
//
// Strategy C (was v6, now rejected):
//   Reading a ptr before a PUSH of the "frostbite" string → gives entity-registry
//   struct, not the vtable.  We no longer use this.

struct VtableCandidate {
    uintptr_t addr;
    int        run;      // number of consecutive mp.dll .text ptrs
    bool       hasDtorPair; // slot[0]==slot[1]
};

static uintptr_t FindFrostbiteVtable(HMODULE hMp) {
    auto* nt = GetNtHeaders(hMp);
    if (!nt) { Log("[FB] GetNtHeaders failed for mp.dll\n"); return 0; }

    auto base   = reinterpret_cast<uintptr_t>(hMp);
    auto mpSize = nt->OptionalHeader.SizeOfImage;
    auto mpEnd  = base + mpSize;

    // mp.dll .text range heuristic: first ~70% of image is code
    uintptr_t textLo = base + 0x1000;
    uintptr_t textHi = base + (mpSize * 7 / 10);

    // ── Strategy A: RTTI TypeDescriptor scan ─────────────────────────────
    // Search for type-name substrings that MSVC embeds for CWeaponFrostbite
    // MSVC mangled: ".?AVCWeaponFrostbite@@"  or  ".?AVCCSWeaponFrostBite@@" etc.
    const char* rttiNames[] = {
        "?AVCWeaponFrostbite",
        "?AVCWeaponFrostBite",
        "?AVCCSWeaponFrostbite",
        "?AVCCSWeaponFrostBite",
        "?AVWeaponFrostbite",
        nullptr
    };

    auto* data = reinterpret_cast<uint8_t*>(base);

    for (int ni = 0; rttiNames[ni]; ni++) {
        const char* needle = rttiNames[ni];
        size_t nlen = strlen(needle);
        Log("[FB-RTTI] Searching for RTTI name '%s'...\n", needle);
        for (size_t off = 0; off + nlen + 4 < mpSize; off++) {
            if (memcmp(data + off, needle, nlen) == 0) {
                uintptr_t typeDescName = base + off;
                // TypeDescriptor is at typeDescName - 8 (pVFTable ptr + spare)
                // Actually the TypeDescriptor starts 8 bytes before the name string
                uintptr_t typeDesc = typeDescName - 8;
                Log("[FB-RTTI] TypeDescriptor candidate @ mp.dll+0x%zX  (name='%s')\n",
                    off - 8, needle);
                // Now search .rdata/.data for a CompleteObjectLocator that refs typeDesc
                // COL layout: sig(4) offset(4) cdOffset(4) pTypeDesc(4) pClassDesc(4)
                // We scan for any DWORD == typeDesc
                for (size_t off2 = 0; off2 + 20 < mpSize; off2 += 4) {
                    uint32_t v = 0;
                    if (!SafeRead32(data + off2, v)) continue;
                    if (v == (uint32_t)typeDesc) {
                        // possible COL + 0xC
                        uintptr_t colAddr = base + off2 - 12;
                        uint32_t sig = 0, colOff = 0;
                        SafeRead32((void*)colAddr, sig);
                        SafeRead32((void*)(colAddr + 4), colOff);
                        if (sig != 0) continue; // COL.signature must be 0 for 32-bit
                        // vftable is at colAddr + 4 (ptr to COL sits 4 bytes before vftable[0])
                        uintptr_t vtableAddr = colAddr + 4 + 4; // skip back-ptr then start
                        // Actually: the ptr-to-COL sits immediately before vftable[0]
                        // So if colAddr is the COL, search for a ptr TO colAddr in .rdata
                        for (size_t off3 = 0; off3 + 4 < mpSize; off3 += 4) {
                            uint32_t pCol = 0;
                            if (!SafeRead32(data + off3, pCol)) continue;
                            if (pCol == (uint32_t)colAddr) {
                                uintptr_t vt = base + off3 + 4; // vftable[0] is right after ptr-to-COL
                                // Validate: first slot should point into mp.dll .text
                                uint32_t slot0 = 0;
                                SafeRead32((void*)vt, slot0);
                                if (slot0 >= textLo && slot0 < textHi) {
                                    Log("[FB-RTTI] FOUND vtable via RTTI @ 0x%08zX  (mp.dll+0x%zX)  slot[0]=0x%08X\n",
                                        vt, vt - base, slot0);
                                    return vt;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Log("[FB-RTTI] RTTI scan did not find vtable, falling back to dense-pointer scan\n");

    // ── Strategy B: Dense pointer block scan ─────────────────────────────
    // We scan ALL of mp.dll (treating everything as potential ptr arrays).
    // For each 4-byte aligned address, count how many consecutive DWORDs point
    // into [textLo, textHi).  Track the best runs (≥50 ptrs).
    // Among those, prefer runs where slot[0]==slot[1] (MSVC dtor pair).

    std::vector<VtableCandidate> candidates;
    int    curRun  = 0;
    size_t runStart = 0;

    for (size_t off = 0; off + 4 <= mpSize; off += 4) {
        uint32_t v = 0;
        bool ok = SafeRead32(data + off, v);
        if (ok && v >= (uint32_t)textLo && v < (uint32_t)textHi) {
            if (curRun == 0) runStart = off;
            curRun++;
        } else {
            if (curRun >= 50) {
                // Check for dtor pair
                uint32_t s0 = 0, s1 = 0;
                SafeRead32(data + runStart, s0);
                SafeRead32(data + runStart + 4, s1);
                VtableCandidate c;
                c.addr = base + runStart;
                c.run  = curRun;
                c.hasDtorPair = (s0 == s1);
                candidates.push_back(c);
            }
            curRun = 0;
        }
    }

    // Sort: dtor pairs first, then by run length descending
    std::sort(candidates.begin(), candidates.end(), [](const VtableCandidate& a, const VtableCandidate& b) {
        if (a.hasDtorPair != b.hasDtorPair) return a.hasDtorPair > b.hasDtorPair;
        return a.run > b.run;
    });

    Log("[FB-Dense] Found %zu dense vtable candidates (run>=50, in mp.dll .text)\n", candidates.size());
    for (size_t i = 0; i < candidates.size() && i < 10; i++) {
        uint32_t s0=0,s1=0,s2=0;
        SafeRead32((void*)candidates[i].addr, s0);
        SafeRead32((void*)(candidates[i].addr+4), s1);
        SafeRead32((void*)(candidates[i].addr+8), s2);
        Log("[FB-Dense]   [%zu] @ 0x%08zX (mp.dll+0x%zX) run=%d dtorPair=%d  s[0..2]=%08X %08X %08X\n",
            i, candidates[i].addr, candidates[i].addr - base,
            candidates[i].run, candidates[i].hasDtorPair, s0, s1, s2);
    }

    // Now narrow down: the frostbite vtable should be near (within 0x200000 of)
    // the "frostbite" string we know is at mp.dll+0x15D48BC (VA ~0x247148BC)
    // But that varies per run — use the string search to find a close candidate
    const char* needle = "frostbite";
    size_t nlen = strlen(needle);
    uintptr_t frostbiteStringOff = 0;
    for (size_t off = 0; off + nlen < mpSize; off++) {
        if (_strnicmp(reinterpret_cast<char*>(data + off), needle, nlen) == 0) {
            frostbiteStringOff = off;
            break;
        }
    }
    Log("[FB-Dense] frostbite string at mp.dll+0x%zX\n", frostbiteStringOff);

    // Among candidates with dtor pair, pick the one whose vtable offset is
    // within 0x400000 of the frostbite string (same class group)
    for (auto& c : candidates) {
        if (!c.hasDtorPair) continue;
        size_t vtOff = c.addr - base;
        if (frostbiteStringOff > 0) {
            size_t dist = (vtOff > frostbiteStringOff)
                ? (vtOff - frostbiteStringOff)
                : (frostbiteStringOff - vtOff);
            if (dist > 0x600000) continue;
        }
        Log("[FB-Dense] BEST candidate vtable @ 0x%08zX (mp.dll+0x%zX)  run=%d\n",
            c.addr, vtOff, c.run);
        return c.addr;
    }

    // Fallback: longest run overall
    if (!candidates.empty()) {
        Log("[FB-Dense] Fallback: using longest run vtable @ 0x%08zX\n", candidates[0].addr);
        return candidates[0].addr;
    }

    Log("[FB-Dense] No vtable candidate found!\n");
    return 0;
}

// ─── Engfuncs validation ────────────────────────────────────────────────────
// In standard HLSDK enginefuncs_s:
//  index 0  = pfnPrecacheModel
//  index 1  = pfnPrecacheSound
//  index 2  = pfnSetModel
//  index 3  = pfnModelIndex
//  index 4  = pfnModelFrames
//  index 5  = pfnSetSize
//  index 6  = pfnChangeLevel
//  ...
//  index 20 = pfnSetOrigin
//  ...
//  DispatchSpawn doesn't have a standard engfunc index — it's in gamedll_funcs
//
// For our patch we only need pfnSetOrigin (engfuncs[20] in standard HLSDK)
// Let's verify by checking that engfuncs[20] points into hw.dll

static bool ValidateEngfuncs(enginefuncs_t* ef, uintptr_t hwBase, uintptr_t hwEnd) {
    auto* ptrs = reinterpret_cast<uint32_t*>(ef);
    // Check first 5 entries all point into hw.dll
    int valid = 0;
    for (int i = 0; i < 5; i++) {
        uint32_t p = ptrs[i];
        if (p >= hwBase && p < hwEnd) valid++;
    }
    if (valid < 4) return false;
    // Check pfnSetOrigin (index 20 in standard HLSDK) also points into hw.dll
    uint32_t setOriginPtr = ptrs[20];
    return (setOriginPtr >= hwBase && setOriginPtr < hwEnd);
}

// ─── g_globals scan — plain function, no C++ objects, so __try is allowed ───

static globalvars_t* ScanForGlobals(const uint8_t* mpData, size_t mpSize) {
    for (size_t off = 0; off + 4 <= mpSize; off += 4) {
        uint32_t ptrVal = 0;
        if (!SafeRead32(mpData + off, ptrVal)) continue;
        if (ptrVal < 0x10000 || ptrVal > 0x7FFFFFFF) continue;
        __try {
            const globalvars_t* g = reinterpret_cast<const globalvars_t*>((uintptr_t)ptrVal);
            if (g->maxClients >= 1 && g->maxClients <= 64 &&
                g->time >= 0.0f && g->frametime >= 0.0f && g->frametime < 1.0f) {
                return const_cast<globalvars_t*>(g);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    return nullptr;
}

// ─── Main init ──────────────────────────────────────────────────────────────

bool GiveFnptrs_Init(HMODULE hMp) {
    Log("[GiveFnptrs] GiveFnptrs_Init starting, mp.dll=0x%08X\n", (uintptr_t)hMp);

    // Locate hw.dll
    HMODULE hHw = GetModuleHandleA("hw.dll");
    if (!hHw) { Log("[GiveFnptrs] hw.dll not found!\n"); return false; }
    auto* hwNt = GetNtHeaders(hHw);
    if (!hwNt) { Log("[GiveFnptrs] hw.dll NT headers invalid!\n"); return false; }
    uintptr_t hwBase = reinterpret_cast<uintptr_t>(hHw);
    uintptr_t hwEnd  = hwBase + hwNt->OptionalHeader.SizeOfImage;
    Log("[GiveFnptrs] hw.dll @ 0x%08zX  size=0x%X\n", hwBase, hwNt->OptionalHeader.SizeOfImage);

    // ── Find engfuncs in mp.dll ─────────────────────────────────────────
    auto* mpNt   = GetNtHeaders(hMp);
    if (!mpNt) { Log("[GiveFnptrs] mp.dll NT headers invalid!\n"); return false; }
    auto  mpBase = reinterpret_cast<uintptr_t>(hMp);
    auto  mpSize = mpNt->OptionalHeader.SizeOfImage;
    auto* mpData = reinterpret_cast<uint8_t*>(mpBase);

    Log("[GiveFnptrs] Scanning mp.dll for engfuncs (looking for 20+ consecutive hw.dll ptrs)...\n");

    struct EngCandidate { size_t offset; int run; };
    std::vector<EngCandidate> engCandidates;

    int curRun = 0; size_t runStart = 0;
    for (size_t off = 0; off + 4 <= mpSize; off += 4) {
        uint32_t v = 0;
        bool ok = SafeRead32(mpData + off, v);
        if (ok && v >= (uint32_t)hwBase && v < (uint32_t)hwEnd) {
            if (curRun == 0) runStart = off;
            curRun++;
        } else {
            if (curRun >= 20) {
                engCandidates.push_back({runStart, curRun});
            }
            curRun = 0;
        }
    }

    Log("[GiveFnptrs] Found %zu engfuncs candidates (run>=20)\n", engCandidates.size());

    // Sort by run length descending
    std::sort(engCandidates.begin(), engCandidates.end(),
        [](const EngCandidate& a, const EngCandidate& b){ return a.run > b.run; });

    enginefuncs_t* found = nullptr;
    for (auto& c : engCandidates) {
        auto* ef = reinterpret_cast<enginefuncs_t*>(mpData + c.offset);
        auto* ptrs = reinterpret_cast<uint32_t*>(ef);
        Log("[GiveFnptrs] Candidate @ mp.dll+0x%zX  run=%d  [0]=%08X [1]=%08X [20]=%08X\n",
            c.offset, c.run, ptrs[0], ptrs[1], ptrs[20]);
        if (ValidateEngfuncs(ef, hwBase, hwEnd)) {
            Log("[GiveFnptrs] VALIDATED engfuncs @ mp.dll+0x%zX\n", c.offset);
            // Dump first 30 for diagnostics
            for (int i = 0; i < 30 && i < c.run; i++) {
                Log("[GiveFnptrs]   [%2d] 0x%08X\n", i, ptrs[i]);
            }
            found = ef;
            break;
        }
    }

    if (!found) {
        Log("[GiveFnptrs] No valid engfuncs table found!\n");
        return false;
    }

    g_engfuncs = *found;

    // Log key functions
    auto* ptrs = reinterpret_cast<uint32_t*>(&g_engfuncs);
    Log("[GiveFnptrs] pfnSetOrigin (index 20) = 0x%08X\n", ptrs[20]);
    Log("[GiveFnptrs] engfuncs SUCCESS\n");

    // g_globals found by ScanForGlobals (plain C helper, no C++ objects, can use __try)
    Log("[GiveFnptrs] Scanning for g_globals...\n");
    g_pGlobals = ScanForGlobals(mpData, mpSize);
    if (!g_pGlobals) {
        Log("[GiveFnptrs] WARNING: g_globals not found, continuing anyway\n");
    } else {
        Log("[GiveFnptrs] g_globals = 0x%08X  (time=%.3f  maxClients=%d)\n",
            (uintptr_t)g_pGlobals, g_pGlobals->time, g_pGlobals->maxClients);
    }

    return true;
}

uintptr_t FindWeaponFrostbiteVtable(HMODULE hMp) {
    return FindFrostbiteVtable(hMp);
}

    return true;
}

uintptr_t FindWeaponFrostbiteVtable(HMODULE hMp) {
    return FindFrostbiteVtable(hMp);
}
