// janus1.cpp — CJanus1 grenade launcher implementation
// Follows CHEGrenade pattern from ReGameDLL exactly.
//
// HOW IT WORKS:
// 1. Janus1_Factory is registered to replace the weapon_janus1 entry point.
// 2. When the engine spawns weapon_janus1, our factory runs.
// 3. We call the ORIGINAL factory (which allocates the object properly).
// 4. We find the allocated object via edict->pvPrivateData.
// 5. We swap its vtable to CJanus1's vtable.
// 6. All virtual calls now go through our CJanus1 methods.
// 7. For PrimaryAttack/Reload we delegate to M79's vtable (which works).
//
// WHY M79 DELEGATION WORKS:
// CJanus1 uses the SAME model, the SAME ammo, the SAME projectile as M79.
// M79's PrimaryAttack and Reload are confirmed working on this server.
// So we just let them handle firing/reloading, and override everything else.

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include "../hlsdk/mp_offsets.h"
#include <new>
#include <windows.h>

// -----------------------------------------------------------------------
// Engine function pointers needed by PRECACHE_* macros (defined in sdk.h).
// g_pTime and g_pGlobals are already defined in hooks.cpp — we just use them.
// We define the individual cached fn pointers here; PostInit fills them.
// -----------------------------------------------------------------------
pfnPrecacheModel_t  g_fnPrecacheModel = nullptr;
pfnPrecacheSound_t  g_fnPrecacheSound = nullptr;
pfnSetModel_t       g_fnSetModel      = nullptr;
pfnPrecacheEvent_t  g_fnPrecacheEvent = nullptr;
// g_pTime is defined in hooks.cpp (extern in sdk.h) — do NOT redefine here.
// g_pGlobals is a janus1-local variable:
static float* s_pGlobalsBase = nullptr;  // base of globalvars_t struct

// Static member definitions (required by linker)
ItemInfo CBasePlayerItem::m_ItemInfoArray[32] = {};
void*    CBasePlayerItem::m_AmmoInfoArray     = nullptr;
void*    CBasePlayerItem::m_SaveData          = nullptr;
void*    CBasePlayerWeapon::m_SaveData        = nullptr;

// -----------------------------------------------------------------------
// M79 vtable delegation helpers
// -----------------------------------------------------------------------
static void** g_m79Vtable = nullptr; // CM79 vtable pointer array

// Call a slot on M79's vtable with 'this' = our object
// This works because CJanus1 and CM79 have compatible layouts
// (both are CBasePlayerWeapon subclasses with the same field structure)
template<typename RetT, typename... Args>
static RetT M79_Call(int slot, void* self, Args... args)
{
    if (!g_m79Vtable || slot >= 220) {
        if constexpr (!std::is_void<RetT>::value) return RetT{};
        else return;
    }
    typedef RetT(__thiscall* Fn)(void*, Args...);
    return reinterpret_cast<Fn>(g_m79Vtable[slot])(self, args...);
}

// -----------------------------------------------------------------------
// CJanus1 method implementations
// -----------------------------------------------------------------------

void CJanus1::Spawn()
{
    Precache();

    m_iId = WEAPON_JANUS1;
    SET_MODEL(edict(), "models/w_janus1.mdl");

    m_iDefaultAmmo = JANUS1_DEFAULT_GIVE;
    m_iClip        = JANUS1_MAX_CLIP;

    // FallInit — let weapon fall to ground when dropped
    // Delegate to M79's Spawn which calls FallInit
    // (M79 slot 0 = Spawn, confirmed from vtable dump)
    // Actually we implement FallInit ourselves since Spawn would recurse
    if (pev) {
        pev->movetype = 6;  // MOVETYPE_TOSS
        pev->solid    = 1;  // SOLID_TRIGGER
    }

    Log("[janus1] Spawn done, id=%d clip=%d\n", m_iId, m_iClip);
}

