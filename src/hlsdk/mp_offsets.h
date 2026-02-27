#pragma once
#include <cstdint>

// mp.dll RVAs (imagebase 0x10000000)
// All confirmed from IDA analysis

static const uintptr_t RVA_weapon_janus1    = 0x0E96640;
static const uintptr_t RVA_weapon_m79       = 0x0F29F30;
static const uintptr_t RVA_pGlobals         = 0x1E51BCC;
static const uintptr_t RVA_pfnPrecacheModel = 0x1E51878;
static const uintptr_t RVA_pfnPrecacheSound = 0x1E5187C;
static const uintptr_t RVA_pfnSetModel      = 0x1E51884;
static const uintptr_t RVA_pfnPrecacheEvent = 0x1E51A78;

// CM79 vtable — from xref analysis: 0x1159FA04 - 0x10000000 = 0x159FA04
static const uintptr_t RVA_CM79_vtable      = 0x159FA04;

// Janus1 original vtable — from log: 0x24C19034 - 0x235D0000 = 0x1649034
static const uintptr_t RVA_Janus1_vtable    = 0x1649034;

// sub_1131AA80 = DefaultDeploy helper (called by M79 Deploy)
// RVA = 0x1131AA80 - 0x10000000 = 0x131AA80
static const uintptr_t RVA_DefaultDeploy    = 0x131AA80;

// -----------------------------------------------------------------------
// M79 vtable slot indices — CONFIRMED from IDA m79_actions.txt scan:
// "M79 vtable - slots pointing to M79-specific code"
// [  3] 0x10F29BF0  = Spawn (calls Precache, sets model, movetype, solid)
// [  4] 0x10F28D40  = Precache (loads sounds, event, ammo model)
// [102] 0x10F294A0  = Deploy
// [165] 0x10F28D80  = WeaponIdle (fires rocket when ammo > 0)
// [175] 0x10F29930  = AddToPlayer
// [189] 0x10F28D90  = Holster
// NOTE: Slots 166/167/191 = SUB_DoNothing (past vtable real end)
// M79 has NO separate PrimaryAttack slot — it fires via WeaponIdle
// -----------------------------------------------------------------------
static const int VTBL_Spawn         = 3;
static const int VTBL_Precache      = 4;
static const int VTBL_Deploy        = 102;
static const int VTBL_WeaponIdle    = 165;
static const int VTBL_AddToPlayer   = 175;
static const int VTBL_Holster       = 189;
