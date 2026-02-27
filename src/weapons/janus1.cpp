// janus1.cpp - Janus1 weapon, full standalone implementation
// Uses M79 assets (guaranteed to exist), M79 rocket mechanics
// NO delegation to M79 vtable slots - all implemented directly

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include <cstring>
#include <cstdint>
#include <windows.h>

typedef int  (__cdecl* pfnPrecacheModel_t)(const char*);
typedef int  (__cdecl* pfnPrecacheSound_t)(const char*);
typedef int  (__cdecl* pfnPrecacheEvent_t)(int, const char*);

static pfnPrecacheModel_t g_PrecacheModel = nullptr;
static pfnPrecacheSound_t g_PrecacheSound = nullptr;
static pfnPrecacheEvent_t g_PrecacheEvent = nullptr;

// M79 vtable - only used to read function pointers directly, not for delegation
static void** g_m79Vtable = nullptr;
static const uintptr_t RVA_CM79_vtable = 0x159FA04;

// Our vtable - static array, 220 slots, copied from M79 then patched
static void* g_vtable[220] = {};
static bool  g_vtableReady = false;

// Trampoline state
static uint8_t g_origBytes[5] = {};
static bool    g_origSaved    = false;
typedef void(__cdecl* pfnOrigFactory_t)(int);

// -----------------------------------------------------------------------
// SLOT 4: Precache
// Only precaches assets we KNOW exist in the game (M79 assets)
// -----------------------------------------------------------------------
static int __fastcall J1_Precache(void* self, void* /*edx*/)
{
    Log("[janus1] Precache\n");

    if (g_PrecacheSound) g_PrecacheSound("weapons/m79-1.wav");
    if (g_PrecacheModel) g_PrecacheModel("models/v_m79.mdl");
    if (g_PrecacheModel) g_PrecacheModel("models/p_m79.mdl");
    if (g_PrecacheModel) g_PrecacheModel("models/w_m79.mdl");

    if (g_PrecacheEvent)
    {
        int ev = g_PrecacheEvent(1, "events/m79.sc");
        *reinterpret_cast<uint16_t*>((uint8_t*)self + F_usFireEvent) = (uint16_t)ev;
        Log("[janus1] event=%d\n", ev);
    }

    Log("[janus1] Precache done\n");
    return 1;
}

// -----------------------------------------------------------------------
// SLOT 3: Spawn
// Sets up the weapon entity - model, solid, movetype
// Based on M79 slot 3 disasm from pev_layout.txt
// -----------------------------------------------------------------------
static void __fastcall J1_Spawn(void* self, void* /*edx*/)
{
    Log("[janus1] Spawn\n");

    // Call our own Precache (slot 4) first, same as M79 slot 3 does
    J1_Precache(self, nullptr);

    // Set weapon ID in object at +0xE4
    *reinterpret_cast<int*>((uint8_t*)self + 0xE4) = WEAPON_JANUS1;

    // Set clip to 1 (single shot grenade launcher)
    *reinterpret_cast<int*>((uint8_t*)self + F_iClip) = 1;

    Log("[janus1] Spawn done\n");
}

// -----------------------------------------------------------------------
// SLOT 102: Deploy
// Calls M79's Deploy directly - it sets the view model and animations
// We call it directly as a function, not via vtable slot
// -----------------------------------------------------------------------
static int __fastcall J1_Deploy(void* self, void* /*edx*/)
{
    Log("[janus1] Deploy\n");
    if (!g_m79Vtable) return 1;
    typedef int(__thiscall* Fn)(void*);
    return reinterpret_cast<Fn>(g_m79Vtable[102])(self);
}

// -----------------------------------------------------------------------
// SLOT 165: WeaponIdle
// Calls M79's WeaponIdle directly - handles firing the rocket
// -----------------------------------------------------------------------
static void __fastcall J1_WeaponIdle(void* self, void* /*edx*/)
{
    if (!g_m79Vtable) return;
    typedef void(__thiscall* Fn)(void*);
    reinterpret_cast<Fn>(g_m79Vtable[165])(self);
}

