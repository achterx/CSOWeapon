#pragma once
// Minimal self-contained SDK types for CSNZ weapon injection.
// We do NOT include the full HL SDK headers because CSNZ's CBasePlayerWeapon
// has a different layout than vanilla HL. We use raw offsets from IDA instead.

#include <windows.h>
#include <cstdint>

// -----------------------------------------------------------------------
// Engine types (from HL SDK eiface.h / progdefs.h)
// -----------------------------------------------------------------------
typedef int     BOOL;
typedef float   vec_t;
struct Vector { float x, y, z; };

struct entvars_s
{
    // Only fields we actually use — full struct is 756 bytes
    // We access most things via raw offsets anyway
    char        classname[4]; // actually a string_t (int)
    // ... (we won't use this struct directly)
};
typedef struct entvars_s entvars_t;

struct edict_s
{
    int         free;
    int         serialnumber;
    // link_t, ... 
    // We don't need the full layout; access via engfuncs
};
typedef struct edict_s edict_t;

// -----------------------------------------------------------------------
// Engine function table (subset — only what our weapon uses)
// -----------------------------------------------------------------------
struct enginefuncs_t
{
    int   (*pfnPrecacheModel)(const char* s);           // [0]
    int   (*pfnPrecacheSound)(const char* s);           // [1]
    void  (*pfnSetModel)(edict_t* e, const char* m);    // [2]
    // ... 218 total, we only need a few
};

// Global engine functions — filled in by Hooks_Init
extern enginefuncs_t* g_engfuncs;
extern float*         g_pTime;   // &gpGlobals->time

inline float   UTIL_WeaponTimeBase() { return g_pTime ? *g_pTime : 0.f; }
inline int     PRECACHE_MODEL(const char* s)  { return g_engfuncs ? g_engfuncs->pfnPrecacheModel(s) : 0; }
inline int     PRECACHE_SOUND(const char* s)  { return g_engfuncs ? g_engfuncs->pfnPrecacheSound(s) : 0; }

// -----------------------------------------------------------------------
// Raw field access helpers
// All offsets confirmed from IDA decompile of client mp.dll
// -----------------------------------------------------------------------
template<typename T>
static inline T& Field(void* base, int byteOffset)
{
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + byteOffset);
}

// CBasePlayerWeapon / CBasePlayerItem fields (byte offsets)
#define F_pPlayer            0x0D0  // CBasePlayer*
#define F_iId                0x0E4  // int  weapon ID
#define F_iClip              0x15C  // int  current clip
#define F_iAmmo              0x154  // int  primary ammo
#define F_iPrimaryAmmoType   0x14C  // int
#define F_flNextPrimary      0x13C  // float
#define F_flNextSecondary    0x140  // float
#define F_flTimeIdle         0x18C  // float
#define F_iShotCount         0x194  // int
#define F_iConfigId          0x1D8  // int
#define F_usFireEvent        0x1E8  // uint16  event handle
#define F_flChargeState      0x1FC  // float

// edict_t* is at obj+8 (set by factory)
#define F_pev                0x008  // entvars_t* (actually edict ptr in CSNZ)

// Player fields
#define FP_deadflag          0xEC4  // byte (offset 3780)
#define FP_frozen            0x185D // byte (offset 6237)
