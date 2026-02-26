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
// Internal log — appends to frostbite_fix.log (same file as main logger)
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
// Safe PE export lookup — SEH-protected, no GetProcAddress.
// Returns nullptr if headers are invalid or export not found.
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
        GLog("[GiveFnptrs] SafeGetExport: SEH exception on %p for '%s'\n", hMod, exportName);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Dump all exports of a module (diagnostic)
// ---------------------------------------------------------------------------
static void DumpExports(HMODULE hMod, const char* modName, int limit = 80)
{
    if (!hMod) return;
    __try {
        auto* base = reinterpret_cast<uint8_t*>(hMod);
        auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) { GLog("[GiveFnptrs]   %s: bad DOS sig\n", modName); return; }
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)  { GLog("[GiveFnptrs]   %s: bad PE sig\n",  modName); return; }
        auto& ed = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!ed.VirtualAddress) { GLog("[GiveFnptrs]   %s: no exports\n", modName); return; }
        auto* exp  = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + ed.VirtualAddress);
        auto* nams = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
        DWORD cnt = exp->NumberOfNames < (DWORD)limit ? exp->NumberOfNames : (DWORD)limit;
        GLog("[GiveFnptrs]   %s has %lu exports (showing first %lu):\n",
             modName, exp->NumberOfNames, cnt);
        for (DWORD k = 0; k < cnt; ++k)
            GLog("[GiveFnptrs]     [%3lu] %s\n", k,
                 reinterpret_cast<const char*>(base + nams[k]));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GLog("[GiveFnptrs]   %s: SEH during dump\n", modName);
    }
}

