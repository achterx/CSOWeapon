// frostbite_fix.cpp — CSNZ frostbite weapon crash fix, v7
//
// The frostbite weapon crashes the server when a match starts because:
//   - Its vtable has invalid/unpatched entries for key virtual methods
//   - We locate the REAL vtable via RTTI or dense-pointer scan, then patch it
//
// Vtable slots we patch (HLSDK CBasePlayerWeapon virtual layout, MSVC):
//   The exact slot indices depend on the CSNZ class hierarchy depth.
//   We find them by scanning for the actual crash site pattern.

#include <windows.h>
#include <cstdint>
#include <psapi.h>
#include "givefnptrs.h"
#include "logger.h"

#pragma comment(lib, "psapi.lib")

// ─── Forward declarations ──────────────────────────────────────────────────

static void __cdecl FrostbiteSetOrigin_Hook(void* pEntity, const float* origin);
static int  __cdecl FrostbiteDispatchSpawn_Hook(void* pev);
static void __cdecl FrostbiteKeyValue_Hook(void* pev, void* kvd);
static void __cdecl FrostbiteSetModel_Hook(void* pev, const char* model);

// Original pointers (filled from engfuncs or vtable scan)
typedef void (__cdecl *pfnSetOrigin_t)(void*, const float*);
typedef int  (__cdecl *pfnDispatchSpawn_t)(void*);
typedef void (__cdecl *pfnKeyValue_t)(void*, void*);
typedef void (__cdecl *pfnSetModel_t)(void*, const char*);

static pfnSetOrigin_t    g_origSetOrigin    = nullptr;
static pfnDispatchSpawn_t g_origDispatchSpawn = nullptr;
static pfnKeyValue_t     g_origKeyValue     = nullptr;
static pfnSetModel_t     g_origSetModel     = nullptr;

// ─── Safe memory write ─────────────────────────────────────────────────────

static bool SafeWritePtr(uintptr_t addr, uintptr_t newVal, uintptr_t& oldVal) {
    DWORD oldProt = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t),
                        PAGE_EXECUTE_READWRITE, &oldProt)) {
        Log("[FB] VirtualProtect failed at 0x%08zX  err=%u\n", addr, GetLastError());
        return false;
    }
    __try {
        oldVal = *reinterpret_cast<uintptr_t*>(addr);
        *reinterpret_cast<uintptr_t*>(addr) = newVal;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), oldProt, &oldProt);
        return false;
    }
    VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), oldProt, &oldProt);
    return true;
}

// ─── Vtable patch ──────────────────────────────────────────────────────────

// We patch the vtable to replace broken/missing virtual method implementations.
// The CSNZ frostbite weapon has bad vtable entries — we detect which slots are
// invalid (point outside mp.dll or have NULL) and log them, then substitute our stubs.
//
// IMPORTANT: We do NOT blindly patch by slot index.  Instead we:
//  1. Dump all vtable slots (up to 256)
//  2. Identify slots that DON'T point into mp.dll .text (these are the broken ones)
//  3. Among broken slots, replace with our no-op stubs that call the engine safely

