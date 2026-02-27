// janus1.cpp — CJanus1 grenade launcher
//
// APPROACH (v22): Factory hook + copy-and-patch vtable.
//
// 1. Hook weapon_janus1 entry point -> Janus1_Factory
// 2. Call original factory (allocates object, sets original vtable)
// 3. At PostInit: copy the FULL original vtable into our own buffer,
//    then patch only the slots we want to override.
// 4. In factory: swap object's vtable ptr to our copied vtable.
//
// This is safe because:
//  - Our copied vtable has ALL original slots (no out-of-bounds reads)
//  - We only change the specific slots we care about
//  - Object layout is untouched — only vtable pointer changes

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include "../hlsdk/mp_offsets.h"
#include <windows.h>
#include <cstring>

// -----------------------------------------------------------------------
// Engine function pointers
// -----------------------------------------------------------------------
pfnPrecacheModel_t  g_fnPrecacheModel = nullptr;
pfnPrecacheSound_t  g_fnPrecacheSound = nullptr;
pfnSetModel_t       g_fnSetModel      = nullptr;
pfnPrecacheEvent_t  g_fnPrecacheEvent = nullptr;

// Static member definitions required by linker
ItemInfo CBasePlayerItem::m_ItemInfoArray[32] = {};
void*    CBasePlayerItem::m_AmmoInfoArray     = nullptr;
void*    CBasePlayerItem::m_SaveData          = nullptr;
void*    CBasePlayerWeapon::m_SaveData        = nullptr;

// -----------------------------------------------------------------------
// Our copied vtable — allocated at PostInit, holds all original slots
// with our overrides patched in.
// -----------------------------------------------------------------------
static void**  g_ourVtable     = nullptr;  // heap-allocated copy
static int     g_vtableSlots   = 0;        // how many slots were copied
static void**  g_m79Vtable     = nullptr;
static bool    g_factoryReady  = false;
static uint8_t g_savedBytes[5] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B };

// -----------------------------------------------------------------------
// Our override functions — plain __fastcall so MSVC thiscall-compatible
// when called as vtable slots (compiler will pass ECX=this, no EDX used)
// -----------------------------------------------------------------------

static void __fastcall J1_Precache(void* self, void* /*edx*/)
{
    Log("[janus1] Precache\n");
    PRECACHE_MODEL("models/v_janus1.mdl");
    PRECACHE_MODEL("models/w_janus1.mdl");
    PRECACHE_SOUND("weapons/janus1-1.wav");
    PRECACHE_SOUND("weapons/janus1-2.wav");
    PRECACHE_SOUND("weapons/janus1_reload.wav");
    Log("[janus1] Precache done\n");
}

static void __fastcall J1_Spawn(void* self, void* /*edx*/)
{
    Log("[janus1] Spawn self=%p\n", self);
    // Delegate to M79 Spawn (sets up model, movetype, solid, ammo etc.)
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[0])(self);
    }
    Log("[janus1] Spawn done\n");
}

static void __fastcall J1_PrimaryAttack(void* self, void* /*edx*/)
{
    Log("[janus1] PrimaryAttack\n");
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[VTBL_PrimaryAttack])(self);
    }
}

static void __fastcall J1_Reload(void* self, void* /*edx*/)
{
    Log("[janus1] Reload\n");
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[VTBL_Reload])(self);
    }
}

static int __fastcall J1_Deploy(void* self, void* /*edx*/)
{
    Log("[janus1] Deploy\n");
    if (g_m79Vtable) {
        typedef int(__thiscall* Fn)(void*);
        return reinterpret_cast<Fn>(g_m79Vtable[VTBL_Deploy])(self);
    }
    return 1;
}

static void __fastcall J1_Holster(void* self, void* /*edx*/, int skiplocal)
{
    Log("[janus1] Holster\n");
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*, int);
        reinterpret_cast<Fn>(g_m79Vtable[VTBL_Holster])(self, skiplocal);
    }
}

