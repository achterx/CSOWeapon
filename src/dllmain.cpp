// dllmain.cpp  v8
// Wait for mp.dll AND for gpGlobals to be non-null before patching.
// gpGlobals is set when the server calls GiveFnptrsToDll — if we patch
// before that, all time-based logic sees t=0 forever.

#include <windows.h>
#include <cstdint>
#include "givefnptrs.h"
#include "logger.h"

bool FrostbiteFix_Init(HMODULE hMp);

// RVA of the gpGlobals pointer in mp.dll (immediately after engfuncs block)
// engfuncs RVA 0x1E51878 + 218*4 = 0x1E51BE0
static const uintptr_t kRVA_ppGlobals = 0x1E51BE0;

static DWORD WINAPI RetryThread(LPVOID)
{
    for (int i = 0; i < 240; i++)  // up to 120 seconds
    {
        Sleep(500);

        HMODULE hMp = GetModuleHandleA("mp.dll");
        if (!hMp) {
            Log("[frostbite_fix] TryInit: mp.dll not loaded yet\n");
            continue;
        }

        // Check if gpGlobals has been set yet (i.e. GiveFnptrsToDll was called)
        uintptr_t mpBase = reinterpret_cast<uintptr_t>(hMp);
        uint32_t* ppGlobals = reinterpret_cast<uint32_t*>(mpBase + kRVA_ppGlobals);
        uint32_t globalsVal = 0;
        __try { globalsVal = *ppGlobals; } __except(EXCEPTION_EXECUTE_HANDLER) {}

        if (!globalsVal) {
            Log("[frostbite_fix] TryInit: mp.dll loaded but gpGlobals=0, waiting...\n");
            continue;
        }

        HMODULE hHw = GetModuleHandleA("hw.dll");
        Log("[frostbite_fix] TryInit: mp.dll=0x%08zX hw.dll=0x%08zX gpGlobals=0x%08X — init\n",
            mpBase, (uintptr_t)hHw, globalsVal);

        __try {
            bool ok = FrostbiteFix_Init(hMp);
            if (ok) {
                Log("[frostbite_fix] RetryThread: init succeeded\n");
                return 0;
            }
            Log("[frostbite_fix] RetryThread: init returned false, retrying...\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[frostbite_fix] RetryThread: EXCEPTION, retrying...\n");
        }
    }
    Log("[frostbite_fix] RetryThread: gave up after 120s\n");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        Log("[frostbite_fix] *** DLL_PROCESS_ATTACH ***\n");
        DisableThreadLibraryCalls(hInst);
        Log("[frostbite_fix] Spawning retry thread\n");
        HANDLE hThread = CreateThread(nullptr, 0, RetryThread, nullptr, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}