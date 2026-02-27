// janus1.cpp - Janus1 weapon
// Slot numbers verified directly from IDA CJanus1 vtable at 0x11649034:
//   Slot   3: Spawn        sub_10E964E0
//   Slot   4: Precache     sub_10E92CD0
//   Slot  95: AddToPlayer  sub_10E95AD0
//   Slot 102: Deploy       sub_10E95CD0
//   Slot 142: WeaponIdle   sub_10E92E70
//   Slot 168: Holster      sub_10E92D80
// Total vtable size: 201 slots

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include <cstring>
#include <cstdint>
#include <windows.h>

static void** g_vtable      = nullptr;
static bool   g_vtableReady = false;

static void* g_origDeploy      = nullptr;
static void* g_origWeaponIdle  = nullptr;
static void* g_origAddToPlayer = nullptr;
static void* g_origHolster     = nullptr;

static uint8_t g_origBytes[5] = {};
static bool    g_origSaved    = false;
typedef void(__cdecl* pfnOrigFactory_t)(int);

// Correct slot numbers from IDA
static const int SLOT_AddToPlayer = 95;
static const int SLOT_Deploy      = 102;
static const int SLOT_WeaponIdle  = 142;
static const int SLOT_Holster     = 168;
static const int VTABLE_SIZE      = 201;

// -----------------------------------------------------------------------
// Overrides - log and call original Janus1 implementations
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
// Build vtable from live object
// -----------------------------------------------------------------------
static bool BuildVtableFromObject(void* obj)
{
    void** origVtbl = *reinterpret_cast<void***>(obj);
    Log("[janus1] Live vtable @ %p\n", (void*)origVtbl);

    g_vtable = reinterpret_cast<void**>(
        VirtualAlloc(nullptr, VTABLE_SIZE * sizeof(void*),
                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!g_vtable) { Log("[janus1] VirtualAlloc failed\n"); return false; }

    __try { memcpy(g_vtable, origVtbl, VTABLE_SIZE * sizeof(void*)); }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[janus1] memcpy failed\n");
        VirtualFree(g_vtable, 0, MEM_RELEASE);
        g_vtable = nullptr;
        return false;
    }

    // Save originals before patching
    g_origDeploy      = origVtbl[SLOT_Deploy];
    g_origWeaponIdle  = origVtbl[SLOT_WeaponIdle];
    g_origAddToPlayer = origVtbl[SLOT_AddToPlayer];
    g_origHolster     = origVtbl[SLOT_Holster];

    Log("[janus1] Saved: Deploy[%d]=%p WeaponIdle[%d]=%p AddToPlayer[%d]=%p Holster[%d]=%p\n",
        SLOT_Deploy, g_origDeploy,
        SLOT_WeaponIdle, g_origWeaponIdle,
        SLOT_AddToPlayer, g_origAddToPlayer,
        SLOT_Holster, g_origHolster);

    g_vtable[SLOT_Deploy]      = (void*)J1_Deploy;
    g_vtable[SLOT_WeaponIdle]  = (void*)J1_WeaponIdle;
    g_vtable[SLOT_AddToPlayer] = (void*)J1_AddToPlayer;
    g_vtable[SLOT_Holster]     = (void*)J1_Holster;

    g_vtableReady = true;
    Log("[janus1] Vtable ready (%d slots)\n", VTABLE_SIZE);
    return true;
}

// -----------------------------------------------------------------------
// Trampoline
// -----------------------------------------------------------------------
static void CallOrigJanus1(int edict)
{
    if (!g_origSaved) return;
    uintptr_t target = GetMpBase() + RVA_weapon_janus1;
    DWORD old = 0;
    VirtualProtect((void*)target, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)target, g_origBytes, 5);
    VirtualProtect((void*)target, 5, old, &old);
    reinterpret_cast<pfnOrigFactory_t>(target)(edict);
    WriteJmp5(target, (uintptr_t)Janus1_Factory, nullptr);
}

// -----------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------
void __cdecl Janus1_Factory(int edict)
{
    Log("[janus1] Factory edict=%d\n", edict);
    CallOrigJanus1(edict);

    void* obj = nullptr;
    __try
    {
        uint8_t* inner = *reinterpret_cast<uint8_t**>(
            reinterpret_cast<uint8_t*>((uintptr_t)edict) + 0x238);
        if (inner)
            obj = *reinterpret_cast<void**>(inner + 0x80);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[janus1] Exception finding obj\n");
        return;
    }
    if (!obj) { Log("[janus1] obj null\n"); return; }

    if (!g_vtableReady)
        if (!BuildVtableFromObject(obj)) return;

    *reinterpret_cast<void**>(obj) = g_vtable;
    Log("[janus1] Swapped on obj=%p\n", obj);
}

// -----------------------------------------------------------------------
// PostInit
// -----------------------------------------------------------------------
void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit\n");
    const uint8_t* sb = GetSavedBytes(RVA_weapon_janus1);
    if (sb) { memcpy(g_origBytes, sb, 5); g_origSaved = true; }
    else
    {
        g_origBytes[0]=0x55; g_origBytes[1]=0x8B; g_origBytes[2]=0xEC;
        g_origBytes[3]=0x56; g_origBytes[4]=0x8B;
        g_origSaved = true;
    }
    Log("[janus1] PostInit done\n");
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------
struct Janus1Reg
{
    Janus1Reg()
    {
        RegisterWeaponHook("weapon_janus1", (void*)Janus1_Factory, RVA_weapon_janus1);
    }
};
static Janus1Reg g_reg;
