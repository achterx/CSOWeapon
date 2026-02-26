// dllmain.cpp  v9
// Wait until mp.dll is loaded AND gpGlobals->time > 0 (server is running).
// The time float lives at *(float*)(*(uintptr_t*)(mp+0x1E51BCC)).
// We poll it until it's > 0.1 before calling FrostbiteFix_Init.

#include <windows.h>
#include <cstdint>
#include "givefnptrs.h"
#include "logger.h"

bool FrostbiteFix_Init(HMODULE hMp);

// dword_11E51BCC RVA — holds gpGlobals pointer (or is itself the time ptr)
static const uintptr_t kRVA_pGlobals = 0x1E51BCC;

static float ReadTime(HMODULE hMp)
{
    uintptr_t mpBase = reinterpret_cast<uintptr_t>(hMp);
    // dword_11E51BCC is a pointer: *(float*)(*(uint32_t*)(mp + RVA)) = time
    uint32_t inner = 0;
    __try { inner = *reinterpret_cast<uint32_t*>(mpBase + kRVA_pGlobals); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -1.0f; }
    if (!inner) return 0.0f;
    float t = 0.0f;
    __try { t = *reinterpret_cast<float*>((uintptr_t)inner); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return -2.0f; }
    return t;
}

static DWORD WINAPI RetryThread(LPVOID)
{
    for (int i = 0; i < 240; i++)  // up to 120 seconds
    {
        Sleep(500);

        HMODULE hMp = GetModuleHandleA("mp.dll");
        if (!hMp) {
            if (i % 4 == 0) Log("[frostbite_fix] waiting for mp.dll...\n");
            continue;
        }

        float t = ReadTime(hMp);
        if (t < 0.1f) {
            if (i % 4 == 0)
                Log("[frostbite_fix] mp.dll loaded, waiting for server start (t=%.3f)...\n", t);
            continue;
        }

        HMODULE hHw = GetModuleHandleA("hw.dll");
        Log("[frostbite_fix] mp.dll=0x%08zX hw.dll=0x%08zX t=%.3f — INIT\n",
            (uintptr_t)hMp, (uintptr_t)hHw, t);

        __try {
            if (FrostbiteFix_Init(hMp)) {
                Log("[frostbite_fix] init OK\n");
                return 0;
            }
            Log("[frostbite_fix] init failed, retrying...\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[frostbite_fix] EXCEPTION during init, retrying...\n");
        }
    }
    Log("[frostbite_fix] gave up after 120s\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        Log("[frostbite_fix] DLL_PROCESS_ATTACH\n");
        DisableThreadLibraryCalls(hInst);
        HANDLE h = CreateThread(nullptr, 0, RetryThread, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
    }
    return TRUE;
}