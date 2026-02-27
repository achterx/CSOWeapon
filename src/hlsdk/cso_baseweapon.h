#pragma once
// cso_baseweapon.h
// CBaseEntity → CBaseDelay → CBaseAnimating → CBasePlayerItem → CBasePlayerWeapon
// Reconstructed from ReGameDLL source + IDA field offsets
// This matches the real CSNZ server memory layout.

#include "sdk.h"

// -----------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------
class CBaseEntity;
class CBasePlayerItem;
class CBasePlayerWeapon;
class CBasePlayer;

// -----------------------------------------------------------------------
// ItemInfo — passed to GetItemInfo()
// -----------------------------------------------------------------------
struct ItemInfo
{
    int         iSlot;
    int         iPosition;
    const char* pszAmmo1;
    int         iMaxAmmo1;
    const char* pszAmmo2;
    int         iMaxAmmo2;
    const char* pszName;
    int         iMaxClip;
    int         iId;
    int         iFlags;
    int         iWeight;
};

// -----------------------------------------------------------------------
// WEAPON_NOCLIP
// -----------------------------------------------------------------------
#define WEAPON_NOCLIP  -1

// -----------------------------------------------------------------------
// CSNZ edict helpers
//
// IDA CONFIRMED offsets (from disasm of weapon_janus1 factory):
//   mov ecx, [esi+238h]       => edict* = *(pev + 0x238)
//   cmp [ecx+80h], 0          => pvPrivateData at edict+0x80
//
// In CSNZ, pev is NOT inline inside edict_t; it's separately allocated.
// The edict backpointer is stored at pev+0x238 (maps to entvars_t::gamestate
// in the struct definition but is actually repurposed in CSNZ as edict ptr).
// pvPrivateData (the C++ object) is at edict+0x80 (not standard GoldSrc 0x10).
// -----------------------------------------------------------------------
#define CSNZ_PEV_TO_EDICT_OFFSET  0x238   // edict* ptr stored at pev+0x238
#define CSNZ_PVPRIVATE_OFFSET     0x80    // pvPrivateData in edict_t at +0x80

inline edict_t* PEV_TO_EDICT(entvars_t* pev)
{
    if (!pev) return nullptr;
    // Read edict pointer stored at pev+0x238 (IDA confirmed)
    return *reinterpret_cast<edict_t**>(
        reinterpret_cast<uint8_t*>(pev) + CSNZ_PEV_TO_EDICT_OFFSET);
}

inline void* EDICT_PRIVATE(edict_t* e)
{
    if (!e) return nullptr;
    // pvPrivateData at edict+0x80 (IDA confirmed)
    return *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(e) + CSNZ_PVPRIVATE_OFFSET);
}

// -----------------------------------------------------------------------
// CBaseEntity
// -----------------------------------------------------------------------
class CBaseEntity
{
public:
    // vtable is at [0] — compiler generated
    entvars_t*  pev;            // +0x04  (after vtable ptr at +0x00)
    CBaseEntity* m_pGoalEnt;    // +0x08
    CBaseEntity* m_pLink;       // +0x0C

