#include "givefnptrs.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

enginefuncs_t* g_engfuncs = nullptr;
globalvars_t*  g_globals  = nullptr;

static enginefuncs_t s_engfuncs{};
static globalvars_t* s_globals_ptr = nullptr;

// ---------------------------------------------------------------------------
// GiveFnptrs_Init
// ---------------------------------------------------------------------------
// CSOWeapon(1).dll confirmed flow from IDA .i64 analysis:
//   1. GetModuleHandle("hw.dll")   (client/listen) or "swds.dll" (dedicated)
//   2. GetProcAddress(hEngine, "GiveFnptrsToDll")
//   3. pfn(&s_engfuncs, &s_globals_ptr)
//
// ROOT BUG IN v1-v3:
//   Called GetProcAddress(hMpDll, "GiveFnptrsToDll").
//   GiveFnptrsToDll lives on the ENGINE DLL (hw.dll / swds.dll), not mp.dll.
//   mp.dll is the game library that RECEIVES the engine funcs.
// ---------------------------------------------------------------------------
bool GiveFnptrs_Init(HMODULE /*hMpDll — kept for API compat, unused*/)
{
    char buf[1024];

    // ------------------------------------------------------------------
    // 1. Find the engine DLL — try all known names
    // ------------------------------------------------------------------
    const char* engineNames[] = { "hw.dll", "swds.dll", "engine.dll", nullptr };
    HMODULE hEngine   = nullptr;
    const char* eName = nullptr;

    for (int i = 0; engineNames[i]; ++i) {
        hEngine = GetModuleHandleA(engineNames[i]);
        if (hEngine) { eName = engineNames[i]; break; }
    }

    if (!hEngine) {
        OutputDebugStringA("[GiveFnptrs] ERROR: engine DLL not loaded yet (hw.dll / swds.dll / engine.dll)\n");
        // Probe what IS loaded to help diagnose timing issues
        const char* probes[] = {
            "hl.exe","hlds.exe","cso.exe","dedicated.exe",
            "tier0.dll","filesystem_stdio.dll", nullptr
        };
        for (int i = 0; probes[i]; ++i) {
            if (GetModuleHandleA(probes[i])) {
                snprintf(buf, sizeof(buf), "[GiveFnptrs]   loaded: %s\n", probes[i]);
                OutputDebugStringA(buf);
            }
        }
        return false;
    }

    snprintf(buf, sizeof(buf), "[GiveFnptrs] Engine DLL: %s base=%p\n", eName, hEngine);
    OutputDebugStringA(buf);

    // ------------------------------------------------------------------
    // 2. GetProcAddress for GiveFnptrsToDll
    //    Try engine DLL first, then mp.dll as fallback for odd builds
    // ------------------------------------------------------------------
    FARPROC pfnRaw = GetProcAddress(hEngine, "GiveFnptrsToDll");
    if (!pfnRaw) {
        snprintf(buf, sizeof(buf), "[GiveFnptrs] Not in %s, trying mp.dll\n", eName);
        OutputDebugStringA(buf);
        HMODULE hMp = GetModuleHandleA("mp.dll");
        if (hMp) pfnRaw = GetProcAddress(hMp, "GiveFnptrsToDll");
    }

    if (!pfnRaw) {
        OutputDebugStringA("[GiveFnptrs] ERROR: GiveFnptrsToDll not found! Dumping engine exports:\n");
        auto* base = reinterpret_cast<uint8_t*>(hEngine);
        auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            auto& ed  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
            if (ed.VirtualAddress) {
                auto* exp  = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + ed.VirtualAddress);
                auto* nams = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
                DWORD cnt  = exp->NumberOfNames < 80 ? exp->NumberOfNames : 80;
                for (DWORD k = 0; k < cnt; ++k) {
                    snprintf(buf, sizeof(buf), "[GiveFnptrs]   export: %s\n",
                        reinterpret_cast<const char*>(base + nams[k]));
                    OutputDebugStringA(buf);
                }
            }
        }
        return false;
    }

    snprintf(buf, sizeof(buf), "[GiveFnptrs] GiveFnptrsToDll @ %p — calling\n", pfnRaw);
    OutputDebugStringA(buf);

    // ------------------------------------------------------------------
    // 3. Call it
    // ------------------------------------------------------------------
    auto pfn = reinterpret_cast<GiveFnptrsToDll_t>(pfnRaw);
    memset(&s_engfuncs, 0, sizeof(s_engfuncs));
    s_globals_ptr = nullptr;
    pfn(&s_engfuncs, &s_globals_ptr);
    g_engfuncs = &s_engfuncs;
    g_globals  = s_globals_ptr;

    // ------------------------------------------------------------------
    // 4. Log every ptr we depend on
    // ------------------------------------------------------------------
    snprintf(buf, sizeof(buf),
        "[GiveFnptrs] engfuncs dump:\n"
        "  g_globals            = %p\n"
        "  pfnPrecacheModel     = %p\n"
        "  pfnAngleVectors      = %p\n"
        "  pfnCreateNamedEntity = %p\n"
        "  pfnAllocString       = %p\n"
        "  pfnIndexOfEdict      = %p\n"
        "  pfnRandomFloat       = %p\n"
        "  pfnTime              = %p\n"
        "  pfnPrecacheEvent     = %p\n"
        "  pfnPlaybackEvent     = %p\n"
        "  pfnSetOrigin         = %p\n",
        g_globals,
        g_engfuncs->pfnPrecacheModel,
        g_engfuncs->pfnAngleVectors,
        g_engfuncs->pfnCreateNamedEntity,
        g_engfuncs->pfnAllocString,
        g_engfuncs->pfnIndexOfEdict,
        g_engfuncs->pfnRandomFloat,
        g_engfuncs->pfnTime,
        g_engfuncs->pfnPrecacheEvent,
        g_engfuncs->pfnPlaybackEvent,
        g_engfuncs->pfnSetOrigin);
    OutputDebugStringA(buf);

    if (!g_globals)
        OutputDebugStringA("[GiveFnptrs] WARNING: g_globals null — gpGlobals->time unavailable\n");
    if (!g_engfuncs->pfnCreateNamedEntity) {
        OutputDebugStringA("[GiveFnptrs] ERROR: pfnCreateNamedEntity null — engfuncs not filled\n");
        return false;
    }

    OutputDebugStringA("[GiveFnptrs] SUCCESS\n");
    return true;
}
