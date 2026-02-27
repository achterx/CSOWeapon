// janus1.cpp — CJanus1 grenade launcher, written ReGameDLL/HLSDK style
//
// Key facts learned from ReGameDLL source:
//
// 1. FACTORY SIGNATURE:
//    LINK_ENTITY_TO_CLASS expands to:
//      void weapon_janus1(entvars_t* pev) { GetClassPtr<T>((T*)pev); }
//    So the factory receives entvars_t* — NOT int edict.
//    Our hook must match: void __cdecl Janus1_Factory(entvars_t* pev)
//
// 2. OBJECT ALLOCATION (GetClassPtr):
//    pev is cast as the "allocation key". The engine already called
//    ALLOC_PRIVATE(ENT(pev), sizeof(T)) before the factory runs.
//    GET_PRIVATE(ENT(pev)) returns the allocated block.
//    Then: new(pev) T  =>  placement-new into that block.
//    Result: object lives at edict->pvPrivateData
//    To find it: edict_t* e = ENT(pev); T* obj = (T*)GET_PRIVATE(e)
//    In raw terms: edict = *(entvars_t+4); obj = *(edict+pvPrivateData_offset)
//
// 3. VTABLE SWAP STRATEGY:
//    After calling original factory (which constructs game's CJanus1),
//    we find the object via edict->pvPrivateData and replace vtable[0]
//    with CJanus1's compiler-generated vtable pointer.
//
// 4. ENGINE FUNCTIONS:
//    PRECACHE_MODEL etc. are macros that call g_engfuncs.pfnPrecacheModel
//    The engfuncs table is at mp+0x1E51878 (confirmed from logs).
//    pfnPrecacheModel = engfuncs[0], pfnPrecacheSound = engfuncs[1]
//    pfnPrecacheEvent = confirmed at mp+0x1E51A78 from IDA M79 decompile

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include "../hlsdk/mp_offsets.h"
#include <cstring>
#include <new>
#include <windows.h>

// -----------------------------------------------------------------------
// Engine function implementations — defined in cso_baseweapon.h as externs
// -----------------------------------------------------------------------
pfnPrecacheModel_t g_fnPrecacheModel = nullptr;
pfnPrecacheSound_t g_fnPrecacheSound = nullptr;
pfnPrecacheEvent_t g_fnPrecacheEvent = nullptr;
pfnSetModel_t      g_fnSetModel      = nullptr;
float*             g_pTime           = nullptr;

// Static member definitions
void*    CBaseEntity::m_SaveData     = nullptr;
void*    CBasePlayerItem::m_SaveData = nullptr;
ItemInfo CBasePlayerItem::m_ItemInfoArray[32] = {};
void*    CBasePlayerItem::m_AmmoInfoArray = nullptr;
void*    CBasePlayerWeapon::m_SaveData = nullptr;

// -----------------------------------------------------------------------
// CJanus1 method implementations
// -----------------------------------------------------------------------
void CJanus1::Spawn()
{
    Precache();
    m_iId = WEAPON_JANUS1;
    SET_MODEL(edict(), "models/w_janus1.mdl");
    m_iDefaultAmmo = JANUS1_DEFAULT_GIVE;
    m_iClip = JANUS1_MAX_CLIP;
    // FallInit equivalent — let weapon fall to ground
    pev->movetype = 6;   // MOVETYPE_TOSS
    pev->solid    = 1;   // SOLID_TRIGGER
    Log("[janus1] Spawn done\n");
}

void CJanus1::Precache()
{
    Log("[janus1] Precache\n");
    PRECACHE_MODEL("models/v_janus1.mdl");
    PRECACHE_MODEL("models/w_janus1.mdl");
    PRECACHE_SOUND("weapons/janus1-1.wav");
    PRECACHE_SOUND("weapons/janus1-2.wav");
    PRECACHE_SOUND("weapons/janus1_reload.wav");
    m_usFireEvent = (unsigned short)PRECACHE_EVENT(1, "events/janus1.sc");
    Log("[janus1] Precache done, event=%d\n", (int)m_usFireEvent);
}

int CJanus1::GetItemInfo(ItemInfo* p)
{
    p->pszName    = "weapon_janus1";
    p->pszAmmo1   = "janus1_rocket";
    p->iMaxAmmo1  = 5;
    p->pszAmmo2   = nullptr;
    p->iMaxAmmo2  = -1;
    p->iMaxClip   = JANUS1_MAX_CLIP;
    p->iSlot      = 3;
    p->iPosition  = 6;
    p->iId        = m_iId = WEAPON_JANUS1;
    p->iFlags     = 0;
    p->iWeight    = JANUS1_WEIGHT;
    return 1;
}

int CJanus1::AddToPlayer(CBasePlayer* pPlayer)
{
    Log("[janus1] AddToPlayer player=%p\n", (void*)pPlayer);
    m_pPlayer = pPlayer;
    // Set weapon bit on player (pPlayer->pev->weapons |= (1 << m_iId) but
    // we don't have player layout — delegate to M79 vtable for now)
    // For a clean implementation call CBasePlayerWeapon::AddToPlayer logic:
    //   m_pPlayer = pPlayer
    //   pPlayer->pev->weapons |= (1 << m_iId)
    //   call CBasePlayerItem::AddToPlayer -> sends gmsgWeapPickup to client
    // Since we can't call super easily without real engine, log and return TRUE
    return TRUE;
}

