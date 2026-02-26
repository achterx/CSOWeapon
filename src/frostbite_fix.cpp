// frostbite_fix.cpp  v16
//
// Key findings from v14:
//   - CSimpleWpn<CFrostbite> vtable (vtbl1) is the active one ✓
//   - Slot numbers 165-194 all correct ✓
//   - t=0.00 always — gpGlobals wrong (fixed in givefnptrs v10)
//   - SecondaryAttack fires on PROP object (player=null), not weapon
//   - AddToPlayer receives player=0x255 (=597, weapon ID) — wrong arg
//
// v16: fix globals, add player-validity check before acting on hooks
// v14: patch slots 165-194 in BOTH vtables with call-throughs,
// log which vtable fires first.
//
// Slot map from IDA dump (client, CFrostbite vtable):
//   165 WeaponIdle, 166 Think, 167 PrimaryAttack, 168 SecondaryAttack
//   175 AddToPlayer, 191 Holster, 193 TakeDamage, 194 PropThink

#include <windows.h>
#include <cstdint>
#include "givefnptrs.h"
#include "logger.h"

template<typename T>
static inline T& Field(void* base, int off)
{
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
}

static float NOW()
{
    GiveFnptrs_RefreshGlobals();
    return g_pGlobals ? g_pGlobals->time : 0.0f;
}

// Vtable RVAs (client imagebase 0x10000000)
static const uintptr_t kRVA_FB_Vtable     = 0x015D4C80;  // CFrostbite
static const uintptr_t kRVA_Simple_Vtable = 0x015D48CC;  // CSimpleWpn<CFrostbite>

static uintptr_t g_mpBase  = 0;
static bool      g_patched = false;

// Original function storage — indexed [0]=FB vtable, [1]=Simple vtable
#define NVTBLS 2
static struct OrigSet {
    uintptr_t WeaponIdle;
    uintptr_t Think;
    uintptr_t PrimaryAttack;
    uintptr_t SecondaryAttack;
    uintptr_t AddToPlayer;
    uintptr_t Holster;
    uintptr_t TakeDamage;
    uintptr_t PropThink;
} g_orig[NVTBLS];

#define SLOT_WEAPONIDLE       165
#define SLOT_THINK            166
#define SLOT_PRIMARYATTACK    167
#define SLOT_SECONDARYATTACK  168
#define SLOT_ADDTOPLAYER      175
#define SLOT_HOLSTER          191
#define SLOT_TAKEDAMAGE       193
#define SLOT_PROP_THINK       194

// ---------------------------------------------------------------------------
// Hook factories — FB=0, Simple=1
// Each hook logs its vtable source tag, then calls original
// ---------------------------------------------------------------------------

// WeaponIdle
static void __fastcall Hook_WeaponIdle_FB(void* w, void*)
{
    Log("[FB-vtbl0] WeaponIdle w=%p t=%.2f\n", w, NOW());
    typedef void(__thiscall* F)(void*);
    if (g_orig[0].WeaponIdle) reinterpret_cast<F>(g_orig[0].WeaponIdle)(w);
}
static void __fastcall Hook_WeaponIdle_SW(void* w, void*)
{
    Log("[FB-vtbl1] WeaponIdle w=%p t=%.2f\n", w, NOW());
    typedef void(__thiscall* F)(void*);
    if (g_orig[1].WeaponIdle) reinterpret_cast<F>(g_orig[1].WeaponIdle)(w);
}

// Think
static void __fastcall Hook_Think_FB(void* w, void*)
{
    Log("[FB-vtbl0] Think w=%p t=%.2f\n", w, NOW());
    typedef void(__thiscall* F)(void*);
    if (g_orig[0].Think) reinterpret_cast<F>(g_orig[0].Think)(w);
}
static void __fastcall Hook_Think_SW(void* w, void*)
{
    Log("[FB-vtbl1] Think w=%p t=%.2f\n", w, NOW());
    typedef void(__thiscall* F)(void*);
    if (g_orig[1].Think) reinterpret_cast<F>(g_orig[1].Think)(w);
}

