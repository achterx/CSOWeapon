// janus1.cpp — CJanus1 grenade launcher (v23)
//
// APPROACH: Factory hook + copy-and-patch vtable.
//
// Correct slot numbers confirmed from IDA (m79_actions.txt):
//   Spawn=3, Precache=4, Deploy=102, WeaponIdle=165,
//   AddToPlayer=175, Holster=189
//
// M79 fires its rocket from WeaponIdle (slot 165), NOT a separate
// PrimaryAttack slot. Slots 166/167/191 are SUB_DoNothing.
//
// Our Janus1 overrides:
//   - Precache[4]: load janus1 assets
//   - Spawn[3]: call M79 Spawn (sets model/movetype/solid)
//   - Deploy[102]: call M79 Deploy with janus1 view model
//   - WeaponIdle[165]: call M79 WeaponIdle (fires rocket)
//   - Holster[189]: call M79 Holster
//   - AddToPlayer[175]: call M79 AddToPlayer

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include "../hlsdk/mp_offsets.h"
#include <windows.h>
#include <cstring>

// Static member definitions required by linker
ItemInfo CBasePlayerItem::m_ItemInfoArray[32] = {};
void*    CBasePlayerItem::m_AmmoInfoArray     = nullptr;
void*    CBasePlayerItem::m_SaveData          = nullptr;
void*    CBasePlayerWeapon::m_SaveData        = nullptr;

// Engine function pointers
pfnPrecacheModel_t  g_fnPrecacheModel = nullptr;
pfnPrecacheSound_t  g_fnPrecacheSound = nullptr;
pfnSetModel_t       g_fnSetModel      = nullptr;
pfnPrecacheEvent_t  g_fnPrecacheEvent = nullptr;

// Global state
static void**  g_ourVtable    = nullptr;
static int     g_vtableSlots  = 0;
static void**  g_m79Vtable    = nullptr;
static bool    g_factoryReady = false;
static uint8_t g_savedBytes[5] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B };

// -----------------------------------------------------------------------
// Inline __thiscall dispatch helpers
// -----------------------------------------------------------------------
static inline void CallM79Slot0(void* self, int slot) {
    typedef void(__thiscall* Fn)(void*);
    reinterpret_cast<Fn>(g_m79Vtable[slot])(self);
}
static inline int CallM79Slot1i(void* self, int slot) {
    typedef int(__thiscall* Fn)(void*);
    return reinterpret_cast<Fn>(g_m79Vtable[slot])(self);
}
static inline void CallM79Slot0i(void* self, int slot, int arg) {
    typedef void(__thiscall* Fn)(void*, int);
    reinterpret_cast<Fn>(g_m79Vtable[slot])(self, arg);
}

// -----------------------------------------------------------------------
// DefaultDeploy — called by M79's Deploy, signature:
//   int __cdecl sub_1131AA80(const char* viewmodel, const char* weapmodel,
//                             int anim, int animext, int hasAmmo,
//                             float/*int*/ unk, float deployTime)
// We call it directly to use janus1 view model.
// -----------------------------------------------------------------------
typedef int(__cdecl* pfnDefaultDeploy_t)(const char*, const char*, int, int, int, int, float);

// -----------------------------------------------------------------------
// Override implementations (__fastcall = thiscall-compatible for vtable)
// -----------------------------------------------------------------------

static void __fastcall J1_Precache(void* self, void* /*edx*/)
{
    Log("[janus1] Precache\n");
    // Load janus1 assets
    PRECACHE_MODEL("models/v_janus1.mdl");
    PRECACHE_MODEL("models/w_janus1.mdl");
    PRECACHE_SOUND("weapons/m79-1.wav");     // reuse M79 fire sound (janus1 shares it)
    PRECACHE_SOUND("weapons/m79_reload.wav");

    // Also run M79's Precache to register the rocket event etc.
    if (g_m79Vtable)
        CallM79Slot0(self, VTBL_Precache);

    Log("[janus1] Precache done\n");
}

static void __fastcall J1_Spawn(void* self, void* /*edx*/)
{
    Log("[janus1] Spawn self=%p\n", self);
    // Run M79 Spawn — sets model, movetype, solid, ammo count etc.
    if (g_m79Vtable)
        CallM79Slot0(self, VTBL_Spawn);
    Log("[janus1] Spawn done\n");
}

static int __fastcall J1_Deploy(void* self, void* /*edx*/)
{
    Log("[janus1] Deploy\n");
    if (!g_m79Vtable) return 1;

    // Call M79 Deploy but it uses "models/v_m79.mdl" internally.
    // For now delegate directly — to use v_janus1 we'd need DefaultDeploy RVA.
    // TODO: swap to direct DefaultDeploy call once model is confirmed working.
    return CallM79Slot1i(self, VTBL_Deploy);
}

static void __fastcall J1_WeaponIdle(void* self, void* /*edx*/)
{
    // M79's WeaponIdle handles firing (checks ammo, spawns rocket, plays event)
    if (g_m79Vtable)
        CallM79Slot0(self, VTBL_WeaponIdle);
}

