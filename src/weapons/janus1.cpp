// janus1.cpp - Janus1 weapon
// 
// NEW APPROACH: Instead of copying+swapping the whole vtable,
// directly patch the specific slots in the ORIGINAL CJanus1 vtable in mp.dll.
// The vtable is in .rdata (read-only), so we VirtualProtect it writable,
// write our function pointers, then restore protection.
//
// This is cleaner: no vtable copy, no object-finding, no pointer chain.
// The original vtable is patched once at startup and affects ALL Janus1 objects.
//
// Slots from IDA (RVA from mp base = 0x1649034):
//   Slot 102: Deploy      @ vtable + 102*4
//   Slot 142: WeaponIdle  @ vtable + 142*4  
//   Slot  95: AddToPlayer @ vtable + 95*4
//   Slot 168: Holster     @ vtable + 168*4

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

// -----------------------------------------------------------------------
// Overrides
// -----------------------------------------------------------------------
static int __fastcall J1_Deploy(void* self, void* /*edx*/)
{
    Log("[janus1] Deploy\n");
    typedef int(__thiscall* Fn)(void*);
    return reinterpret_cast<Fn>(g_origDeploy)(self);
}

static void __fastcall J1_WeaponIdle(void* self, void* /*edx*/)
{
    typedef void(__thiscall* Fn)(void*);
    reinterpret_cast<Fn>(g_origWeaponIdle)(self);
}

static int __fastcall J1_AddToPlayer(void* self, void* /*edx*/, void* player)
{
    Log("[janus1] AddToPlayer\n");
    typedef int(__thiscall* Fn)(void*, void*);
    return reinterpret_cast<Fn>(g_origAddToPlayer)(self, player);
}

static void __fastcall J1_Holster(void* self, void* /*edx*/, int skiplocal)
{
    Log("[janus1] Holster\n");
    typedef void(__thiscall* Fn)(void*, int);
    reinterpret_cast<Fn>(g_origHolster)(self, skiplocal);
}

// -----------------------------------------------------------------------
// Patch a single vtable slot
// -----------------------------------------------------------------------
static bool PatchVtableSlot(void** vtable, int slot, void* newFn, void** outOrig)
{
    void** entry = &vtable[slot];
    DWORD old = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &old))
    {
        Log("[janus1] VirtualProtect failed for slot %d\n", slot);
        return false;
    }
    *outOrig = *entry;
    *entry = newFn;
    VirtualProtect(entry, sizeof(void*), old, &old);
    return true;
}

// -----------------------------------------------------------------------
// PostInit - patch the live vtable directly
// -----------------------------------------------------------------------
void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit\n");

    void** vtable = reinterpret_cast<void**>(mpBase + RVA_CJanus1_vtable);
    Log("[janus1] Patching CJanus1 vtable @ %p\n", (void*)vtable);

    PatchVtableSlot(vtable, SLOT_Deploy,      (void*)J1_Deploy,      &g_origDeploy);
    PatchVtableSlot(vtable, SLOT_WeaponIdle,  (void*)J1_WeaponIdle,  &g_origWeaponIdle);
    PatchVtableSlot(vtable, SLOT_AddToPlayer, (void*)J1_AddToPlayer, &g_origAddToPlayer);
    PatchVtableSlot(vtable, SLOT_Holster,     (void*)J1_Holster,     &g_origHolster);

    Log("[janus1] Patched: Deploy[%d]=%p WeaponIdle[%d]=%p AddToPlayer[%d]=%p Holster[%d]=%p\n",
        SLOT_Deploy,      g_origDeploy,
        SLOT_WeaponIdle,  g_origWeaponIdle,
        SLOT_AddToPlayer, g_origAddToPlayer,
        SLOT_Holster,     g_origHolster);
    Log("[janus1] PostInit done\n");
}

// No factory hook needed - vtable is patched globally
// weapon_janus1 factory runs normally, objects get our patched slots automatically
void __cdecl Janus1_Factory(int edict) {}

// -----------------------------------------------------------------------
// Registration - still register hook so hooks.cpp installs correctly,
// but we don't actually need the factory intercept anymore
// -----------------------------------------------------------------------
struct Janus1Reg
{
    Janus1Reg()
    {
        // No factory hook needed - direct vtable patch in PostInit
    }
};
static Janus1Reg g_reg;
