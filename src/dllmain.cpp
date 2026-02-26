// dllmain.cpp — CSNZ frostbite fix, v7

#include <windows.h>
#include <cstdint>
#include "givefnptrs.h"
#include "logger.h"

bool FrostbiteFix_Init(HMODULE hMp);

static DWORD WINAPI RetryThread(LPVOID) {
    for (int i = 0; i < 120; i++) {
        Sleep(500);
        HMODULE hMp = GetModuleHandleA("mp.dll");
        if (!hMp) {
            Log("[frostbite_fix] TryInit: mp.dll not loaded yet\n");
            continue;
        }
        uintptr_t mpBase = reinterpret_cast<uintptr_t>(hMp);
        uintptr_t engBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("hw.dll"));
        Log("[frostbite_fix] TryInit: mp.dll=0x%08zX  hw.dll=0x%08zX — calling FrostbiteFix_Init\n",
            mpBase, engBase);

        __try {
            bool ok = FrostbiteFix_Init(hMp);
            if (ok) {
                Log("[frostbite_fix] RetryThread: init succeeded, thread exiting\n");
                return 0;
            }
            Log("[frostbite_fix] RetryThread: init returned false, retrying...\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[frostbite_fix] RetryThread: EXCEPTION during init, retrying...\n");
        }
    }
    Log("[frostbite_fix] RetryThread: gave up after 60s\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        Log("[frostbite_fix] *** DLL_PROCESS_ATTACH — DLL loaded! ***\n");
        DisableThreadLibraryCalls(hInst);

        HMODULE hMp = GetModuleHandleA("mp.dll");
        if (hMp) {
            uintptr_t mpBase = reinterpret_cast<uintptr_t>(hMp);
            uintptr_t engBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("hw.dll"));
            Log("[frostbite_fix] mp.dll already loaded: 0x%08zX  hw.dll=0x%08zX\n", mpBase, engBase);
            __try {
                FrostbiteFix_Init(hMp);
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                Log("[frostbite_fix] EXCEPTION in immediate init\n");
            }
        } else {
            Log("[frostbite_fix] Spawning retry thread (mp.dll not ready yet)\n");
            HANDLE hThread = CreateThread(nullptr, 0, RetryThread, nullptr, 0, nullptr);
            if (hThread) CloseHandle(hThread);
        }
    }
    return TRUE;
}
