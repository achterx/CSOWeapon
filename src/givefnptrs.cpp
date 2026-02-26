#include "givefnptrs.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <TlHelp32.h>

enginefuncs_t* g_engfuncs = nullptr;
globalvars_t*  g_globals  = nullptr;

static enginefuncs_t s_engfuncs{};
static globalvars_t* s_globals_ptr = nullptr;

// ---------------------------------------------------------------------------
// Log helper
// ---------------------------------------------------------------------------
static void GLog(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    const char* paths[] = { "frostbite_fix.log", "C:\\frostbite_fix.log",
                             "C:\\Temp\\frostbite_fix.log", nullptr };
    for (int i = 0; paths[i]; ++i) {
        FILE* f = fopen(paths[i], "a");
        if (f) { fputs(buf, f); fflush(f); fclose(f); break; }
    }
}

// ---------------------------------------------------------------------------
// Safe PE export lookup (SEH-protected)
// ---------------------------------------------------------------------------
static FARPROC SafeGetExport(HMODULE hMod, const char* exportName)
{
    if (!hMod) return nullptr;
    __try {
        auto* base = reinterpret_cast<uint8_t*>(hMod);
        auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)  return nullptr;
        auto& ed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!ed.VirtualAddress) return nullptr;
        auto* exp  = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + ed.VirtualAddress);
        auto* nams = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
        auto* ords = reinterpret_cast<WORD* >(base + exp->AddressOfNameOrdinals);
        auto* fns  = reinterpret_cast<DWORD*>(base + exp->AddressOfFunctions);
        for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
            if (strcmp(reinterpret_cast<const char*>(base + nams[i]), exportName) == 0)
                return reinterpret_cast<FARPROC>(base + fns[ords[i]]);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GLog("[GiveFnptrs] SafeGetExport SEH on %p for '%s'\n", hMod, exportName);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Dump raw bytes of a function (first N bytes) for diagnosis
// ---------------------------------------------------------------------------
static void DumpFuncBytes(void* fn, const char* name, int count = 32)
{
    if (!fn) return;
    __try {
        auto* p = reinterpret_cast<uint8_t*>(fn);
        char line[256]; int pos = 0;
        pos += snprintf(line, sizeof(line), "[GiveFnptrs] %s @ %p bytes: ", name, fn);
        for (int i = 0; i < count; ++i)
            pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", p[i]);
        line[pos] = '\n'; line[pos+1] = 0;
        GLog("%s", line);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GLog("[GiveFnptrs] DumpFuncBytes SEH on %p\n", fn);
    }
}

// ---------------------------------------------------------------------------
// CSNZ ARCHITECTURE (confirmed from log):
//
//   GiveFnptrsToDll lives in mp.dll at 0x242D70F0.
//   In standard GoldSrc: engine calls mp.dll's GiveFnptrsToDll(engfuncs, globals)
//   so mp.dll can receive engine function pointers.
//
//   CSNZ appears to work the same way — but the struct passed in is a CSNZ
//   custom enginefuncs that is MUCH LARGER than our stub enginefuncs_t.
//   When the engine calls GiveFnptrsToDll normally, it fills the struct.
//   But WE are calling it with no arguments yet — we need the engine to
//   call it, or we need to find the already-filled global pointer.
//
//   THE REAL SOLUTION:
//   mp.dll has already been called by the CSNZ engine before we get here.
//   The engine funcs are stored in a GLOBAL VARIABLE inside mp.dll.
//   We scan mp.dll's .data/.rdata section for a pointer that looks like
//   a valid enginefuncs_t (first entry = pfnPrecacheModel, which must point
//   into the engine DLL's code section — hw.dll @ 02B50000, size 69906432).
//
//   hw.dll range: 0x02B50000 .. 0x02B50000 + 69906432 = ~0x06AF4000
//   Any pointer in that range is almost certainly an engine function pointer.
// ---------------------------------------------------------------------------

// Check if 'ptr' looks like it points into the engine (hw.dll)
static bool IsEnginePtr(uint32_t ptr, uint32_t engBase, uint32_t engSize)
{
    return ptr >= engBase && ptr < engBase + engSize;
}

