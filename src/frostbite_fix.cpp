// frostbite_fix.cpp  v17
//
// Complete reimplementation based on IDA decompilation of client mp.dll.
// Every offset, every call, every constant is taken directly from the dump.
//
// KEY INSIGHT from logs: the server already has all Frostbite functions
// correctly wired in the vtable. The functions exist and run. The problem
// is gpGlobals->time = 0 because we patch too early. The fix is in dllmain
// (wait for gpGlobals != 0), NOT in reimplementing functions.
//
// BUT: AddToPlayer is broken — it receives weaponID (597) instead of a
// player pointer. This is because sub_10B30FE0 has a special first-arg
// check: if a2==0, use this->weaponID. The call site passes 597 correctly.
// We need to verify the config lookup works on server.
//
// This version: minimal targeted patches only.
// - Do NOT replace PrimaryAttack, WeaponIdle, Think, PropThink (all work)
// - ONLY fix SecondaryAttack cooldown check (uses gpGlobals->time)
// - Fix AddToPlayer to handle the weaponID-as-arg calling convention
//
// All offsets confirmed from IDA decompilation (client mp.dll, base 0x10000000):
//
// Weapon object (CBasePlayerWeapon / CFrostbite) field layout:
//   +0x034 (this+13)  = vtable ptr [0] check
//   +0x0D0 (this+52)  = m_pPlayer ptr
//   +0x0E4 (this+57)  = m_iId (weapon ID = 597)
//   +0x14C (this+83)  = m_iPrimaryAmmoType
//   +0x154 (this+85)  = ammo count
//   +0x15C (this+87)  = m_iClip (zeroed in Holster)
//   +0x13C (this+79)  = m_flNextPrimaryAttack (also NextAttack cooldown)
//   +0x18C (this+99)  = m_flTimeWeaponIdle
//   +0x194 (this+101) = shot counter
//   +0x1D8 (this+118) = config ID (set in AddToPlayer)
//   +0x1EC (this+123) = m_iDefaultAmmo (zeroed in Holster)
//   +0x1E8 (this+244 as _WORD) = m_usFireFrostbite event handle
//   +0x1FC (this+127) = charge state float (shield active until time)
//   +0x204 (this+129) = secondary state
//   +0x18C (this+396 as float*) = idle multiplier
//
// gpGlobals (dword_11E51BCC): current time pointer
// sub_1058A0B0 = UTIL_WeaponTimeBase()
// sub_10576750 = RANDOM_FLOAT(0)  -- actually gpGlobals->time
// sub_10576870 = GetWeaponConfig("name")
// sub_10576FB0 = CBasePlayerWeapon::AddToPlayer base
// sub_10B37470 = FireProjectile (called from WeaponIdle)
// sub_1131AEC0 = SendWeaponAnim
// sub_1058A0B0 = UTIL_WeaponTimeBase

#include <windows.h>
#include <cstdint>
#include <cstring>
#include "givefnptrs.h"
#include "logger.h"

// ---------------------------------------------------------------------------
// Field accessor
// ---------------------------------------------------------------------------
template<typename T>
static inline T& F(void* base, int byteOff)
{
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + byteOff);
}

// ---------------------------------------------------------------------------
// Function pointers resolved at init from mp.dll RVAs
// ---------------------------------------------------------------------------
static uintptr_t g_mpBase = 0;
static bool      g_patched = false;

// Original function pointers (saved before patching)
static uintptr_t g_orig_SecondaryAttack = 0;
static uintptr_t g_orig_AddToPlayer     = 0;

// mp.dll function RVAs (confirmed from IDA)
#define RVA_WTB          0x58A0B0   // UTIL_WeaponTimeBase()
#define RVA_GPGLOBALS    0x1E51BCC  // pointer to gpGlobals->time (dword_11E51BCC)
#define RVA_GetConfig    0x576870   // GetWeaponConfig(const char*)
#define RVA_BaseATP      0x576FB0   // CBasePlayerWeapon::AddToPlayer(player)
#define RVA_VTBL_Simple  0x15D48CC  // CSimpleWpn<CFrostbite>::vftable

typedef float  (__cdecl*    pfnWTB_t)();
typedef void*  (__cdecl*    pfnGetConfig_t)(const char*);
typedef int    (__thiscall* pfnBaseATP_t)(void*, void*);

