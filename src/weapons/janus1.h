#pragma once
#include "../hlsdk/cso_baseweapon.h"

#define WEAPON_JANUS1       570
#define JANUS1_MAX_CLIP     1
#define JANUS1_DEFAULT_GIVE 1
#define JANUS1_WEIGHT       15

enum janus1_e { JANUS1_IDLE=0, JANUS1_SHOOT=1, JANUS1_RELOAD=2, JANUS1_DRAW=3 };

void Janus1_PostInit(uintptr_t mpBase);
void __cdecl Janus1_Factory(entvars_t* pev);
