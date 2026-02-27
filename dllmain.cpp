// dllmain.cpp
// DLL injection entry point.
// Step 4 from the plan: on DLL attach, hook the weapon_janus1 entry point.
// We wait for the server to be fully running first.

#include <windows.h>
#include "hooks.h"
#include "logger.h"
#include "hlsdk/mp_offsets.h"

void Janus1_PostInit(uintptr_t mpBase);

static float ReadTime(HMODULE hMp)
{
    uint32_t pGlobals = 0;
    __try { pGlobals = *reinterpret_cast<uint32_t*>((uintptr_t)hMp + RVA_pGlobals); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -1.f; }
    if (!pGlobals) return 0.f;
    float t = 0.f;
    __try { t = *reinterpret_cast<float*>((uintptr_t)pGlobals); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -2.f; }
    return t;
}

static DWORD WINAPI MainThread(LPVOID)
{
    Log("=== csnz_weapons loaded ===\n");
    Log("Following approach: HLSDK weapon class, injected DLL, entry point hook\n");

    for (int i = 0; i < 480; i++)
    {
        Sleep(500);

        HMODULE hMp = GetModuleHandleA("mp.dll");
        if (!hMp) { if (i%10==0) Log("[main] Waiting for mp.dll...\n"); continue; }

        float t = ReadTime(hMp);
        if (t < 0.1f) { if (i%10==0) Log("[main] Waiting for server (t=%.3f)...\n",t); continue; }

        Log("[main] Server ready! mp=0x%08zX t=%.3f\n", (uintptr_t)hMp, t);

        // Step 4: hook entry points
        if (!Hooks_Install(hMp))
        {
            Log("[main] Hooks_Install failed, retrying...\n");
            continue;
        }

        // Post-init per weapon (build vtables, resolve fns)
        Janus1_PostInit(GetMpBase());

        Log("[main] All done. Hooks active.\n");
        return 0;
    }

    Log("[main] Timed out.\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        Log("[main] DLL_PROCESS_ATTACH\n");
        DisableThreadLibraryCalls(hInst);
        HANDLE h = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}