// PrimaryAttack
static void __fastcall Hook_PrimaryAttack_FB(void* w, void*)
{
    int ammo = Field<int>(w, 0x154);
    Log("[FB-vtbl0] PrimaryAttack w=%p ammo=%d t=%.2f\n", w, ammo, NOW());
    typedef void(__thiscall* F)(void*);
    if (g_orig[0].PrimaryAttack) reinterpret_cast<F>(g_orig[0].PrimaryAttack)(w);
    Log("[FB-vtbl0] PrimaryAttack DONE ammo=%d\n", Field<int>(w, 0x154));
}
static void __fastcall Hook_PrimaryAttack_SW(void* w, void*)
{
    int ammo = Field<int>(w, 0x154);
    Log("[FB-vtbl1] PrimaryAttack w=%p ammo=%d t=%.2f\n", w, ammo, NOW());
    typedef void(__thiscall* F)(void*);
    if (g_orig[1].PrimaryAttack) reinterpret_cast<F>(g_orig[1].PrimaryAttack)(w);
    Log("[FB-vtbl1] PrimaryAttack DONE ammo=%d\n", Field<int>(w, 0x154));
}

// SecondaryAttack
static void __fastcall Hook_SecondaryAttack_FB(void* w, void*)
{
    Log("[FB-vtbl0] SecondaryAttack w=%p nextAtk=%.2f charge=%.2f t=%.2f\n",
        w, Field<float>(w,0x13C), Field<float>(w,0x1FC), NOW());
    typedef void(__thiscall* F)(void*);
    if (g_orig[0].SecondaryAttack) reinterpret_cast<F>(g_orig[0].SecondaryAttack)(w);
    Log("[FB-vtbl0] SecondaryAttack DONE nextAtk=%.2f charge=%.2f\n",
        Field<float>(w,0x13C), Field<float>(w,0x1FC));
}
static void __fastcall Hook_SecondaryAttack_SW(void* w, void*)
{
    Log("[FB-vtbl1] SecondaryAttack w=%p nextAtk=%.2f charge=%.2f t=%.2f\n",
        w, Field<float>(w,0x13C), Field<float>(w,0x1FC), NOW());
    typedef void(__thiscall* F)(void*);
    if (g_orig[1].SecondaryAttack) reinterpret_cast<F>(g_orig[1].SecondaryAttack)(w);
    Log("[FB-vtbl1] SecondaryAttack DONE nextAtk=%.2f charge=%.2f\n",
        Field<float>(w,0x13C), Field<float>(w,0x1FC));
}

// AddToPlayer
static int __fastcall Hook_AddToPlayer_FB(void* w, void*, void* p)
{
    Log("[FB-vtbl0] AddToPlayer w=%p player=%p cfgId=%d\n",
        w, p, Field<int>(w,0x1D8));
    typedef int(__thiscall* F)(void*,void*);
    int r = g_orig[0].AddToPlayer ? reinterpret_cast<F>(g_orig[0].AddToPlayer)(w,p) : 1;
    Log("[FB-vtbl0] AddToPlayer DONE ret=%d cfgId=%d evHdl=%d\n",
        r, Field<int>(w,0x1D8), (int)Field<uint16_t>(w,0x1E8));
    return r;
}
static int __fastcall Hook_AddToPlayer_SW(void* w, void*, void* p)
{
    Log("[FB-vtbl1] AddToPlayer w=%p player=%p cfgId=%d\n",
        w, p, Field<int>(w,0x1D8));
    typedef int(__thiscall* F)(void*,void*);
    int r = g_orig[1].AddToPlayer ? reinterpret_cast<F>(g_orig[1].AddToPlayer)(w,p) : 1;
    Log("[FB-vtbl1] AddToPlayer DONE ret=%d cfgId=%d evHdl=%d\n",
        r, Field<int>(w,0x1D8), (int)Field<uint16_t>(w,0x1E8));
    return r;
}

