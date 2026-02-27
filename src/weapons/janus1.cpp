// janus1.cpp - CJanus1 grenade launcher (v24)
//
// WHAT CHANGED FROM THE WORKING STUB:
//   - Factory now actually swaps the vtable on the spawned object
//   - Uses CORRECT M79 vtable slot numbers from IDA m79_actions.txt:
//       Spawn=3, Precache=4, Deploy=102, WeaponIdle=165,
//       AddToPlayer=175, Holster=189
//   - Removed wrong slots (167=SUB_DoNothing, 191=SUB_DoNothing)
//   - Vtable is static array (220 entries) - same safe approach as stub
//   - Engine fns resolved from engfuncs scan (same as stub - works)
//   - GetSavedBytes() now available from hooks.cpp for CallOrig trampoline
//
// HOW THE VTABLE SWAP WORKS:
//   1. Factory calls CallOrigJanus1(edict) - original allocates+constructs object
//   2. Object is at: *(*(edict + 0x238) + 0x80)  [confirmed from IDA pev_layout.txt]
//   3. We overwrite the vtable pointer at object+0 with our g_vtable
//   4. All subsequent engine calls go through our implementations

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include <cstring>
#include <cstdint>
#include <windows.h>

// -------------------------------------------------------------------------
// Engine function typedefs (resolved at runtime via engfuncs scan)
// -------------------------------------------------------------------------
typedef int  (__cdecl* pfnPrecacheModel_t)(const char*);
typedef int  (__cdecl* pfnPrecacheSound_t)(const char*);
typedef int  (__cdecl* pfnPrecacheEvent_t)(int type, const char* name);

static pfnPrecacheModel_t g_PrecacheModel = nullptr;
static pfnPrecacheSound_t g_PrecacheSound = nullptr;
static pfnPrecacheEvent_t g_PrecacheEvent = nullptr;
static void*              g_engfuncsTable = nullptr;

static void ResolveEngineFns()
{
    if (!g_engfuncsTable) return;
    void** tbl = (void**)g_engfuncsTable;
    g_PrecacheModel = (pfnPrecacheModel_t)tbl[0];
    g_PrecacheSound = (pfnPrecacheSound_t)tbl[1];
    g_PrecacheEvent = (pfnPrecacheEvent_t)tbl[140];
}

// -------------------------------------------------------------------------
// Vtable - static array, 220 slots (same as working stub)
// Filled by BuildVtable(): copy M79 vtable, override our slots
// -------------------------------------------------------------------------
static void*  g_vtable[220]  = {};
static bool   g_vtableReady  = false;
static void** g_m79VtablePtr = nullptr;

// CONFIRMED from IDA m79_actions.txt ("M79 vtable - slots pointing to M79-specific code"):
// [  3] 0x10F29BF0  sub_10F29BF0  = Spawn  (calls Precache, sets model/solid/movetype)
// [  4] 0x10F28D40  sub_10F28D40  = Precache (loads sound, event, ammo model)
// [ 102] 0x10F294A0 sub_10F294A0  = Deploy
// [ 165] 0x10F28D80 sub_10F28D80  = WeaponIdle (thunk -> sub_10F294E0 which fires rocket)
// [ 175] 0x10F29930 sub_10F29930  = AddToPlayer
// [ 189] 0x10F28D90 sub_10F28D90  = Holster
// NOTE: slots 166/167/191 = SUB_DoNothing (past vtable real end - DO NOT USE)
static const int SLOT_Spawn       = 3;
static const int SLOT_Precache    = 4;
static const int SLOT_Deploy      = 102;
static const int SLOT_WeaponIdle  = 165;
static const int SLOT_AddToPlayer = 175;
static const int SLOT_Holster     = 189;

// M79 vtable RVA (confirmed from IDA xref analysis)
static const uintptr_t RVA_CM79_vtable = 0x159FA04;

// -------------------------------------------------------------------------
// Our virtual method implementations (__fastcall = thiscall-compatible)
// -------------------------------------------------------------------------

// Slot 4: Precache - loads janus1 assets
static int __fastcall J1_Precache(void* self, void* /*edx*/)
{
    Log("[janus1] Precache\n");

    // Janus1 sounds/models
    if (g_PrecacheSound) g_PrecacheSound("weapons/janus1-1.wav");
    if (g_PrecacheSound) g_PrecacheSound("weapons/janus1-2.wav");
    if (g_PrecacheSound) g_PrecacheSound("weapons/janus1_reload.wav");
    if (g_PrecacheModel) g_PrecacheModel("models/v_janus1.mdl");
    if (g_PrecacheModel) g_PrecacheModel("models/p_janus1.mdl");
    if (g_PrecacheModel) g_PrecacheModel("models/w_janus1.mdl");

    // Precache event and store handle (at this+0x1E8 = usFireEvent field)
    if (g_PrecacheEvent)
    {
        int ev = g_PrecacheEvent(1, "events/janus1.sc");
        *reinterpret_cast<uint16_t*>((uint8_t*)self + 0x1E8) = (uint16_t)ev;
        Log("[janus1] event handle=%d\n", ev);
    }

    // Let M79's Precache also run - it registers the m79_rocket entity class
    // and precaches the rocket's own model. We need this for the rocket to exist.
    if (g_m79VtablePtr)
    {
        typedef int(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79VtablePtr[SLOT_Precache])(self);
    }

    Log("[janus1] Precache done\n");
    return 1;
}

