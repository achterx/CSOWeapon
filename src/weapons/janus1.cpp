// janus1.cpp - Direct vtable patch on CJanus1 in mp.dll
// CRITICAL: vtable slots are called as __thiscall by the engine.
// Wrappers must be __thiscall. Use a dummy class to get __thiscall methods.

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include <cstring>
#include <cstdint>
#include <windows.h>

static const uintptr_t RVA_CJanus1_vtable = 0x1649034;

static const int SLOT_AddToPlayer = 95;
static const int SLOT_Deploy      = 102;
static const int SLOT_WeaponIdle  = 142;
static const int SLOT_Holster     = 168;

static void* g_origDeploy      = nullptr;
static void* g_origWeaponIdle  = nullptr;
static void* g_origAddToPlayer = nullptr;
static void* g_origHolster     = nullptr;

// Dummy class so we can write __thiscall methods
struct CJanus1Hook
{
    int Deploy()
    {
        Log("[janus1] Deploy\n");
        typedef int(__thiscall* Fn)(void*);
        return reinterpret_cast<Fn>(g_origDeploy)(this);
    }

    void WeaponIdle()
    {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_origWeaponIdle)(this);
    }

    int AddToPlayer(void* player)
    {
        Log("[janus1] AddToPlayer\n");
        typedef int(__thiscall* Fn)(void*, void*);
        return reinterpret_cast<Fn>(g_origAddToPlayer)(this, player);
    }

    void Holster()
    {
        Log("[janus1] Holster\n");
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_origHolster)(this);
    }
};

static bool PatchVtableSlot(void** vtable, int slot, void* newFn, void** outOrig)
{
    void** entry = &vtable[slot];
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
    {
        Log("[janus1] VirtualProtect failed slot %d\n", slot);
        return false;
    }
    *outOrig = *entry;
    *entry = newFn;
    VirtualProtect(entry, sizeof(void*), old, &old);
    return true;
}

void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit\n");
    void** vtable = reinterpret_cast<void**>(mpBase + RVA_CJanus1_vtable);

    // Get method pointers from the dummy class - these are __thiscall
    void* fnDeploy      = (void*)(int(CJanus1Hook::*)())      &CJanus1Hook::Deploy;
    void* fnWeaponIdle  = (void*)(void(CJanus1Hook::*)())     &CJanus1Hook::WeaponIdle;
    void* fnAddToPlayer = (void*)(int(CJanus1Hook::*)(void*)) &CJanus1Hook::AddToPlayer;
    void* fnHolster     = (void*)(void(CJanus1Hook::*)())     &CJanus1Hook::Holster;

    PatchVtableSlot(vtable, SLOT_Deploy,      fnDeploy,      &g_origDeploy);
    PatchVtableSlot(vtable, SLOT_WeaponIdle,  fnWeaponIdle,  &g_origWeaponIdle);
    PatchVtableSlot(vtable, SLOT_AddToPlayer, fnAddToPlayer, &g_origAddToPlayer);
    PatchVtableSlot(vtable, SLOT_Holster,     fnHolster,     &g_origHolster);

    Log("[janus1] PostInit done\n");
}

void __cdecl Janus1_Factory(int /*edict*/) {}
