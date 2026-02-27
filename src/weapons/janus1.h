#pragma once
// janus1.h — CJanus1, written HLSDK/ReGameDLL style
#include "../hlsdk/cso_baseweapon.h"

// Janus-1 weapon ID in CSNZ
#define WEAPON_JANUS1       570
#define JANUS1_MAX_CLIP     1
#define JANUS1_DEFAULT_GIVE 1
#define JANUS1_WEIGHT       15

class CJanus1 : public CBasePlayerWeapon
{
public:
    void Spawn()   override;
    void Precache() override;
    int  GetItemInfo(ItemInfo* p) override;
    int  AddToPlayer(CBasePlayer* pPlayer) override;
    BOOL Deploy() override;
    void Holster(int skiplocal = 0) override;
    void PrimaryAttack()  override;
    void SecondaryAttack() override;
    void WeaponIdle()     override;
    void Reload()         override;
    int  iItemSlot()      override { return 3; }
    BOOL UseDecrement()   override { return FALSE; }
};

// Called after hooks are installed — resolves engine fns, builds vtable
void Janus1_PostInit(uintptr_t mpBase);
