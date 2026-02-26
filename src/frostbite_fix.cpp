// frostbite_fix.cpp  v13
//
// DIAGNOSTIC + CALL-THROUGH hooks.
//
// The server already has all 8 CFrostbite functions (confirmed: v12 patched
// slots with the same values they already had). So the code exists but
// something is silently failing.
//
// v13 strategy:
//   1. Hook each slot with a CALL-THROUGH wrapper that logs inputs/state
//   2. NEVER reimplement logic — just call the original and log around it
//   3. This tells us WHICH function is failing and WHY
//
// From IDA (client mp.dll, imagebase 0x10000000):
//   WeaponIdle      RVA 0xB30C80   slot 165
//   Think           RVA 0xB31100   slot 166
//   PrimaryAttack   RVA 0xB30E90   slot 167
//   SecondaryAttack RVA 0xB30F60   slot 168
//   AddToPlayer     RVA 0xB30FE0   slot 175
//   Holster         RVA 0xB31040   slot 191
//   TakeDamage      RVA 0xB31060   slot 193
//   PropThink       RVA 0xB312C0   slot 194
//
// Key offsets (byte-based, confirmed from decompilation):
//   this+0x13C = m_flNextPrimaryAttack
//   this+0x154 = ammo
//   this+0x15C = m_iClip
//   this+0x18C = m_flTimeWeaponIdle
//   this+0x194 = shot counter
//   this+0x1D8 = config ID
//   this+0x1EC = m_iDefaultAmmo
//   this+0x1E8 = event handle (uint16)
//   this+0x1FC = charge state float
//   this+0x204 = secondary state
//   this+0x0D0 = m_pPlayer

#include <windows.h>
#include <cstdint>
#include <cstring>
#include "givefnptrs.h"
#include "logger.h"

// ---------------------------------------------------------------------------
// Field accessor
// ---------------------------------------------------------------------------
template<typename T>
static inline T& Field(void* base, int byteOff)
{
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + byteOff);
}

// ---------------------------------------------------------------------------
// Vtable helpers
// ---------------------------------------------------------------------------
static inline void** GetVtable(void* obj)
{
    return *reinterpret_cast<void***>(obj);
}

// Save/restore original function pointers
struct OrigFns {
    uintptr_t WeaponIdle;
    uintptr_t Think;
    uintptr_t PrimaryAttack;
    uintptr_t SecondaryAttack;
    uintptr_t AddToPlayer;
    uintptr_t Holster;
    uintptr_t TakeDamage;
    uintptr_t PropThink;
};
static OrigFns g_orig = {};

// Vtable slot numbers (client IDA confirmed)
#define SLOT_WEAPONIDLE       165
#define SLOT_THINK            166
#define SLOT_PRIMARYATTACK    167
#define SLOT_SECONDARYATTACK  168
#define SLOT_ADDTOPLAYER      175
#define SLOT_HOLSTER          191
#define SLOT_TAKEDAMAGE       193
#define SLOT_PROP_THINK       194

// CFrostbite vtable RVA
static const uintptr_t kRVA_Vtable = 0x15D4C80;

static uintptr_t g_mpBase  = 0;
static bool      g_patched = false;

// gpGlobals->time shorthand
static float NOW()
{
    return g_pGlobals ? g_pGlobals->time : 0.0f;
}

// ---------------------------------------------------------------------------
// HOOK: WeaponIdle (slot 165)
// Log state, then call original
// ---------------------------------------------------------------------------
static void __fastcall Hook_WeaponIdle(void* wpn, void* edx)
{
    float idleTime = Field<float>(wpn, 0x18C);
    float t        = NOW();
    void* pPlayer  = Field<void*>(wpn, 0x0D0);

    Log("[FB] WeaponIdle wpn=%p player=%p idleTime=%.2f now=%.2f\n",
        wpn, pPlayer, idleTime, t);

    typedef void(__thiscall* Fn)(void*);
    reinterpret_cast<Fn>(g_orig.WeaponIdle)(wpn);
}

// ---------------------------------------------------------------------------
// HOOK: Think (slot 166)
// ---------------------------------------------------------------------------
static void __fastcall Hook_Think(void* wpn, void* edx)
{
    float t       = NOW();
    int   freeze  = Field<int>  (wpn, 348);
    float active  = Field<float>(wpn, 508);

    Log("[FB] Think wpn=%p t=%.2f freeze=%d active=%.2f\n",
        wpn, t, freeze, active);

    typedef void(__thiscall* Fn)(void*);
    reinterpret_cast<Fn>(g_orig.Think)(wpn);
}

