#pragma once
#include "../hlsdk/sdk.h"
#include "../hlsdk/mp_offsets.h"
#include <cstdint>

// Object size: M79=0x1F0 (496), weapon_janus1=0x1F8 (504)
static const int JANUS1_OBJ_SIZE = 504;

// Field offsets are defined in sdk.h as macros (F_usFireEvent, F_iClip, etc.)

void __cdecl Janus1_Factory(int edict);
void         Janus1_PostInit(uintptr_t mpBase);