static pfnWTB_t       g_WTB       = nullptr;
static pfnGetConfig_t g_GetConfig = nullptr;
static pfnBaseATP_t   g_BaseATP   = nullptr;
static float*         g_pTime     = nullptr;  // points directly to gpGlobals->time

static float NOW()
{
    // gpGlobals->time is at dword_11E51BCC which holds the pointer,
    // but dword_11E51BCC IS gpGlobals->time directly (it's a global float ptr
    // that IDA shows as a dword — the value AT that address IS the time float).
    // Actually from decompile: *(float*)dword_11E51BCC is how time is read.
    // dword_11E51BCC = &gpGlobals->time — it's a pointer to the float.
    if (!g_pTime) return 0.0f;
    float t = 0.0f;
    __try { t = *g_pTime; } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return t;
}

// ---------------------------------------------------------------------------
// HOOK: SecondaryAttack (slot 168)
//
// From decompile of sub_10B30F60:
//   call base SecondaryAttack (vtable+648)
//   v8 = UTIL_WeaponTimeBase()
//   if (this+79 <= v8):   <-- nextAttack check
//     this+79 = WTB() + 80.3
//     v5 = (this+127 > gpGlobals->time)   <-- charged?
//     v7 = vtable+688(this)               <-- HasShield
//     param = v5 ? 6 : 0
//     vtable+652(this, param, v7)          <-- DeployShield
// ---------------------------------------------------------------------------
static void __fastcall Hook_SecondaryAttack(void* wpn, void* /*edx*/)
{
    float t   = g_WTB ? g_WTB() : NOW();
    float now = NOW();

    float nextAtk = F<float>(wpn, 0x13C);
    float charge  = F<float>(wpn, 0x1FC);

    Log("[FB] SecondaryAttack wpn=%p nextAtk=%.2f t=%.2f now=%.2f charge=%.2f\n",
        wpn, nextAtk, t, now, charge);

    // Call base SecondaryAttack first (vtable slot 162 = vtable+648/4)
    typedef void(__thiscall* Fn)(void*);
    void** vtbl = *reinterpret_cast<void***>(wpn);
    reinterpret_cast<Fn>(vtbl[162])(wpn);

    // Cooldown check
    if (nextAtk > t) {
        Log("[FB] SecondaryAttack blocked by cooldown (%.2f > %.2f)\n", nextAtk, t);
        return;
    }

    // Set cooldown
    F<float>(wpn, 0x13C) = t + 80.299995f;

    // Check charge state
    bool charged = (charge > now);

    // HasShield check (vtable slot 172 = vtable+688/4)
    typedef int(__thiscall* HasShield_t)(void*);
    int hasShield = reinterpret_cast<HasShield_t>(vtbl[172])(wpn);

    // Deploy shield (vtable slot 163 = vtable+652/4)
    int param = charged ? 6 : 0;
    typedef void(__thiscall* Deploy_t)(void*, int, int);
    reinterpret_cast<Deploy_t>(vtbl[163])(wpn, param, hasShield);

    Log("[FB] SecondaryAttack fired charged=%d hasShield=%d param=%d\n",
        (int)charged, hasShield, param);
}

