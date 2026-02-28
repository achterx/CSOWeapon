// janus1.cpp - Direct vtable patch on CJanus1 in mp.dll
// Slots verified from IDA: AddToPlayer=95, Deploy=102, WeaponIdle=142, Holster=168

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

static int __fastcall J1_Deploy(void* self, void* /*edx*/)
{
    Log("[janus1] Deploy enter\n");
    typedef int(__thiscall* Fn)(void*);
    int r = reinterpret_cast<Fn>(g_origDeploy)(self);
    Log("[janus1] Deploy exit ret=%d\n", r);
    return r;
}

static void __fastcall J1_WeaponIdle(void* self, void* /*edx*/)
{
    typedef void(__thiscall* Fn)(void*);
    reinterpret_cast<Fn>(g_origWeaponIdle)(self);
}

static int __fastcall J1_AddToPlayer(void* self, void* /*edx*/, void* player)
{
    Log("[janus1] AddToPlayer enter self=%p player=%p\n", self, player);
    typedef int(__thiscall* Fn)(void*, void*);
    int r = reinterpret_cast<Fn>(g_origAddToPlayer)(self, player);
    Log("[janus1] AddToPlayer exit ret=%d\n", r);
    return r;
}

static void __fastcall J1_Holster(void* self, void* /*edx*/, int skiplocal)
{
    Log("[janus1] Holster enter skip=%d\n", skiplocal);
    typedef void(__thiscall* Fn)(void*, int);
    reinterpret_cast<Fn>(g_origHolster)(self, skiplocal);
    Log("[janus1] Holster exit\n");
}

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
    Log("[janus1] Patching vtable @ %p\n", (void*)vtable);

    PatchVtableSlot(vtable, SLOT_Deploy,      (void*)J1_Deploy,      &g_origDeploy);
    PatchVtableSlot(vtable, SLOT_WeaponIdle,  (void*)J1_WeaponIdle,  &g_origWeaponIdle);
    PatchVtableSlot(vtable, SLOT_AddToPlayer, (void*)J1_AddToPlayer, &g_origAddToPlayer);
    PatchVtableSlot(vtable, SLOT_Holster,     (void*)J1_Holster,     &g_origHolster);

    Log("[janus1] PostInit done\n");
}

void __cdecl Janus1_Factory(int /*edict*/) {}
