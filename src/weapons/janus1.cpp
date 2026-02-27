// janus1.cpp - Janus1 weapon
// 
// THE ONLY THING THIS DOES:
// 1. Let original weapon_janus1 factory run completely (Spawn, Precache all happen normally)
// 2. Read the vtable pointer off the live object AFTER construction
// 3. Make a copy of that vtable
// 4. Patch ONLY Deploy(102), WeaponIdle(165), AddToPlayer(175), Holster(189)
// 5. Swap the object's vtable to our copy
//
// Spawn(3) and Precache(4) stay as original Janus1 code - they work fine already.
// We copy from the live object so we never need a hardcoded vtable RVA.

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include <cstring>
#include <cstdint>
#include <windows.h>

static void** g_m79Vtable = nullptr;
static const uintptr_t RVA_CM79_vtable = 0x159FA04;

// Our patched vtable - allocated once from the first live object
static void** g_vtable    = nullptr;
static int    g_vtableLen = 0;
static bool   g_vtableReady = false;

static uint8_t g_origBytes[5] = {};
static bool    g_origSaved    = false;
typedef void(__cdecl* pfnOrigFactory_t)(int);

// -----------------------------------------------------------------------
// ONLY these 4 slots - all safe to call at runtime, none do precaching
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
// Build vtable from a LIVE object's vtable pointer
// Called once on the first spawned object
// -----------------------------------------------------------------------
static bool BuildVtableFromObject(void* obj)
{
    void** origVtbl = *reinterpret_cast<void***>(obj);
    Log("[janus1] Live vtable @ %p slot[0]=%p slot[3]=%p slot[4]=%p\n",
        (void*)origVtbl, origVtbl[0], origVtbl[3], origVtbl[4]);

    // Count slots by scanning for executable memory
    int n = 0;
    for (int i = 0; i < 512; i++)
    {
        void* fn = origVtbl[i];
        if (!fn) break;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(fn, &mbi, sizeof(mbi))) break;
        DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & exec)) break;
        n++;
    }
    if (n < 10) { Log("[janus1] Vtable too short (%d)\n", n); return false; }
    Log("[janus1] Vtable has %d slots\n", n);

    // Allocate and copy
    g_vtable = reinterpret_cast<void**>(
        VirtualAlloc(nullptr, n * sizeof(void*),
                     MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_vtable) { Log("[janus1] VirtualAlloc failed\n"); return false; }

    memcpy(g_vtable, origVtbl, n * sizeof(void*));
    g_vtableLen = n;

    // Patch ONLY the 4 runtime-safe slots
    // Slots 3 (Spawn) and 4 (Precache) are deliberately left as original
    if (102 < n) g_vtable[102] = (void*)J1_Deploy;
    if (165 < n) g_vtable[165] = (void*)J1_WeaponIdle;
    if (175 < n) g_vtable[175] = (void*)J1_AddToPlayer;
    if (189 < n) g_vtable[189] = (void*)J1_Holster;

    g_vtableReady = true;
    Log("[janus1] Vtable built (%d slots, patched Deploy[102] WeaponIdle[165] AddToPlayer[175] Holster[189])\n", n);
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

    // Run original factory - this allocates object, sets vtable, calls Spawn+Precache
    CallOrigJanus1(edict);

    // Find the object via confirmed pointer chain
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

    // Build vtable from this live object on first call
    if (!g_vtableReady)
    {
        if (!BuildVtableFromObject(obj))
            return;
    }

    // Swap vtable
    *reinterpret_cast<void**>(obj) = g_vtable;
    Log("[janus1] Swapped on obj=%p\n", obj);
}

// -----------------------------------------------------------------------
// PostInit
// -----------------------------------------------------------------------
void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit\n");

    g_m79Vtable = reinterpret_cast<void**>(mpBase + RVA_CM79_vtable);

    const uint8_t* sb = GetSavedBytes(RVA_weapon_janus1);
    if (sb) { memcpy(g_origBytes, sb, 5); g_origSaved = true; }
    else
    {
        g_origBytes[0]=0x55; g_origBytes[1]=0x8B; g_origBytes[2]=0xEC;
        g_origBytes[3]=0x56; g_origBytes[4]=0x8B;
        g_origSaved = true;
    }

    Log("[janus1] PostInit done. M79vtable=%p\n", (void*)g_m79Vtable);
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