void CJanus1::Precache()
{
    Log("[janus1] Precache\n");

    PRECACHE_MODEL("models/v_janus1.mdl");
    PRECACHE_MODEL("models/w_janus1.mdl");
    PRECACHE_SOUND("weapons/janus1-1.wav");
    PRECACHE_SOUND("weapons/janus1-2.wav");
    PRECACHE_SOUND("weapons/janus1_reload.wav");

    // PRECACHE_EVENT returns a handle we store for PLAYBACK_EVENT_FULL
    m_usFireEvent = PRECACHE_EVENT(1, "events/janus1.sc");

    Log("[janus1] Precache done, evHdl=%d\n", (int)m_usFireEvent);
}

int CJanus1::GetItemInfo(ItemInfo* p)
{
    p->pszName   = "weapon_janus1";
    p->pszAmmo1  = "m79_rocket";   // same ammo type as M79
    p->iMaxAmmo1 = 5;
    p->pszAmmo2  = nullptr;
    p->iMaxAmmo2 = -1;
    p->iMaxClip  = JANUS1_MAX_CLIP;
    p->iSlot     = 3;
    p->iPosition = 6;
    p->iId       = m_iId = WEAPON_JANUS1;
    p->iFlags    = 0;
    p->iWeight   = JANUS1_WEIGHT;
    return 1;
}

BOOL CJanus1::AddToPlayer(CBasePlayer* pPlayer)
{
    Log("[janus1] AddToPlayer pPlayer=%p\n", (void*)pPlayer);
    m_pPlayer = pPlayer;
    // Call M79's AddToPlayer which does the full weapon registration:
    //   pPlayer->pev->weapons |= (1 << m_iId)  — sets weapon bit
    //   sends gmsgWeapPickup to client HUD
    // We pass 'this' as self — M79 will treat it as CM79, layout is compatible
    if (g_m79Vtable) {
        typedef BOOL(__thiscall* Fn)(void*, CBasePlayer*);
        return reinterpret_cast<Fn>(g_m79Vtable[VTBL_AddToPlayer])(this, pPlayer);
    }
    return TRUE;
}

BOOL CJanus1::Deploy()
{
    Log("[janus1] Deploy\n");
    m_fMaxSpeed = 250.0f;

    // Delegate to M79's Deploy — sets view model to v_m79.mdl, etc.
    // Since Janus-1 uses same model as M79, this is correct behavior.
    if (g_m79Vtable) {
        typedef BOOL(__thiscall* Fn)(void*);
        return reinterpret_cast<Fn>(g_m79Vtable[VTBL_Deploy])(this);
    }
    return TRUE;
}

void CJanus1::Holster(int skiplocal)
{
    Log("[janus1] Holster\n");
    m_fInReload = FALSE;

    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*, int);
        reinterpret_cast<Fn>(g_m79Vtable[VTBL_Holster])(this, skiplocal);
        return;
    }

    // Fallback: nothing (M79 vtable should always be available)
}

void CJanus1::PrimaryAttack()
{
    // Fully delegate to M79's PrimaryAttack — it fires the grenade projectile
    // M79's PrimaryAttack checks m_iClip, fires CGrenade projectile,
    // plays event, sets m_flNextPrimaryAttack, does animation — all correct.
    Log("[janus1] PrimaryAttack t=%.3f clip=%d\n", SafeTime(), m_iClip);

    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[VTBL_PrimaryAttack])(this);
        return;
    }

    // Fallback: empty click
    if (m_iClip <= 0) {
        PlayEmptySound();
        m_flNextPrimaryAttack = SafeTime() + 0.15f;
    }
}

void CJanus1::SecondaryAttack()
{
    // Janus-1 has no secondary attack in this version
    m_flNextSecondaryAttack = SafeTime() + 0.5f;
}

void CJanus1::WeaponIdle()
{
    if (m_flTimeWeaponIdle > SafeTime())
        return;

    if (m_iClip) {
        SendWeaponAnim(JANUS1_IDLE);
        m_flTimeWeaponIdle = SafeTime() + 20.0f;
    }
}

void CJanus1::Reload()
{
    Log("[janus1] Reload clip=%d\n", m_iClip);

    // Delegate to M79's Reload — handles animation, timing, ammo refill
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[VTBL_Reload])(this);
    }
}

// -----------------------------------------------------------------------
// Factory mechanism
// -----------------------------------------------------------------------