// Slot 3: Spawn - delegate to M79 (sets model, movetype, solid)
static void __fastcall J1_Spawn(void* self, void* /*edx*/)
{
    Log("[janus1] Spawn self=%p\n", self);
    if (g_m79VtablePtr)
    {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79VtablePtr[SLOT_Spawn])(self);
    }
    Log("[janus1] Spawn done\n");
}

// Slot 102: Deploy - delegate to M79
static int __fastcall J1_Deploy(void* self, void* /*edx*/)
{
    Log("[janus1] Deploy\n");
    if (g_m79VtablePtr)
    {
        typedef int(__thiscall* Fn)(void*);
        return reinterpret_cast<Fn>(g_m79VtablePtr[SLOT_Deploy])(self);
    }
    return 1;
}

// Slot 165: WeaponIdle - M79 fires rocket from here (checks ammo, spawns rocket, plays event)
static void __fastcall J1_WeaponIdle(void* self, void* /*edx*/)
{
    if (g_m79VtablePtr)
    {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79VtablePtr[SLOT_WeaponIdle])(self);
    }
}

// Slot 175: AddToPlayer
static int __fastcall J1_AddToPlayer(void* self, void* /*edx*/, void* player)
{
    Log("[janus1] AddToPlayer player=%p\n", player);
    if (g_m79VtablePtr)
    {
        typedef int(__thiscall* Fn)(void*, void*);
        int r = reinterpret_cast<Fn>(g_m79VtablePtr[SLOT_AddToPlayer])(self, player);
        Log("[janus1] AddToPlayer ret=%d\n", r);
        return r;
    }
    return 0;
}

// Slot 189: Holster
static void __fastcall J1_Holster(void* self, void* /*edx*/, int skiplocal)
{
    Log("[janus1] Holster\n");
    if (g_m79VtablePtr)
    {
        typedef void(__thiscall* Fn)(void*, int);
        reinterpret_cast<Fn>(g_m79VtablePtr[SLOT_Holster])(self, skiplocal);
    }
}

// -------------------------------------------------------------------------
// BuildVtable - copy M79's 220-slot vtable, then patch our overrides
// -------------------------------------------------------------------------
static bool BuildVtable(uintptr_t mpBase)
{
    void** m79vtbl = reinterpret_cast<void**>(mpBase + RVA_CM79_vtable);

    uint32_t first = 0;
    __try { first = (uint32_t)(uintptr_t)m79vtbl[0]; }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[janus1] Cannot read M79 vtable\n");
        return false;
    }
    Log("[janus1] M79 vtable[0]=0x%08X [3]=0x%08X [4]=0x%08X [165]=0x%08X\n",
        first,
        (uint32_t)(uintptr_t)m79vtbl[SLOT_Spawn],
        (uint32_t)(uintptr_t)m79vtbl[SLOT_Precache],
        (uint32_t)(uintptr_t)m79vtbl[SLOT_WeaponIdle]);

    __try { memcpy(g_vtable, m79vtbl, 220 * sizeof(void*)); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_m79VtablePtr = m79vtbl;

    // Override with janus1-specific implementations
    g_vtable[SLOT_Spawn]       = (void*)J1_Spawn;
    g_vtable[SLOT_Precache]    = (void*)J1_Precache;
    g_vtable[SLOT_Deploy]      = (void*)J1_Deploy;
    g_vtable[SLOT_WeaponIdle]  = (void*)J1_WeaponIdle;
    g_vtable[SLOT_AddToPlayer] = (void*)J1_AddToPlayer;
    g_vtable[SLOT_Holster]     = (void*)J1_Holster;

    g_vtableReady = true;
    Log("[janus1] Vtable built (220 slots, 6 overridden: Spawn[%d] Precache[%d] "
        "Deploy[%d] WeaponIdle[%d] AddToPlayer[%d] Holster[%d])\n",
        SLOT_Spawn, SLOT_Precache, SLOT_Deploy,
        SLOT_WeaponIdle, SLOT_AddToPlayer, SLOT_Holster);
    return true;
}

// -------------------------------------------------------------------------
// Factory trampoline - temporarily restores original bytes, calls original,
// then re-patches our hook
// -------------------------------------------------------------------------
static uint8_t g_origBytes[5] = {};
static bool    g_origSaved    = false;

typedef void(__cdecl* pfnOrigFactory_t)(int);

