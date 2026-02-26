#include "givefnptrs.h"
#include <cstdio>

enginefuncs_t* g_engfuncs = nullptr;
globalvars_t*  g_globals   = nullptr;

// Static storage — GiveFnptrsToDll writes into these
static enginefuncs_t s_engfuncs{};
static globalvars_t* s_globals_ptr = nullptr;

// The actual callback signature mp.dll expects:
// void GiveFnptrsToDll(enginefuncs_t* pengfuncsFromEngine, globalvars_t* pGlobals)
// It fills pengfuncsFromEngine IN PLACE (copies engine vtable into our struct)
// and sets pGlobals to point at the engine's live globalvars_t.

bool GiveFnptrs_Init(HMODULE hMpDll)
{
    if (!hMpDll) {
        OutputDebugStringA("[frostbite_fix] GiveFnptrs_Init: hMpDll is null\n");
        return false;
    }

    auto pfn = reinterpret_cast<GiveFnptrsToDll_t>(
        GetProcAddress(hMpDll, "GiveFnptrsToDll"));

    if (!pfn) {
        OutputDebugStringA("[frostbite_fix] GiveFnptrsToDll export not found in mp.dll!\n");
        return false;
    }

    // Call it — fills s_engfuncs in place, and writes the engine's globalvars_t* into s_globals_ptr
    pfn(&s_engfuncs, &s_globals_ptr);

    g_engfuncs = &s_engfuncs;
    g_globals  = s_globals_ptr;

    char buf[256];
    snprintf(buf, sizeof(buf),
        "[frostbite_fix] GiveFnptrsToDll OK — pfnTime=%p pfnPlaybackEvent=%p\n",
        g_engfuncs->pfnTime, g_engfuncs->pfnPlaybackEvent);
    OutputDebugStringA(buf);

    return (g_engfuncs->pfnCreateNamedEntity != nullptr);
}
