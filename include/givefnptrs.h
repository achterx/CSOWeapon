#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ============================================================
// GiveFnptrsToDll bridge
// ============================================================
// mp.dll exports "GiveFnptrsToDll" as a named export.
// Calling it gives us the full enginefuncs_t table + globalvars_t ptr.
// This is EXACTLY how CSOWeapon(1).dll resolves everything —
// no hardcoded addresses, works across mp.dll versions.
// ============================================================

// Minimal enginefuncs_t — only the fields we actually use.
// Extend as needed. Offsets match GoldSrc/CSNZ engine layout.
struct enginefuncs_t
{
    void* pfnPrecacheModel;         // +0
    void* pfnPrecacheSound;         // +4
    void* pfnSetModel;              // +8
    void* pfnModelIndex;            // +12
    void* pfnModelFrames;           // +16
    void* pfnSetSize;               // +20
    void* pfnChangeLevel;           // +24
    void* pfnGetSpawnParms;         // +28
    void* pfnSaveSpawnParms;        // +32
    void* pfnVecToYaw;              // +36
    void* pfnVecToAngles;           // +40
    void* pfnMoveToOrigin;          // +44
    void* pfnChangeYaw;             // +48
    void* pfnChangePitch;           // +52
    void* pfnFindEntityByString;    // +56
    void* pfnGetEntityIllum;        // +60
    void* pfnFindEntityInSphere;    // +64
    void* pfnFindClientInPVS;       // +68
    void* pfnEntitiesInPVS;         // +72
    void* pfnMakeVectors;           // +76
    void* pfnAngleVectors;          // +80
    void* pfnCreateEntity;          // +84
    void* pfnRemoveEntity;          // +88
    void* pfnCreateNamedEntity;     // +92  ← CREATE_NAMED_ENTITY
    void* pfnMakeStatic;            // +96
    void* pfnEntIsOnFloor;          // +100
    void* pfnDropToFloor;           // +104
    void* pfnWalkMove;              // +108
    void* pfnSetOrigin;             // +112
    void* pfnEmitSound;             // +116
    void* pfnEmitAmbientSound;      // +120
    void* pfnTraceLine;             // +124
    void* pfnTraceToss;             // +128
    void* pfnTraceMonsterHull;      // +132
    void* pfnTraceHull;             // +136
    void* pfnTraceModel;            // +140
    void* pfnTraceTexture;          // +144
    void* pfnTraceSphere;           // +148
    void* pfnGetAimVector;          // +152
    void* pfnServerCommand;         // +156
    void* pfnServerExecute;         // +160
    void* pfnClientCommand;         // +164
    void* pfnParticleEffect;        // +168
    void* pfnLightStyle;            // +172
    void* pfnDecalIndex;            // +176
    void* pfnPointContents;         // +180
    void* pfnMessageBegin;          // +184
    void* pfnMessageEnd;            // +188
    void* pfnWriteByte;             // +192
    void* pfnWriteChar;             // +196
    void* pfnWriteShort;            // +200
    void* pfnWriteLong;             // +204
    void* pfnWriteAngle;            // +208
    void* pfnWriteCoord;            // +212
    void* pfnWriteString;           // +216
    void* pfnWriteEntity;           // +220
    void* pfnCVarRegister;          // +224
    void* pfnCVarGetFloat;          // +228
    void* pfnCVarGetString;         // +232
    void* pfnCVarSetFloat;          // +236
    void* pfnCVarSetString;         // +240
    void* pfnAlertMessage;          // +244  ← ALERT()
    void* pfnEngineFprintf;         // +248
    void* pfnPvAllocEntPrivateData; // +252
    void* pfnPvEntPrivateData;      // +256
    void* pfnFreeEntPrivateData;    // +260
    void* pfnSzFromIndex;           // +264
    void* pfnAllocString;           // +268
    void* pfnGetVarsOfEnt;          // +272
    void* pfnPEntityOfEntOffset;    // +276
    void* pfnEntOffsetOfPEntity;    // +280
    void* pfnIndexOfEdict;          // +284
    void* pfnPEntityOfEntIndex;     // +288
    void* pfnFindEntityByVars;      // +292
    void* pfnGetModelPtr;           // +296
    void* pfnRegUserMsg;            // +300
    void* pfnAnimationAutomove;     // +304
    void* pfnGetBonePosition;       // +308
    void* pfnFunctionFromName;      // +312
    void* pfnNameForFunction;       // +316
    void* pfnClientPrintf;          // +320
    void* pfnServerPrint;           // +324
    void* pfnCmd_Args;              // +328
    void* pfnCmd_Argv;              // +332
    void* pfnCmd_Argc;              // +336
    void* pfnGetAttachment;         // +340
    void* pfnCRC32_Init;            // +344
    void* pfnCRC32_ProcessBuffer;   // +348
    void* pfnCRC32_ProcessByte;     // +352
    void* pfnCRC32_Final;           // +356
    void* pfnRandomLong;            // +360
    void* pfnRandomFloat;           // +364  ← RANDOM_FLOAT
    void* pfnSetView;               // +368
    void* pfnTime;                  // +372  ← gpGlobals->time equivalent
    void* pfnCrossProductLength;    // +376
    void* pfnRegistercvar;          // +380  (dup)
    void* pfnGetGameDir;            // +384
    void* pfnCvar_RegisterVariable; // +388
    void* pfnFadeClientVolume;      // +392
    void* pfnSetClientMaxspeed;     // +396
    void* pfnCreateFakeClient;      // +400
    void* pfnRunPlayerMove;         // +404
    void* pfnNumberOfEntities;      // +408
    void* pfnGetInfoKeyBuffer;      // +412
    void* pfnInfoKeyValue;          // +416
    void* pfnSetKeyValue;           // +420
    void* pfnSetClientKeyValue;     // +424
    void* pfnIsMapValid;            // +428
    void* pfnStaticDecal;           // +432
    void* pfnPrecacheGeneric;       // +436
    void* pfnGetPlayerUserId;       // +440
    void* pfnBuildSoundMsg;         // +444
    void* pfnIsDedicatedServer;     // +448
    void* pfnCVarGetPointer;        // +452
    void* pfnGetPlayerWONId;        // +456
    void* pfnInfo_RemoveKey;        // +460
    void* pfnGetPhysicsKeyValue;    // +464
    void* pfnSetPhysicsKeyValue;    // +468
    void* pfnGetPhysicsInfoString;  // +472
    void* pfnPrecacheEvent;         // +476  ← PRECACHE_EVENT
    void* pfnPlaybackEvent;         // +480  ← PLAYBACK_EVENT_FULL
    // ... more follows but above covers all we need
};

