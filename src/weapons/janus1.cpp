// janus1.cpp — CJanus1, Janus-1 grenade launcher
#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include <cstring>
#include <cstdint>
#include <windows.h>

// -----------------------------------------------------------------------
// Engine function pointers — resolved from g_engfuncs in PostInit
// -----------------------------------------------------------------------
typedef int  (__cdecl* pfnPrecacheModel_t)(const char*);
typedef int  (__cdecl* pfnPrecacheSound_t)(const char*);
typedef int  (__cdecl* pfnPrecacheEvent_t)(int type, const char* name);

static pfnPrecacheModel_t g_PrecacheModel = nullptr;
static pfnPrecacheSound_t g_PrecacheSound = nullptr;
static pfnPrecacheEvent_t g_PrecacheEvent = nullptr;

// -----------------------------------------------------------------------
// Vtable
// -----------------------------------------------------------------------
static void*  g_vtable[220] = {};
static bool   g_vtableReady = false;
static void** g_m79Vtable   = nullptr;

// CM79 vtable RVA: ??_7CM79@@6B@ @ 0x1159FA04
static const uintptr_t RVA_CM79_vtable = 0x159FA04;

// -----------------------------------------------------------------------
// Virtual method implementations
// -----------------------------------------------------------------------

// Slot 3: Precache
static int __fastcall J1_Precache(void* self, void*)
{
    Log("[janus1] Precache self=%p\n", self);
    if (g_PrecacheSound) {
        g_PrecacheSound("weapons/janus1-1.wav");
        g_PrecacheSound("weapons/janus1-2.wav");
        g_PrecacheSound("weapons/janus1_reload.wav");
    }
    if (g_PrecacheModel) {
        g_PrecacheModel("models/v_janus1.mdl");
        g_PrecacheModel("models/p_janus1.mdl");
        g_PrecacheModel("models/w_janus1.mdl");
    }
    if (g_PrecacheEvent) {
        uint16_t ev = (uint16_t)g_PrecacheEvent(1, "events/janus1.sc");
        *reinterpret_cast<uint16_t*>((uint8_t*)self + F_usFireEvent) = ev;
        Log("[janus1] event handle=%d\n", ev);
    }
    // Also register the rocket entity class
    if (g_m79Vtable) {
        typedef int(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[3])(self);
    }
    return 1;
}

// Slot 102: Deploy
static int __fastcall J1_Deploy(void* self, void*)
{
    Log("[janus1] Deploy\n");
    if (g_m79Vtable) {
        typedef int(__thiscall* Fn)(void*);
        return reinterpret_cast<Fn>(g_m79Vtable[102])(self);
    }
    return 1;
}

// Slot 165: WeaponIdle
static void __fastcall J1_WeaponIdle(void* self, void*)
{
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[165])(self);
    }
}

// Slot 167: PrimaryAttack
static void __fastcall J1_PrimaryAttack(void* self, void*)
{
    Log("[janus1] PrimaryAttack t=%.3f\n", GetTime());
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[167])(self);
    }
}

// Slot 175: AddToPlayer
static int __fastcall J1_AddToPlayer(void* self, void*, void* player)
{
    Log("[janus1] AddToPlayer player=%p\n", player);
    if (g_m79Vtable) {
        typedef int(__thiscall* Fn)(void*, void*);
        int r = reinterpret_cast<Fn>(g_m79Vtable[175])(self, player);
        Log("[janus1] AddToPlayer ret=%d\n", r);
        return r;
    }
    return 0;
}

// Slot 191: Holster
static void __fastcall J1_Holster(void* self, void*, int skip)
{
    Log("[janus1] Holster\n");
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*, int);
        reinterpret_cast<Fn>(g_m79Vtable[191])(self, skip);
    }
}