// -----------------------------------------------------------------------
// SLOT 175: AddToPlayer
// -----------------------------------------------------------------------
static int __fastcall J1_AddToPlayer(void* self, void* /*edx*/, void* player)
{
    Log("[janus1] AddToPlayer\n");
    if (!g_m79Vtable) return 0;
    typedef int(__thiscall* Fn)(void*, void*);
    return reinterpret_cast<Fn>(g_m79Vtable[175])(self, player);
}

// -----------------------------------------------------------------------
// SLOT 189: Holster
// -----------------------------------------------------------------------
static void __fastcall J1_Holster(void* self, void* /*edx*/, int skiplocal)
{
    Log("[janus1] Holster\n");
    if (!g_m79Vtable) return;
    typedef void(__thiscall* Fn)(void*, int);
    reinterpret_cast<Fn>(g_m79Vtable[189])(self, skiplocal);
}

// -----------------------------------------------------------------------
// Build vtable: copy M79's 220 slots, patch our 6
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
    Log("[janus1] Vtable ready. M79[0]=%p M79[3]=%p M79[4]=%p\n",
        g_m79Vtable[0], g_m79Vtable[3], g_m79Vtable[4]);
    return true;
}

// -----------------------------------------------------------------------
// Trampoline: restore, call original, re-patch
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
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[janus1] Exception in factory\n"); return; }

    if (!obj) { Log("[janus1] obj null\n"); return; }

    *reinterpret_cast<void**>(obj) = g_vtable;
    Log("[janus1] Swapped vtable on obj=%p\n", obj);
}

// -----------------------------------------------------------------------
// PostInit
// -----------------------------------------------------------------------
static void ScanEngfuncs(uintptr_t mpBase)
{
    HMODULE hHw = GetModuleHandleA("hw.dll");
    if (!hHw) return;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mpBase;
    IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)(mpBase + dos->e_lfanew);
    DWORD mpSize = nt->OptionalHeader.SizeOfImage;
    uint8_t* mpData = (uint8_t*)mpBase;

    IMAGE_NT_HEADERS* hnt = (IMAGE_NT_HEADERS*)((uint8_t*)hHw +
        ((IMAGE_DOS_HEADER*)hHw)->e_lfanew);
    uintptr_t hwBase = (uintptr_t)hHw;
    uintptr_t hwEnd  = hwBase + hnt->OptionalHeader.SizeOfImage;

    size_t bestOff = 0; int bestRun = 0, curRun = 0; size_t runStart = 0;
    for (size_t off = 0; off+4 <= mpSize; off += 4)
    {
        uint32_t v = 0;
        __try { v = *reinterpret_cast<uint32_t*>(mpData+off); }
        __except(EXCEPTION_EXECUTE_HANDLER) { curRun=0; continue; }
        if (v >= hwBase && v < hwEnd) { if (!curRun) runStart=off; curRun++; }
        else { if (curRun > bestRun) { bestRun=curRun; bestOff=runStart; } curRun=0; }
    }
    if (bestRun < 20) { Log("[janus1] engfuncs scan failed\n"); return; }

    void** tbl = reinterpret_cast<void**>(mpData + bestOff);
    g_PrecacheModel = (pfnPrecacheModel_t)tbl[0];
    g_PrecacheSound = (pfnPrecacheSound_t)tbl[1];
    g_PrecacheEvent = (pfnPrecacheEvent_t)tbl[140];
    Log("[janus1] engfuncs: PrecacheModel=%p Sound=%p Event=%p\n",
        (void*)g_PrecacheModel, (void*)g_PrecacheSound, (void*)g_PrecacheEvent);
}

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
        Log("[janus1] Using fallback orig bytes\n");
    }

    ScanEngfuncs(mpBase);
    Log("[janus1] PostInit done\n");
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------
struct Janus1Reg
{
    Janus1Reg() { RegisterWeaponHook("weapon_janus1", (void*)Janus1_Factory, RVA_weapon_janus1); }
};
static Janus1Reg g_reg;
