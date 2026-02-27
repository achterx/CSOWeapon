#pragma once
#include <cstdint>

// mp.dll RVAs (imagebase 0x10000000, confirmed from IDA + logs)
static const uintptr_t RVA_weapon_janus1 = 0x0E96640;
static const uintptr_t RVA_weapon_m79    = 0x0F29F30;
static const uintptr_t RVA_pGlobals      = 0x1E51BCC; // *(ptr) = globalvars_t, first float = time
static const uintptr_t RVA_engfuncs      = 0x1E51878; // engfuncs table base

// Individual engine function pointers stored in mp.dll (read as void**)
// Confirmed: engfuncs[0]=PrecacheModel engfuncs[1]=PrecacheSound
static const uintptr_t RVA_pfnPrecacheModel = 0x1E51878; // engfuncs[0]
static const uintptr_t RVA_pfnPrecacheSound = 0x1E5187C; // engfuncs[1]
static const uintptr_t RVA_pfnSetModel      = 0x1E51884; // engfuncs[3]  (SET_MODEL)
static const uintptr_t RVA_pfnPrecacheEvent = 0x1E51A78; // confirmed from M79 Precache IDA

// CM79 vtable RVA
static const uintptr_t RVA_CM79_vtable      = 0x159FA04;

// Vtable slot indices (from IDA CSimpleWpn vtable dump)
static const int VTBL_Deploy        = 102;
static const int VTBL_PrimaryAttack = 167;
static const int VTBL_WeaponIdle    = 165;
static const int VTBL_Reload        = 166;
static const int VTBL_AddToPlayer   = 175;
static const int VTBL_Holster       = 191;
