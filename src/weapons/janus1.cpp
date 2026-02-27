// janus1.cpp - Janus1 weapon
// DO NOT override Precache (slot 4) or Spawn (slot 3).
// These run at map-load time via the original Janus1 vtable -- they already
// work. We only swap the vtable AFTER the factory runs (t=1.000), so any
// slot that gets called immediately on spawn must delegate safely.
// Slots 3 and 4 are NOT in our override list -- the original Janus1
// implementations stay in place for those.

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include <cstring>
#include <cstdint>
#include <windows.h>

static void** g_m79Vtable   = nullptr;
static void** g_j1OrigVtable = nullptr; // original Janus1 vtable (kept for safe fallback)
static const uintptr_t RVA_CM79_vtable = 0x159FA04;

static void* g_vtable[220] = {};
static bool  g_vtableReady = false;

static uint8_t g_origBytes[5] = {};
static bool    g_origSaved    = false;
typedef void(__cdecl* pfnOrigFactory_t)(int);

// -----------------------------------------------------------------------
// ONLY override slots that are safe to call at runtime (not precache-phase)
// Slot 102: Deploy   - safe, called when player equips weapon
// Slot 165: WeaponIdle - safe, called every frame
// Slot 175: AddToPlayer - safe, called when player picks up weapon
// Slot 189: Holster  - safe, called when player switches weapon
//
// Slots 3 (Spawn) and 4 (Precache) are LEFT AS ORIGINAL Janus1 code.
// -----------------------------------------------------------------------

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
// Build vtable: start from ORIGINAL JANUS1 vtable (not M79!)
// This means all slots default to what Janus1 already does correctly.
// We then patch only the 4 runtime-safe slots.
// -----------------------------------------------------------------------
static bool BuildVtable(uintptr_t mpBase)
{
    g_m79Vtable    = reinterpret_cast<void**>(mpBase + RVA_CM79_vtable);
    g_j1OrigVtable = reinterpret_cast<void**>(mpBase + RVA_Janus1_vtable);

    // Copy Janus1's original vtable as base
    __try { memcpy(g_vtable, g_j1OrigVtable, 220 * sizeof(void*)); }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[janus1] Failed to copy Janus1 vtable\n");
        return false;
    }

    // Only patch the 4 runtime-safe slots
    g_vtable[102] = (void*)J1_Deploy;
    g_vtable[165] = (void*)J1_WeaponIdle;
    g_vtable[175] = (void*)J1_AddToPlayer;
    g_vtable[189] = (void*)J1_Holster;

    g_vtableReady = true;
    Log("[janus1] Vtable ready (base=Janus1orig, patched: Deploy[102] WeaponIdle[165] AddToPlayer[175] Holster[189])\n");
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

    // Call original - allocates object with Janus1's original vtable
    CallOrigJanus1(edict);
    if (!g_vtableReady) return;

    // Find the object
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

    // Swap to our vtable (Janus1-based, 4 slots patched)
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
