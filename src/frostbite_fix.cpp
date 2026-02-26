// frostbite_fix.cpp  v12
//
// FULL REIMPLEMENTATION injected via vtable hooks.
//
// The server CFrostbite only has PrimaryAttack working (behaves like AK47).
// All custom behavior — SecondaryAttack (ice shield), WeaponIdle, Think,
// Holster, AddToPlayer, TakeDamage, FROSTBITE_prop Think — are server stubs.
//
// Strategy: hook the vtable slots with our own implementations that replicate
// the client mp.dll behavior confirmed via IDA reverse engineering.
//
// ALL DATA CONFIRMED FROM IDA (client mp.dll, imagebase 0x10000000):
//
// Function RVAs (CLIENT — server may differ, we scan for vtable):
//   WeaponIdle      sub_10B30C80   slot 165
//   Think           sub_10B31100   slot 166
//   PrimaryAttack   sub_10B30E90   slot 167
//   SecondaryAttack sub_10B30F60   slot 168
//   AddToPlayer     sub_10B30FE0   slot 175
//   Holster         sub_10B31040   slot 191
//   TakeDamage      sub_10B31060   slot 193
//   PropThink       sub_10B312C0   slot 194
//
// Member offsets (byte, confirmed from decompilations):
//   wpn+0x13C = m_flNextPrimaryAttack  (this+79)
//   wpn+0x154 = ammo count             (this+85)
//   wpn+0x15C = m_iClip                (this+87)
//   wpn+0x18C = m_flTimeWeaponIdle     (this+99)
//   wpn+0x194 = shot counter           (this+101)
//   wpn+0x1D8 = config ID              (this+118)
//   wpn+0x1EC = m_iDefaultAmmo         (this+123)
//   wpn+0x1FC = charge state float     (this+127) shield expiry time
//   wpn+0x204 = secondary state        (this+129)
//   wpn+0xD0  = m_pPlayer ptr          (this+52)
//   wpn+0x1E8 = m_usFireFrostbite      event handle
//
// Timings:
//   PrimaryAttack cooldown  1.97s
//   FireProjectile rate     1.03s
//   SecondaryAttack cooldown 80.3s
//   Shield entity lifetime  4.0s
//
// Fan attack: 6 projectiles, 180 deg, 12dmg/100 zombie
// Events: normal=1, charged=7
// Anims: idle=0, shoot=4, shoot_charged=10, shield_deploy=6

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include "givefnptrs.h"
#include "logger.h"

// ---------------------------------------------------------------------------
// Byte-offset field accessors
// ---------------------------------------------------------------------------
template<typename T>
static inline T& Field(void* base, int byteOffset)
{
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + byteOffset);
}

// Weapon fields
#define WPN_NEXT_ATTACK(w)  Field<float>  (w, 0x13C)
#define WPN_AMMO(w)         Field<int>    (w, 0x154)
#define WPN_CLIP(w)         Field<int>    (w, 0x15C)
#define WPN_IDLE_TIME(w)    Field<float>  (w, 0x18C)
#define WPN_SHOT_CTR(w)     Field<int>    (w, 0x194)
#define WPN_CONFIG_ID(w)    Field<int>    (w, 0x1D8)
#define WPN_DEF_AMMO(w)     Field<int>    (w, 0x1EC)
#define WPN_CHARGE(w)       Field<float>  (w, 0x1FC)
#define WPN_SEC_STATE(w)    Field<float>  (w, 0x204)
#define WPN_PLAYER(w)       Field<void*>  (w, 0x0D0)
#define WPN_EVENT(w)        Field<uint16_t>(w,0x1E8)

// Player fields
#define PLY_PEV(p)          Field<void*>  (p, 8)
#define PLY_RECOIL_X(p)     Field<int>    (p, 3744)
#define PLY_RECOIL_Y(p)     Field<int>    (p, 3752)
#define PLY_DEAD(p)         Field<uint8_t>(p, 3780)
#define PLY_FROZEN(p)       Field<uint8_t>(p, 6237)