static void __fastcall J1_WeaponIdle(void* self, void* /*edx*/)
{
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[VTBL_WeaponIdle])(self);
    }
}

// -----------------------------------------------------------------------
// Count vtable slots by scanning until we hit a non-code pointer.
// Uses VirtualQuery to check if each pointer is executable.
// -----------------------------------------------------------------------
static int CountVtableSlots(void** vtbl)
{
    for (int i = 0; i < 512; i++) {
        void* fn = vtbl[i];
        if (!fn) return i;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(fn, &mbi, sizeof(mbi))) return i;
        DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & exec)) return i;
    }
    return 512;
}

// -----------------------------------------------------------------------
// Build our vtable copy at PostInit.
// Copies all N slots from original Janus1 vtable, then patches our overrides.
// -----------------------------------------------------------------------
static bool BuildVtable(uintptr_t mpBase)
{
    void** origVtbl = reinterpret_cast<void**>(mpBase + RVA_Janus1_vtable);
    Log("[janus1] Original Janus1 vtable @ %p, slot[0]=%p\n",
        (void*)origVtbl, origVtbl[0]);

    int n = CountVtableSlots(origVtbl);
    Log("[janus1] Vtable has %d slots\n", n);
    if (n < 10) {
        Log("[janus1] ERROR: vtable slot count too small (%d) — bad RVA?\n", n);
        return false;
    }

    // Allocate executable memory for our copy
    g_ourVtable = reinterpret_cast<void**>(
        VirtualAlloc(nullptr, n * sizeof(void*),
                     MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE));
    if (!g_ourVtable) {
        Log("[janus1] VirtualAlloc failed!\n");
        return false;
    }

    // Copy all original slots
    memcpy(g_ourVtable, origVtbl, n * sizeof(void*));
    g_vtableSlots = n;

    Log("[janus1] Vtable copy @ %p (%d slots)\n", (void*)g_ourVtable, n);

    // Patch our overrides
    g_ourVtable[0]                  = (void*)J1_Spawn;
    g_ourVtable[VTBL_Precache]      = (void*)J1_Precache;
    g_ourVtable[VTBL_PrimaryAttack] = (void*)J1_PrimaryAttack;
    g_ourVtable[VTBL_Reload]        = (void*)J1_Reload;
    g_ourVtable[VTBL_Deploy]        = (void*)J1_Deploy;
    g_ourVtable[VTBL_Holster]       = (void*)J1_Holster;
    g_ourVtable[VTBL_WeaponIdle]    = (void*)J1_WeaponIdle;

    Log("[janus1] Patched slots: Spawn[0] Precache[%d] PrimaryAtk[%d] "
        "Reload[%d] Deploy[%d] Holster[%d] WeaponIdle[%d]\n",
        VTBL_Precache, VTBL_PrimaryAttack,
        VTBL_Reload, VTBL_Deploy, VTBL_Holster, VTBL_WeaponIdle);

    return true;
}

// -----------------------------------------------------------------------
// Swap vtable pointer on a spawned object
// -----------------------------------------------------------------------
static void SwapVtable(void* obj)
{
    if (!obj || !g_ourVtable) return;
    void* prev = *reinterpret_cast<void**>(obj);
    *reinterpret_cast<void**>(obj) = g_ourVtable;
    Log("[janus1] Vtable swapped on obj=%p: %p -> %p\n", obj, prev, g_ourVtable);
}

// -----------------------------------------------------------------------
// Factory
// -----------------------------------------------------------------------
typedef void(__cdecl* pfnFactory_t)(entvars_t*);

static void CallOrigFactory(entvars_t* pev)
{
    uintptr_t fnAddr = GetMpBase() + RVA_weapon_janus1;
    DWORD old = 0;
    VirtualProtect((void*)fnAddr, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)fnAddr, g_savedBytes, 5);
    VirtualProtect((void*)fnAddr, 5, old, &old);
    reinterpret_cast<pfnFactory_t>(fnAddr)(pev);
    WriteJmp5(fnAddr, (uintptr_t)Janus1_Factory, nullptr);
}

