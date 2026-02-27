#pragma once
#include <cstdint>

// mp.dll RVAs (imagebase 0x10000000, confirmed from IDA)
static const uintptr_t RVA_weapon_janus1       = 0x0E96640; // factory entry point we hook
static const uintptr_t RVA_weapon_m79          = 0x0F29F30; // M79 factory (reference impl)
static const uintptr_t RVA_pGlobals            = 0x1E51BCC; // ptr to globalvars_t
static const uintptr_t RVA_UTIL_WeaponTimeBase = 0x058A0B0;
static const uintptr_t RVA_GetWeaponConfig     = 0x0576870;
static const uintptr_t RVA_BaseAddToPlayer     = 0x0576FB0;
static const uintptr_t RVA_PrecacheModel       = 0x0000000; // from engfuncs — set at runtime
static const uintptr_t RVA_PrecacheSound       = 0x0000001; // from engfuncs — set at runtime

// Janus-1 weapon ID
static const int WEAPON_JANUS1 = 570; // from game data (adjust if needed)

// Janus-1 assets (from IDA Precache dump)
#define JANUS1_MODEL_V  "models/v_janus1.mdl"
#define JANUS1_MODEL_P  "models/p_janus1.mdl"
#define JANUS1_MODEL_W  "models/w_janus1.mdl"
#define JANUS1_SOUND_FIRE  "weapons/janus1-1.wav"
#define JANUS1_SOUND_RELOAD "weapons/janus1_reload.wav"
#define JANUS1_EVENT    "events/janus1.sc"