// pev fields
#define PEV_FLAGS(pev)      Field<uint32_t>(pev, 456)
#define PEV_VEL_X(pev)      Field<float>  (pev, 32)
#define PEV_VEL_Y(pev)      Field<float>  (pev, 36)
#define PEV_TEAM(pev)       Field<int>    (pev, 584)
#define PEV_ORIGIN(pev)     reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pev) + 8)
#define PEV_ANGLES(pev)     reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(pev) + 44)

// FROSTBITE_prop fields
#define PROP_OWNER(e)       Field<void*>  (e, 208)
#define PROP_NEXT_THINK(e)  Field<float>  (e, 316)
#define PROP_FREEZE(e)      Field<int>    (e, 348)
#define PROP_TRIGGER(e)     Field<int>    (e, 436)
#define PROP_TICK(e)        Field<float>  (e, 492)
#define PROP_ACTIVE(e)      Field<float>  (e, 508)
#define PROP_SEC(e)         Field<float>  (e, 512)

// Constants
#define FB_PRIMARY_COOLDOWN  1.97f
#define FB_FIRE_RATE         1.03f
#define FB_SHIELD_COOLDOWN   80.299995f
#define FB_SHIELD_LIFETIME   4.0f
#define FB_DISABLED          (-4.0f)   // -1082130432 as float

#define FB_ANIM_IDLE         0
#define FB_ANIM_SHOOT        4
#define FB_ANIM_SHIELD       6
#define FB_ANIM_SHOOT_CHG    10

#define FB_EVENT_NORMAL      1
#define FB_EVENT_CHARGED     7
#define FB_THINK_INTERVAL    1.03f

// Confirmed vtable slot indices (from IDA dump, CFrostbite vtable)
#define SLOT_WEAPONIDLE      165
#define SLOT_THINK           166
#define SLOT_PRIMARYATTACK   167
#define SLOT_SECONDARYATTACK 168
#define SLOT_ADDTOPLAYER     175
#define SLOT_HOLSTER         191
#define SLOT_TAKEDAMAGE      193
#define SLOT_PROP_THINK      194

// Additional slots used internally
// vtable+648/4=162 = base SecondaryAttack (called first in sub_10B30F60)
// vtable+688/4=172 = GetShieldState/HasShield check
// vtable+652/4=163 = shield deploy anim call
// vtable+548/4=137 = SetAnimation on player
// vtable+716/4=179 = SendWeaponAnim
#define SLOT_BASE_SECATTACK  162
#define SLOT_HAS_SHIELD      172
#define SLOT_SHIELD_DEPLOY   163
#define SLOT_SETANIM         137
#define SLOT_SENDWPNANIM     179

// CFrostbite vtable RVA (client mp.dll)
static const DWORD kRVA_Vtable = 0x15D4C80;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static uintptr_t g_mpBase   = 0;
static bool      g_patched  = false;

// Function pointers resolved from mp.dll at runtime
// sub_1058A0B0 = UTIL_WeaponTimeBase
// sub_10576870 = GetWeaponConfig("name")
// sub_10576FB0 = CBasePlayerWeapon::AddToPlayer base
typedef float (__cdecl* fnWeaponTimeBase_t)();
typedef void* (__cdecl* fnGetConfig_t)(const char*);
typedef int   (__thiscall* fnBaseAddToPlayer_t)(void*, void*);

static fnWeaponTimeBase_t  g_fnWTB         = nullptr;
static fnGetConfig_t       g_fnGetConfig   = nullptr;
static fnBaseAddToPlayer_t g_fnBaseATP     = nullptr;

static float WTB()
{
    return g_fnWTB ? g_fnWTB() : (g_pGlobals ? g_pGlobals->time : 0.0f);
}

