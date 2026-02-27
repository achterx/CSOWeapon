// hooks.cpp
#include "hooks.h"
#include "logger.h"
#include "hlsdk/mp_offsets.h"
#include "hlsdk/sdk.h"
#include <cstring>

enginefuncs_t* g_engfuncs = nullptr;
float*         g_pTime    = nullptr;

static uintptr_t g_mpBase = 0;
uintptr_t GetMpBase() { return g_mpBase; }

float GetTime()
{
    if (!g_pTime) return 0.f;
    float t = 0.f;
    __try { t = *g_pTime; } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return t;
}

// -------------------------------------------------------------------------
// Registration table
// -------------------------------------------------------------------------
struct HookEntry
{
    const char* name;
    void*       hookFn;
    uintptr_t   origRVA;
    uint8_t     origBytes[5];
    bool        done;
};

static HookEntry g_hooks[32];
static int       g_hookCount = 0;

void RegisterWeaponHook(const char* name, void* fn, uintptr_t rva)
{
    if (g_hookCount >= 32) return;
    HookEntry& h = g_hooks[g_hookCount++];
    h.name    = name;
    h.hookFn  = fn;
    h.origRVA = rva;
    h.done    = false;
    memset(h.origBytes, 0x90, 5);
}

// -------------------------------------------------------------------------
// Memory helpers
// -------------------------------------------------------------------------
static bool SafeRead32(uintptr_t addr, uint32_t& out)
{
    __try { out = *reinterpret_cast<uint32_t*>(addr); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool WriteJmp5(uintptr_t from, uintptr_t to, uint8_t* outOrig)
{
    DWORD old = 0;
    if (!VirtualProtect((void*)from, 5, PAGE_EXECUTE_READWRITE, &old))
        return false;
    if (outOrig)
    {
        __try { memcpy(outOrig, (void*)from, 5); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    uint8_t jmp[5] = { 0xE9 };
    *reinterpret_cast<int32_t*>(jmp + 1) = (int32_t)(to - from - 5);
    __try { memcpy((void*)from, jmp, 5); }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        VirtualProtect((void*)from, 5, old, &old);
        return false;
    }
    VirtualProtect((void*)from, 5, old, &old);
    return true;
}

// -------------------------------------------------------------------------
// Resolve gpGlobals and engfuncs
// RVA_engfuncs = 0x1E51878  (confirmed from log: engfuncs @ mp+0x1E51878
//                             pfnPrecacheModel=0x03125190 which is valid hw.dll)
// -------------------------------------------------------------------------
static const uintptr_t RVA_engfuncs = 0x1E51878;

static bool ResolveGlobals(HMODULE hMp)
{
    uintptr_t base = (uintptr_t)hMp;

    // gpGlobals->time
    uint32_t pGlobals = 0;
    if (!SafeRead32(base + RVA_pGlobals, pGlobals) || !pGlobals)
    {
        Log("[hooks] gpGlobals ptr null\n");
        return false;
    }
    g_pTime = reinterpret_cast<float*>((uintptr_t)pGlobals);
    Log("[hooks] gpGlobals @ 0x%08X  time=%.3f\n", pGlobals, *g_pTime);

    // Engfuncs — fixed RVA confirmed from logs
    static enginefuncs_t ef;
    void* engfuncsPtr = reinterpret_cast<void*>(base + RVA_engfuncs);
    __try { memcpy(&ef, engfuncsPtr, sizeof(ef)); }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[hooks] Failed to read engfuncs\n");
        return false;
    }
    g_engfuncs = &ef;
    Log("[hooks] engfuncs @ mp+0x%zX  pfnPrecacheModel=0x%08X  pfnPrecacheSound=0x%08X\n",
        RVA_engfuncs,
        (uint32_t)(uintptr_t)ef.pfnPrecacheModel,
        (uint32_t)(uintptr_t)ef.pfnPrecacheSound);

    return true;
}

// -------------------------------------------------------------------------
const uint8_t* GetSavedBytes(uintptr_t origRVA)
{
    for (int i = 0; i < g_hookCount; i++)
        if (g_hooks[i].origRVA == origRVA && g_hooks[i].done)
            return g_hooks[i].origBytes;
    return nullptr;
}

bool Hooks_Install(HMODULE hMp)
{
    g_mpBase = (uintptr_t)hMp;
    Log("[hooks] Hooks_Install mp=0x%08zX\n", g_mpBase);

    if (!ResolveGlobals(hMp)) return false;

    int n = 0;
    for (int i = 0; i < g_hookCount; i++)
    {
        HookEntry& h = g_hooks[i];
        uintptr_t target = g_mpBase + h.origRVA;
        if (WriteJmp5(target, (uintptr_t)h.hookFn, h.origBytes))
        {
            h.done = true; n++;
            Log("[hooks] %-20s patched 0x%08zX -> 0x%08zX  orig: %02X %02X %02X %02X %02X\n",
                h.name, target, (uintptr_t)h.hookFn,
                h.origBytes[0], h.origBytes[1], h.origBytes[2],
                h.origBytes[3], h.origBytes[4]);
        }
        else Log("[hooks] FAILED: %s\n", h.name);
    }
    Log("[hooks] %d/%d installed\n", n, g_hookCount);
    return n == g_hookCount;
}