#pragma once
// janus1.h — CJanus1 grenade launcher (no secondary attack, like M79)
// Follows ReGameDLL/HLSDK weapon pattern exactly.

#include "../hlsdk/cso_baseweapon.h"

// Janus-1 weapon constants
#define WEAPON_JANUS1           570
#define JANUS1_MAX_CLIP         1
#define JANUS1_DEFAULT_GIVE     1
#define JANUS1_WEIGHT           15

// Animation indices (from client model — same as M79)
enum janus1_e
{
    JANUS1_IDLE   = 0,
    JANUS1_SHOOT  = 1,
    JANUS1_RELOAD = 2,
    JANUS1_DRAW   = 3,
};

class CJanus1 : public CBasePlayerWeapon
{
public:
    void Spawn()   override;
    void Precache() override;
    int  GetItemInfo(ItemInfo* p) override;
    BOOL Deploy()  override;
    void Holster(int skiplocal = 0) override;
    BOOL AddToPlayer(CBasePlayer* pPlayer) override;
    void PrimaryAttack()  override;
    void SecondaryAttack() override;
    void WeaponIdle()     override;
    void Reload()         override;
    int  iItemSlot()      override { return 3; }
    BOOL UseDecrement()   override { return FALSE; }
};

// Called from dllmain after Hooks_Install succeeds
void Janus1_PostInit(uintptr_t mpBase);

// The factory hook — replaces weapon_janus1 entry point
void __cdecl Janus1_Factory(entvars_t* pev);