struct globalvars_t
{
    float   time;           // +0  gpGlobals->time
    float   frametime;      // +4
    float   force_retouch;  // +8
    // string_t  mapname etc, we only need time
};

// ============================================================
// Resolved pointers — filled by GiveFnptrs_Init()
// ============================================================
extern enginefuncs_t* g_engfuncs;
extern globalvars_t*  g_globals;

// typedef for the export
typedef void (*GiveFnptrsToDll_t)(enginefuncs_t* pengfuncsFromEngine, globalvars_t** pGlobals);

// Call once after mp.dll is loaded.
// Returns true on success.
bool GiveFnptrs_Init(HMODULE hMpDll);

// Convenience wrappers
inline float   gpTime()         { return g_globals ? g_globals->time : 0.f; }
inline int     CREATE_NAMED_ENTITY(int classname) {
    return reinterpret_cast<int(*)(int)>(g_engfuncs->pfnCreateNamedEntity)(classname);
}
inline int     ALLOC_STRING(const char* str) {
    return reinterpret_cast<int(*)(const char*)>(g_engfuncs->pfnAllocString)(str);
}
inline float   RANDOM_FLOAT(float lo, float hi) {
    return reinterpret_cast<float(*)(float,float)>(g_engfuncs->pfnRandomFloat)(lo, hi);
}
inline long    RANDOM_LONG(long lo, long hi) {
    return reinterpret_cast<long(*)(long,long)>(g_engfuncs->pfnRandomLong)(lo, hi);
}
inline void    PLAYBACK_EVENT_FULL(int flags, void* pInvoker, unsigned short eventIndex,
    float delay, float* origin, float* angles, float fparam1, float fparam2,
    int iparam1, int iparam2, int bparam1, int bparam2) {
    reinterpret_cast<void(*)(int,void*,unsigned short,float,float*,float*,float,float,int,int,int,int)>(
        g_engfuncs->pfnPlaybackEvent)(flags,pInvoker,eventIndex,delay,origin,angles,
            fparam1,fparam2,iparam1,iparam2,bparam1,bparam2);
}