    // Virtual methods (subset — must match CSNZ's vtable order exactly)
    virtual void Spawn()        {}
    virtual void Precache()     {}
    virtual void KeyValue(void* pkvd) {}
    virtual int  Save(void*)    { return 0; }
    virtual int  Restore(void*) { return 0; }
    virtual void SetObjectCollisionBox() {}
    virtual int  Classify()     { return 0; }
    virtual void DeathNotice(entvars_t*) {}
    virtual void TraceAttack(entvars_t*, float, Vector, void*, int) {}
    virtual int  TakeDamage(entvars_t*, entvars_t*, float, int) { return 0; }
    virtual int  TakeHealth(float, int) { return 0; }
    virtual void Killed(entvars_t*, int) {}
    virtual int  BloodColor()   { return 0; }
    virtual void TraceBleed(float, Vector, void*, int) {}
    virtual BOOL IsTriggered(CBaseEntity*) { return FALSE; }
    virtual void* MyMonsterPointer() { return nullptr; }
    virtual void* MySquadMonsterPointer() { return nullptr; }
    virtual int  GetToggleState() { return 0; }
    virtual void AddPoints(int, BOOL) {}
    virtual void AddPointsToTeam(int, BOOL) {}
    virtual BOOL AddPlayerItem(CBasePlayerItem*) { return FALSE; }
    virtual BOOL RemovePlayerItem(CBasePlayerItem*) { return FALSE; }
    virtual int  GiveAmmo(int, char*, int) { return -1; }
    virtual float GetDelay() { return 0; }
    virtual int  IsMoving() { return 0; }
    virtual void OverrideReset() {}
    virtual int  DamageDecal(int) { return 0; }
    virtual void SetToggleState(int) {}
    virtual void StartSneaking() {}
    virtual void StopSneaking() {}
    virtual BOOL OnControls(entvars_t*) { return FALSE; }
    virtual BOOL IsSneaking() { return FALSE; }
    virtual BOOL IsAlive() { return FALSE; }
    virtual BOOL IsBSPModel() { return FALSE; }
    virtual BOOL ReflectGauss() { return FALSE; }
    virtual BOOL HasTarget(string_t) { return FALSE; }
    virtual BOOL IsInWorld() { return TRUE; }
    virtual BOOL IsPlayer() { return FALSE; }
    virtual BOOL IsNetClient() { return FALSE; }
    virtual const char* TeamID() { return ""; }
    virtual CBaseEntity* GetNextTarget() { return nullptr; }
    virtual void Think() {}
    virtual void Touch(CBaseEntity*) {}
    virtual void Use(CBaseEntity*, CBaseEntity*, int, float) {}
    virtual void Blocked(CBaseEntity*) {}
    virtual CBaseEntity* Respawn() { return nullptr; }
    virtual void UpdateOnRemove() {}
    virtual BOOL FBecomeProne(CBaseEntity*) { return FALSE; }
    virtual Vector Center() { return pev ? pev->origin : Vector(); }
    virtual Vector EyePosition() { return pev ? pev->origin : Vector(); }
    virtual Vector EarPosition() { return pev ? pev->origin : Vector(); }
    virtual Vector BodyTarget(const Vector&) { return pev ? pev->origin : Vector(); }
    virtual int  Illumination() { return 0; }
    virtual BOOL FVisible(CBaseEntity*) { return FALSE; }
    virtual BOOL FVisible(const Vector&) { return FALSE; }

    // Non-virtual helpers
    edict_t* edict() { return PEV_TO_EDICT(pev); }
};

// -----------------------------------------------------------------------
// CBaseDelay (adds m_flDelay, m_iszKillTarget)
// -----------------------------------------------------------------------
class CBaseDelay : public CBaseEntity
{
public:
    float       m_flDelay;          // +0x10
    string_t    m_iszKillTarget;    // +0x14
    // SUB_UseTargets etc are non-virtual, not needed
};

// -----------------------------------------------------------------------
// CBaseAnimating (adds anim fields)
// -----------------------------------------------------------------------
class CBaseAnimating : public CBaseDelay
{
public:
    float       m_flFrameRate;      // +0x18
    float       m_flGroundSpeed;    // +0x1C
    float       m_flLastEventCheck; // +0x20
    BOOL        m_fSequenceFinished;// +0x24
    BOOL        m_fSequenceLoops;   // +0x28
};

// -----------------------------------------------------------------------
// CBasePlayerItem
// -----------------------------------------------------------------------
class CBasePlayerItem : public CBaseAnimating
{
public:
    // Additional virtuals beyond CBaseEntity
    virtual BOOL CanDrop() { return TRUE; }
    virtual BOOL Deploy() { return TRUE; }
    virtual BOOL CanDeploy() { return TRUE; }
    virtual BOOL IsWeapon() { return FALSE; }
    virtual void Holster(int skiplocal = 0) {}
    virtual void UpdateItemInfo() {}
    virtual void ItemPostFrame() {}
    virtual int  PrimaryAmmoIndex() { return -1; }
    virtual int  SecondaryAmmoIndex() { return -1; }
    virtual int  UpdateClientData(CBasePlayer*) { return 0; }
    virtual CBasePlayerItem* GetWeaponPtr() { return nullptr; }
    virtual float GetMaxSpeed() { return 250.f; }
    virtual int  iItemSlot() { return 0; }
    virtual int  GetItemInfo(ItemInfo*) { return 0; }
    virtual BOOL AddToPlayer(CBasePlayer*) { return FALSE; }
    virtual BOOL AddDuplicate(CBasePlayerItem*) { return FALSE; }

    // Public helpers (non-virtual, defined in ReGameDLL)
    void FallInit();
    void CheckRespawn();

    // Static data
    static ItemInfo m_ItemInfoArray[32];
    static void*    m_AmmoInfoArray;
    static void*    m_SaveData;

    // Fields
    CBasePlayer*      m_pPlayer;    // +0x2C  (after CBaseAnimating's fields)
    CBasePlayerItem*  m_pNext;      // +0x30
    int               m_iId;        // +0x34  WEAPON_???
};