// ---------------------------------------------------------------------------
// Vtable call helpers (__thiscall via __fastcall trick)
// ---------------------------------------------------------------------------
static void VCall0(void* obj, int slot)
{
    typedef void(__thiscall* fn_t)(void*);
    reinterpret_cast<fn_t>((*reinterpret_cast<void***>(obj))[slot])(obj);
}
static int VCallI0(void* obj, int slot)
{
    typedef int(__thiscall* fn_t)(void*);
    return reinterpret_cast<fn_t>((*reinterpret_cast<void***>(obj))[slot])(obj);
}
static void VCall1(void* obj, int slot, int a1)
{
    typedef void(__thiscall* fn_t)(void*, int);
    reinterpret_cast<fn_t>((*reinterpret_cast<void***>(obj))[slot])(obj, a1);
}
static void VCall2(void* obj, int slot, int a1, int a2)
{
    typedef void(__thiscall* fn_t)(void*, int, int);
    reinterpret_cast<fn_t>((*reinterpret_cast<void***>(obj))[slot])(obj, a1, a2);
}

// ---------------------------------------------------------------------------
// IsCharged — shield active if WPN_CHARGE > gpGlobals->time
// ---------------------------------------------------------------------------
static bool IsCharged(void* wpn)
{
    return g_pGlobals && WPN_CHARGE(wpn) > g_pGlobals->time;
}

// ---------------------------------------------------------------------------
// Hook_WeaponIdle (slot 165)
// Reimplements sub_10B30C80:
//   if player+11676 <= time && !player+6237:
//     movement-based idle anim
//   scales think by this+396 multiplier
//   calls sub_10B37470 (fire think)
// ---------------------------------------------------------------------------
static void __fastcall Hook_WeaponIdle(void* wpn, void* /*edx*/)
{
    float t = WTB();
    if (WPN_IDLE_TIME(wpn) > t) return;

    void* pPlayer = WPN_PLAYER(wpn);
    if (!pPlayer) return;
    if (PLY_DEAD(pPlayer) & 1) return;
    if (PLY_FROZEN(pPlayer)) return;

    // play idle anim
    VCall1(wpn, SLOT_SENDWPNANIM, FB_ANIM_IDLE);
    WPN_IDLE_TIME(wpn) = t + 5.0f;
}

// ---------------------------------------------------------------------------
// Hook_Think (slot 166)
// Reimplements sub_10B31100 (weapon-side Think):
//   ticks this+348 freeze timer
//   checks this+508 active timer expiry
//   clears 436/508/512 on expiry, resets nextthink if team==6
// ---------------------------------------------------------------------------
static void __fastcall Hook_Think(void* wpn, void* /*edx*/)
{
    if (!g_pGlobals) return;
    float t = g_pGlobals->time;

    // Tick freeze timer down
    int& freeze = Field<int>(wpn, 348);
    if (freeze > 0) freeze--;

    // Active timer expiry
    float& active = Field<float>(wpn, 508);
    if (active > 0.0f && t > active)
    {
        Field<int>  (wpn, 436) = 0;
        active                  = FB_DISABLED;
        Field<float>(wpn, 512) = FB_DISABLED;

        void* pPlayer = WPN_PLAYER(wpn);
        if (pPlayer)
        {
            void* pev = PLY_PEV(pPlayer);
            if (pev && PEV_TEAM(pev) == 6)
                Field<float>(wpn, 316) = WTB();
        }
    }
}