static uint8_t g_savedBytes[5]  = { 0x90,0x90,0x90,0x90,0x90 };
static bool    g_factoryReady   = false;

// Static buffer + vtable capture
// We do a placement-new into a static buffer so the compiler generates our
// vtable and we can capture the vtable pointer.
alignas(16) static char g_vtableBuf[sizeof(CJanus1)];
static void*            g_ourVtable = nullptr;

static void InitVtable()
{
    memset(g_vtableBuf, 0, sizeof(g_vtableBuf));
    // Placement-new: constructs CJanus1 in static memory, sets up vtable ptr
    CJanus1* dummy = new(g_vtableBuf) CJanus1();
    (void)dummy;
    g_ourVtable = *reinterpret_cast<void**>(g_vtableBuf);
    Log("[janus1] CJanus1 vtable captured @ %p\n", g_ourVtable);
}

// Swap vtable on a game-allocated object
static void SwapVtable(void* obj)
{
    if (!obj || !g_ourVtable) return;
    DWORD old = 0;
    if (VirtualProtect(obj, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) {
        void* prev = *reinterpret_cast<void**>(obj);
        *reinterpret_cast<void**>(obj) = g_ourVtable;
        VirtualProtect(obj, sizeof(void*), old, &old);
        Log("[janus1] Vtable swapped %p -> %p (CJanus1)\n", prev, g_ourVtable);
    } else {
        Log("[janus1] VirtualProtect failed on obj=%p\n", obj);
    }
}

typedef void(__cdecl* pfnFactory_t)(entvars_t*);

// Call original factory (temporarily restore bytes), then re-hook
static void CallOrigFactory(entvars_t* pev)
{
    uintptr_t fnAddr = GetMpBase() + RVA_weapon_janus1;

    // Restore original bytes
    DWORD old = 0;
    VirtualProtect((void*)fnAddr, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)fnAddr, g_savedBytes, 5);
    VirtualProtect((void*)fnAddr, 5, old, &old);

    // Call original — allocates the object, runs constructor, sets vtable
    reinterpret_cast<pfnFactory_t>(fnAddr)(pev);

    // Re-install our hook immediately
    WriteJmp5(fnAddr, (uintptr_t)Janus1_Factory, nullptr);
}

void __cdecl Janus1_Factory(entvars_t* pev)
{
    Log("[janus1] Factory pev=%p t=%.3f\n", pev, SafeTime());

    if (!g_factoryReady) {
        Log("[janus1] Factory: not ready yet, calling original\n");
        // Call original but don't swap vtable — weapon will use game's code
        CallOrigFactory(pev);
        return;
    }

    // Call original factory to properly allocate and construct the object
    CallOrigFactory(pev);

    // IDA disasm of weapon_janus1 confirmed:
    //   esi = pev  (arg_0)
    //   ecx = *(pev + 0x238)        <- edict pointer
    //   eax = ALLOC(ecx, 0x1F8)     <- allocated object (returned in eax)
    //   [eax+8] = esi               <- obj->pev = pev  (pev stored at obj+8)
    //   pvPrivateData stored at edict+0x80  (cmp [ecx+80h], 0 before alloc)
    //
    // So: edict = *(pev + 0x238),  obj = *(edict + 0x80)

    void* obj = nullptr;
    __try {
        void* edict = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(pev) + 0x238);
        Log("[janus1] edict=%p\n", edict);
        if (edict) {
            obj = *reinterpret_cast<void**>(
                reinterpret_cast<uint8_t*>(edict) + 0x80);
        }
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[janus1] Exception finding obj from pev=%p\n", pev);
        return;
    }

    if (!obj) {
        Log("[janus1] obj null after factory\n");
        return;
    }

    Log("[janus1] obj=%p orig_vtable=%p\n", obj, *reinterpret_cast<void**>(obj));

    // Swap the vtable
    SwapVtable(obj);

    Log("[janus1] Factory done\n");
}