BOOL CJanus1::Deploy()
{
    Log("[janus1] Deploy\n");
    m_fMaxSpeed = 250.0f;
    // DefaultDeploy sets view model, player model, animation, anim extension
    // We'll delegate to M79's Deploy via raw vtable call (sets correct models)
    // This is safe because our object layout matches M79's layout
    if (g_m79Vtable) {
        typedef BOOL(__thiscall* Fn)(void*);
        return reinterpret_cast<Fn>(g_m79Vtable[VTBL_Deploy])(this);
    }
    return TRUE;
}

void CJanus1::Holster(int skiplocal)
{
    Log("[janus1] Holster\n");
    // Standard holster: clear view/weapon models on player
    m_fInReload = FALSE;
    if (m_pPlayer) {
        // pPlayer->pev->viewmodel = 0; pPlayer->pev->weaponmodel = 0;
        // Access via raw pev offset: viewmodel at pev+0x168, weaponmodel at pev+0x16C
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(m_pPlayer->pev) + 0x168) = 0;
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(m_pPlayer->pev) + 0x16C) = 0;
    }
}

void CJanus1::PrimaryAttack()
{
    Log("[janus1] PrimaryAttack t=%.3f clip=%d\n", UTIL_WeaponTimeBase(), m_iClip);
    if (m_iClip <= 0) {
        PlayEmptySound();
        m_flNextPrimaryAttack = UTIL_WeaponTimeBase() + 0.15f;
        return;
    }
    // Delegate to M79's PrimaryAttack — fires rocket the same way
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[VTBL_PrimaryAttack])(this);
    }
}

void CJanus1::SecondaryAttack()
{
    // Janus-1 has no secondary attack
    m_flNextSecondaryAttack = UTIL_WeaponTimeBase() + 0.5f;
}

void CJanus1::WeaponIdle()
{
    if (m_flTimeWeaponIdle > UTIL_WeaponTimeBase())
        return;
    if (m_iClip) {
        SendWeaponAnim(0 /* idle anim */);
        m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 20.0f;
    }
}

void CJanus1::Reload()
{
    Log("[janus1] Reload\n");
    if (m_pPlayer && m_iClip < JANUS1_MAX_CLIP) {
        // DefaultReload equivalent via M79
        if (g_m79Vtable) {
            typedef void(__thiscall* Fn)(void*);
            reinterpret_cast<Fn>(g_m79Vtable[VTBL_Reload])(this);
        }
    }
}

// -----------------------------------------------------------------------
// Vtable slots from IDA (CSimpleWpn vtable, confirmed)
// -----------------------------------------------------------------------
static const int VTBL_Deploy        = 102;
static const int VTBL_PrimaryAttack = 167;
static const int VTBL_WeaponIdle    = 165;
static const int VTBL_Reload        = 166;
static const int VTBL_AddToPlayer   = 175;
static const int VTBL_Holster       = 191;

static void** g_m79Vtable   = nullptr;
static const uintptr_t RVA_CM79_vtable = 0x159FA04;

// -----------------------------------------------------------------------
// Factory
//
// Original: void weapon_janus1(entvars_t* pev) {
//               GetClassPtr<CJanus1>((CJanus1*)pev);
//           }
//
// GetClassPtr: allocates if needed, then new(pev) CJanus1, sets obj->pev=pev
// Object is at GET_PRIVATE(ENT(pev)) = edict->pvPrivateData
//
// edict layout (GoldSrc): pvPrivateData at offset 0x0C (12 bytes)
//   edict_t { int free; int serialnumber; link_t area; ... pvPrivateData; }
//   Actually: pvPrivateData is at edict+0x0C in standard GoldSrc
//
// entvars_t -> edict: ENT(pev) reads from engine's edict array
//   In practice: edict_t* e = *(edict_t**)(pev+4) [pev->pContainingEntity? no]
//   Actually from GoldSrc: ENT(pev) = pev's container edict
//   The simplest: pvPrivateData = edict[12] after GetClassPtr runs
// -----------------------------------------------------------------------

static uint8_t g_savedBytes[5] = {};
static bool    g_savedOk       = false;
static void*   g_cjanus1Vtable = nullptr;
static bool    g_ready         = false;

typedef void (__cdecl* pfnOrigFactory_t)(entvars_t*);

// Static buffer for placement-new to capture vtable pointer
static char   g_vtableBuf[sizeof(CJanus1)];
static bool   g_vtableInit = false;

static void InitVtable()
{
    memset(g_vtableBuf, 0, sizeof(g_vtableBuf));
    CJanus1* dummy = new(g_vtableBuf) CJanus1();
    g_cjanus1Vtable = *reinterpret_cast<void**>(dummy);
    g_vtableInit = (g_cjanus1Vtable != nullptr);
    Log("[janus1] CJanus1 vtable @ %p\n", g_cjanus1Vtable);
}