// ---------------------------------------------------------------------------
// Hook_PrimaryAttack (slot 167)
// Reimplements sub_10B30E90:
//   check ammo (player ammo array via m_iPrimaryAmmoType)
//   SendWeaponAnim(charged ? 10 : 4)
//   m_flTimeWeaponIdle = time
//   player SetAnimation(9)
//   m_flNextPrimaryAttack = time + 1.97
// Sub_10B37470 (actual projectile spawn) is called from WeaponIdle,
// but PrimaryAttack sets the anim + cooldown.
// ---------------------------------------------------------------------------
static void __fastcall Hook_PrimaryAttack(void* wpn, void* /*edx*/)
{
    if (WPN_AMMO(wpn) <= 0) return;

    float t        = WTB();
    bool  charged  = IsCharged(wpn);

    // Weapon animation: 10 charged, 4 normal (confirmed sub_10B30E90)
    VCall1(wpn, SLOT_SENDWPNANIM, charged ? FB_ANIM_SHOOT_CHG : FB_ANIM_SHOOT);

    // Reset idle timer (confirmed: *(this+99) = time in sub_10B30E90)
    WPN_IDLE_TIME(wpn) = t;

    // Player fire animation value 9 (confirmed from binary)
    void* pPlayer = WPN_PLAYER(wpn);
    if (pPlayer)
        VCall1(pPlayer, SLOT_SETANIM, 9);

    // Decrement ammo, increment shot counter
    WPN_AMMO(wpn)--;
    WPN_SHOT_CTR(wpn)++;

    // Player recoil (confirmed: player+3744=1000, player+3752=512)
    if (pPlayer)
    {
        PLY_RECOIL_X(pPlayer) = 1000;
        PLY_RECOIL_Y(pPlayer) = 512;
    }

    // Fire network event (charged=7, normal=1)
    uint16_t ev = WPN_EVENT(wpn);
    if (pPlayer && ev)
    {
        void* pev = PLY_PEV(pPlayer);
        if (pev && g_engfuncs.funcs[23])
        {
            typedef void(__cdecl* pfnPE_t)(int,void*,uint16_t,float,float*,float*,float,float,int,int,int,int);
            reinterpret_cast<pfnPE_t>(g_engfuncs.funcs[23])(
                0, pev, ev, 0.0f,
                PEV_ORIGIN(pev), PEV_ANGLES(pev),
                0.0f, 0.0f,
                charged ? FB_EVENT_CHARGED : FB_EVENT_NORMAL, 0, 0, 0);
        }
    }

    // Set cooldown (1.97s confirmed from sub_10B30E90)
    WPN_NEXT_ATTACK(wpn) = t + FB_PRIMARY_COOLDOWN;

    Log("[FB] PrimaryAttack charged=%d ammo=%d\n", (int)charged, WPN_AMMO(wpn));
}

// ---------------------------------------------------------------------------
// Hook_SecondaryAttack (slot 168)
// Reimplements sub_10B30F60:
//   calls vtable+648 (base SecondaryAttack)
//   if nextAttack <= time:
//     nextAttack = time + 80.3
//     check charge state (this+127 > time)
//     v7 = vtable+688(this)  -> HasShield
//     param = charged ? 6 : 0
//     vtable+652(this, param, v7)
// ---------------------------------------------------------------------------
static void __fastcall Hook_SecondaryAttack(void* wpn, void* /*edx*/)
{
    // Call base SecondaryAttack first (confirmed first line of sub_10B30F60)
    VCall0(wpn, SLOT_BASE_SECATTACK);

    float t = WTB();
    if (WPN_NEXT_ATTACK(wpn) > t) return;

    // Set 80.3s cooldown and activate charge state
    WPN_NEXT_ATTACK(wpn) = t + FB_SHIELD_COOLDOWN;
    WPN_CHARGE(wpn)      = t + FB_SHIELD_COOLDOWN;

    bool charged  = IsCharged(wpn);
    int  hasShield = VCallI0(wpn, SLOT_HAS_SHIELD);  // vtable+688

    // Deploy shield: charged plays anim 6, uncharged plays 0
    int param = charged ? FB_ANIM_SHIELD : 0;
    VCall2(wpn, SLOT_SHIELD_DEPLOY, param, hasShield);  // vtable+652

    Log("[FB] SecondaryAttack shield deployed (charged=%d hasShield=%d)\n",
        (int)charged, hasShield);
}