// ---------------------------------------------------------------------------
// HOOK: PrimaryAttack (slot 167)
// ---------------------------------------------------------------------------
static void __fastcall Hook_PrimaryAttack(void* wpn, void* edx)
{
    int   ammo      = Field<int>  (wpn, 0x154);
    float nextAtk   = Field<float>(wpn, 0x13C);
    float charge    = Field<float>(wpn, 0x1FC);
    uint16_t evHdl  = Field<uint16_t>(wpn, 0x1E8);
    void* pPlayer   = Field<void*>(wpn, 0x0D0);
    float t         = NOW();

    Log("[FB] PrimaryAttack wpn=%p player=%p ammo=%d nextAtk=%.2f charge=%.2f evHdl=%d t=%.2f\n",
        wpn, pPlayer, ammo, nextAtk, charge, (int)evHdl, t);

    typedef void(__thiscall* Fn)(void*);
    reinterpret_cast<Fn>(g_orig.PrimaryAttack)(wpn);

    // Log state after
    Log("[FB] PrimaryAttack DONE ammo=%d nextAtk=%.2f\n",
        Field<int>(wpn, 0x154), Field<float>(wpn, 0x13C));
}

// ---------------------------------------------------------------------------
// HOOK: SecondaryAttack (slot 168)
// ---------------------------------------------------------------------------
static void __fastcall Hook_SecondaryAttack(void* wpn, void* edx)
{
    float nextAtk = Field<float>(wpn, 0x13C);
    float charge  = Field<float>(wpn, 0x1FC);
    float secSt   = Field<float>(wpn, 0x204);
    int   cfgId   = Field<int>  (wpn, 0x1D8);
    void* pPlayer = Field<void*>(wpn, 0x0D0);
    float t       = NOW();

    Log("[FB] SecondaryAttack wpn=%p player=%p nextAtk=%.2f charge=%.2f sec=%.2f cfg=%d t=%.2f\n",
        wpn, pPlayer, nextAtk, charge, secSt, cfgId, t);

    typedef void(__thiscall* Fn)(void*);
    reinterpret_cast<Fn>(g_orig.SecondaryAttack)(wpn);

    Log("[FB] SecondaryAttack DONE nextAtk=%.2f charge=%.2f\n",
        Field<float>(wpn, 0x13C), Field<float>(wpn, 0x1FC));
}

// ---------------------------------------------------------------------------
// HOOK: AddToPlayer (slot 175)
// ---------------------------------------------------------------------------
static int __fastcall Hook_AddToPlayer(void* wpn, void* edx, void* pPlayer)
{
    int cfgId = Field<int>(wpn, 0x1D8);
    Log("[FB] AddToPlayer wpn=%p player=%p cfgId=%d\n", wpn, pPlayer, cfgId);

    typedef int(__thiscall* Fn)(void*, void*);
    int ret = reinterpret_cast<Fn>(g_orig.AddToPlayer)(wpn, pPlayer);

    Log("[FB] AddToPlayer DONE ret=%d cfgId=%d evHdl=%d\n",
        ret, Field<int>(wpn, 0x1D8), (int)Field<uint16_t>(wpn, 0x1E8));
    return ret;
}

// ---------------------------------------------------------------------------
// HOOK: Holster (slot 191)
// ---------------------------------------------------------------------------
static void __fastcall Hook_Holster(void* wpn, void* edx, int skipLocal)
{
    Log("[FB] Holster wpn=%p skip=%d clip=%d ammo=%d\n",
        wpn, skipLocal, Field<int>(wpn, 0x15C), Field<int>(wpn, 0x154));

    typedef void(__thiscall* Fn)(void*, int);
    reinterpret_cast<Fn>(g_orig.Holster)(wpn, skipLocal);
}

// ---------------------------------------------------------------------------
// HOOK: TakeDamage (slot 193)
// ---------------------------------------------------------------------------
static int __fastcall Hook_TakeDamage(void* wpn, void* edx, void* info, float dmg)
{
    Log("[FB] TakeDamage wpn=%p info=%p dmg=%.1f\n", wpn, info, dmg);

    typedef int(__thiscall* Fn)(void*, void*, float);
    int ret = reinterpret_cast<Fn>(g_orig.TakeDamage)(wpn, info, dmg);

    Log("[FB] TakeDamage DONE ret=%d\n", ret);
    return ret;
}