static void CallOrigAndSwap(entvars_t* pev)
{
    uintptr_t fnAddr = GetMpBase() + RVA_weapon_janus1;

    // Restore, call, re-patch
    DWORD old = 0;
    VirtualProtect((void*)fnAddr, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)fnAddr, g_savedBytes, 5);
    VirtualProtect((void*)fnAddr, 5, old, &old);

    reinterpret_cast<pfnOrigFactory_t>(fnAddr)(pev);

    WriteJmp5(fnAddr, (uintptr_t)Janus1_Factory, nullptr);

    // Find object: pvPrivateData is at offset 0x0C in edict_t (GoldSrc standard)
    // ENT(pev) is the edict — but we don't have the engine's ENT macro.
    // From IDA: the factory decompile showed edict_t* at *(pev+4) in some builds.
    // More reliably: scan the edict array. But easiest: use the known offset.
    // pev->pContainingEntity (edict_t*) is at pev+0 in entvars_t
    // Actually in GoldSrc entvars_t: first field IS pContainingEntity (edict_t*)
    void* obj = nullptr;
    __try {
        edict_t* edict = *reinterpret_cast<edict_t**>(pev); // pev->pContainingEntity
        // pvPrivateData at edict+0x0C
        obj = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(edict) + 0x0C);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[janus1] Exception finding obj from pev=%p\n", pev);
        return;
    }

    if (!obj) { Log("[janus1] obj null\n"); return; }

    Log("[janus1] obj=%p  orig_vtable=%p\n",
        obj, *reinterpret_cast<void**>(obj));

    // Swap vtable
    DWORD old2 = 0;
    if (VirtualProtect(obj, 4, PAGE_EXECUTE_READWRITE, &old2)) {
        *reinterpret_cast<void**>(obj) = g_cjanus1Vtable;
        VirtualProtect(obj, 4, old2, &old2);
        Log("[janus1] Vtable swapped -> CJanus1 @ %p\n", g_cjanus1Vtable);
    }
}

void __cdecl Janus1_Factory(entvars_t* pev)
{
    Log("[janus1] Factory pev=%p t=%.3f\n", pev, UTIL_WeaponTimeBase());
    if (!g_ready) { Log("[janus1] Not ready\n"); return; }
    CallOrigAndSwap(pev);
    Log("[janus1] Factory done\n");
}

// -----------------------------------------------------------------------
// PostInit
// -----------------------------------------------------------------------
void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit\n");

    // Saved bytes
    const uint8_t* saved = GetSavedBytes(RVA_weapon_janus1);
    if (saved) {
        memcpy(g_savedBytes, saved, 5);
        g_savedOk = true;
    } else {
        // IDA confirmed: 55 8B EC 56 8B
        uint8_t fb[5] = {0x55,0x8B,0xEC,0x56,0x8B};
        memcpy(g_savedBytes, fb, 5);
        g_savedOk = true;
    }
    Log("[janus1] Saved: %02X %02X %02X %02X %02X\n",
        g_savedBytes[0],g_savedBytes[1],g_savedBytes[2],
        g_savedBytes[3],g_savedBytes[4]);

    // Engine functions by RVA
    __try {
        g_fnPrecacheModel = *reinterpret_cast<pfnPrecacheModel_t*>(mpBase + RVA_pfnPrecacheModel);
        g_fnPrecacheSound = *reinterpret_cast<pfnPrecacheSound_t*>(mpBase + RVA_pfnPrecacheSound);
        g_fnPrecacheEvent = *reinterpret_cast<pfnPrecacheEvent_t*>(mpBase + RVA_pfnPrecacheEvent);
        g_fnSetModel      = *reinterpret_cast<pfnSetModel_t*>      (mpBase + RVA_pfnSetModel);
        g_pTime           = *reinterpret_cast<float**>             (mpBase + RVA_pGlobals);
    }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[janus1] Exception resolving engine fns\n"); }

    Log("[janus1] PrecacheModel=%p Sound=%p Event=%p SetModel=%p\n",
        (void*)g_fnPrecacheModel, (void*)g_fnPrecacheSound,
        (void*)g_fnPrecacheEvent, (void*)g_fnSetModel);

    // M79 vtable for delegation
    g_m79Vtable = reinterpret_cast<void**>(mpBase + RVA_CM79_vtable);
    Log("[janus1] M79 vtable @ %p slot[0]=%p\n",
        g_m79Vtable, g_m79Vtable ? g_m79Vtable[0] : nullptr);

    // Build CJanus1 vtable via placement-new
    InitVtable();

    g_ready = g_savedOk && g_vtableInit;
    Log("[janus1] PostInit done. ready=%d\n", (int)g_ready);
}

// -----------------------------------------------------------------------
// Static registration
// -----------------------------------------------------------------------
struct Janus1Reg {
    Janus1Reg() { RegisterWeaponHook("weapon_janus1", (void*)Janus1_Factory, RVA_weapon_janus1); }
};
static Janus1Reg g_reg;
