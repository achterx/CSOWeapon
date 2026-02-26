#pragma once
#include <windows.h>
#include <cstdint>

// Minimal HLSDK types needed
struct enginefuncs_s {
    // Standard HLSDK enginefuncs_t — 218 function pointers
    // We only need pfnSetOrigin (index 20) and pfnDispatchSpawn is in gamedll, not here.
    // Declare as raw array of void* for simplicity; we access by index.
    void* funcs[218];
};
typedef struct enginefuncs_s enginefuncs_t;

struct globalvars_s {
    float        time;
    float        frametime;
    float        force_retouch;
    const char*  mapname;
    const char*  startspot;
    float        deathmatch;
    float        coop;
    float        teamplay;
    float        serverflags;
    float        found_secrets;
    float        v_forward[3];
    float        v_up[3];
    float        v_right[3];
    float        trace_allsolid;
    float        trace_startsolid;
    float        trace_fraction;
    float        trace_endpos[3];
    float        trace_plane_normal[3];
    float        trace_plane_dist;
    void*        trace_ent;
    float        trace_inopen;
    float        trace_inwater;
    int          trace_hitgroup;
    int          trace_flags;
    int          msg_entity;
    int          cdAudioTrack;
    int          maxClients;
    int          maxEntities;
    const char*  pStringBase;
    void*        pSaveData;
    float        vecLandmarkOffset[3];
};
typedef struct globalvars_s globalvars_t;

extern enginefuncs_t  g_engfuncs;
extern globalvars_t*  g_pGlobals;

bool      GiveFnptrs_Init(HMODULE hMp);
uintptr_t FindWeaponFrostbiteVtable(HMODULE hMp);