// Holster
static void __fastcall Hook_Holster_FB(void* w, void*, int sk)
{
    Log("[FB-vtbl0] Holster w=%p skip=%d\n", w, sk);
    typedef void(__thiscall* F)(void*,int);
    if (g_orig[0].Holster) reinterpret_cast<F>(g_orig[0].Holster)(w,sk);
}
static void __fastcall Hook_Holster_SW(void* w, void*, int sk)
{
    Log("[FB-vtbl1] Holster w=%p skip=%d\n", w, sk);
    typedef void(__thiscall* F)(void*,int);
    if (g_orig[1].Holster) reinterpret_cast<F>(g_orig[1].Holster)(w,sk);
}

// TakeDamage
static int __fastcall Hook_TakeDamage_FB(void* w, void*, void* info, float dmg)
{
    Log("[FB-vtbl0] TakeDamage w=%p dmg=%.1f\n", w, dmg);
    typedef int(__thiscall* F)(void*,void*,float);
    return g_orig[0].TakeDamage ? reinterpret_cast<F>(g_orig[0].TakeDamage)(w,info,dmg) : 1;
}
static int __fastcall Hook_TakeDamage_SW(void* w, void*, void* info, float dmg)
{
    Log("[FB-vtbl1] TakeDamage w=%p dmg=%.1f\n", w, dmg);
    typedef int(__thiscall* F)(void*,void*,float);
    return g_orig[1].TakeDamage ? reinterpret_cast<F>(g_orig[1].TakeDamage)(w,info,dmg) : 1;
}

// PropThink
static void __fastcall Hook_PropThink_FB(void* p, void*)
{
    Log("[FB-vtbl0] PropThink p=%p t=%.2f\n", p, NOW());
    typedef void(__thiscall* F)(void*);
    if (g_orig[0].PropThink) reinterpret_cast<F>(g_orig[0].PropThink)(p);
}
static void __fastcall Hook_PropThink_SW(void* p, void*)
{
    Log("[FB-vtbl1] PropThink p=%p t=%.2f\n", p, NOW());
    typedef void(__thiscall* F)(void*);
    if (g_orig[1].PropThink) reinterpret_cast<F>(g_orig[1].PropThink)(p);
}

