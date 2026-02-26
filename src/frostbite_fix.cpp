// frostbite_fix.cpp  v8
//
// ROOT CAUSE (confirmed via IDA):
//   CFrostbite::Precache() = nullsub_2 (empty function, just ret).
//   The weapon never calls pfnPrecacheModel / pfnPrecacheSound for any of
//   its assets.  When the engine tries to use the model or play a sound
//   that was never precached -> crash.
//
// FIX:
//   Patch vtable slot 2 (Precache) at RVA 0x15D4C88 to point to our
//   FrostbitePrecache() stub which precaches all 22 known assets.
//   All asset strings are read directly from mp.dll .rdata using their
//   confirmed RVAs so we never hardcode heap pointers.

#include <windows.h>
#include <cstdint>
#include <psapi.h>
#include "givefnptrs.h"
#include "logger.h"

#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Confirmed RVAs from IDA (imagebase 0x10000000)
// ---------------------------------------------------------------------------

// vtable slot 2 = Precache(), currently nullsub_2
static const DWORD kVtable_CFrostbite   = 0x15D4C80;  // vtable base
static const DWORD kSlot_Precache       = 2;           // slot index

// Asset string RVAs — all confirmed in IDA strings window
static const DWORD kAssetRVAs[] = {
    // models
    0x15D4FE4,  // models/ef_frostbite_Amode.mdl
    0x15D5004,  // models/ef_frostbite_skill.mdl
    0x15D5048,  // models/ef_frostbite_shield02.mdl
    0x15D506C,  // models/ef_frostbite_shield.mdl
    0x15D50AC,  // models/ef_frostbite_shoot_c.mdl
    0x15D50CC,  // models/ef_frostbite_shoot_c02.mdl
    // sounds
    0x15D5110,  // weapons/frostbite_idle.wav
    0x15D512C,  // weapons/frostbite-1.wav
    0x15D5144,  // weapons/frostbite-2.wav
    0x15D515C,  // weapons/frostbite-3.wav
    0x15D5174,  // weapons/frostbite-1_exp1.wav
    0x15D5194,  // weapons/frostbite-1_exp2.wav
    0x15D51B4,  // weapons/frostbite-2_exp.wav
    0x15D51D0,  // weapons/frostbite_fx_loop.wav
    0x15D51F0,  // weapons/frostbite_fx_loop_end.wav
    0x15D5214,  // weapons/frostbite_fx_exp.wav
    // sprites
    0x15D4FC4,  // sprites/ef_frostbite_charge.spr
    0x15D5024,  // sprites/ef_frostbite_debuff_exp.spr
    0x15D508C,  // sprites/ef_frostbite_laser.spr
    0x15D50F0,  // sprites/frostbite_attack.spr
    // event
    0x15D4FB0,  // events/frostbite.sc
};
static const int kAssetCount = sizeof(kAssetRVAs) / sizeof(kAssetRVAs[0]);

// ---------------------------------------------------------------------------
// Engine function typedefs (from engfuncs_t indices, standard HLSDK layout)
// ---------------------------------------------------------------------------
typedef int  (__cdecl *pfnPrecacheModel_t)(const char*);
typedef int  (__cdecl *pfnPrecacheSound_t)(const char*);
typedef int  (__cdecl *pfnPrecacheGeneric_t)(const char*);

// engfuncs indices (standard HLSDK enginefuncs_s):
//  0  = pfnPrecacheModel
//  1  = pfnPrecacheSound
//  24 = pfnPrecacheGeneric  (sprites, events)
static pfnPrecacheModel_t   g_PrecacheModel   = nullptr;
static pfnPrecacheSound_t   g_PrecacheSound   = nullptr;
static pfnPrecacheGeneric_t g_PrecacheGeneric = nullptr;

// Base address of mp.dll — set once at init
static uintptr_t g_mpBase = 0;

// ---------------------------------------------------------------------------
// Our Precache replacement — called as __thiscall (ecx = this, ignored)
// No C++ objects here so __try is safe if needed, but we don't need it.
// ---------------------------------------------------------------------------