// ---------------------------------------------------------------------------
// HOOK: PropThink (slot 194)
// ---------------------------------------------------------------------------
static void __fastcall Hook_PropThink(void* prop, void* edx)
{
    float t      = NOW();
    float active = Field<float>(prop, 508);
    int   freeze = Field<int>  (prop, 348);

    Log("[FB] PropThink prop=%p t=%.2f active=%.2f freeze=%d\n",
        prop, t, active, freeze);

    typedef void(__thiscall* Fn)(void*);
    reinterpret_cast<Fn>(g_orig.PropThink)(prop);
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------
static bool SafeWrite(uintptr_t addr, uintptr_t val)
{
    DWORD old = 0;
    if (!VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    __try { *(uintptr_t*)addr = val; }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect((void*)addr, 4, old, &old);
        return false;
    }
    VirtualProtect((void*)addr, 4, old, &old);
    return true;
}

static bool SafeRead(uintptr_t addr, uintptr_t& out)
{
    __try { out = *(uintptr_t*)addr; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// FrostbiteFix_Init
// ---------------------------------------------------------------------------
bool FrostbiteFix_Init(HMODULE hMp)
{
    Log("\n=== FrostbiteFix_Init v13 === mp.dll=0x%08X\n", (uintptr_t)hMp);

    if (g_patched) { Log("[FB] already patched\n"); return true; }

    if (!GiveFnptrs_Init(hMp)) {
        Log("[FB] FATAL: GiveFnptrs_Init failed\n");
        return false;
    }

    g_mpBase = (uintptr_t)hMp;

    uintptr_t vtbl = g_mpBase + kRVA_Vtable;
    uintptr_t s0   = 0;
    if (!SafeRead(vtbl, s0)) {
        Log("[FB] FATAL: cannot read vtable @ 0x%08zX\n", vtbl);
        return false;
    }
    Log("[FB] vtable @ 0x%08zX slot[0]=0x%08zX\n", vtbl, s0);

    struct Slot {
        int         idx;
        const char* name;
        uintptr_t*  orig;
        void*       hook;
    } slots[] = {
        { SLOT_WEAPONIDLE,      "WeaponIdle",      &g_orig.WeaponIdle,      (void*)Hook_WeaponIdle      },
        { SLOT_THINK,           "Think",           &g_orig.Think,           (void*)Hook_Think           },
        { SLOT_PRIMARYATTACK,   "PrimaryAttack",   &g_orig.PrimaryAttack,   (void*)Hook_PrimaryAttack   },
        { SLOT_SECONDARYATTACK, "SecondaryAttack", &g_orig.SecondaryAttack, (void*)Hook_SecondaryAttack },
        { SLOT_ADDTOPLAYER,     "AddToPlayer",     &g_orig.AddToPlayer,     (void*)Hook_AddToPlayer     },
        { SLOT_HOLSTER,         "Holster",         &g_orig.Holster,         (void*)Hook_Holster         },
        { SLOT_TAKEDAMAGE,      "TakeDamage",      &g_orig.TakeDamage,      (void*)Hook_TakeDamage      },
        { SLOT_PROP_THINK,      "PropThink",       &g_orig.PropThink,       (void*)Hook_PropThink       },
    };

    int ok = 0, fail = 0;
    for (auto& s : slots)
    {
        uintptr_t slotAddr = vtbl + s.idx * 4;
        uintptr_t current  = 0;
        if (!SafeRead(slotAddr, current)) {
            Log("[FB]   Slot %3d %-16s: UNREADABLE\n", s.idx, s.name);
            fail++; continue;
        }

        *s.orig = current;  // save original

        Log("[FB]   Slot %3d %-16s: orig=0x%08zX hook=0x%08zX\n",
            s.idx, s.name, current, (uintptr_t)s.hook);

        if (SafeWrite(slotAddr, (uintptr_t)s.hook)) {
            Log("[FB]     -> patched OK\n");
            ok++;
        } else {
            Log("[FB]     -> FAILED\n");
            *s.orig = 0;
            fail++;
        }
    }

    Log("[FB] %d OK, %d failed\n", ok, fail);
    g_patched = true;
    Log("=== FrostbiteFix_Init COMPLETE ===\n");
    return fail == 0;
}