// -----------------------------------------------------------------------
// Build vtable — copy M79's 220 slots then override ours
// -----------------------------------------------------------------------
static bool BuildVtable(uintptr_t mpBase)
{
    void** m79 = reinterpret_cast<void**>(mpBase + RVA_CM79_vtable);
    uint32_t check = 0;
    __try { check = (uint32_t)(uintptr_t)m79[0]; }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[janus1] Bad M79 vtable\n"); return false; }
    Log("[janus1] M79 vtable @ 0x%08zX  slot[0]=0x%08X\n", (uintptr_t)m79, check);

    __try { memcpy(g_vtable, m79, 220 * sizeof(void*)); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_m79Vtable = m79;

    g_vtable[3]   = (void*)J1_Precache;
    g_vtable[102] = (void*)J1_Deploy;
    g_vtable[165] = (void*)J1_WeaponIdle;
    g_vtable[167] = (void*)J1_PrimaryAttack;
    g_vtable[175] = (void*)J1_AddToPlayer;
    g_vtable[191] = (void*)J1_Holster;

    g_vtableReady = true;
    Log("[janus1] Vtable ready\n");
    return true;
}

// -----------------------------------------------------------------------
// Factory — hooks weapon_janus1 entry point
// -----------------------------------------------------------------------
static uint8_t g_savedBytes[5] = {};
static bool    g_savedOk       = false;

typedef void(__cdecl* pfnOrigFactory_t)(int);

static void CallOrigAndSwap(int edict)
{
    uintptr_t fnAddr = GetMpBase() + RVA_weapon_janus1;

    // Restore original bytes
    DWORD old = 0;
    VirtualProtect((void*)fnAddr, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)fnAddr, g_savedBytes, 5);
    VirtualProtect((void*)fnAddr, 5, old, &old);

    // Call original constructor
    reinterpret_cast<pfnOrigFactory_t>(fnAddr)(edict);

    // Re-patch immediately
    WriteJmp5(fnAddr, (uintptr_t)Janus1_Factory, nullptr);

    // Find the object: edict_t* -> pev (+568) -> pvPrivateData (+128)
    void* obj = nullptr;
    __try
    {
        uint8_t* edictPtr = reinterpret_cast<uint8_t*>((uintptr_t)edict);
        uint8_t* pev      = *reinterpret_cast<uint8_t**>(edictPtr + 568);
        obj               = *reinterpret_cast<void**>(pev + 128);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[janus1] Exception reading object from edict=0x%08X\n", edict);
        return;
    }

    if (!obj) { Log("[janus1] obj null after factory\n"); return; }

    Log("[janus1] obj=%p  vtable_before=0x%08X\n",
        obj, *reinterpret_cast<uint32_t*>(obj));

    // Swap vtable pointer
    DWORD old2 = 0;
    if (VirtualProtect(obj, 4, PAGE_EXECUTE_READWRITE, &old2))
    {
        *reinterpret_cast<void**>(obj) = (void*)g_vtable;
        VirtualProtect(obj, 4, old2, &old2);
        Log("[janus1] Vtable swapped -> 0x%08zX\n", (uintptr_t)g_vtable);
    }
    else Log("[janus1] VirtualProtect on obj failed\n");
}

void __cdecl Janus1_Factory(int edict)
{
    Log("[janus1] Factory edict=0x%08X t=%.3f\n", edict, GetTime());

    if (!g_vtableReady || !g_savedOk)
    {
        Log("[janus1] ERROR: not ready (vtable=%d saved=%d)\n",
            (int)g_vtableReady, (int)g_savedOk);
        return;
    }

    CallOrigAndSwap(edict);
    Log("[janus1] Factory done\n");
}

// -----------------------------------------------------------------------
// PostInit — called after Hooks_Install succeeds
// -----------------------------------------------------------------------
void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit\n");

    // Get saved bytes from hooks.cpp (saved during WriteJmp5)
    const uint8_t* saved = GetSavedBytes(RVA_weapon_janus1);
    if (saved)
    {
        memcpy(g_savedBytes, saved, 5);
        g_savedOk = true;
        Log("[janus1] Saved bytes: %02X %02X %02X %02X %02X\n",
            saved[0], saved[1], saved[2], saved[3], saved[4]);
    }
    else
    {
        // IDA-confirmed fallback: 55 8B EC 56 8B
        g_savedBytes[0]=0x55; g_savedBytes[1]=0x8B; g_savedBytes[2]=0xEC;
        g_savedBytes[3]=0x56; g_savedBytes[4]=0x8B;
        g_savedOk = true;
        Log("[janus1] Saved bytes: fallback 55 8B EC 56 8B\n");
    }

    // Build vtable from M79
    BuildVtable(mpBase);

    // Resolve engine functions from g_engfuncs (set by hooks.cpp)
    if (g_engfuncs)
    {
        void** tbl = reinterpret_cast<void**>(g_engfuncs);
        g_PrecacheModel = (pfnPrecacheModel_t)tbl[0];
        g_PrecacheSound = (pfnPrecacheSound_t)tbl[1];
        g_PrecacheEvent = (pfnPrecacheEvent_t)tbl[140];
        Log("[janus1] PrecacheModel=0x%08zX  PrecacheSound=0x%08zX  PrecacheEvent=0x%08zX\n",
            (uintptr_t)g_PrecacheModel, (uintptr_t)g_PrecacheSound, (uintptr_t)g_PrecacheEvent);
    }
    else Log("[janus1] g_engfuncs is null!\n");

    Log("[janus1] PostInit done. vtable=%s savedOk=%d\n",
        g_vtableReady ? "OK" : "FAIL", (int)g_savedOk);
}

// -----------------------------------------------------------------------
// Static registration
// -----------------------------------------------------------------------
struct Janus1Reg {
    Janus1Reg() { RegisterWeaponHook("weapon_janus1", (void*)Janus1_Factory, RVA_weapon_janus1); }
};
static Janus1Reg g_reg;