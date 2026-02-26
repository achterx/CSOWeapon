#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstddef>

// ============================================================
// Pattern scanner — same approach as CSOWeapon(1).dll
// ============================================================
// Uses sig+mask pairs (x = match byte, ? = wildcard).
// Scans the full .text section of a loaded module.
// ============================================================

// Scan [base, base+size) for sig/mask. Returns pointer or nullptr.
inline void* PatternScan(const uint8_t* base, size_t size,
                         const uint8_t* sig, const char* mask)
{
    size_t len = strlen(mask);
    if (size < len) return nullptr;

    for (size_t i = 0; i <= size - len; ++i) {
        bool found = true;
        for (size_t j = 0; j < len; ++j) {
            if (mask[j] == 'x' && base[i + j] != sig[j]) {
                found = false;
                break;
            }
        }
        if (found) return (void*)(base + i);
    }
    return nullptr;
}

// Scan the .text section of hModule. Returns matched address or nullptr.
inline void* PatternScanModule(HMODULE hModule, const uint8_t* sig, const char* mask)
{
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hModule);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(
                    reinterpret_cast<uint8_t*>(hModule) + dos->e_lfanew);

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        // Scan .text or any executable section
        if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            auto* base = reinterpret_cast<const uint8_t*>(hModule)
                       + section->VirtualAddress;
            size_t size = section->Misc.VirtualSize;
            void* result = PatternScan(base, size, sig, mask);
            if (result) return result;
        }
    }
    return nullptr;
}

// Helper: read a 4-byte relative offset from addr+offset and resolve it
// (for following CALL/JMP rel32 instructions)
inline void* ResolveRel32(void* addr, int offset = 1)
{
    auto* p = reinterpret_cast<uint8_t*>(addr);
    int32_t rel = *reinterpret_cast<int32_t*>(p + offset);
    return p + offset + 4 + rel;
}

// ============================================================
// Frostbite-specific signatures for mp.dll
// ============================================================
// All derived from our IDA analysis. Each one is verified
// against a unique byte sequence surrounding a known instruction.
// ============================================================

namespace FrostbiteSigs
{
    // ----------------------------------------------------------
    // CFrostbite::PrimaryAttack — sub_10B30E90
    // Unique sequence: check ammo > 0, then SendWeaponAnim(4 or 10)
    // Bytes: 8B 47 34 85 C0 74 ? 80 BF ...
    // ----------------------------------------------------------
    constexpr uint8_t PrimaryAttack_sig[] = {
        0x8B, 0x47, 0x34, 0x85, 0xC0, 0x74, 0x00,
        0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    constexpr char PrimaryAttack_mask[] = "xxxxxx?xx????x";

    // ----------------------------------------------------------
    // CFrostbite::SecondaryAttack — sub_10B30F60
    // Unique sequence: loads 80.299995 (0x42A09A3D) into xmm
    // Bytes: F3 0F 10 05 [80.3f imm] F3 0F 11 ...
    // ----------------------------------------------------------
    constexpr uint8_t SecondaryAttack_sig[] = {
        0xF3, 0x0F, 0x10, 0x05,
        0x3D, 0x9A, 0xA0, 0x42,   // 80.299995f in little-endian IEEE 754
        0xF3, 0x0F, 0x11
    };
    constexpr char SecondaryAttack_mask[] = "xxxxxxxxxxx";

    // ----------------------------------------------------------
    // CFrostbite::Precache — sub_10B30B50
    // Unique: push offset "ef_frostbite_charge.spr"
    // The string is unique, find the PUSH opcode before it.
    // Bytes: 68 [ptr to "ef_frostbite_charge.spr"] E8 ...
    // ----------------------------------------------------------
    // We locate Precache differently — string xref (see frostbite_fix.cpp)

    // ----------------------------------------------------------
    // CFrostbite weapon vtable — 0x115D4C80 in the client IDB
    // Server offset will differ. We locate by:
    //   1. Find "FROSTBITE\0" string in mp.dll
    //   2. Find PUSH that refs it (in the config constructor sub_10B31520)
    //   3. Walk back to the function entry
    //   4. Find the MOV [ecx], imm32 that writes the vtable ptr
    // ----------------------------------------------------------

    // ----------------------------------------------------------
    // FROSTBITE_prop vtable — 0x115D4C64 in client IDB
    // Located right before CFrostbite vtable (+28 bytes / 7 ptrs)
    // ----------------------------------------------------------

    // ----------------------------------------------------------
    // CFrostbite::Think — sub_10B312C0
    // Unique: loads -1082130432 (0xBF800000 = -1.0f) twice
    // Bytes: C7 45 ? 00 00 80 BF C7 45 ? 00 00 80 BF
    // ----------------------------------------------------------
    constexpr uint8_t Think_sig[] = {
        0xC7, 0x45, 0x00, 0x00, 0x00, 0x80, 0xBF,
        0xC7, 0x45, 0x00, 0x00, 0x00, 0x80, 0xBF
    };
    constexpr char Think_mask[] = "xx?xxxxxx?xxxx";

    // ----------------------------------------------------------
    // CFrostbite::WeaponIdle — sub_10B30C80
    // Unique: checks player velocity X*X + Y*Y < 0.0 (dead code but present)
    // Bytes: 0F 57 C0 0F 2F C1 (XORPS + COMISS sequence)
    // ----------------------------------------------------------
    constexpr uint8_t WeaponIdle_sig[] = {
        0x0F, 0x57, 0xC0, 0x0F, 0x2F, 0xC1
    };
    constexpr char WeaponIdle_mask[] = "xxxxxx";

    // ----------------------------------------------------------
    // sub_10B37470 — the actual fire dispatch function
    // Unique: references dword 1106247680 (8.0f = AModeDamage default)
    // Then immediately calls playback event with 134 flag
    // ----------------------------------------------------------
    constexpr uint8_t FireDispatch_sig[] = {
        0x6A, 0x86, 0x00,   // PUSH 134 (0x86)
        0xFF, 0x15           // CALL [pfnPlaybackEvent]
    };
    constexpr char FireDispatch_mask[] = "xxxxx";

    // ----------------------------------------------------------
    // GiveFnptrsToDll — this is a NAMED EXPORT, no sig needed.
    // Just GetProcAddress(hMpDll, "GiveFnptrsToDll")
    // ----------------------------------------------------------
}