// ---------------------------------------------------------------------------
// Scan every loaded module for 'exportName'.
// This is the fallback for when the engine DLL has an unexpected name.
// ---------------------------------------------------------------------------
static FARPROC ScanAllModules(const char* exportName, char* outModName, size_t outLen)
{
    GLog("[GiveFnptrs] Full module scan for '%s'...\n", exportName);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) {
        GLog("[GiveFnptrs] Snapshot failed: %lu\n", GetLastError());
        return nullptr;
    }
    MODULEENTRY32 me; me.dwSize = sizeof(me);
    FARPROC result = nullptr;
    if (Module32First(snap, &me)) {
        do {
            HMODULE h = reinterpret_cast<HMODULE>(me.modBaseAddr);
            FARPROC fn = SafeGetExport(h, exportName);
            GLog("[GiveFnptrs]   %-32s @ %p  size=%7lu  %s\n",
                 me.szModule, me.modBaseAddr, me.modBaseSize, fn ? "<-- FOUND" : "");
            if (fn) {
                if (outModName) { strncpy(outModName, me.szModule, outLen-1); outModName[outLen-1]=0; }
                result = fn;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return result;
}

// ---------------------------------------------------------------------------
// GiveFnptrs_Init
// ---------------------------------------------------------------------------
bool GiveFnptrs_Init(HMODULE /*hMpDll — API compat, unused*/)
{
    GLog("[GiveFnptrs] GiveFnptrs_Init starting\n");

    FARPROC pfnRaw = nullptr;
    char foundIn[MAX_PATH] = {};

    // ------------------------------------------------------------------
    // 1. Try known names — include CSNZ-specific ones.
    //    The server log shows [ENGDLL] tag — try engdll.dll first.
    // ------------------------------------------------------------------
    const char* candidates[] = {
        "engdll.dll",    // CSNZ dedicated server engine (seen in log tag)
        "hw.dll",        // GoldSrc listen server
        "swds.dll",      // GoldSrc dedicated server
        "engine.dll",    // some wrappers
        "csods.exe",     // CSNZ dedi exe — may export directly
        "hlds.exe",
        "hl.exe",
        nullptr
    };

    for (int i = 0; candidates[i] && !pfnRaw; ++i) {
        HMODULE h = GetModuleHandleA(candidates[i]);
        if (!h) continue;
        GLog("[GiveFnptrs] %s is loaded @ %p\n", candidates[i], h);
        FARPROC fn = SafeGetExport(h, "GiveFnptrsToDll");
        if (fn) {
            pfnRaw = fn;
            strncpy(foundIn, candidates[i], sizeof(foundIn)-1);
            GLog("[GiveFnptrs] GiveFnptrsToDll in %s @ %p\n", candidates[i], fn);
        } else {
            GLog("[GiveFnptrs]   %s loaded but no GiveFnptrsToDll export\n", candidates[i]);
            // Dump its exports so we know what's there
            DumpExports(h, candidates[i], 30);
        }
    }

    // ------------------------------------------------------------------
    // 2. Scan everything
    // ------------------------------------------------------------------
    if (!pfnRaw) {
        pfnRaw = ScanAllModules("GiveFnptrsToDll", foundIn, sizeof(foundIn));
    }

    // ------------------------------------------------------------------
    // 3. Try mp.dll as last resort (some CSNZ builds forward the export)
    // ------------------------------------------------------------------
    if (!pfnRaw) {
        HMODULE hMp = GetModuleHandleA("mp.dll");
        if (hMp) {
            pfnRaw = SafeGetExport(hMp, "GiveFnptrsToDll");
            if (pfnRaw) { strcpy(foundIn, "mp.dll"); }
        }
    }

    if (!pfnRaw) {
        GLog("[GiveFnptrs] FATAL: GiveFnptrsToDll not found in any module!\n");
        // Full module list for diagnostics
        GLog("[GiveFnptrs] All loaded modules:\n");
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE) {
            MODULEENTRY32 me; me.dwSize = sizeof(me);
            if (Module32First(snap, &me)) {
                do { GLog("[GiveFnptrs]   %-32s @ %p  size=%lu\n",
                          me.szModule, me.modBaseAddr, me.modBaseSize);
                } while (Module32Next(snap, &me));
            }
            CloseHandle(snap);
        }
        return false;
    }

    // ------------------------------------------------------------------
    // 4. Call it — SEH-protected in case struct layout is wrong
    // ------------------------------------------------------------------
    GLog("[GiveFnptrs] Calling GiveFnptrsToDll @ %p (found in %s)\n", pfnRaw, foundIn);
    __try {
        auto pfn = reinterpret_cast<GiveFnptrsToDll_t>(pfnRaw);
        memset(&s_engfuncs, 0, sizeof(s_engfuncs));
        s_globals_ptr = nullptr;
        pfn(&s_engfuncs, &s_globals_ptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        GLog("[GiveFnptrs] SEH EXCEPTION calling GiveFnptrsToDll!\n");
        GLog("[GiveFnptrs] enginefuncs_t layout likely wrong for this CSNZ build.\n");
        return false;
    }

    g_engfuncs = &s_engfuncs;
    g_globals  = s_globals_ptr;

    // ------------------------------------------------------------------
    // 5. Log results
    // ------------------------------------------------------------------
    GLog("[GiveFnptrs] g_globals            = %p\n", g_globals);
    GLog("[GiveFnptrs] pfnPrecacheModel     = %p\n", g_engfuncs->pfnPrecacheModel);
    GLog("[GiveFnptrs] pfnPrecacheSound     = %p\n", g_engfuncs->pfnPrecacheSound);
    GLog("[GiveFnptrs] pfnAngleVectors      = %p\n", g_engfuncs->pfnAngleVectors);
    GLog("[GiveFnptrs] pfnCreateNamedEntity = %p\n", g_engfuncs->pfnCreateNamedEntity);
    GLog("[GiveFnptrs] pfnAllocString       = %p\n", g_engfuncs->pfnAllocString);
    GLog("[GiveFnptrs] pfnIndexOfEdict      = %p\n", g_engfuncs->pfnIndexOfEdict);
    GLog("[GiveFnptrs] pfnRandomFloat       = %p\n", g_engfuncs->pfnRandomFloat);
    GLog("[GiveFnptrs] pfnPrecacheEvent     = %p\n", g_engfuncs->pfnPrecacheEvent);
    GLog("[GiveFnptrs] pfnPlaybackEvent     = %p\n", g_engfuncs->pfnPlaybackEvent);

    if (!g_engfuncs->pfnPrecacheModel) {
        GLog("[GiveFnptrs] ERROR: pfnPrecacheModel null — struct layout mismatch!\n");
        GLog("[GiveFnptrs] GiveFnptrsToDll ran but filled nothing. Wrong struct size.\n");
        return false;
    }

    if (!g_globals)
        GLog("[GiveFnptrs] WARNING: g_globals null — gpGlobals->time = 0\n");

    GLog("[GiveFnptrs] SUCCESS\n");
    return true;
}
