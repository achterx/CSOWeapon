// frostbite_fix.cpp  v7
#include <windows.h>
#include <cstdint>
#include <vector>
#include <psapi.h>
#include "givefnptrs.h"
#include "logger.h"

#pragma comment(lib, "psapi.lib")

// ---------------------------------------------------------------------------
// Plain helpers — may use __try  (no C++ objects)
// ---------------------------------------------------------------------------

static bool SafeWritePtr(uintptr_t addr, uintptr_t newVal, uintptr_t& oldVal)
{
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t),
                        PAGE_EXECUTE_READWRITE, &old))
    {
        Log("[FB] VirtualProtect failed @ 0x%08zX err=%u\n", addr, GetLastError());
        return false;
    }
    __try {
        oldVal = *reinterpret_cast<uintptr_t*>(addr);
        *reinterpret_cast<uintptr_t*>(addr) = newVal;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), old, &old);
        return false;
    }
    VirtualProtect(reinterpret_cast<void*>(addr), sizeof(uintptr_t), old, &old);
    return true;
}

// Read one vtable slot with SEH — plain fn, no C++ objects
static bool ReadSlot(uintptr_t addr, uint32_t& out)
{
    __try {
        out = *reinterpret_cast<uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------------------------------------------------------------------------
// Vtable slot classification — uses VirtualQuery (no __try needed)
// ---------------------------------------------------------------------------

static bool SlotIsExecutable(uint32_t v)
{
    if (v == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>((uintptr_t)v), &mbi, sizeof(mbi)))
        return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// ---------------------------------------------------------------------------
// PatchVtable — uses std::vector.  All __try calls go through ReadSlot /
// SafeWritePtr so there is no __try directly in this function.
// ---------------------------------------------------------------------------

static bool PatchVtable(uintptr_t vtable, HMODULE hMp)
{
    if (!vtable) return false;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMp);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
                    reinterpret_cast<uint8_t*>(hMp) + dos->e_lfanew);
    uintptr_t mpBase = reinterpret_cast<uintptr_t>(hMp);
    uintptr_t mpEnd  = mpBase + nt->OptionalHeader.SizeOfImage;

    Log("[FB] Vtable @ 0x%08zX — scanning slots:\n", vtable);

    std::vector<int> badSlots;

    for (int i = 0; i < 256; ++i) {
        uintptr_t slotAddr = vtable + (uintptr_t)i * 4;
        uint32_t  v        = 0;
        if (!ReadSlot(slotAddr, v)) {
            Log("[FB]   vtable[%3d] AV — stop scan\n", i);
            break;
        }

        bool inMp   = (v >= (uint32_t)mpBase && v < (uint32_t)mpEnd);
        bool isExec = inMp || SlotIsExecutable(v);

        if (!isExec) {
            Log("[FB]   vtable[%3d] = 0x%08X  *** BAD ***\n", i, v);
            badSlots.push_back(i);
        } else if (i < 20) {
            Log("[FB]   vtable[%3d] = 0x%08X\n", i, v);
        }
    }

    Log("[FB] %d bad slots\n", (int)badSlots.size());
    if (badSlots.empty()) {
        Log("[FB] vtable looks intact — no patch needed\n");
        return true;
    }

    // Allocate a small executable stub: xor eax,eax ; ret
    void* stub = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
    if (!stub) {
        Log("[FB] VirtualAlloc for stub failed\n");
        return false;
    }
    auto* s = reinterpret_cast<uint8_t*>(stub);
    s[0] = 0x33; s[1] = 0xC0; s[2] = 0xC3;  // xor eax,eax ; ret
    uintptr_t stubAddr = reinterpret_cast<uintptr_t>(stub);
    Log("[FB] stub @ 0x%08zX\n", stubAddr);

    int patched = 0;
    for (int slot : badSlots) {
        uintptr_t slotAddr = vtable + (uintptr_t)slot * 4;
        uintptr_t oldVal   = 0;
        if (SafeWritePtr(slotAddr, stubAddr, oldVal)) {
            Log("[FB]   Patched[%3d] @ 0x%08zX  old=0x%08zX\n", slot, slotAddr, oldVal);
            ++patched;
        } else {
            Log("[FB]   FAIL[%3d]\n", slot);
        }
    }

    Log("[FB] Patched %d/%d\n", patched, (int)badSlots.size());
    return patched > 0;
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

    auto* ptrs = reinterpret_cast<void**>(&g_engfuncs);
    Log("[FB] pfnSetOrigin = 0x%08zX\n", reinterpret_cast<uintptr_t>(ptrs[20]));

    __try {
        uintptr_t vtable = FindWeaponFrostbiteVtable(hMp);
        if (!vtable)
            Log("[FB] Could not find frostbite vtable!\n");
        else
            PatchVtable(vtable, hMp);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[FB] EXCEPTION in vtable find/patch\n");
    }

    Log("=== FrostbiteFix_Init COMPLETE ===\n");
    return true;
}