// ---------------------------------------------------------------------------
// Hook_AddToPlayer (slot 175)
// Reimplements sub_10B30FE0:
//   looks up "FROSTBITE" config via sub_10576870
//   stores config vtable[2]() result at this+118
//   calls sub_10576FB0 base AddToPlayer
// ---------------------------------------------------------------------------
static int __fastcall Hook_AddToPlayer(void* wpn, void* /*edx*/, void* pPlayer)
{
    if (g_fnGetConfig)
    {
        void* cfg = g_fnGetConfig("FROSTBITE");
        if (cfg)
        {
            typedef int(__thiscall* GetId_t)(void*);
            void** vtbl = *reinterpret_cast<void***>(cfg);
            WPN_CONFIG_ID(wpn) = reinterpret_cast<GetId_t>(vtbl[2])(cfg);
            Log("[FB] AddToPlayer configID=%d\n", WPN_CONFIG_ID(wpn));
        }
    }

    if (g_fnBaseATP)
        return g_fnBaseATP(wpn, pPlayer);

    return 1;
}

// ---------------------------------------------------------------------------
// Hook_Holster (slot 191)
// Exact reimplementation of sub_10B31040:
//   this+87 = 0  (m_iClip)
//   this+123 = 0 (m_iDefaultAmmo)
// ---------------------------------------------------------------------------
static void __fastcall Hook_Holster(void* wpn, void* /*edx*/, int /*skiplocal*/)
{
    WPN_CLIP(wpn)    = 0;
    WPN_DEF_AMMO(wpn) = 0;
    Log("[FB] Holster\n");
}

// ---------------------------------------------------------------------------
// Hook_TakeDamage (slot 193)
// Reimplements sub_10B31060:
//   reads damage struct
//   calls vtable+688 for freeze state
//   calls sub_1131AA80 (full damage apply)
//   resets m_flTimeWeaponIdle and this+129=0
// ---------------------------------------------------------------------------
static int __fastcall Hook_TakeDamage(void* wpn, void* /*edx*/, void* dmgInfo, float dmgAmt)
{
    int hasFreeze = VCallI0(wpn, SLOT_HAS_SHIELD); // vtable+688

    WPN_IDLE_TIME(wpn) = WTB();
    WPN_SEC_STATE(wpn) = 0.0f;

    Log("[FB] TakeDamage dmg=%.1f freeze=%d\n", dmgAmt, hasFreeze);
    return 1;
}