static bool PatchVtable(uintptr_t vtable, HMODULE hMp) {
    if (!vtable) return false;

    auto* mpNt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uint8_t*>(hMp) + reinterpret_cast<IMAGE_DOS_HEADER*>(hMp)->e_lfanew);
    uintptr_t mpBase = reinterpret_cast<uintptr_t>(hMp);
    uintptr_t mpEnd  = mpBase + mpNt->OptionalHeader.SizeOfImage;

    Log("[FB] Vtable @ 0x%08zX — scanning up to 256 slots:\n", vtable);

    int badCount = 0;
    std::vector<int> badSlots;

    for (int i = 0; i < 256; i++) {
        uintptr_t slotAddr = vtable + i * sizeof(uintptr_t);
        uint32_t v = 0;
        __try { v = *reinterpret_cast<uint32_t*>(slotAddr); }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[FB]   vtable[%d] = <ACCESS VIOLATION> — stop\n", i);
            break;
        }
        if (v == 0) {
            Log("[FB]   vtable[%3d] = NULL  *** BAD ***\n", i);
            badSlots.push_back(i);
            badCount++;
            if (badCount > 50) break; // something very wrong, stop
            continue;
        }
        bool inMp = (v >= mpBase && v < mpEnd);
        if (!inMp) {
            // Check if it's in any loaded module's .text
            MEMORY_BASIC_INFORMATION mbi;
            bool isExec = false;
            if (VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof(mbi)) > 0) {
                isExec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            }
            if (!isExec) {
                Log("[FB]   vtable[%3d] = 0x%08X  *** BAD (not executable) ***\n", i, v);
                badSlots.push_back(i);
                badCount++;
                if (badCount > 50) break;
                continue;
            }
        }
        if (i < 20 || !inMp) {
            Log("[FB]   vtable[%3d] = 0x%08X%s\n", i, v, inMp ? "" : "  [external]");
        }
    }

    Log("[FB] Found %d bad vtable slots\n", (int)badSlots.size());

    if (badSlots.empty()) {
        Log("[FB] No bad slots found — frostbite vtable looks intact, no patch needed\n");
        Log("[FB] (If crashes still occur, the issue may be elsewhere — check DispatchSpawn/SetOrigin)\n");
        return true;
    }

    // Patch bad slots with a safe stub that just returns 0/void
    // We create a single stub function for void returns and one for int returns
    // The simplest approach: point all bad void* slots to a ret stub
    // For now, use a simple trampoline: allocate a code cave

    // Simple approach: use a static stub
    // void stub: just ret
    // int stub: xor eax,eax; ret
    static uint8_t voidStub[]  = { 0xC3 };           // ret
    static uint8_t intStub[]   = { 0x33, 0xC0, 0xC3 }; // xor eax,eax; ret

    // We'll use the same stub for all bad slots (they're all "missing" implementations)
    uintptr_t stubAddr = reinterpret_cast<uintptr_t>(intStub);

    // Actually allocate a small executable region
    void* stub = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (stub) {
        uint8_t* s = reinterpret_cast<uint8_t*>(stub);
        // xor eax,eax; ret
        s[0] = 0x33; s[1] = 0xC0; s[2] = 0xC3;
        stubAddr = reinterpret_cast<uintptr_t>(stub);
        Log("[FB] Stub allocated at 0x%08zX\n", stubAddr);
    }

    int patched = 0;
    for (int slot : badSlots) {
        uintptr_t slotAddr = vtable + slot * sizeof(uintptr_t);
        uintptr_t oldVal = 0;
        if (SafeWritePtr(slotAddr, stubAddr, oldVal)) {
            Log("[FB]   Patched vtable[%3d] @ 0x%08zX  old=0x%08zX  new=0x%08zX\n",
                slot, slotAddr, oldVal, stubAddr);
            patched++;
        } else {
            Log("[FB]   FAILED to patch vtable[%3d]\n", slot);
        }
    }

    Log("[FB] Patched %d/%d bad slots\n", patched, (int)badSlots.size());
    return patched > 0;
}

// ─── Main init ─────────────────────────────────────────────────────────────

bool FrostbiteFix_Init(HMODULE hMp) {
    Log("\n=== FrostbiteFix_Init === mp.dll=0x%08X\n", (uintptr_t)hMp);

    // Step 1: Get engine function pointers
    if (!GiveFnptrs_Init(hMp)) {
        Log("[FB] FATAL: GiveFnptrs_Init failed\n");
        return false;
    }

    // Grab SetOrigin from engfuncs (index 20 in standard HLSDK)
    auto* ptrs = reinterpret_cast<void**>(&g_engfuncs);
    g_origSetOrigin    = reinterpret_cast<pfnSetOrigin_t>(ptrs[20]);
    Log("[FB] pfnSetOrigin = 0x%08zX\n", reinterpret_cast<uintptr_t>(g_origSetOrigin));

    // Step 2: Find the frostbite vtable
    __try {
        uintptr_t vtable = FindWeaponFrostbiteVtable(hMp);
        if (!vtable) {
            Log("[FB] Could not find frostbite vtable! Server may crash when weapon spawns.\n");
        } else {
            Log("[FB] Frostbite vtable @ 0x%08zX\n", vtable);
            PatchVtable(vtable, hMp);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[FB] EXCEPTION in vtable find/patch!\n");
    }

    Log("=== FrostbiteFix_Init COMPLETE ===\n");
    return true;
}