static void __fastcall FrostbitePrecache(void* /*thisptr*/, void* /*edx_unused*/)
{
    Log("[FB-Precache] FrostbitePrecache called\n");

    if (!g_mpBase) {
        Log("[FB-Precache] ERROR: g_mpBase not set\n");
        return;
    }

    for (int i = 0; i < kAssetCount; ++i) {
        const char* path = reinterpret_cast<const char*>(g_mpBase + kAssetRVAs[i]);
        Log("[FB-Precache]   [%2d] %s\n", i, path);

        // Route to correct precache function based on prefix
        if (strncmp(path, "models/", 7) == 0) {
            if (g_PrecacheModel) g_PrecacheModel(path);
        } else if (strncmp(path, "weapons/", 8) == 0 ||
                   strncmp(path, "sound/",   6) == 0) {
            if (g_PrecacheSound) g_PrecacheSound(path);
        } else {
            // sprites/, events/ -> PrecacheGeneric
            if (g_PrecacheGeneric) g_PrecacheGeneric(path);
        }
    }

    Log("[FB-Precache] done\n");
}

// ---------------------------------------------------------------------------
// Plain helpers — __try allowed (no C++ objects)
// ---------------------------------------------------------------------------

static bool SafeWritePtr(uintptr_t addr, uintptr_t newVal, uintptr_t& oldVal)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 4, PAGE_EXECUTE_READWRITE, &old))
        return false;
    __try {
        oldVal = *reinterpret_cast<uintptr_t*>(addr);
        *reinterpret_cast<uintptr_t*>(addr) = newVal;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect(reinterpret_cast<void*>(addr), 4, old, &old);
        return false;
    }
    VirtualProtect(reinterpret_cast<void*>(addr), 4, old, &old);
    return true;
}

static bool ReadU32(uintptr_t addr, uint32_t& out)
{
    __try {
        out = *reinterpret_cast<uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// FrostbiteFix_Init — no C++ objects, __try is fine
// ---------------------------------------------------------------------------

bool FrostbiteFix_Init(HMODULE hMp)
{
    Log("\n=== FrostbiteFix_Init === mp.dll=0x%08X\n", (uintptr_t)hMp);

    if (!GiveFnptrs_Init(hMp)) {
        Log("[FB] FATAL: GiveFnptrs_Init failed\n");
        return false;
    }

    g_mpBase = reinterpret_cast<uintptr_t>(hMp);

    // Grab precache functions from engfuncs (indices 0, 1, 24)
    auto** ef = reinterpret_cast<void**>(&g_engfuncs);
    g_PrecacheModel   = reinterpret_cast<pfnPrecacheModel_t>(ef[0]);
    g_PrecacheSound   = reinterpret_cast<pfnPrecacheSound_t>(ef[1]);
    g_PrecacheGeneric = reinterpret_cast<pfnPrecacheGeneric_t>(ef[24]);

    Log("[FB] pfnPrecacheModel   [0] = 0x%08zX\n", reinterpret_cast<uintptr_t>(g_PrecacheModel));
    Log("[FB] pfnPrecacheSound   [1] = 0x%08zX\n", reinterpret_cast<uintptr_t>(g_PrecacheSound));
    Log("[FB] pfnPrecacheGeneric[24] = 0x%08zX\n", reinterpret_cast<uintptr_t>(g_PrecacheGeneric));

    // Patch vtable slot 2 (Precache) of CFrostbite
    // vtable is at mp.dll + kVtable_CFrostbite
    // slot 2 is at vtable + 2*4 = vtable + 8
    uintptr_t vtableAddr  = g_mpBase + kVtable_CFrostbite;
    uintptr_t slotAddr    = vtableAddr + kSlot_Precache * sizeof(uintptr_t);
    uintptr_t hookAddr    = reinterpret_cast<uintptr_t>(&FrostbitePrecache);
    uintptr_t oldVal      = 0;

    uint32_t currentSlot  = 0;
    ReadU32(slotAddr, currentSlot);
    Log("[FB] CFrostbite vtable @ 0x%08zX\n", vtableAddr);
    Log("[FB] Slot[2] (Precache) @ 0x%08zX  current=0x%08X\n", slotAddr, currentSlot);

    if (SafeWritePtr(slotAddr, hookAddr, oldVal)) {
        Log("[FB] Patched Precache slot: 0x%08zX -> 0x%08zX\n", oldVal, hookAddr);
    } else {
        Log("[FB] FAILED to patch Precache slot!\n");
        return false;
    }

    Log("=== FrostbiteFix_Init COMPLETE ===\n");
    return true;
}
