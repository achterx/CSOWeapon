// frostbite_fix.cpp  v18
//
// Hook the weapon_frostbite entity factory (sub_10B3AD50).
// This is called by the engine every time it spawns a weapon_frostbite entity.
// We intercept it, call the original, then fix up the resulting object.
//
// From IDA decompile of weapon_frostbite (sub_10B3AD50):
//   void __cdecl weapon_frostbite(int edict)
//   {
//     if (!edict) edict = CreateEntityByName() + 132
//     v3 = *(edict + 568)   // entvars_t*
//     if (!v3 || !*(v3+128))
//     {
//       obj = ALLOC(v3, 536)          // dword_11E51988 = pfnPvAllocEntPrivateData
//       if (obj)
//         *(sub_10B31450(obj) + 8) = edict  // construct CFrostbite, link edict
//     }
//   }
//
// sub_10B31450 = CFrostbite constructor (returns this ptr)
// Object size = 536 bytes
// Object+8 = edict ptr
//
// The factory table at RVA 0x1856C48 holds a ptr to weapon_frostbite.
// We patch that ptr to point to our hook.
//
// gpGlobals: dword_11E51BCC is a pointer to gpGlobals struct.
// gpGlobals->time is at offset 0 of that struct.
// So: time = *(float*)(*(uint32_t*)(mp + 0x1E51BCC))
//
// SecondaryAttack (sub_10B30F60) uses *(float*)dword_11E51BCC for time.
// This means dword_11E51BCC stores a float* that points to gpGlobals->time.
// We read it fresh on every call.

#include <windows.h>
#include <cstdint>
#include <cstring>
#include "givefnptrs.h"
#include "logger.h"

template<typename T>
static inline T& F(void* base, int off)
{
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
}

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static uintptr_t g_mpBase  = 0;
static bool      g_patched = false;

// RVAs from IDA
static const uintptr_t RVA_factory_fn    = 0x0B3AD50;  // weapon_frostbite()
static const uintptr_t RVA_factory_table = 0x1856C48;  // table entry ptr to fn
static const uintptr_t RVA_factory_tab2  = 0x18595A8;  // second table entry
static const uintptr_t RVA_pTimePtr      = 0x1E51BCC;  // dword holding float* to time
static const uintptr_t RVA_vtbl_simple   = 0x15D48CC;  // CSimpleWpn<CFrostbite> vtable
static const uintptr_t RVA_SecondaryAtk  = 0x0B30F60;  // sub_10B30F60
static const uintptr_t RVA_WTB           = 0x058A0B0;  // UTIL_WeaponTimeBase

typedef float(__cdecl* pfnWTB_t)();
static pfnWTB_t g_WTB = nullptr;