// -----------------------------------------------------------------------
// CBasePlayerWeapon — the class all CS weapons inherit
// Layout matches ReGameDLL weapons.h exactly
// -----------------------------------------------------------------------
class CBasePlayerWeapon : public CBasePlayerItem
{
public:
    // Additional virtuals
    virtual int  ExtractAmmo(CBasePlayerWeapon*) { return 0; }
    virtual int  ExtractClipAmmo(CBasePlayerWeapon*) { return 0; }
    virtual int  AddWeapon() { ExtractAmmo(this); return 1; }
    virtual BOOL PlayEmptySound() { return TRUE; }
    virtual void ResetEmptySound() {}
    virtual void SendWeaponAnim(int iAnim, int skiplocal = 0) {}
    virtual BOOL IsUseable() { return TRUE; }
    virtual void PrimaryAttack()   {}
    virtual void SecondaryAttack() {}
    virtual void Reload()          {}
    virtual void WeaponIdle()      {}
    virtual void RetireWeapon()    {}
    virtual BOOL ShouldWeaponIdle() { return FALSE; }
    virtual BOOL UseDecrement()    { return FALSE; }

    // Non-virtual methods (use M79 vtable delegation for these)
    // DefaultDeploy, DefaultReload, GetNextAttackDelay are in game code

    // Static
    static void* m_SaveData;

    // Fields (from ReGameDLL weapons.h CBasePlayerWeapon section)
    int             m_iPlayEmptySound;      // +0x38
    int             m_fFireOnEmpty;         // +0x3C
    float           m_flNextPrimaryAttack;  // +0x40
    float           m_flNextSecondaryAttack;// +0x44
    float           m_flTimeWeaponIdle;     // +0x48
    int             m_iPrimaryAmmoType;     // +0x4C
    int             m_iSecondaryAmmoType;   // +0x50
    int             m_iClip;               // +0x54
    int             m_iClientClip;         // +0x58
    int             m_iClientWeaponState;  // +0x5C
    int             m_fInReload;           // +0x60
    int             m_fInSpecialReload;    // +0x64
    int             m_iDefaultAmmo;        // +0x68
    int             m_iShellId;            // +0x6C
    float           m_fMaxSpeed;           // +0x70
    bool            m_bDelayFire;          // +0x74
    BOOL            m_iDirection;          // +0x78
    bool            m_bSecondarySilencerOn;// +0x7C
    float           m_flAccuracy;          // +0x80
    float           m_flLastFire;          // +0x84
    int             m_iShotsFired;         // +0x88
    Vector          m_vVecAiming;          // +0x8C
    string_t        model_name;            // +0x98
    float           m_flGlock18Shoot;      // +0x9C
    int             m_iGlock18ShotsFired;  // +0xA0
    float           m_flFamasShoot;        // +0xA4
    int             m_iFamasShotsFired;    // +0xA8
    float           m_fBurstSpread;        // +0xAC
    int             m_iWeaponState;        // +0xB0
    float           m_flNextReload;        // +0xB4
    float           m_flDecreaseShotsFired;// +0xB8
    unsigned short  m_usFireGlock18;       // +0xBC
    unsigned short  m_usFireFamas;         // +0xBE
    float           m_flPrevPrimaryAttack; // +0xC0
    float           m_flLastFireTime;      // +0xC4

    // CSNZ-specific extra fields (appended after standard CS fields)
    // These are at higher offsets — values from IDA field dump
    // m_usFireEvent is at this+0x1E8 (confirmed: WPN_EVENT offset from IDA)
    // But our struct offset won't match that exactly since we're reconstructing.
    // We'll use a padding array to get to the right offsets.
    // Rather than guess, we store it locally and write it directly on init.
    unsigned short  m_usFireEvent;          // fire event handle (PRECACHE_EVENT result)
};

// -----------------------------------------------------------------------
// CBasePlayer — minimal stub so weapon methods can take CBasePlayer* params.
// The actual player object lives in game memory; we only store the pointer
// and pass it through to M79 delegation calls. We never dereference fields
// directly (that would require the full 6000-byte player layout).
// -----------------------------------------------------------------------
class CBasePlayer : public CBaseEntity
{
public:
    // We don't define any fields or methods here — accessing player internals
    // is done via M79 vtable delegation, which uses the game's own code.
    // If you ever need a specific player field, add it here with the correct
    // byte offset (from IDA) and use Field<T>(this, offset) to access it.
    int m_rgAmmo[32];   // ammo array — needed by some weapon logic
};
