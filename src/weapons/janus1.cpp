// janus1.cpp
// CJanus1 — Janus-1 grenade launcher for CSNZ server
//
// Approach (per friend's advice):
//   1. Use Half-Life SDK patterns for weapon logic
//   2. Create weapon entity with proper Spawn/Precache/PrimaryAttack/etc.
//   3. Inject as DLL
//   4. On DLL attach, hook weapon_janus1 entry point -> our factory
//
// The factory allocates the object, installs our vtable, links the edict.
// The engine then calls all virtual methods through our vtable normally.
//
// Based on M79 implementation from IDA decompile:
//   - Same object size (536 bytes)
//   - Same vtable slot layout
//   - Precache: register sounds, event, model
//   - PrimaryAttack: fire rocket via CSNZ rocket factory
//   - AddToPlayer: config lookup + base call
//   - WeaponIdle: play idle anim

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include <cstring>
#include <cstdint>

// -------------------------------------------------------------------------
// mp.dll function typedefs (resolved at runtime)
// -------------------------------------------------------------------------
typedef int   (__cdecl*  pfnPrecacheModel_t)(const char*);
typedef int   (__cdecl*  pfnPrecacheSound_t)(const char*);
typedef int   (__cdecl*  pfnPrecacheEvent_t)(int type, const char* name);
typedef void* (__cdecl*  pfnAllocEntPriv_t)(void* ent, int size);
typedef void  (__cdecl*  pfnRegisterConfig_t)(const char* name);
typedef void* (__cdecl*  pfnGetConfig_t)(const char* name);
typedef int   (__thiscall* pfnBaseAddToPlayer_t)(void* wpn, void* player);
typedef float (__cdecl*  pfnWTB_t)();

static pfnPrecacheModel_t  g_PrecacheModel  = nullptr;
static pfnPrecacheSound_t  g_PrecacheSound  = nullptr;
static pfnPrecacheEvent_t  g_PrecacheEvent  = nullptr;
static pfnAllocEntPriv_t   g_AllocEntPriv   = nullptr;
static pfnGetConfig_t      g_GetConfig      = nullptr;
static pfnBaseAddToPlayer_t g_BaseAddToPlayer = nullptr;
static pfnWTB_t            g_WTB            = nullptr;

// Engfuncs are at a scanned offset — pfnPrecacheModel = engfuncs[0]
// pfnPrecacheSound = engfuncs[1]
// pfnAllocEntPrivateData = engfuncs[23] (from HLSDK eiface.h ordering)
// pfnPrecacheEvent = engfuncs[140]

// We'll grab these from the engfuncs table found by hooks.cpp
static void* g_engfuncsTable = nullptr;

static void ResolveEngineFns()
{
    if (!g_engfuncsTable) return;
    void** tbl = (void**)g_engfuncsTable;
    g_PrecacheModel  = (pfnPrecacheModel_t) tbl[0];
    g_PrecacheSound  = (pfnPrecacheSound_t) tbl[1];
    g_AllocEntPriv   = (pfnAllocEntPriv_t)  tbl[23];
    g_PrecacheEvent  = (pfnPrecacheEvent_t) tbl[140];
}

// -------------------------------------------------------------------------
// Weapon state — stored inside the allocated object
// We overlay our state on top of the raw memory block.
// Layout matches CBasePlayerWeapon field offsets from IDA.
// -------------------------------------------------------------------------
struct Janus1State
{
    // These are at fixed offsets matching the vtable-called code expectations
    // We don't redefine the whole struct — just access via Field() macros
};

// -------------------------------------------------------------------------
// Vtable — array of function pointers, installed at object+0
// Must be large enough for all slots the engine might call (220 slots)
// Slots we don't implement point to no-op stubs from the base game.
// We copy the M79 vtable first, then override our specific slots.
// -------------------------------------------------------------------------
static void*  g_vtable[220]    = {};
static bool   g_vtableReady    = false;
static void** g_m79VtablePtr   = nullptr; // CSimpleWpn<CM79> vtable from mp.dll