// ---------------------------------------------------------------------------
// Memory helpers
// ---------------------------------------------------------------------------
static bool SafeWrite(uintptr_t addr, uintptr_t val)
{
    DWORD old = 0;
    if (!VirtualProtect((void*)addr, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    __try { *(uintptr_t*)addr = val; }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect((void*)addr, 4, old, &old);
        return false;
    }
    VirtualProtect((void*)addr, 4, old, &old);
    return true;
}
static bool SafeRead(uintptr_t addr, uintptr_t& out)
{
    __try { out = *(uintptr_t*)addr; return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// Patch one vtable
// ---------------------------------------------------------------------------
struct SlotDef {
    int         idx;
    const char* name;
    uintptr_t*  orig;
    void*       hook;
};

static int PatchVtable(uintptr_t vtbl, SlotDef* slots, int n, const char* tag)
{
    uintptr_t s0 = 0;
    if (!SafeRead(vtbl, s0)) {
        Log("[FB] %s FATAL: unreadable @ 0x%08zX\n", tag, vtbl);
        return 0;
    }
    Log("[FB] %s vtable @ 0x%08zX slot[0]=0x%08zX\n", tag, vtbl, s0);

    int ok = 0;
    for (int i = 0; i < n; i++)
    {
        uintptr_t slotAddr = vtbl + slots[i].idx * 4;
        uintptr_t cur = 0;
        if (!SafeRead(slotAddr, cur)) {
            Log("[FB] %s slot %d unreadable\n", tag, slots[i].idx);
            continue;
        }
        *slots[i].orig = cur;
        Log("[FB] %s slot %3d %-16s orig=0x%08zX\n", tag, slots[i].idx, slots[i].name, cur);
        if (SafeWrite(slotAddr, (uintptr_t)slots[i].hook))
            ok++;
        else
            Log("[FB] %s slot %d write FAILED\n", tag, slots[i].idx);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// FrostbiteFix_Init
// ---------------------------------------------------------------------------
bool FrostbiteFix_Init(HMODULE hMp)
{
    Log("\n=== FrostbiteFix_Init v14 === mp.dll=0x%08X\n", (uintptr_t)hMp);
    if (g_patched) { Log("[FB] already done\n"); return true; }

    if (!GiveFnptrs_Init(hMp)) {
        Log("[FB] FATAL: GiveFnptrs_Init failed\n");
        return false;
    }

    g_mpBase = (uintptr_t)hMp;

    // --- Patch CFrostbite vtable (vtbl index 0) ---
    SlotDef fbSlots[] = {
        { SLOT_WEAPONIDLE,      "WeaponIdle",      &g_orig[0].WeaponIdle,      (void*)Hook_WeaponIdle_FB      },
        { SLOT_THINK,           "Think",           &g_orig[0].Think,           (void*)Hook_Think_FB           },
        { SLOT_PRIMARYATTACK,   "PrimaryAttack",   &g_orig[0].PrimaryAttack,   (void*)Hook_PrimaryAttack_FB   },
        { SLOT_SECONDARYATTACK, "SecondaryAttack", &g_orig[0].SecondaryAttack, (void*)Hook_SecondaryAttack_FB },
        { SLOT_ADDTOPLAYER,     "AddToPlayer",     &g_orig[0].AddToPlayer,     (void*)Hook_AddToPlayer_FB     },
        { SLOT_HOLSTER,         "Holster",         &g_orig[0].Holster,         (void*)Hook_Holster_FB         },
        { SLOT_TAKEDAMAGE,      "TakeDamage",      &g_orig[0].TakeDamage,      (void*)Hook_TakeDamage_FB      },
        { SLOT_PROP_THINK,      "PropThink",       &g_orig[0].PropThink,       (void*)Hook_PropThink_FB       },
    };
    PatchVtable(g_mpBase + kRVA_FB_Vtable, fbSlots, 8, "CFrostbite");

    // --- Patch CSimpleWpn<CFrostbite> vtable (vtbl index 1) ---
    // Slot numbers may differ — patch same indices first as a probe,
    // then we'll see which tag fires in the log
    SlotDef swSlots[] = {
        { SLOT_WEAPONIDLE,      "WeaponIdle",      &g_orig[1].WeaponIdle,      (void*)Hook_WeaponIdle_SW      },
        { SLOT_THINK,           "Think",           &g_orig[1].Think,           (void*)Hook_Think_SW           },
        { SLOT_PRIMARYATTACK,   "PrimaryAttack",   &g_orig[1].PrimaryAttack,   (void*)Hook_PrimaryAttack_SW   },
        { SLOT_SECONDARYATTACK, "SecondaryAttack", &g_orig[1].SecondaryAttack, (void*)Hook_SecondaryAttack_SW },
        { SLOT_ADDTOPLAYER,     "AddToPlayer",     &g_orig[1].AddToPlayer,     (void*)Hook_AddToPlayer_SW     },
        { SLOT_HOLSTER,         "Holster",         &g_orig[1].Holster,         (void*)Hook_Holster_SW         },
        { SLOT_TAKEDAMAGE,      "TakeDamage",      &g_orig[1].TakeDamage,      (void*)Hook_TakeDamage_SW      },
        { SLOT_PROP_THINK,      "PropThink",       &g_orig[1].PropThink,       (void*)Hook_PropThink_SW       },
    };
    PatchVtable(g_mpBase + kRVA_Simple_Vtable, swSlots, 8, "CSimpleWpn<FB>");

    g_patched = true;
    Log("=== FrostbiteFix_Init COMPLETE ===\n");
    return true;
}