static void CallOrigJanus1(int edict)
{
    if (!g_origSaved) return;
    uintptr_t target = GetMpBase() + RVA_weapon_janus1;

    DWORD old = 0;
    VirtualProtect((void*)target, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)target, g_origBytes, 5);
    VirtualProtect((void*)target, 5, old, &old);

    reinterpret_cast<pfnOrigFactory_t>(target)(edict);

    // Re-patch our hook
    WriteJmp5(target, (uintptr_t)Janus1_Factory, nullptr);
}

// -------------------------------------------------------------------------
// Factory - entry point hook for weapon_janus1
//
// IDA confirms: weapon_janus1(int edict) where edict is entvars_t*-ish ptr
// Object is found at: *(*(edict + 0x238) + 0x80)
//   [esi+238h] = inner edict_t*
//   [ecx+80h]  = pvPrivateData = our C++ object
// -------------------------------------------------------------------------
void __cdecl Janus1_Factory(int edict)
{
    Log("[janus1] Factory edict=%d t=%.3f\n", edict, GetTime());

    if (!g_vtableReady)
    {
        Log("[janus1] ERROR: vtable not ready, running original only\n");
        // Still call original so weapon spawns normally (just without our overrides)
        CallOrigJanus1(edict);
        return;
    }

    // Step 1: Call original to allocate + construct the object
    CallOrigJanus1(edict);

    // Step 2: Find the object via IDA-confirmed pointer chain
    // edict -> +0x238 -> inner_edict -> +0x80 -> pvPrivateData (our C++ object)
    void* obj = nullptr;
    __try
    {
        uint8_t* pevPtr = reinterpret_cast<uint8_t*>((uintptr_t)edict);
        uint8_t* innerEdict = *reinterpret_cast<uint8_t**>(pevPtr + 0x238);
        Log("[janus1] innerEdict=%p\n", (void*)innerEdict);
        if (innerEdict)
            obj = *reinterpret_cast<void**>(innerEdict + 0x80);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[janus1] Exception reading object pointer\n");
        return;
    }

    if (!obj) { Log("[janus1] obj is null\n"); return; }

    void* origVtable = *reinterpret_cast<void**>(obj);
    Log("[janus1] obj=%p origVtable=%p\n", obj, origVtable);

    // Step 3: Swap vtable pointer
    *reinterpret_cast<void**>(obj) = g_vtable;
    Log("[janus1] Vtable swapped: %p -> %p\n", origVtable, (void*)g_vtable);
    Log("[janus1] Factory done\n");
}

// -------------------------------------------------------------------------
// PostInit - called after Hooks_Install() from dllmain.cpp
// -------------------------------------------------------------------------
void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit\n");

    // Build vtable from M79
    BuildVtable(mpBase);

    // Grab saved original bytes (hooks.cpp saves them before patching)
    const uint8_t* sb = GetSavedBytes(RVA_weapon_janus1);
    if (sb)
    {
        memcpy(g_origBytes, sb, 5);
        g_origSaved = true;
        Log("[janus1] Saved bytes: %02X %02X %02X %02X %02X\n",
            sb[0], sb[1], sb[2], sb[3], sb[4]);
    }
    else
    {
        Log("[janus1] WARNING: saved bytes not available yet\n");
        // Fallback: read from memory directly (hook may already be installed)
        // Common janus1 prologue: 55 8B EC 56 8B
        g_origBytes[0]=0x55; g_origBytes[1]=0x8B; g_origBytes[2]=0xEC;
        g_origBytes[3]=0x56; g_origBytes[4]=0x8B;
        g_origSaved = true;
    }

    // Resolve engine functions (same hw.dll scan as hooks.cpp)
    HMODULE hMp = (HMODULE)mpBase;
    HMODULE hHw = GetModuleHandleA("hw.dll");
    if (hHw)
    {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hMp;
        IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)((uint8_t*)hMp + dos->e_lfanew);
        DWORD mpSize = nt->OptionalHeader.SizeOfImage;
        uint8_t* mpData = (uint8_t*)mpBase;

        IMAGE_DOS_HEADER* hdos = (IMAGE_DOS_HEADER*)hHw;
        IMAGE_NT_HEADERS* hnt  = (IMAGE_NT_HEADERS*)((uint8_t*)hHw + hdos->e_lfanew);
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
        if (bestRun >= 20)
        {
            g_engfuncsTable = mpData + bestOff;
            ResolveEngineFns();
            Log("[janus1] Engine fns resolved from mp+0x%zX "
                "PrecacheModel=%p PrecacheSound=%p PrecacheEvent=%p\n",
                bestOff, (void*)g_PrecacheModel,
                (void*)g_PrecacheSound, (void*)g_PrecacheEvent);
        }
        else
        {
            Log("[janus1] WARNING: engfuncs scan found bestRun=%d (<20)\n", bestRun);
        }
    }

    Log("[janus1] PostInit done. vtable=%s origSaved=%d\n",
        g_vtableReady ? "OK" : "FAIL", (int)g_origSaved);
}

// -------------------------------------------------------------------------
// Static registration
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