// CSimpleWpn<CM79> vtable RVA (from IDA: ??_7?$CSimpleWpn@VCM79Zhc@@@@6B@ but we want CM79 base)
// CM79 vtable: ??_7CM79@@6B@ @ 0x1159FA04
static const uintptr_t RVA_CM79_vtable = 0x159FA04;

// -------------------------------------------------------------------------
// Our virtual method implementations
// All use __thiscall convention (this in ECX on MSVC x86)
// -------------------------------------------------------------------------

// Slot 3: Precache
static int __fastcall Janus1_Precache(void* self, void* /*edx*/)
{
    Log("[janus1] Precache\n");
    if (g_PrecacheSound)  g_PrecacheSound("weapons/janus1-1.wav");
    if (g_PrecacheSound)  g_PrecacheSound("weapons/janus1-2.wav");
    if (g_PrecacheSound)  g_PrecacheSound("weapons/janus1_reload.wav");
    if (g_PrecacheModel)  g_PrecacheModel("models/v_janus1.mdl");
    if (g_PrecacheModel)  g_PrecacheModel("models/p_janus1.mdl");
    if (g_PrecacheModel)  g_PrecacheModel("models/w_janus1.mdl");

    // Precache event and store handle in usFireEvent field
    if (g_PrecacheEvent)
    {
        int evHandle = g_PrecacheEvent(1, "events/janus1.sc");
        // Store in object: this+0x1E8 (F_usFireEvent as uint16)
        *reinterpret_cast<uint16_t*>((uint8_t*)self + F_usFireEvent) = (uint16_t)evHandle;
        Log("[janus1] event handle = %d\n", evHandle);
    }

    // Register rocket entity
    // In M79: sub_1130E7D0("m79_rocket") — same pattern for janus1
    // We'll call the original M79 Precache via vtable to handle the rocket class
    // For now just log — rocket registration is done by the rocket entity itself
    Log("[janus1] Precache done\n");
    return 1;
}

// Slot 102: Deploy (sets view/player model, returns BOOL)
// Based on M79 slot 102: sub_10F294A0
static int __fastcall Janus1_Deploy(void* self, void* /*edx*/)
{
    Log("[janus1] Deploy\n");
    // Call base DefaultDeploy equivalent via vtable
    // For now delegate to M79 vtable slot 102 if available
    if (g_m79VtablePtr)
    {
        typedef int(__thiscall* Fn)(void*);
        return reinterpret_cast<Fn>(g_m79VtablePtr[102])(self);
    }
    return 1;
}

// Slot 165: WeaponIdle
static void __fastcall Janus1_WeaponIdle(void* self, void* /*edx*/)
{
    // Delegate to M79's WeaponIdle — identical behavior
    if (g_m79VtablePtr)
    {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79VtablePtr[165])(self);
    }
}

// Slot 167: PrimaryAttack
// Based on M79 PrimaryAttack — fire a janus1 rocket
static void __fastcall Janus1_PrimaryAttack(void* self, void* /*edx*/)
{
    Log("[janus1] PrimaryAttack t=%.3f\n", GetTime());
    // Delegate to M79 PrimaryAttack — same fire logic, just different model/sound
    // The event system handles client-side visuals
    if (g_m79VtablePtr)
    {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79VtablePtr[167])(self);
    }
}

// Slot 175: AddToPlayer
// Based on M79 slot 175: sub_10F29930
// Looks up "JANUS1" config, stores config ID, calls base AddToPlayer
static int __fastcall Janus1_AddToPlayer(void* self, void* /*edx*/, void* player)
{
    Log("[janus1] AddToPlayer player=%p\n", player);

    // Config lookup — use M79's slot 175 for now (same pattern)
    // Proper impl: g_GetConfig("JANUS1") -> store config ID -> call base
    if (g_m79VtablePtr)
    {
        typedef int(__thiscall* Fn)(void*, void*);
        int r = reinterpret_cast<Fn>(g_m79VtablePtr[175])(self, player);
        Log("[janus1] AddToPlayer done ret=%d\n", r);
        return r;
    }
    return 0;
}

