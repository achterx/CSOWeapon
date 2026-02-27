#pragma once
#include <cstdint>

// mp.dll RVAs (imagebase 0x10000000, confirmed from IDA + server logs)
// All offsets = actual_address - 0x10000000

// Weapon factory entry points
static const uintptr_t RVA_weapon_janus1    = 0x0E96640;  // IDA: weapon_janus1 label
static const uintptr_t RVA_weapon_m79       = 0x0F29F30;  // IDA: weapon_m79 label

// gpGlobals — stored as a pointer in mp.dll data section
// *((globalvars_t**)(mp + RVA_pGlobals)) = gpGlobals
// gpGlobals->time is the first float field
static const uintptr_t RVA_pGlobals         = 0x1E51BCC;

// Engine functions table base (confirmed from logs: PrecacheModel=0x03125190 was valid)
// Each entry is a function pointer (4 bytes, 32-bit)
static const uintptr_t RVA_engfuncs         = 0x1E51878;

// Individual engine fn pointers (each is a void* stored in mp.dll)
// engfuncs[0] = pfnPrecacheModel
// engfuncs[1] = pfnPrecacheSound
// engfuncs[2] = pfnPrecacheGeneric
// engfuncs[3] = pfnSetModel (SET_MODEL)
static const uintptr_t RVA_pfnPrecacheModel = 0x1E51878;  // engfuncs+0x00
static const uintptr_t RVA_pfnPrecacheSound = 0x1E5187C;  // engfuncs+0x04
static const uintptr_t RVA_pfnSetModel      = 0x1E51884;  // engfuncs+0x0C

// pfnPrecacheEvent — confirmed from M79 Precache IDA decompile:
//   dword_11E51A78(1, "events/m79.sc") — this IS pfnPrecacheEvent
static const uintptr_t RVA_pfnPrecacheEvent = 0x1E51A78;

// CM79 vtable (for delegation — M79 has working PrimaryAttack, Reload, Deploy)
static const uintptr_t RVA_CM79_vtable      = 0x159FA04;

// CSimpleWpn<CM79> vtable — the actual runtime vtable for M79 weapon objects
// From IDA: 0x115D48CC (client). Server RVA = 0x159F48C
// NOTE: We use CM79 vtable slots to delegate specific functions
// Vtable slot indices in CSimpleWpn confirmed from IDA vtable dump:
static const int VTBL_Spawn         = 0;    // (not needed, we override)
static const int VTBL_Precache      = 3;    // confirmed <<<M79 slot
static const int VTBL_Deploy        = 102;
static const int VTBL_Holster       = 191;
static const int VTBL_PrimaryAttack = 167;
static const int VTBL_SecondaryAtk  = 168;
static const int VTBL_Reload        = 166;
static const int VTBL_WeaponIdle    = 165;
static const int VTBL_AddToPlayer   = 175;
