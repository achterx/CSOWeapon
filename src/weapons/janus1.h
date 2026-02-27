#pragma once
#include "../hlsdk/sdk.h"
#include "../hlsdk/mp_offsets.h"
#include <cstdint>

// Object size matching CSNZ's CM79 subclass (IDA: pfnAllocEntPrivateData(v3, 0x1F0) = 496 bytes)
// Note: M79 uses 0x1F0 = 496, weapon_janus1 uses 0x1F8 = 504
static const int JANUS1_OBJ_SIZE = 504;

// Field offsets within the weapon object (from IDA analysis)
// usFireEvent is stored as uint16 — used to hold PrecacheEvent handle
static const int F_usFireEvent = 0x1E8;
static const int F_iClip       = 0x154; // m_iClip

void __cdecl Janus1_Factory(int edict);
void         Janus1_PostInit(uintptr_t mpBase);