static float NOW()
{
    // *(float*)(*(uint32_t*)(mp + RVA_pTimePtr))
    uintptr_t addr = g_mpBase + RVA_pTimePtr;
    uint32_t inner = 0;
    __try { inner = *reinterpret_cast<uint32_t*>(addr); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
    if (!inner) return 0.0f;
    float t = 0.0f;
    __try { t = *reinterpret_cast<float*>((uintptr_t)inner); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return 0.0f; }
    return t;
}

// ---------------------------------------------------------------------------
// Original factory function (called via trampoline)
// ---------------------------------------------------------------------------
typedef void(__cdecl* pfnFactory_t)(int);
static pfnFactory_t g_origFactory = nullptr;

// Trampoline bytes (first 5 bytes of original fn, then JMP back)
static uint8_t g_trampoline[16] = {};

// ---------------------------------------------------------------------------
// SecondaryAttack hook — only patches the cooldown time source
// Original sub_10B30F60 uses *(float*)dword_11E51BCC for time comparison.
// If that ptr is valid, SecondaryAttack works natively. We don't need to
// reimplement it — we just need time to be non-zero when it runs.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Our weapon_frostbite factory hook
// Called instead of the real weapon_frostbite(int edict)
// ---------------------------------------------------------------------------
static void __cdecl Hook_WeaponFrostbiteFactory(int edict)
{
    Log("[FB] Hook_WeaponFrostbiteFactory edict=%d t=%.3f\n", edict, NOW());

    // Call original via trampoline
    if (g_origFactory)
        g_origFactory(edict);

    Log("[FB] Factory complete\n");
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------
static bool SafeWrite(uintptr_t addr, const void* data, size_t len)
{
    DWORD old = 0;
    if (!VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy((void*)addr, data, len);
    VirtualProtect((void*)addr, len, old, &old);
    return true;
}

static bool SafeRead32(uintptr_t addr, uint32_t& out)
{
    __try { out = *reinterpret_cast<uint32_t*>(addr); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool SafeWrite32(uintptr_t addr, uint32_t val)
{
    DWORD old = 0;
    if (!VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    __try { *reinterpret_cast<uint32_t*>(addr) = val; }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect((void*)addr, 4, old, &old);
        return false;
    }
    VirtualProtect((void*)addr, 4, old, &old);
    return true;
}

// Write a 32-bit relative JMP at `from` to `to`
static bool WriteJmp(uintptr_t from, uintptr_t to)
{
    uint8_t jmp[5];
    jmp[0] = 0xE9;
    *reinterpret_cast<int32_t*>(jmp + 1) = (int32_t)(to - from - 5);
    return SafeWrite(from, jmp, 5);
}

// ---------------------------------------------------------------------------
// FrostbiteFix_Init
// ---------------------------------------------------------------------------
bool FrostbiteFix_Init(HMODULE hMp)
{
    Log("\n=== FrostbiteFix_Init v18 === mp.dll=0x%08X t=%.3f\n",
        (uintptr_t)hMp, NOW());

    if (g_patched) { Log("[FB] already patched\n"); return true; }

    if (!GiveFnptrs_Init(hMp)) {
        Log("[FB] GiveFnptrs_Init failed\n");
        return false;
    }

    g_mpBase = (uintptr_t)hMp;
    g_WTB    = (pfnWTB_t)(g_mpBase + RVA_WTB);

    float t = NOW();
    Log("[FB] NOW()=%.3f  WTB()=%.3f\n", t, g_WTB());

    // --- Patch factory table entry ---
    // The table at RVA_factory_table holds a ptr to weapon_frostbite fn.
    // We replace that ptr with our hook.
    uintptr_t factoryFn   = g_mpBase + RVA_factory_fn;
    uintptr_t tableEntry1 = g_mpBase + RVA_factory_table;
    uintptr_t tableEntry2 = g_mpBase + RVA_factory_tab2;

    uint32_t cur1 = 0, cur2 = 0;
    SafeRead32(tableEntry1, cur1);
    SafeRead32(tableEntry2, cur2);
    Log("[FB] factory table[0] @ 0x%08zX = 0x%08X (expected 0x%08zX)\n",
        tableEntry1, cur1, factoryFn);
    Log("[FB] factory table[1] @ 0x%08zX = 0x%08X\n", tableEntry2, cur2);

    // Build trampoline: copy first 5 bytes of original fn, then JMP back
    uint8_t orig5[5];
    __try { memcpy(orig5, (void*)factoryFn, 5); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[FB] FATAL: cannot read factory fn bytes\n");
        return false;
    }
    Log("[FB] factory fn first bytes: %02X %02X %02X %02X %02X\n",
        orig5[0], orig5[1], orig5[2], orig5[3], orig5[4]);

    // Allocate executable trampoline
    void* trampolineMem = VirtualAlloc(nullptr, 32,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampolineMem) {
        Log("[FB] FATAL: VirtualAlloc for trampoline failed\n");
        return false;
    }
    // trampoline: [orig 5 bytes] [JMP factoryFn+5]
    uint8_t* tb = reinterpret_cast<uint8_t*>(trampolineMem);
    memcpy(tb, orig5, 5);
    tb[5] = 0xE9;
    *reinterpret_cast<int32_t*>(tb + 6) =
        (int32_t)((factoryFn + 5) - ((uintptr_t)tb + 5 + 5));

    g_origFactory = reinterpret_cast<pfnFactory_t>(trampolineMem);
    Log("[FB] trampoline @ %p\n", trampolineMem);

    // Patch the factory fn with JMP to our hook
    if (WriteJmp(factoryFn, (uintptr_t)Hook_WeaponFrostbiteFactory)) {
        Log("[FB] factory fn patched with JMP to hook\n");
    } else {
        Log("[FB] FAILED to patch factory fn\n");
        VirtualFree(trampolineMem, 0, MEM_RELEASE);
        return false;
    }

    // Also patch the table entries to point directly to our hook
    // (in case engine uses the table ptr directly, not the fn)
    if (cur1 == (uint32_t)factoryFn) {
        SafeWrite32(tableEntry1, (uint32_t)(uintptr_t)Hook_WeaponFrostbiteFactory);
        Log("[FB] table[0] patched\n");
    }
    if (cur2 == (uint32_t)factoryFn) {
        SafeWrite32(tableEntry2, (uint32_t)(uintptr_t)Hook_WeaponFrostbiteFactory);
        Log("[FB] table[1] patched\n");
    }

    g_patched = true;
    Log("=== FrostbiteFix_Init COMPLETE ===\n");
    return true;
}