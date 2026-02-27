#pragma once
// sdk.h — Minimal GoldSrc/CSNZ types for our injected weapon DLL
// Designed to match the real memory layout used by CSNZ's mp.dll

#include <windows.h>
#include <cstdint>
#include <cstring>

// -----------------------------------------------------------------------
// Basic types
// -----------------------------------------------------------------------
#ifndef BOOL
typedef int BOOL;
#endif
#define TRUE  1
#define FALSE 0

typedef float vec_t;

struct Vector
{
    float x, y, z;
    Vector() : x(0), y(0), z(0) {}
    Vector(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
    Vector operator+(const Vector& v) const { return {x+v.x, y+v.y, z+v.z}; }
    Vector operator*(float f) const { return {x*f, y*f, z*f}; }
};

typedef int string_t;

// -----------------------------------------------------------------------
// Engine types
// -----------------------------------------------------------------------
struct edict_t;

// entvars_t — GoldSrc entity variables (~756 bytes)
// We only define the fields we use. The real struct is much larger.
struct entvars_t
{
    string_t    classname;     // +0x00
    string_t    globalname;    // +0x04
    Vector      origin;        // +0x08
    Vector      oldorigin;     // +0x14
    Vector      velocity;      // +0x20
    Vector      basevelocity;  // +0x2C
    Vector      clbasevelocity;// +0x38
    Vector      movedir;       // +0x44
    Vector      angles;        // +0x50
    Vector      avelocity;     // +0x5C
    Vector      punchangle;    // +0x68
    Vector      v_angle;       // +0x74
    Vector      endpos;        // +0x80
    Vector      startpos;      // +0x8C
    float       impacttime;    // +0x98
    float       starttime;     // +0x9C
    int         fixangle;      // +0xA0
    float       idealpitch;    // +0xA4
    float       pitch_speed;   // +0xA8
    float       ideal_yaw;     // +0xAC
    float       yaw_speed;     // +0xB0
    int         modelindex;    // +0xB4
    string_t    model;         // +0xB8
    int         viewmodel;     // +0xBC — view model (string_t)
    int         weaponmodel;   // +0xC0 — weapon model shown to others
    Vector      absmin;        // +0xC4
    Vector      absmax;        // +0xD0
    Vector      mins;          // +0xDC
    Vector      maxs;          // +0xE8
    Vector      size;          // +0xF4
    float       ltime;         // +0x100
    float       nextthink;     // +0x104
    int         movetype;      // +0x108
    int         solid;         // +0x10C
    int         skin;          // +0x110
    int         body;          // +0x114
    int         effects;       // +0x118
    float       gravity;       // +0x11C
    float       friction;      // +0x120
    int         light_level;   // +0x124
    int         sequence;      // +0x128
    int         gaitsequence;  // +0x12C
    float       frame;         // +0x130
    float       animtime;      // +0x134
    float       framerate;     // +0x138
    uint8_t     controller[4]; // +0x13C
    uint8_t     blending[2];   // +0x140
    float       scale;         // +0x144
    int         rendermode;    // +0x148
    float       renderamt;     // +0x14C
    Vector      rendercolor;   // +0x150
    int         renderfx;      // +0x15C
    float       health;        // +0x160
    float       frags;         // +0x164
    int         weapons;       // +0x168 — weapon bits
    float       takedamage;    // +0x16C
    float       deadflag;      // +0x170 // actually int in some builds
    Vector      view_ofs;      // +0x174
    int         button;        // +0x180
    int         impulse;       // +0x184
    edict_t*    chain;         // +0x188
    edict_t*    dmg_inflictor; // +0x18C
    edict_t*    enemy;         // +0x190
    edict_t*    aiment;        // +0x194
    edict_t*    owner;         // +0x198
    edict_t*    groundentity;  // +0x19C
    int         spawnflags;    // +0x1A0
    int         flags;         // +0x1A4
    int         colormap;      // +0x1A8
    int         team;          // +0x1AC
    float       max_health;    // +0x1B0
    float       teleport_time; // +0x1B4
    float       armortype;     // +0x1B8
    float       armorvalue;    // +0x1BC
    int         waterlevel;    // +0x1C0
    int         watertype;     // +0x1C4
    string_t    target;        // +0x1C8
    string_t    targetname;    // +0x1CC
    string_t    netname;       // +0x1D0
    string_t    message;       // +0x1D4
    float       dmg_take;      // +0x1D8
    float       dmg_save;      // +0x1DC
    float       dmg;           // +0x1E0
    float       dmgtime;       // +0x1E4
    string_t    noise;         // +0x1E8
    string_t    noise1;        // +0x1EC
    string_t    noise2;        // +0x1F0
    string_t    noise3;        // +0x1F4
    float       speed;         // +0x1F8
    float       air_finished;  // +0x1FC
    float       pain_finished; // +0x200
    float       radsuit_finished; // +0x204
    edict_t*    pContainingEntity; // +0x208 — the edict that holds this pev
    int         playerclass;   // +0x20C
    float       maxspeed;      // +0x210
    float       fov;           // +0x214
    int         weaponanim;    // +0x218
    int         pushmsec;      // +0x21C
    int         bInDuck;       // +0x220
    int         flTimeStepSound; // +0x224
    int         flSwimTime;    // +0x228
    int         flDuckTime;    // +0x22C
    int         iStepLeft;     // +0x230
    float       flFallVelocity;// +0x234
    int         gamestate;     // +0x238
    int         oldbuttons;    // +0x23C
    int         groupinfo;     // +0x240
    int         iuser1;        // +0x244
    int         iuser2;        // +0x248
    int         iuser3;        // +0x24C
    int         iuser4;        // +0x250
    float       fuser1;        // +0x254
    float       fuser2;        // +0x258
    float       fuser3;        // +0x25C
    float       fuser4;        // +0x260
    Vector      vuser1;        // +0x264
    Vector      vuser2;        // +0x270
    Vector      vuser3;        // +0x27C
    Vector      vuser4;        // +0x288
    edict_t*    euser1;        // +0x294
    edict_t*    euser2;        // +0x298
    edict_t*    euser3;        // +0x29C
    edict_t*    euser4;        // +0x2A0
};

// edict_t layout for CSNZ
// IDA confirmed: pvPrivateData at offset +0x80
// The full struct is larger than standard GoldSrc (extra fields between serial and pvPrivateData)
// We define it as opaque with correct size padding so any pointer arithmetic is safe.
// In practice we only ever pass edict_t* to engine functions — we never access fields directly.
struct edict_t
{
    int         free;               // +0x00
    int         serialnumber;       // +0x04
    uint8_t     _pad[0x78];         // +0x08  padding to reach pvPrivateData
    void*       pvPrivateData;      // +0x80  CBaseEntity* (CSNZ confirmed by IDA)
    // entvars_t* v follows (pev is a pointer, NOT inline in CSNZ)
    entvars_t*  v;                  // +0x84
};

// -----------------------------------------------------------------------
// Engine function pointer types
// -----------------------------------------------------------------------
typedef int            (*pfnPrecacheModel_t)(const char*);
typedef int            (*pfnPrecacheSound_t)(const char*);
typedef void           (*pfnSetModel_t)(edict_t*, const char*);
typedef unsigned short (*pfnPrecacheEvent_t)(int, const char*);
typedef int            (*pfnMessageBegin_t)(int, int, const float*, edict_t*);
typedef void           (*pfnMessageEnd_t)();
typedef void           (*pfnWriteByte_t)(int);
typedef void           (*pfnWriteShort_t)(int);

// -----------------------------------------------------------------------
// Global engine fn pointers — defined once (in janus1.cpp), extern everywhere else.
// g_pTime is also set by hooks.cpp after ResolveGlobals().
// -----------------------------------------------------------------------
extern pfnPrecacheModel_t  g_fnPrecacheModel;
extern pfnPrecacheSound_t  g_fnPrecacheSound;
extern pfnSetModel_t       g_fnSetModel;
extern pfnPrecacheEvent_t  g_fnPrecacheEvent;
extern float*              g_pTime;

// -----------------------------------------------------------------------
// Engine macro wrappers
// -----------------------------------------------------------------------
inline int  PRECACHE_MODEL(const char* s)  { return g_fnPrecacheModel ? g_fnPrecacheModel(s) : 0; }
inline int  PRECACHE_SOUND(const char* s)  { return g_fnPrecacheSound ? g_fnPrecacheSound(s) : 0; }
inline void SET_MODEL(edict_t* e, const char* m) { if (g_fnSetModel && e) g_fnSetModel(e, m); }
inline unsigned short PRECACHE_EVENT(int type, const char* name) { return g_fnPrecacheEvent ? g_fnPrecacheEvent(type, name) : 0; }
inline float UTIL_WeaponTimeBase() { return g_pTime ? *g_pTime : 0.f; }
inline float gpGlobals_time() { return g_pTime ? *g_pTime : 0.f; }

// Safe time read
inline float SafeTime() { 
    if (!g_pTime) return 0.f;
    float t = 0.f;
    __try { t = *g_pTime; } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return t;
}