// Slot 191: Holster
static void __fastcall Janus1_Holster(void* self, void* /*edx*/, int skiplocal)
{
    Log("[janus1] Holster\n");
    // Reset clip/ammo state
    *reinterpret_cast<int*>((uint8_t*)self + F_iClip) = 0;
    // Delegate to M79
    if (g_m79VtablePtr)
    {
        typedef void(__thiscall* Fn)(void*, int);
        reinterpret_cast<Fn>(g_m79VtablePtr[191])(self, skiplocal);
    }
}

// -------------------------------------------------------------------------
// Build our vtable by copying M79's and overriding our slots
// -------------------------------------------------------------------------
static bool BuildVtable(uintptr_t mpBase)
{
    void** m79vtbl = reinterpret_cast<void**>(mpBase + RVA_CM79_vtable);

    // Validate
    uint32_t first = 0;
    __try { first = (uint32_t)(uintptr_t)m79vtbl[0]; }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[janus1] Cannot read M79 vtable\n");
        return false;
    }
    Log("[janus1] M79 vtable[0] = 0x%08X\n", first);

    // Copy all 220 slots
    __try { memcpy(g_vtable, m79vtbl, 220 * sizeof(void*)); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_m79VtablePtr = m79vtbl;

    // Override with our implementations
    g_vtable[3]   = (void*)Janus1_Precache;
    g_vtable[102] = (void*)Janus1_Deploy;
    g_vtable[165] = (void*)Janus1_WeaponIdle;
    g_vtable[167] = (void*)Janus1_PrimaryAttack;
    g_vtable[175] = (void*)Janus1_AddToPlayer;
    g_vtable[191] = (void*)Janus1_Holster;

    g_vtableReady = true;
    Log("[janus1] Vtable built (%d slots, %d overridden)\n", 220, 6);
    return true;
}

// -------------------------------------------------------------------------
// Factory — called instead of weapon_janus1(int edict)
//
// Original weapon_janus1 (from IDA):
//   void __cdecl weapon_janus1(int edict)
//   {
//     if (!edict) edict = CreateEntity() + 132
//     v3 = *(edict + 568)         // entvars_t*
//     if (!v3 || !*(v3+128)) {
//       obj = pfnPvAllocEntPrivateData(v3, 536)
//       if (obj)
//         *(ctor(obj) + 8) = edict   // construct, link edict
//     }
//   }
//
// We replicate this exactly but install our vtable instead of CJanus1's.
// -------------------------------------------------------------------------

// pfnPvAllocEntPrivateData from engfuncs[23]
typedef void* (__cdecl* pfnAlloc_t)(void* entvars, int size);

// The original janus1 constructor (sub_10B31450 equivalent for janus1)
// We call this to get the properly initialized object, then swap vtable
// Actually: we call the original weapon_janus1 factory via trampoline,
// then find the object and swap its vtable pointer.

// Trampoline to original weapon_janus1
static uint8_t  g_origBytes[5] = {};
static bool     g_origSaved    = false;
typedef void(__cdecl* pfnOrigFactory_t)(int);

// Call original via inline trampoline
static void CallOrigJanus1(int edict)
{
    if (!g_origSaved) return;
    uintptr_t target = GetMpBase() + RVA_weapon_janus1;

    // Temporarily restore original bytes, call, re-patch
    DWORD old = 0;
    VirtualProtect((void*)target, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)target, g_origBytes, 5);
    VirtualProtect((void*)target, 5, old, &old);

    reinterpret_cast<pfnOrigFactory_t>(target)(edict);

    // Re-patch
    WriteJmp5(target, (uintptr_t)Janus1_Factory, nullptr);
}

