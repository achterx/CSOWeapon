// janus1.cpp - Janus1 weapon
// Key insight: Precache is called by the engine BEFORE map starts (t=0).
// Our factory is called AFTER (t=1.000) - so Precache already ran via M79.
// We only need to override slots where we differ from M79.
// For now: pure M79 behavior, just with our vtable installed.
// This proves the vtable swap + dispatch chain works end-to-end.

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include <cstring>
#include <cstdint>
#include <windows.h>

static void** g_m79Vtable = nullptr;
static const uintptr_t RVA_CM79_vtable = 0x159FA04;

static void* g_vtable[220] = {};
static bool  g_vtableReady = false;

static uint8_t g_origBytes[5] = {};
static bool    g_origSaved    = false;
typedef void(__cdecl* pfnOrigFactory_t)(int);

// -----------------------------------------------------------------------
// All slots delegate to M79 directly - proves the chain works
// Once stable, replace individual slots with custom logic
// -----------------------------------------------------------------------

static void __fastcall J1_Spawn(void* self, void* /*edx*/)
{
    Log("[janus1] Spawn\n");
    typedef void(__thiscall* Fn)(void*);
    reinterpret_cast<Fn>(g_m79Vtable[3])(self);
}

static int __fastcall J1_Precache(void* self, void* /*edx*/)
{
    Log("[janus1] Precache\n");
    typedef int(__thiscall* Fn)(void*);
    return reinterpret_cast<Fn>(g_m79Vtable[4])(self);
}

static int __fastcall J1_Deploy(void* self, void* /*edx*/)
{
    Log("[janus1] Deploy\n");
    typedef int(__thiscall* Fn)(void*);
    return reinterpret_cast<Fn>(g_m79Vtable[102])(self);
}

static void __fastcall J1_WeaponIdle(void* self, void* /*edx*/)
{
    typedef void(__thiscall* Fn)(void*);
    reinterpret_cast<Fn>(g_m79Vtable[165])(self);
}

static int __fastcall J1_AddToPlayer(void* self, void* /*edx*/, void* player)
{
    Log("[janus1] AddToPlayer\n");
    typedef int(__thiscall* Fn)(void*, void*);
    return reinterpret_cast<Fn>(g_m79Vtable[175])(self, player);
}

static void __fastcall J1_Holster(void* self, void* /*edx*/, int skiplocal)
{
    Log("[janus1] Holster\n");
    typedef void(__thiscall* Fn)(void*, int);
    reinterpret_cast<Fn>(g_m79Vtable[189])(self, skiplocal);
}

// -----------------------------------------------------------------------
// Build vtable
// -----------------------------------------------------------------------
static bool BuildVtable(uintptr_t mpBase)
{
    g_m79Vtable = reinterpret_cast<void**>(mpBase + RVA_CM79_vtable);

    __try { memcpy(g_vtable, g_m79Vtable, 220 * sizeof(void*)); }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[janus1] Failed to copy M79 vtable\n");
        return false;
    }

    g_vtable[3]   = (void*)J1_Spawn;
    g_vtable[4]   = (void*)J1_Precache;
    g_vtable[102] = (void*)J1_Deploy;
    g_vtable[165] = (void*)J1_WeaponIdle;
    g_vtable[175] = (void*)J1_AddToPlayer;
    g_vtable[189] = (void*)J1_Holster;

    g_vtableReady = true;
    Log("[janus1] Vtable ready\n");
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
    if (!g_vtableReady) return;

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

    *reinterpret_cast<void**>(obj) = g_vtable;
    Log("[janus1] Vtable swapped on obj=%p\n", obj);
}

// -----------------------------------------------------------------------
// PostInit
// -----------------------------------------------------------------------
void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit\n");
    BuildVtable(mpBase);

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