void __cdecl Janus1_Factory(entvars_t* pev)
{
    Log("[janus1] Factory pev=%p t=%.3f\n", pev, SafeTime());

    // Always call original to properly allocate and construct the object
    CallOrigFactory(pev);

    if (!g_factoryReady) {
        Log("[janus1] Not ready, skipping vtable swap\n");
        return;
    }

    // Find the object: edict = *(pev+0x238), obj = *(edict+0x80)
    void* obj = nullptr;
    __try {
        void* edict = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(pev) + 0x238);
        Log("[janus1] edict=%p\n", edict);
        if (edict) {
            obj = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(edict) + 0x80);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[janus1] Exception finding obj\n");
        return;
    }

    if (!obj) { Log("[janus1] obj is null\n"); return; }

    Log("[janus1] obj=%p orig_vtable=%p\n", obj, *reinterpret_cast<void**>(obj));

    SwapVtable(obj);

    Log("[janus1] Factory done\n");
}

// -----------------------------------------------------------------------
// PostInit
// -----------------------------------------------------------------------
void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit mpBase=0x%08zX\n", mpBase);

    // Engine fns
    __try {
        g_fnPrecacheModel = *reinterpret_cast<pfnPrecacheModel_t*>(mpBase + RVA_pfnPrecacheModel);
        g_fnPrecacheSound = *reinterpret_cast<pfnPrecacheSound_t*>(mpBase + RVA_pfnPrecacheSound);
        g_fnSetModel      = *reinterpret_cast<pfnSetModel_t*>      (mpBase + RVA_pfnSetModel);
        g_fnPrecacheEvent = *reinterpret_cast<pfnPrecacheEvent_t*> (mpBase + RVA_pfnPrecacheEvent);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[janus1] Exception reading engine fns!\n");
    }

    // gpGlobals->time
    __try {
        uintptr_t pGlobalsAddr = *reinterpret_cast<uintptr_t*>(mpBase + RVA_pGlobals);
        if (pGlobalsAddr)
            g_pTime = reinterpret_cast<float*>(pGlobalsAddr);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[janus1] Exception reading pGlobals!\n");
    }

    Log("[janus1] PrecacheModel=%p Sound=%p SetModel=%p Event=%p time=%p (%.3f)\n",
        (void*)g_fnPrecacheModel, (void*)g_fnPrecacheSound,
        (void*)g_fnSetModel, (void*)g_fnPrecacheEvent,
        (void*)g_pTime, SafeTime());

    // M79 vtable
    g_m79Vtable = reinterpret_cast<void**>(mpBase + RVA_CM79_vtable);
    Log("[janus1] M79 vtable @ %p slot[0]=%p slot[%d]=%p\n",
        (void*)g_m79Vtable,
        g_m79Vtable[0],
        VTBL_PrimaryAttack, g_m79Vtable[VTBL_PrimaryAttack]);

    // Saved bytes
    const uint8_t* sb = GetSavedBytes(RVA_weapon_janus1);
    if (sb) {
        memcpy(g_savedBytes, sb, 5);
        Log("[janus1] Saved bytes: %02X %02X %02X %02X %02X\n",
            sb[0], sb[1], sb[2], sb[3], sb[4]);
    }

    // Build vtable copy with our overrides
    bool ok = BuildVtable(mpBase);

    bool engOk = (uintptr_t)g_fnPrecacheModel > 0x01000000;
    g_factoryReady = ok && engOk && g_pTime;
    Log("[janus1] PostInit done. factoryReady=%d vtableCopy=%p slots=%d\n",
        (int)g_factoryReady, (void*)g_ourVtable, g_vtableSlots);
}

// -----------------------------------------------------------------------
// Static registration
// -----------------------------------------------------------------------
struct Janus1Registrar {
    Janus1Registrar() {
        RegisterWeaponHook("weapon_janus1", (void*)Janus1_Factory, RVA_weapon_janus1);
    }
};
static Janus1Registrar g_janus1Reg;
