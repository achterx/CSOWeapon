#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>

// Forward declarations
bool FrostbiteFix_Init(HMODULE hMpDll);
void FrostbiteFix_Shutdown();

// ============================================================
// LoadLibraryA hook — mirrors CSOWeapon(1).dll behavior exactly.
// We intercept LoadLibraryA/W so we can react the moment
// the engine loads mp.dll, then call our init.
// ============================================================

// Original function pointers
static decltype(&LoadLibraryA) orig_LoadLibraryA = nullptr;
static decltype(&LoadLibraryW) orig_LoadLibraryW = nullptr;

static bool g_initialized = false;

static void TryInit()
{
    if (g_initialized) return;

    HMODULE hMp = GetModuleHandleA("mp.dll");
    if (!hMp) return;

    g_initialized = true;

    char buf[MAX_PATH];
    GetModuleFileNameA(hMp, buf, sizeof(buf));

    // Also grab hw.dll size like CSOWeapon.dll does (for validation)
    HMODULE hHw = GetModuleHandleA("hw.dll");
    if (hHw) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hHw);
        auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<uint8_t*>(hHw) + dos->e_lfanew);
        OutputDebugStringA("[frostbite_fix] hw.dll found\n");
        (void)nt->OptionalHeader.SizeOfImage; // just validate it's accessible
    }

    FrostbiteFix_Init(hMp);
}

static HMODULE WINAPI Hook_LoadLibraryA(LPCSTR lpLibFileName)
{
    HMODULE hMod = orig_LoadLibraryA(lpLibFileName);

    if (hMod && lpLibFileName) {
        // Check if this is mp.dll being loaded
        const char* name = strrchr(lpLibFileName, '\\');
        name = name ? name + 1 : lpLibFileName;
        if (_stricmp(name, "mp.dll") == 0) {
            TryInit();
        }
    }
    return hMod;
}

static HMODULE WINAPI Hook_LoadLibraryW(LPCWSTR lpLibFileName)
{
    HMODULE hMod = orig_LoadLibraryW(lpLibFileName);

    if (hMod && lpLibFileName) {
        const wchar_t* name = wcsrchr(lpLibFileName, L'\\');
        name = name ? name + 1 : lpLibFileName;
        if (_wcsicmp(name, L"mp.dll") == 0) {
            TryInit();
        }
    }
    return hMod;
}

// ============================================================
// Minimal IAT hook — patches kernel32.dll's LoadLibraryA/W
// in our own module's import table.
// Simpler than MinHook, sufficient for this use case.
// ============================================================
static bool HookIAT(HMODULE hModule, const char* dllName,
                    const char* funcName, void* newFunc, void** origFunc)
{
    auto* base = reinterpret_cast<uint8_t*>(hModule);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDir.VirtualAddress) return false;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
    for (; desc->Name; ++desc) {
        const char* name = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(name, dllName) != 0) continue;

        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);

        for (size_t i = 0; thunk[i].u1.Function; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk[i].u1.Ordinal)) continue;

            auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + origThunk[i].u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(ibn->Name), funcName) != 0) continue;

            if (origFunc) *origFunc = reinterpret_cast<void*>(thunk[i].u1.Function);

            DWORD old;
            VirtualProtect(&thunk[i].u1.Function, sizeof(void*), PAGE_READWRITE, &old);
            thunk[i].u1.Function = reinterpret_cast<ULONG_PTR>(newFunc);
            VirtualProtect(&thunk[i].u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// ============================================================
// DllMain
// ============================================================
BOOL WINAPI DllMain(HINSTANCE hInstDll, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInstDll);

        // Hook our module's IAT for LoadLibraryA/W
        HookIAT(hInstDll, "kernel32.dll", "LoadLibraryA",
                (void*)Hook_LoadLibraryA, (void**)&orig_LoadLibraryA);
        HookIAT(hInstDll, "kernel32.dll", "LoadLibraryW",
                (void*)Hook_LoadLibraryW, (void**)&orig_LoadLibraryW);

        // Fallback originals if IAT hook missed
        if (!orig_LoadLibraryA) orig_LoadLibraryA = LoadLibraryA;
        if (!orig_LoadLibraryW) orig_LoadLibraryW = LoadLibraryW;

        // If mp.dll is ALREADY loaded (late injection scenario), init now
        TryInit();

        OutputDebugStringA("[frostbite_fix] Attached — watching for mp.dll\n");
        break;

    case DLL_PROCESS_DETACH:
        FrostbiteFix_Shutdown();
        break;
    }
    return TRUE;
}
