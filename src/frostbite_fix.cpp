// frostbite_fix.cpp  v9
//
// ROOT CAUSE (confirmed via mp.dll analysis):
//   sub_5897F0 (the CFrostbite Precache function called from the CSNZ weapon
//   descriptor table via sub_802AE0) is a stub: C2 08 00 = RETN 8.
//   It never calls pfnPrecacheModel/Sound for any of the 21 frostbite assets.
//   sub_802AE0 calls this stub directly (not through the vtable), so patching
//   the vtable has no effect.
//
// FIX:
//   Overwrite the stub at mp.dll+0x5897F0 with a JMP to our FrostbitePrecache.
//   The stub has 16 bytes total (3 bytes RETN8 + 13 bytes CC padding), giving us
//   room for a full absolute JMP trampoline (PUSH addr; RET = 6 bytes).
//   Our hook precaches all 21 known frostbite assets then does RETN 8 itself.
//
// All RVAs confirmed from IDA (imagebase 0x10000000).

#include <windows.h>
#include <cstdint>
#include <cstring>
#include "givefnptrs.h"
#include "logger.h"

// ---------------------------------------------------------------------------
// Confirmed RVAs from IDA
// ---------------------------------------------------------------------------
static const DWORD kRVA_PrecacheStub = 0x5897F0;  // C2 08 00 stub, called from sub_802AE0

// Asset RVAs (all confirmed in IDA strings window)
static const DWORD kAssetRVAs[] = {
    0x15D4FE4,  // models/ef_frostbite_Amode.mdl
    0x15D5004,  // models/ef_frostbite_skill.mdl
    0x15D5048,  // models/ef_frostbite_shield02.mdl
    0x15D506C,  // models/ef_frostbite_shield.mdl
    0x15D50AC,  // models/ef_frostbite_shoot_c.mdl
    0x15D50CC,  // models/ef_frostbite_shoot_c02.mdl
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
    0x15D4FC4,  // sprites/ef_frostbite_charge.spr
    0x15D5024,  // sprites/ef_frostbite_debuff_exp.spr
    0x15D508C,  // sprites/ef_frostbite_laser.spr
    0x15D50F0,  // sprites/frostbite_attack.spr
    0x15D4FB0,  // events/frostbite.sc
};
static const int kAssetCount = (int)(sizeof(kAssetRVAs) / sizeof(kAssetRVAs[0]));

// ---------------------------------------------------------------------------
// Engine precache typedefs (standard HLSDK engfuncs indices)
// ---------------------------------------------------------------------------
typedef int (__cdecl *pfnPrecacheModel_t)  (const char*);
typedef int (__cdecl *pfnPrecacheSound_t)  (const char*);
typedef int (__cdecl *pfnPrecacheGeneric_t)(const char*);

static pfnPrecacheModel_t   g_PrecacheModel   = nullptr;
static pfnPrecacheSound_t   g_PrecacheSound   = nullptr;
static pfnPrecacheGeneric_t g_PrecacheGeneric = nullptr;
static uintptr_t            g_mpBase          = 0;
static bool                 g_stubPatched     = false;

// ---------------------------------------------------------------------------
// Our Precache replacement.
// Called as __stdcall with 2 args (8 bytes) — must RETN 8.
// We use __cdecl + manual RET to keep it simple; the JMP trampoline will
// land here and we emit RETN 8 at the end via naked asm.
//
// Actually: use __declspec(naked) to control the stack frame exactly.
// ---------------------------------------------------------------------------

static void __declspec(naked) FrostbitePrecache()
{
    __asm {
        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi
    }

    // Do the actual precache work
    Log("[FB-Precache] FrostbitePrecache called — precaching %d assets\n", kAssetCount);

    for (int i = 0; i < kAssetCount; ++i) {
        const char* path = reinterpret_cast<const char*>(g_mpBase + kAssetRVAs[i]);
        Log("[FB-Precache]   [%2d] %s\n", i, path);

        if (strncmp(path, "models/", 7) == 0) {
            if (g_PrecacheModel) g_PrecacheModel(path);
        } else if (strncmp(path, "weapons/", 8) == 0) {
            if (g_PrecacheSound) g_PrecacheSound(path);
        } else {
            if (g_PrecacheGeneric) g_PrecacheGeneric(path);
        }
    }

    Log("[FB-Precache] done\n");

    __asm {
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret     8       // stdcall: pop 2 args (8 bytes) from caller's stack
    }
}

// ---------------------------------------------------------------------------
// Plain helpers — may use __try (no C++ objects)
// ---------------------------------------------------------------------------

