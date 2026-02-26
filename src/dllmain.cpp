#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>

// Forward declarations
bool FrostbiteFix_Init(HMODULE hMpDll);
void FrostbiteFix_Shutdown();

// ============================================================
// Init state
// ============================================================
static bool    g_initialized  = false;
static HMODULE g_hMpDll       = nullptr;

// ============================================================
// Log helper — available before frostbite_fix.cpp opens its log
// Writes to both OutputDebugString and a fallback log file.
// Tries several paths in case the CWD is not writable.
// ============================================================
static FILE* g_earlyLog = nullptr;

static void EarlyLog(const char* msg)
{
    OutputDebugStringA(msg);
    if (!g_earlyLog) {
        // Try several locations for the log file
        const char* paths[] = {
            "frostbite_fix.log",          // CWD (usually hlds/cstrike dir)
            "C:\\frostbite_fix.log",
            "C:\\Temp\\frostbite_fix.log",
            nullptr
        };
        for (int i = 0; paths[i]; ++i) {
            g_earlyLog = fopen(paths[i], "a");
            if (g_earlyLog) break;
        }
    }
    if (g_earlyLog) {
        fputs(msg, g_earlyLog);
        fflush(g_earlyLog);
    }
}

// ============================================================
// TryInit — called on mp.dll load AND from the retry thread.
// Waits until both mp.dll AND the engine DLL are in memory
// before proceeding, since GiveFnptrsToDll lives on hw.dll/swds.dll.
// ============================================================
static void TryInit()
{
    if (g_initialized) return;

    HMODULE hMp = GetModuleHandleA("mp.dll");
    if (!hMp) {
        EarlyLog("[frostbite_fix] TryInit: mp.dll not loaded yet\n");
        return;
    }

    // Engine DLL must also be present — GiveFnptrsToDll is its export
    HMODULE hEngine = GetModuleHandleA("hw.dll");
    if (!hEngine) hEngine = GetModuleHandleA("swds.dll");
    if (!hEngine) hEngine = GetModuleHandleA("engine.dll");
    if (!hEngine) {
        EarlyLog("[frostbite_fix] TryInit: engine DLL not loaded yet — will retry\n");
        return;
    }

    g_initialized = true;
    g_hMpDll      = hMp;

    char buf[512];
    snprintf(buf, sizeof(buf),
        "[frostbite_fix] TryInit: mp.dll=%p  engine=%p — calling FrostbiteFix_Init\n",
        hMp, hEngine);
    EarlyLog(buf);

    FrostbiteFix_Init(hMp);
}

// ============================================================
// Retry thread — polls until both DLLs are present.
// Handles the race where our DLL loads before hw.dll.
// ============================================================
static DWORD WINAPI RetryThread(LPVOID)
{
    EarlyLog("[frostbite_fix] RetryThread started\n");
    for (int attempt = 0; attempt < 60 && !g_initialized; ++attempt) {
        Sleep(500);  // poll every 500ms, up to 30 seconds
        TryInit();
    }
    if (!g_initialized)
        EarlyLog("[frostbite_fix] RetryThread: gave up after 30s — init never succeeded\n");
    else
        EarlyLog("[frostbite_fix] RetryThread: init succeeded, thread exiting\n");
    return 0;
}

// ============================================================
// IAT hook helpers — intercept LoadLibraryA/W so we react
// the moment mp.dll or hw.dll is loaded dynamically.
// ============================================================
static decltype(&LoadLibraryA) orig_LoadLibraryA = nullptr;
static decltype(&LoadLibraryW) orig_LoadLibraryW = nullptr;

static bool IsInterestingDll(const char* name)
{
    if (!name) return false;
    const char* base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    return _stricmp(base, "mp.dll")    == 0
        || _stricmp(base, "hw.dll")    == 0
        || _stricmp(base, "swds.dll")  == 0
        || _stricmp(base, "engine.dll")== 0;
}

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR name)
{
    HMODULE h = orig_LoadLibraryA(name);
    if (h && IsInterestingDll(name)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[frostbite_fix] LoadLibraryA: %s loaded\n", name);
        EarlyLog(buf);
        TryInit();
    }
    return h;
}

static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR name)
{
    HMODULE h = orig_LoadLibraryW(name);
    if (h && name) {
        // Quick ASCII check for the dll names we care about
        char narrow[MAX_PATH];
        WideCharToMultiByte(CP_ACP, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
        if (IsInterestingDll(narrow)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "[frostbite_fix] LoadLibraryW: %s loaded\n", narrow);
            EarlyLog(buf);
            TryInit();
        }
    }
    return h;
}

static bool HookIAT(HMODULE hMod, const char* dll, const char* func,
                    void* newFn, void** origFn)
{
    auto* base = reinterpret_cast<uint8_t*>(hMod);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& id = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!id.VirtualAddress) return false;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + id.VirtualAddress);
    for (; desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<char*>(base + desc->Name), dll) != 0) continue;
        auto* thunk  = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        auto* oThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        for (size_t i = 0; thunk[i].u1.Function; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL(oThunk[i].u1.Ordinal)) continue;
            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + oThunk[i].u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(ibn->Name), func) != 0) continue;
            if (origFn) *origFn = reinterpret_cast<void*>(thunk[i].u1.Function);
            DWORD old;
            VirtualProtect(&thunk[i].u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            thunk[i].u1.Function = reinterpret_cast<ULONG_PTR>(newFn);
            VirtualProtect(&thunk[i].u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// ============================================================
// DllMain
// ============================================================
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInst);
        EarlyLog("[frostbite_fix] *** DLL_PROCESS_ATTACH — DLL is loaded! ***\n");

        // Hook LoadLibraryA/W in our own IAT
        HookIAT(hInst, "kernel32.dll", "LoadLibraryA",
                (void*)Hook_LoadLibraryA, (void**)&orig_LoadLibraryA);
        HookIAT(hInst, "kernel32.dll", "LoadLibraryW",
                (void*)Hook_LoadLibraryW, (void**)&orig_LoadLibraryW);
        if (!orig_LoadLibraryA) orig_LoadLibraryA = LoadLibraryA;
        if (!orig_LoadLibraryW) orig_LoadLibraryW = LoadLibraryW;

        // Try immediately — works if we're injected after everything is loaded
        TryInit();

        // Spawn retry thread for the common case where hw.dll isn't loaded yet
        if (!g_initialized) {
            EarlyLog("[frostbite_fix] Spawning retry thread (engine not ready yet)\n");
            HANDLE hThread = CreateThread(nullptr, 0, RetryThread, nullptr, 0, nullptr);
            if (hThread) CloseHandle(hThread);
        }
        break;

    case DLL_PROCESS_DETACH:
        FrostbiteFix_Shutdown();
        if (g_earlyLog) { fclose(g_earlyLog); g_earlyLog = nullptr; }
        break;
    }
    return TRUE;
}
