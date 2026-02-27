#pragma once
#include "../hlsdk/sdk.h"
#include "../hlsdk/mp_offsets.h"

// -------------------------------------------------------------------------
// CJanus1 — Janus-1 grenade launcher
//
// Inherits from CBasePlayerWeapon via raw vtable layout.
// We don't actually derive from HLSDK classes at compile time because
// CSNZ's CBasePlayerWeapon has a different vtable/layout than vanilla HL.
// Instead we implement the weapon logic in plain functions that match
// the __thiscall convention CSNZ expects, then build a vtable manually.
//
// Architecture:
//   game calls weapon_janus1(edict)  [hooked entry point]
//   -> our factory allocates a block of memory (object size = 536 bytes,
//      same as M79 from IDA: dword_11E51988(v3, 536))
//   -> sets our vtable pointer at offset 0
//   -> links edict at offset 8
//   -> engine calls virtual functions through our vtable
// -------------------------------------------------------------------------

// Object size matching CSNZ's CBasePlayerWeapon subclass (from IDA: 536 bytes for M79)
static const int JANUS1_OBJ_SIZE = 536;

// Vtable slot indices (confirmed from CSimpleWpn vtable dump)
static const int VTBL_Spawn         = 1;    // slot 1
static const int VTBL_Precache      = 3;    // slot 3/4 (Precache)
static const int VTBL_WeaponIdle    = 165;
static const int VTBL_Think         = 166;
static const int VTBL_PrimaryAttack = 167;
static const int VTBL_AddToPlayer   = 175;
static const int VTBL_Holster       = 191;
static const int VTBL_TakeDamage    = 193;
static const int VTBL_Deploy        = 102;  // slot 102 from M79 dump

// Factory function — registered as the weapon_janus1 entry point hook
void __cdecl Janus1_Factory(int edict);
