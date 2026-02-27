#pragma once
#include <windows.h>
#include <cstdint>

void           RegisterWeaponHook(const char* classname, void* hookFn, uintptr_t origRVA);
bool           Hooks_Install(HMODULE hMp);
const uint8_t* GetSavedBytes(uintptr_t origRVA);
uintptr_t      GetMpBase();
float          GetTime();
bool           WriteJmp5(uintptr_t from, uintptr_t to, uint8_t* outOrig);