void __cdecl Janus1_Factory(int edict)
{
    Log("[janus1] Factory edict=%d t=%.3f\n", edict, GetTime());

    if (!g_vtableReady)
    {
        Log("[janus1] ERROR: vtable not ready!\n");
        return;
    }

    // Call original constructor to allocate and init the object
    CallOrigJanus1(edict);

    // Now find the object: it's at *(entvars+128) where entvars = *(edict+568)
    // edict is an int = index; actual edict_t* = pfnPEntityOfEntIndex(edict)
    // But we can find it via the edict's private data pointer
    // entvars_t* pev = (entvars_t*)(edict + 568)  <- edict is actually edict_t* here
    // private = *(pev + 128) = pev->pContainingEntity->pvPrivateData? 
    // From IDA: v3 = *(edict+568); obj = v3's private data
    // Simpler: the object vtable ptr is at obj+0, and obj+8 = edict
    // We can scan recently allocated memory... but that's complex.
    //
    // BETTER APPROACH: instead of calling original + patching,
    // we replicate the allocation ourselves.

    // Read edict_t ptr: in GoldSrc, the factory receives int edict_t index
    // The engine's pfnPEntityOfEntIndex gives us edict_t*
    // For now log and return — the original already ran correctly
    // In next iteration we'll intercept the allocation and swap vtable

    Log("[janus1] Factory complete (using original + vtable swap on next iteration)\n");
}

// -------------------------------------------------------------------------
// Post-init: called after Hooks_Install() succeeds
// -------------------------------------------------------------------------
void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit\n");

    // Save original bytes before they were overwritten by hook
    // (hooks.cpp already wrote JMP — we need to read the saved bytes)
    // Actually hooks.cpp currently passes nullptr for outOrig.
    // We need to save them BEFORE patching. See note below.
    // For now build vtable and resolve fns.

    // Build vtable from M79
    BuildVtable(mpBase);

    // Resolve engine functions
    // g_engfuncsTable is not exposed from hooks.cpp yet — we find it ourselves
    // The engfuncs table is where g_engfuncs points (set in hooks.cpp ResolveGlobals)
    // We need to expose g_engfuncs from hooks.cpp
    // For now: re-scan (lazy but works)
    HMODULE hMp = (HMODULE)mpBase;
    HMODULE hHw = GetModuleHandleA("hw.dll");
    if (hHw)
    {
        IMAGE_DOS_HEADER* hdos = (IMAGE_DOS_HEADER*)hHw;
        IMAGE_NT_HEADERS* hnt  = (IMAGE_NT_HEADERS*)((uint8_t*)hHw + hdos->e_lfanew);
        uintptr_t hwBase = (uintptr_t)hHw;
        uintptr_t hwEnd  = hwBase + hnt->OptionalHeader.SizeOfImage;

        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hMp;
        IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)((uint8_t*)hMp + dos->e_lfanew);
        uint8_t* mpData = (uint8_t*)mpBase;
        DWORD mpSize = nt->OptionalHeader.SizeOfImage;

        size_t bestOff = 0; int bestRun = 0, curRun = 0; size_t runStart = 0;
        for (size_t off = 0; off+4 <= mpSize; off += 4)
        {
            uint32_t v = 0;
            __try { v = *reinterpret_cast<uint32_t*>(mpData+off); }
            __except(EXCEPTION_EXECUTE_HANDLER) { curRun=0; continue; }
            if (v >= hwBase && v < hwEnd) { if (!curRun) runStart=off; curRun++; }
            else { if (curRun > bestRun) { bestRun=curRun; bestOff=runStart; } curRun=0; }
        }
        if (bestRun >= 20)
        {
            g_engfuncsTable = mpData + bestOff;
            ResolveEngineFns();
            Log("[janus1] Engine fns resolved from mp+0x%zX\n", bestOff);
        }
    }

    Log("[janus1] PostInit done. vtable=%s\n", g_vtableReady ? "OK" : "FAIL");
}

// -------------------------------------------------------------------------
// Static registration — runs before main()
// -------------------------------------------------------------------------
struct Janus1Reg
{
    Janus1Reg()
    {
        RegisterWeaponHook("weapon_janus1",
                           (void*)Janus1_Factory,
                           RVA_weapon_janus1);
        Log("[janus1] Registered hook\n");
    }
};
static Janus1Reg g_reg;
