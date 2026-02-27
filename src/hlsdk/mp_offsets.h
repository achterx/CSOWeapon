#pragma once
#include <cstdint>

// mp.dll RVAs (imagebase 0x10000000)
// From log: mp_base=0x235D0000

static const uintptr_t RVA_weapon_janus1    = 0x0E96640;
static const uintptr_t RVA_weapon_m79       = 0x0F29F30;
static const uintptr_t RVA_pGlobals         = 0x1E51BCC;
static const uintptr_t RVA_pfnPrecacheModel = 0x1E51878;
static const uintptr_t RVA_pfnPrecacheSound = 0x1E5187C;
static const uintptr_t RVA_pfnSetModel      = 0x1E51884;
static const uintptr_t RVA_pfnPrecacheEvent = 0x1E51A78;

// CM79 vtable for delegation
// From log: 0x24B6FA04 - 0x235D0000 = 0x159FA04
static const uintptr_t RVA_CM79_vtable      = 0x159FA04;

// Janus1 original vtable
// From log: orig_vtable=0x24C19034, mp_base=0x235D0000
// RVA = 0x24C19034 - 0x235D0000 = 0x1649034
static const uintptr_t RVA_Janus1_vtable    = 0x1649034;

// Vtable slot indices (from IDA M79 vtable dump, confirmed working)
static const int VTBL_Precache      = 3;
static const int VTBL_Deploy        = 102;
static const int VTBL_Holster       = 191;
static const int VTBL_PrimaryAttack = 167;
static const int VTBL_SecondaryAtk  = 168;
static const int VTBL_Reload        = 166;
static const int VTBL_WeaponIdle    = 165;
static const int VTBL_AddToPlayer   = 175;
