#pragma once
#include <windows.h>
#include <cstdint>

// Register a weapon hook — call this from a global/static constructor
// classname : "weapon_janus1" etc.
// hookFn    : our __cdecl factory  void fn(int edict)
// origRVA   : RVA of the game's original factory in mp.dll
void  RegisterWeaponHook(const char* classname, void* hookFn, uintptr_t origRVA);

// Called once server is ready — installs all registered hooks
bool  Hooks_Install(HMODULE hMp);

// Accessors
uintptr_t GetMpBase();
float     GetTime();   // gpGlobals->time

// Write 5-byte JMP from->to, saves original bytes into outOrig (may be null)
bool WriteJmp5(uintptr_t from, uintptr_t to, uint8_t* outOrig);