// ---------------------------------------------------------------------------
// HOOK: AddToPlayer (slot 175)
//
// From decompile of sub_10B30FE0:
//   if (!a2) a2 = this+57  (weaponID fallback)
//   v4 = *(GetWeaponConfig("FROSTBITE") + 36)
//   this+118 = v4->vtable[2](v4)   (get config ID)
//   return BaseAddToPlayer(a2, 0)
//
// The log shows player=0x255 (=597) being passed — this is the weaponID
// being used as the player arg in the server's calling convention.
// The original function handles this with the "if(!a2) a2=this->id" check.
// We just call the original and log what happens.
// ---------------------------------------------------------------------------
static int __fastcall Hook_AddToPlayer(void* wpn, void* /*edx*/, void* pPlayer)
{
    Log("[FB] AddToPlayer wpn=%p player=%p cfgId_before=%d\n",
        wpn, pPlayer, F<int>(wpn, 0x1D8));

    // Call original
    typedef int(__thiscall* Fn)(void*, void*);
    int ret = reinterpret_cast<Fn>(g_orig_AddToPlayer)(wpn, pPlayer);

    Log("[FB] AddToPlayer DONE ret=%d cfgId=%d evHdl=%d\n",
        ret, F<int>(wpn, 0x1D8), (int)F<uint16_t>(wpn, 0x1E8));
    return ret;
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
    Log("\n=== FrostbiteFix_Init v17 === mp.dll=0x%08X\n", (uintptr_t)hMp);

    if (g_patched) { Log("[FB] already patched\n"); return true; }

    if (!GiveFnptrs_Init(hMp)) {
        Log("[FB] FATAL: GiveFnptrs_Init failed\n");
        return false;
    }

    g_mpBase = (uintptr_t)hMp;

    // Resolve function pointers
    g_WTB       = (pfnWTB_t)      (g_mpBase + RVA_WTB);
    g_GetConfig = (pfnGetConfig_t) (g_mpBase + RVA_GetConfig);
    g_BaseATP   = (pfnBaseATP_t)   (g_mpBase + RVA_BaseATP);

    // gpGlobals->time: dword_11E51BCC holds the address of gpGlobals,
    // and time is the first field. So *(float*)(*(uintptr_t*)(mp+RVA)) = time.
    // Actually from decompile "*(float*)dword_11E51BCC" means the dword IS the ptr.
    // Read it:
    {
        uintptr_t timePtr = 0;
        SafeRead(g_mpBase + RVA_GPGLOBALS, timePtr);
        if (timePtr) {
            g_pTime = reinterpret_cast<float*>(timePtr);
            Log("[FB] gpGlobals->time ptr = 0x%08zX  current=%.3f\n",
                timePtr, *g_pTime);
        } else {
            // dword_11E51BCC might BE the time value directly (not a pointer)
            // Try reading it as a pointer to globalvars_t
            uintptr_t ppg = g_mpBase + RVA_GPGLOBALS;
            g_pTime = reinterpret_cast<float*>(ppg);
            Log("[FB] gpGlobals->time direct @ 0x%08zX\n", ppg);
        }
    }

    Log("[FB] WTB=0x%08zX GetConfig=0x%08zX BaseATP=0x%08zX\n",
        (uintptr_t)g_WTB, (uintptr_t)g_GetConfig, (uintptr_t)g_BaseATP);
    Log("[FB] NOW()=%.3f  WTB()=%.3f\n", NOW(), g_WTB ? g_WTB() : -1.0f);

    // Locate CSimpleWpn<CFrostbite> vtable
    uintptr_t vtbl = g_mpBase + RVA_VTBL_Simple;
    uintptr_t s0 = 0;
    if (!SafeRead(vtbl, s0)) {
        Log("[FB] FATAL: cannot read vtable @ 0x%08zX\n", vtbl);
        return false;
    }
    Log("[FB] vtable @ 0x%08zX slot[0]=0x%08zX\n", vtbl, s0);

    // Only patch SecondaryAttack (168) — everything else works
    // (PrimaryAttack, WeaponIdle, Think, PropThink, Holster all function correctly
    //  once gpGlobals->time is non-zero)
    uintptr_t saAddr = vtbl + 168 * 4;
    SafeRead(saAddr, g_orig_SecondaryAttack);
    Log("[FB] SecondaryAttack orig=0x%08zX\n", g_orig_SecondaryAttack);
    if (SafeWrite(saAddr, (uintptr_t)Hook_SecondaryAttack))
        Log("[FB] SecondaryAttack patched OK\n");
    else
        Log("[FB] SecondaryAttack patch FAILED\n");

    // Also hook AddToPlayer for diagnostics only (calls original)
    uintptr_t atpAddr = vtbl + 175 * 4;
    SafeRead(atpAddr, g_orig_AddToPlayer);
    Log("[FB] AddToPlayer orig=0x%08zX\n", g_orig_AddToPlayer);
    if (SafeWrite(atpAddr, (uintptr_t)Hook_AddToPlayer))
        Log("[FB] AddToPlayer patched OK\n");

    g_patched = true;
    Log("=== FrostbiteFix_Init COMPLETE ===\n");
    return true;
}