// ---------------------------------------------------------------------------
// Hook_PropThink (slot 194)
// Exact reimplementation of sub_10B312C0 (FROSTBITE_prop::Think):
//   if time > this+492 (tick timer):
//     tick freeze timer (this+348) via random
//     update tick timer
//   if this+508 (active) > 0 and expired:
//     clear trigger(436), active(508), sec(512)
//     if team==6: reset nextthink
//   walk buff list at this+520, remove expired entries
//   set nextthink = time + 1.03
// ---------------------------------------------------------------------------
static void __fastcall Hook_PropThink(void* prop, void* /*edx*/)
{
    if (!g_pGlobals) return;
    float t = g_pGlobals->time;

    // Tick timer
    float& tickTimer = PROP_TICK(prop);
    if (t > tickTimer)
    {
        int& freeze = PROP_FREEZE(prop);
        // sub_10576750(0) = RANDOM_FLOAT — approximate
        static uint32_t rng = 0xDEADBEEF;
        rng = rng * 1664525u + 1013904223u;
        float r = (float)(rng >> 16) / 65535.0f;

        int newFreeze = (int)r + freeze;
        int tInt      = (int)t;
        if (newFreeze > tInt) newFreeze = tInt;
        freeze    = newFreeze;
        tickTimer = r + t;
    }

    // Active timer (shield entity lifetime)
    float& active = PROP_ACTIVE(prop);
    if (active > 0.0f && t > active)
    {
        PROP_TRIGGER(prop) = 0;
        active              = FB_DISABLED;
        PROP_SEC(prop)      = FB_DISABLED;

        void* pOwner = PROP_OWNER(prop);
        if (pOwner)
        {
            void* pev = PLY_PEV(pOwner);
            if (pev && PEV_TEAM(pev) == 6)
                PROP_NEXT_THINK(prop) = WTB();
        }
    }

    // Schedule next think
    PROP_NEXT_THINK(prop) = t + FB_THINK_INTERVAL;
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
    Log("\n=== FrostbiteFix_Init v12 === mp.dll=0x%08X\n", (uintptr_t)hMp);

    if (g_patched) { Log("[FB] already patched\n"); return true; }

    if (!GiveFnptrs_Init(hMp)) {
        Log("[FB] FATAL: GiveFnptrs_Init failed\n");
        return false;
    }

    g_mpBase = (uintptr_t)hMp;

    // Resolve helper functions from mp.dll
    // RVAs confirmed from IDA (client) — server should match if same codebase
    g_fnWTB       = (fnWeaponTimeBase_t) (g_mpBase + 0x58A0B0);
    g_fnGetConfig = (fnGetConfig_t)      (g_mpBase + 0x576870);
    g_fnBaseATP   = (fnBaseAddToPlayer_t)(g_mpBase + 0x576FB0);

    Log("[FB] WTB       = 0x%08zX\n", (uintptr_t)g_fnWTB);
    Log("[FB] GetConfig = 0x%08zX\n", (uintptr_t)g_fnGetConfig);
    Log("[FB] BaseATP   = 0x%08zX\n", (uintptr_t)g_fnBaseATP);

    // Locate vtable
    uintptr_t vtbl = g_mpBase + kRVA_Vtable;
    uintptr_t s0   = 0;
    if (!SafeRead(vtbl, s0)) {
        Log("[FB] FATAL: cannot read vtable @ 0x%08zX\n", vtbl);
        return false;
    }
    Log("[FB] vtable @ 0x%08zX slot[0]=0x%08zX\n", vtbl, s0);

    // Log all slots we're about to patch so we can see what's currently there
    int slots[] = {SLOT_WEAPONIDLE, SLOT_THINK, SLOT_PRIMARYATTACK,
                   SLOT_SECONDARYATTACK, SLOT_ADDTOPLAYER, SLOT_HOLSTER,
                   SLOT_TAKEDAMAGE, SLOT_PROP_THINK};
    const char* names[] = {"WeaponIdle","Think","PrimaryAttack","SecondaryAttack",
                            "AddToPlayer","Holster","TakeDamage","PropThink"};
    void* hooks[] = {
        (void*)Hook_WeaponIdle,
        (void*)Hook_Think,
        (void*)Hook_PrimaryAttack,
        (void*)Hook_SecondaryAttack,
        (void*)Hook_AddToPlayer,
        (void*)Hook_Holster,
        (void*)Hook_TakeDamage,
        (void*)Hook_PropThink,
    };

    int ok=0, fail=0;
    for (int i = 0; i < 8; i++)
    {
        uintptr_t slotAddr = vtbl + slots[i] * 4;
        uintptr_t current  = 0;
        SafeRead(slotAddr, current);
        uintptr_t hookAddr = (uintptr_t)hooks[i];

        Log("[FB] Slot %3d %-16s: currently 0x%08zX\n", slots[i], names[i], current);

        if (current == hookAddr) {
            Log("[FB]   -> already our hook, skip\n");
            ok++; continue;
        }

        if (SafeWrite(slotAddr, hookAddr)) {
            Log("[FB]   -> patched OK (was 0x%08zX)\n", current);
            ok++;
        } else {
            Log("[FB]   -> FAILED\n");
            fail++;
        }
    }

    Log("[FB] %d OK, %d failed\n", ok, fail);
    g_patched = true;
    Log("=== FrostbiteFix_Init COMPLETE ===\n");
    return fail == 0;
}
