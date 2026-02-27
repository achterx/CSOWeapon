// hooks.cpp - weapon entry point hooking engine
#include "hooks.h"
#include "logger.h"
#include "hlsdk/mp_offsets.h"
#include "hlsdk/sdk.h"
#include <cstring>

// Globals used by sdk.h helpers
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
    bool        done;
    uint8_t     origBytes[5]; // saved before patching
};
static HookEntry g_hooks[32];
static int       g_hookCount = 0;

void RegisterWeaponHook(const char* name, void* fn, uintptr_t rva)
{
    if (g_hookCount >= 32) return;
    g_hooks[g_hookCount++] = { name, fn, rva, false, {0,0,0,0,0} };
}

const uint8_t* GetSavedBytes(uintptr_t origRVA)
{
    for (int i = 0; i < g_hookCount; i++)
        if (g_hooks[i].origRVA == origRVA && g_hooks[i].done)
            return g_hooks[i].origBytes;
    return nullptr;
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
    *reinterpret_cast<int32_t*>(jmp+1) = (int32_t)(to - from - 5);
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
// Find gpGlobals->time and engfuncs in mp.dll
// -------------------------------------------------------------------------
static bool ResolveGlobals(HMODULE hMp)
{
    uintptr_t base = (uintptr_t)hMp;

    uint32_t pGlobals = 0;
    if (!SafeRead32(base + RVA_pGlobals, pGlobals) || !pGlobals)
    {
        Log("[hooks] gpGlobals ptr is null\n");
        return false;
    }
    g_pTime = reinterpret_cast<float*>((uintptr_t)pGlobals);
    Log("[hooks] gpGlobals @ 0x%08X  time=%.3f\n", pGlobals, *g_pTime);

    HMODULE hHw = GetModuleHandleA("hw.dll");
    if (!hHw) { Log("[hooks] hw.dll not found\n"); return false; }

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hMp;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)((uint8_t*)hMp + dos->e_lfanew);
    DWORD mpSize = nt->OptionalHeader.SizeOfImage;
    uint8_t* mpData = (uint8_t*)base;

    IMAGE_DOS_HEADER* hdos = (IMAGE_DOS_HEADER*)hHw;
    IMAGE_NT_HEADERS* hnt  = (IMAGE_NT_HEADERS*)((uint8_t*)hHw + hdos->e_lfanew);
    uintptr_t hwBase = (uintptr_t)hHw;
    uintptr_t hwEnd  = hwBase + hnt->OptionalHeader.SizeOfImage;

    size_t bestOff = 0; int bestRun = 0;
    int curRun = 0; size_t runStart = 0;
    for (size_t off = 0; off+4 <= mpSize; off += 4)
    {
        uint32_t v = 0;
        __try { v = *reinterpret_cast<uint32_t*>(mpData+off); }
        __except(EXCEPTION_EXECUTE_HANDLER) { curRun=0; continue; }
        if (v >= hwBase && v < hwEnd) { if (!curRun) runStart=off; curRun++; }
        else { if (curRun > bestRun) { bestRun=curRun; bestOff=runStart; } curRun=0; }
    }
    if (bestRun < 20) { Log("[hooks] engfuncs not found\n"); return false; }

    static enginefuncs_t ef;
    memcpy(&ef, mpData+bestOff, sizeof(ef));
    g_engfuncs = &ef;
    Log("[hooks] engfuncs @ mp+0x%zX  pfnPrecacheModel=0x%08X\n",
        bestOff, (uint32_t)(uintptr_t)ef.pfnPrecacheModel);
    return true;
}

// -------------------------------------------------------------------------
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
        // Save original bytes BEFORE patching
        __try { memcpy(h.origBytes, (void*)target, 5); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}

        if (WriteJmp5(target, (uintptr_t)h.hookFn, nullptr))
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