static bool SafeWriteBytes(uintptr_t addr, const uint8_t* bytes, size_t len)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &old))
    {
        Log("[FB] VirtualProtect failed @ 0x%08zX err=%u\n", addr, GetLastError());
        return false;
    }
    __try {
        memcpy(reinterpret_cast<void*>(addr), bytes, len);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect(reinterpret_cast<void*>(addr), len, old, &old);
        return false;
    }
    VirtualProtect(reinterpret_cast<void*>(addr), len, old, &old);
    return true;
}

// ---------------------------------------------------------------------------
// FrostbiteFix_Init
// ---------------------------------------------------------------------------

bool FrostbiteFix_Init(HMODULE hMp)
{
    Log("\n=== FrostbiteFix_Init === mp.dll=0x%08X\n", (uintptr_t)hMp);

    if (!GiveFnptrs_Init(hMp)) {
        Log("[FB] FATAL: GiveFnptrs_Init failed\n");
        return false;
    }

    g_mpBase = reinterpret_cast<uintptr_t>(hMp);

    // Grab precache fns from engfuncs (indices 0=PrecacheModel, 1=PrecacheSound, 24=PrecacheGeneric)
    auto** ef = reinterpret_cast<void**>(&g_engfuncs);
    g_PrecacheModel   = reinterpret_cast<pfnPrecacheModel_t>(ef[0]);
    g_PrecacheSound   = reinterpret_cast<pfnPrecacheSound_t>(ef[1]);
    g_PrecacheGeneric = reinterpret_cast<pfnPrecacheGeneric_t>(ef[24]);

    // Sanity check: all must be non-null and 4-byte aligned
    uintptr_t pm  = reinterpret_cast<uintptr_t>(g_PrecacheModel);
    uintptr_t ps  = reinterpret_cast<uintptr_t>(g_PrecacheSound);
    uintptr_t pg  = reinterpret_cast<uintptr_t>(g_PrecacheGeneric);
    if (!pm || (pm & 3) || !ps || (ps & 3) || !pg || (pg & 3)) {
        Log("[FB] FATAL: engfuncs pointers look invalid (unaligned or null) — aborting\n");
        Log("[FB]   PrecacheModel=0x%08zX PrecacheSound=0x%08zX PrecacheGeneric=0x%08zX\n", pm, ps, pg);
        return false;
    }

    Log("[FB] pfnPrecacheModel   = 0x%08zX\n", reinterpret_cast<uintptr_t>(g_PrecacheModel));
    Log("[FB] pfnPrecacheSound   = 0x%08zX\n", reinterpret_cast<uintptr_t>(g_PrecacheSound));
    Log("[FB] pfnPrecacheGeneric = 0x%08zX\n", reinterpret_cast<uintptr_t>(g_PrecacheGeneric));

    // Overwrite the Precache stub at mp.dll+0x5897F0 with a JMP to FrostbitePrecache.
    // The stub is: C2 08 00 CC CC CC CC CC CC CC CC CC CC CC CC CC (16 bytes)
    // We write: PUSH hookAddr (5 bytes) + RET (1 byte) = absolute indirect JMP.
    // This fits in 6 bytes, well within the 16 byte stub.
    uintptr_t stubAddr = g_mpBase + kRVA_PrecacheStub;
    uintptr_t hookAddr = reinterpret_cast<uintptr_t>(&FrostbitePrecache);

    // Verify stub still looks like we expect (C2 08 00 = RETN 8)
    // If it's already been patched (starts with 0x68 = PUSH), skip.
    uint8_t existing[3] = {};
    __try {
        memcpy(existing, reinterpret_cast<void*>(stubAddr), 3);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[FB] Cannot read stub at 0x%08zX\n", stubAddr);
        return false;
    }
    Log("[FB] Stub @ 0x%08zX: %02X %02X %02X\n", stubAddr, existing[0], existing[1], existing[2]);

    if (g_stubPatched || existing[0] == 0x68 || existing[0] == 0xE9) {
        Log("[FB] Stub already patched — skipping\n");
        return true;
    }

    if (existing[0] != 0xC2) {
        Log("[FB] Stub has unexpected bytes — RVA mismatch? Aborting patch.\n");
        return false;
    }

    // Build PUSH imm32 + RETN trampoline (6 bytes)
    uint8_t trampoline[6];
    trampoline[0] = 0x68;  // PUSH imm32
    memcpy(&trampoline[1], &hookAddr, 4);
    trampoline[5] = 0xC3;  // RETN (near return = jump to pushed address)

    if (SafeWriteBytes(stubAddr, trampoline, sizeof(trampoline))) {
        g_stubPatched = true;
        Log("[FB] Patched stub @ 0x%08zX -> JMP 0x%08zX (FrostbitePrecache)\n",
            stubAddr, hookAddr);
    } else {
        Log("[FB] FAILED to patch stub!\n");
        return false;
    }

    Log("=== FrostbiteFix_Init COMPLETE ===\n");
    return true;
}