static void __fastcall J1_Holster(void* self, void* /*edx*/, int skiplocal)
{
    Log("[janus1] Holster skiplocal=%d\n", skiplocal);
    if (g_m79Vtable)
        CallM79Slot0i(self, VTBL_Holster, skiplocal);
}

static void __fastcall J1_AddToPlayer(void* self, void* /*edx*/, int pPlayer)
{
    Log("[janus1] AddToPlayer player=%p\n", (void*)(uintptr_t)pPlayer);
    if (g_m79Vtable)
        CallM79Slot0i(self, VTBL_AddToPlayer, pPlayer);
}

// -----------------------------------------------------------------------
// Count vtable slots (scan until non-executable pointer)
// -----------------------------------------------------------------------
static int CountVtableSlots(void** vtbl)
{
    for (int i = 0; i < 512; i++) {
        void* fn = vtbl[i];
        if (!fn) return i;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(fn, &mbi, sizeof(mbi))) return i;
        DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!(mbi.Protect & exec)) return i;
    }
    return 512;
}

// -----------------------------------------------------------------------
// Build our vtable copy at PostInit.
// -----------------------------------------------------------------------
static bool BuildVtable(uintptr_t mpBase)
{
    void** origVtbl = reinterpret_cast<void**>(mpBase + RVA_Janus1_vtable);
    Log("[janus1] Original Janus1 vtable @ %p, slot[0]=%p slot[3]=%p slot[4]=%p\n",
        (void*)origVtbl, origVtbl[0], origVtbl[3], origVtbl[4]);

    int n = CountVtableSlots(origVtbl);
    Log("[janus1] Vtable has %d slots\n", n);
    if (n < 10) {
        Log("[janus1] ERROR: vtable slot count too small (%d) — bad RVA?\n", n);
        return false;
    }

    g_ourVtable = reinterpret_cast<void**>(
        VirtualAlloc(nullptr, n * sizeof(void*),
                     MEM_COMMIT | MEM_RESERVE,
                     PAGE_EXECUTE_READWRITE));
    if (!g_ourVtable) {
        Log("[janus1] VirtualAlloc failed!\n");
        return false;
    }

    memcpy(g_ourVtable, origVtbl, n * sizeof(void*));
    g_vtableSlots = n;

    // Patch our overrides (using corrected slot numbers)
    g_ourVtable[VTBL_Spawn]       = (void*)J1_Spawn;
    g_ourVtable[VTBL_Precache]    = (void*)J1_Precache;
    g_ourVtable[VTBL_Deploy]      = (void*)J1_Deploy;
    g_ourVtable[VTBL_WeaponIdle]  = (void*)J1_WeaponIdle;
    g_ourVtable[VTBL_Holster]     = (void*)J1_Holster;
    g_ourVtable[VTBL_AddToPlayer] = (void*)J1_AddToPlayer;

    Log("[janus1] Vtable copy @ %p (%d slots)\n"
        "[janus1] Patched: Spawn[%d] Precache[%d] Deploy[%d] "
        "WeaponIdle[%d] Holster[%d] AddToPlayer[%d]\n",
        (void*)g_ourVtable, n,
        VTBL_Spawn, VTBL_Precache, VTBL_Deploy,
        VTBL_WeaponIdle, VTBL_Holster, VTBL_AddToPlayer);

    return true;
}

// -----------------------------------------------------------------------
// Swap vtable pointer on a freshly spawned object
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

    __try {
        g_fnPrecacheModel = *reinterpret_cast<pfnPrecacheModel_t*>(mpBase + RVA_pfnPrecacheModel);
        g_fnPrecacheSound = *reinterpret_cast<pfnPrecacheSound_t*>(mpBase + RVA_pfnPrecacheSound);
        g_fnSetModel      = *reinterpret_cast<pfnSetModel_t*>      (mpBase + RVA_pfnSetModel);
        g_fnPrecacheEvent = *reinterpret_cast<pfnPrecacheEvent_t*> (mpBase + RVA_pfnPrecacheEvent);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[janus1] Exception reading engine fns!\n");
    }

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

    // M79 vtable for delegation
    g_m79Vtable = reinterpret_cast<void**>(mpBase + RVA_CM79_vtable);
    Log("[janus1] M79 vtable @ %p slot[%d]=%p slot[%d]=%p slot[%d]=%p\n",
        (void*)g_m79Vtable,
        VTBL_Spawn,       g_m79Vtable[VTBL_Spawn],
        VTBL_Precache,    g_m79Vtable[VTBL_Precache],
        VTBL_WeaponIdle,  g_m79Vtable[VTBL_WeaponIdle]);

    // Save original bytes for hot-patch restore
    const uint8_t* sb = GetSavedBytes(RVA_weapon_janus1);
    if (sb) {
        memcpy(g_savedBytes, sb, 5);
        Log("[janus1] Saved bytes: %02X %02X %02X %02X %02X\n",
            sb[0], sb[1], sb[2], sb[3], sb[4]);
    }

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