// ---------------------------------------------------------------------------
// Find the already-populated enginefuncs_t inside mp.dll's global data.
// Strategy: scan mp.dll .data section for a run of N consecutive pointers
// that all point into hw.dll. That's the engfuncs table.
// ---------------------------------------------------------------------------
static enginefuncs_t* FindEngfuncsInMpDll(HMODULE hMp, HMODULE hEngine)
{
    auto* mpBase   = reinterpret_cast<uint8_t*>(hMp);
    auto* mpDos    = reinterpret_cast<IMAGE_DOS_HEADER*>(mpBase);
    auto* mpNt     = reinterpret_cast<IMAGE_NT_HEADERS*>(mpBase + mpDos->e_lfanew);

    uint32_t engBase = (uint32_t)(uintptr_t)hEngine;
    uint32_t engSize = 0;
    __try {
        auto* engDos = reinterpret_cast<IMAGE_DOS_HEADER*>(hEngine);
        auto* engNt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
                           reinterpret_cast<uint8_t*>(hEngine) + engDos->e_lfanew);
        engSize = engNt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GLog("[GiveFnptrs] FindEngfuncs: SEH reading engine headers\n");
        return nullptr;
    }

    GLog("[GiveFnptrs] Engine range: %08X .. %08X\n", engBase, engBase + engSize);

    // Also include mp.dll range — some engfuncs may be forwarded/thunked inside mp
    uint32_t mpBase32  = (uint32_t)(uintptr_t)mpBase;
    uint32_t mpSize    = mpNt->OptionalHeader.SizeOfImage;

    // We need at least 8 consecutive engine pointers to be confident
    // (pfnPrecacheModel, pfnPrecacheSound, pfnSetModel, pfnModelIndex... all engine)
    constexpr int MIN_CONSECUTIVE = 8;

    auto* sec = IMAGE_FIRST_SECTION(mpNt);
    for (WORD si = 0; si < mpNt->FileHeader.NumberOfSections; ++si, ++sec) {
        // Only scan writable data sections (.data, .bss) — not .text or .rdata
        bool isData = (sec->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        bool isExec = (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (!isData || isExec) continue;

        uint8_t* sb   = mpBase + sec->VirtualAddress;
        size_t   ss   = sec->Misc.VirtualSize;
        char secName[9] = {};
        memcpy(secName, sec->Name, 8);
        GLog("[GiveFnptrs] Scanning mp.dll section %s (RW, %zu bytes) @ %p\n",
             secName, ss, sb);

        // Walk 4-byte aligned
        for (size_t off = 0; off + (MIN_CONSECUTIVE * 4) <= ss; off += 4) {
            int consecutive = 0;
            __try {
                for (int k = 0; k < 24; ++k) {  // check up to 24 slots
                    uint32_t v = *reinterpret_cast<uint32_t*>(sb + off + k * 4);
                    bool inEng = IsEnginePtr(v, engBase, engSize);
                    bool inMp  = (v >= mpBase32 && v < mpBase32 + mpSize);
                    if (inEng || inMp) {
                        consecutive++;
                    } else {
                        break;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { break; }

            if (consecutive >= MIN_CONSECUTIVE) {
                void** table = reinterpret_cast<void**>(sb + off);
                GLog("[GiveFnptrs] Candidate engfuncs table @ mp.dll+0x%X  (%d consecutive engine ptrs)\n",
                     (unsigned)(sb + off - mpBase), consecutive);
                // Log first 12 entries
                for (int k = 0; k < 12 && k < consecutive; ++k)
                    GLog("[GiveFnptrs]   [%2d] %p\n", k, table[k]);

                // Sanity: slot 0 (pfnPrecacheModel) and slot 1 (pfnPrecacheSound)
                // must be non-null engine ptrs
                if (IsEnginePtr((uint32_t)(uintptr_t)table[0], engBase, engSize) &&
                    IsEnginePtr((uint32_t)(uintptr_t)table[1], engBase, engSize)) {
                    GLog("[GiveFnptrs] CONFIRMED: this looks like enginefuncs_t!\n");
                    return reinterpret_cast<enginefuncs_t*>(table);
                }
            }
        }
    }

    GLog("[GiveFnptrs] FindEngfuncsInMpDll: not found\n");
    return nullptr;
}

// ---------------------------------------------------------------------------
// Also find g_globals (globalvars_t*) — it's a float* with time near 0..10000
// stored right after or near the engfuncs ptr in mp.dll globals
// ---------------------------------------------------------------------------
static globalvars_t* FindGlobalsInMpDll(HMODULE hMp, enginefuncs_t* engfuncs)
{
    // globalvars_t** is typically stored right after the enginefuncs_t* global,
    // or it's a global ptr in mp.dll's .data that points to a float (time value 0..86400)
    // We'll scan for a pointer whose target[0] (time) is 0 <= x <= 86400 and
    // target[1] (frametime) is 0 <= x <= 1.0

    auto* mpBase = reinterpret_cast<uint8_t*>(hMp);
    auto* dos    = reinterpret_cast<IMAGE_DOS_HEADER*>(mpBase);
    auto* nt     = reinterpret_cast<IMAGE_NT_HEADERS*>(mpBase + dos->e_lfanew);
    uint32_t mpBase32 = (uint32_t)(uintptr_t)mpBase;
    uint32_t mpSize   = nt->OptionalHeader.SizeOfImage;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD si = 0; si < nt->FileHeader.NumberOfSections; ++si, ++sec) {
        bool isData = (sec->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        bool isExec = (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (!isData || isExec) continue;

        uint8_t* sb = mpBase + sec->VirtualAddress;
        size_t   ss = sec->Misc.VirtualSize;

        for (size_t off = 0; off + 4 <= ss; off += 4) {
            __try {
                uint32_t ptr = *reinterpret_cast<uint32_t*>(sb + off);
                // Must point somewhere valid (not null, not obviously wrong)
                if (ptr < 0x00010000 || ptr > 0x7FFFFFFF) continue;
                // The pointed-to memory should be readable
                float time      = *reinterpret_cast<float*>(ptr + 0);
                float frametime = *reinterpret_cast<float*>(ptr + 4);
                // time in [0, 86400], frametime in [0, 0.2]
                if (time >= 0.f && time <= 86400.f &&
                    frametime >= 0.f && frametime <= 0.2f) {
                    GLog("[GiveFnptrs] Candidate g_globals @ mp.dll+0x%X  ptr=%p  time=%.3f  frametime=%.6f\n",
                         (unsigned)(sb + off - mpBase), (void*)(uintptr_t)ptr, time, frametime);
                    return reinterpret_cast<globalvars_t*>((void*)(uintptr_t)ptr);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
        }
        break; // Only scan first writable section
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// GiveFnptrs_Init
// ---------------------------------------------------------------------------
bool GiveFnptrs_Init(HMODULE hMpDll)
{
    GLog("[GiveFnptrs] GiveFnptrs_Init starting\n");

    HMODULE hMp = hMpDll ? hMpDll : GetModuleHandleA("mp.dll");
    if (!hMp) { GLog("[GiveFnptrs] mp.dll not found\n"); return false; }

    // hw.dll is the engine — confirmed 02B50000, size 69MB from previous log
    HMODULE hEngine = GetModuleHandleA("hw.dll");
    if (!hEngine) hEngine = GetModuleHandleA("swds.dll");
    if (!hEngine) hEngine = GetModuleHandleA("engine.dll");
    GLog("[GiveFnptrs] mp.dll=%p  hw.dll=%p\n", hMp, hEngine);

    if (!hEngine) {
        GLog("[GiveFnptrs] FATAL: engine DLL (hw.dll) not found\n");
        return false;
    }

    // ------------------------------------------------------------------
    // APPROACH 1: Scan mp.dll .data for the already-filled engfuncs table.
    // The engine called GiveFnptrsToDll(mp) before we got here, so the
    // table is already populated — we just need to find it.
    // ------------------------------------------------------------------
    GLog("[GiveFnptrs] === APPROACH 1: scan mp.dll .data for engfuncs table ===\n");
    enginefuncs_t* found = nullptr;
    __try {
        found = FindEngfuncsInMpDll(hMp, hEngine);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GLog("[GiveFnptrs] SEH in FindEngfuncsInMpDll\n");
    }

    if (found) {
        g_engfuncs = found;
        GLog("[GiveFnptrs] engfuncs found via scan: %p\n", found);

        // Find globals
        __try {
            g_globals = FindGlobalsInMpDll(hMp, found);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            GLog("[GiveFnptrs] SEH in FindGlobalsInMpDll\n");
        }
        GLog("[GiveFnptrs] g_globals = %p\n", g_globals);
        GLog("[GiveFnptrs] SUCCESS via scan\n");
        return true;
    }

    // ------------------------------------------------------------------
    // APPROACH 2: Dump the GiveFnptrsToDll function bytes in mp.dll
    // so we can see what it actually does and reverse it properly.
    // Also dump mp.dll exports related to engine funcs.
    // ------------------------------------------------------------------
    GLog("[GiveFnptrs] === APPROACH 2: diagnostic dump ===\n");

    FARPROC pfnGive = SafeGetExport(hMp, "GiveFnptrsToDll");
    GLog("[GiveFnptrs] GiveFnptrsToDll in mp.dll @ %p\n", pfnGive);
    if (pfnGive) DumpFuncBytes((void*)pfnGive, "GiveFnptrsToDll", 64);

    // Also look for GetEntityAPI, GetNewDLLFunctions — common mp.dll exports
    // that receive engine funcs via different paths
    const char* interestingExports[] = {
        "GetEntityAPI", "GetEntityAPI2", "GetNewDLLFunctions",
        "Server_GetPhysicsInterface",
        nullptr
    };
    for (int i = 0; interestingExports[i]; ++i) {
        FARPROC fn = SafeGetExport(hMp, interestingExports[i]);
        if (fn) {
            GLog("[GiveFnptrs] mp.dll exports '%s' @ %p\n", interestingExports[i], fn);
            DumpFuncBytes((void*)fn, interestingExports[i], 32);
        }
    }

    // Dump mp.dll export list (first 60)
    __try {
        auto* base = reinterpret_cast<uint8_t*>(hMp);
        auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        auto& ed   = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        auto* exp  = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + ed.VirtualAddress);
        auto* nams = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
        DWORD cnt  = exp->NumberOfNames < 60 ? exp->NumberOfNames : 60;
        GLog("[GiveFnptrs] mp.dll has %lu exports, first %lu:\n", exp->NumberOfNames, cnt);
        for (DWORD k = 0; k < cnt; ++k)
            GLog("[GiveFnptrs]   [%3lu] %s\n", k,
                 reinterpret_cast<const char*>(base + nams[k]));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GLog("[GiveFnptrs] SEH dumping mp.dll exports\n");
    }

    GLog("[GiveFnptrs] FATAL: could not find engfuncs — need diagnostic output above\n");
    return false;
}
