// frostbite_fix.cpp  v11
//
// ROOT CAUSE (confirmed via IDA vtable diff + slot dump):
//   CFrostbite has all its logic functions correctly compiled into mp.dll,
//   but several vtable slots are wired to stubs instead of the real functions.
//
// CONFIRMED SLOT MAP (client mp.dll, imagebase 0x10000000):
//   CFrostbite::vftable @ RVA 0x15D4C80
//   Slot 165 = WeaponIdle       sub_10B30C80
//   Slot 166 = Think            sub_10B31100
//   Slot 167 = PrimaryAttack    sub_10B30E90
//   Slot 168 = SecondaryAttack  sub_10B30F60
//   Slot 175 = AddToPlayer      sub_10B30FE0
//   Slot 191 = Holster          sub_10B31040
//   Slot 193 = TakeDamage/OnHit sub_10B31060
//   Slot 194 = prop Think       sub_10B312C0
//
// THE FIX:
//   The functions already exist in mp.dll. We just patch each vtable slot
//   to point to the correct function. No reimplementation needed.

#include <windows.h>
#include <cstdint>
#include "logger.h"

// ---------------------------------------------------------------------------
// CFrostbite vtable RVA and slot/function definitions
// ---------------------------------------------------------------------------
static const DWORD kRVA_Vtable = 0x15D4C80;

struct SlotPatch {
    int   slot;
    DWORD fnRVA;
    const char* name;
};

static const SlotPatch kPatches[] = {
    { 165, 0xB30C80, "WeaponIdle"      },
    { 166, 0xB31100, "Think"           },
    { 167, 0xB30E90, "PrimaryAttack"   },
    { 168, 0xB30F60, "SecondaryAttack" },
    { 175, 0xB30FE0, "AddToPlayer"     },
    { 191, 0xB31040, "Holster"         },
    { 193, 0xB31060, "TakeDamage"      },
    { 194, 0xB312C0, "PropThink"       },
};
static const int kPatchCount = (int)(sizeof(kPatches) / sizeof(kPatches[0]));

static bool g_patchDone = false;

// ---------------------------------------------------------------------------
// Helpers — plain functions, no C++ objects, safe for __try
// ---------------------------------------------------------------------------
static bool SafeWritePtr(uintptr_t addr, uintptr_t value)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 4, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[FB] VirtualProtect failed @ 0x%08zX err=%u\n", addr, GetLastError());
        return false;
    }
    __try {
        *reinterpret_cast<uintptr_t*>(addr) = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect(reinterpret_cast<void*>(addr), 4, old, &old);
        return false;
    }
    VirtualProtect(reinterpret_cast<void*>(addr), 4, old, &old);
    return true;
}

static bool SafeReadPtr(uintptr_t addr, uintptr_t& out)
{
    __try {
        out = *reinterpret_cast<uintptr_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// FrostbiteFix_Init
// ---------------------------------------------------------------------------
bool FrostbiteFix_Init(HMODULE hMp)
{
    Log("\n=== FrostbiteFix_Init === mp.dll=0x%08X\n", (uintptr_t)hMp);

    if (g_patchDone) {
        Log("[FB] Already patched — skipping\n");
        return true;
    }

    uintptr_t base       = reinterpret_cast<uintptr_t>(hMp);
    uintptr_t vtableAddr = base + kRVA_Vtable;

    // Sanity check — read slot 0
    uintptr_t slot0 = 0;
    if (!SafeReadPtr(vtableAddr, slot0)) {
        Log("[FB] FATAL: cannot read vtable @ 0x%08zX\n", vtableAddr);
        return false;
    }
    Log("[FB] CFrostbite vtable @ 0x%08zX  slot[0]=0x%08zX\n", vtableAddr, slot0);

    int ok = 0, fail = 0;
    for (int i = 0; i < kPatchCount; i++) {
        const SlotPatch& p = kPatches[i];
        uintptr_t slotAddr = vtableAddr + p.slot * 4;
        uintptr_t fnAddr   = base + p.fnRVA;
        uintptr_t current  = 0;
        SafeReadPtr(slotAddr, current);

        if (current == fnAddr) {
            Log("[FB]   Slot %3d (%s): already correct\n", p.slot, p.name);
            ++ok;
            continue;
        }

        if (SafeWritePtr(slotAddr, fnAddr)) {
            Log("[FB]   Slot %3d (%s): 0x%08zX -> 0x%08zX OK\n",
                p.slot, p.name, current, fnAddr);
            ++ok;
        } else {
            Log("[FB]   Slot %3d (%s): FAILED\n", p.slot, p.name);
            ++fail;
        }
    }

    Log("[FB] %d patched OK, %d failed\n", ok, fail);
    g_patchDone = true;
    Log("=== FrostbiteFix_Init COMPLETE ===\n");
    return fail == 0;
}