// -----------------------------------------------------------------------
// PostInit — called after Hooks_Install succeeds
// -----------------------------------------------------------------------
void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit mpBase=0x%08zX\n", mpBase);

    // --- Engine function pointers ---
    // Each entry in the engfuncs table is a 4-byte fn pointer
    // RVA_pfnPrecacheModel = engfuncs[0]
    // We read the POINTER-TO-FUNCTION stored at each location
    __try {
        g_fnPrecacheModel = *reinterpret_cast<pfnPrecacheModel_t*>(mpBase + RVA_pfnPrecacheModel);
        g_fnPrecacheSound = *reinterpret_cast<pfnPrecacheSound_t*>(mpBase + RVA_pfnPrecacheSound);
        g_fnSetModel      = *reinterpret_cast<pfnSetModel_t*>      (mpBase + RVA_pfnSetModel);
        g_fnPrecacheEvent = *reinterpret_cast<pfnPrecacheEvent_t*> (mpBase + RVA_pfnPrecacheEvent);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[janus1] Exception reading engine fns!\n");
    }

    // --- gpGlobals time ---
    // RVA_pGlobals: mp.dll stores a pointer to globalvars_t
    // *((globalvars_t**)(mp + RVA_pGlobals)) = gpGlobals
    // gpGlobals->time is at offset 0 (first field)
    __try {
        uintptr_t pGlobalsAddr = *reinterpret_cast<uintptr_t*>(mpBase + RVA_pGlobals);
        if (pGlobalsAddr) {
            g_pTime          = reinterpret_cast<float*>(pGlobalsAddr); // time = offset 0
            s_pGlobalsBase   = reinterpret_cast<float*>(pGlobalsAddr); // base for other fields
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[janus1] Exception reading pGlobals!\n");
    }

    Log("[janus1] PrecacheModel=%p Sound=%p SetModel=%p Event=%p time=%p\n",
        (void*)g_fnPrecacheModel, (void*)g_fnPrecacheSound,
        (void*)g_fnSetModel,      (void*)g_fnPrecacheEvent, (void*)g_pTime);
    Log("[janus1] Current time=%.3f\n", SafeTime());

    // --- M79 vtable ---
    g_m79Vtable = reinterpret_cast<void**>(mpBase + RVA_CM79_vtable);
    Log("[janus1] M79 vtable @ %p, slot[0]=%p slot[167]=%p\n",
        (void*)g_m79Vtable,
        g_m79Vtable ? g_m79Vtable[0] : nullptr,
        g_m79Vtable ? g_m79Vtable[167] : nullptr);

    // --- Saved bytes ---
    const uint8_t* sb = GetSavedBytes(RVA_weapon_janus1);
    if (sb) {
        memcpy(g_savedBytes, sb, 5);
        Log("[janus1] Saved bytes: %02X %02X %02X %02X %02X\n",
            sb[0], sb[1], sb[2], sb[3], sb[4]);
    } else {
        // Fallback — IDA confirmed: 55 8B EC 56 8B
        g_savedBytes[0] = 0x55; g_savedBytes[1] = 0x8B;
        g_savedBytes[2] = 0xEC; g_savedBytes[3] = 0x56;
        g_savedBytes[4] = 0x8B;
        Log("[janus1] Using fallback saved bytes\n");
    }

    // --- Build CJanus1 vtable ---
    InitVtable();

    // --- Verify engine fns are valid hw.dll addresses (should be > 0x02000000) ---
    bool engOk = (uintptr_t)g_fnPrecacheModel > 0x01000000 &&
                 (uintptr_t)g_fnPrecacheSound  > 0x01000000;

    g_factoryReady = g_ourVtable && engOk && g_pTime;
    Log("[janus1] PostInit done. factoryReady=%d vtable=%p engOk=%d time=%p\n",
        (int)g_factoryReady, g_ourVtable, (int)engOk, (void*)g_pTime);
}

// -----------------------------------------------------------------------
// Static registration — runs before main(), registers the hook
// -----------------------------------------------------------------------
struct Janus1Registrar {
    Janus1Registrar() {
        RegisterWeaponHook("weapon_janus1", (void*)Janus1_Factory, RVA_weapon_janus1);
    }
};
static Janus1Registrar g_janus1Reg;
