#pragma once
#include <windows.h>
#include <cstdint>

// enginefuncs_t as a flat array of 218 void* — we access by index
struct enginefuncs_t {
    void* funcs[218];
};

struct globalvars_t {
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

extern enginefuncs_t  g_engfuncs;
extern globalvars_t*  g_pGlobals;

bool GiveFnptrs_Init(HMODULE hMp);
