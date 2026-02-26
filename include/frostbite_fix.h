#pragma once
#include <cstdint>

// ============================================================
// Frostbite weapon — confirmed offsets from IDA reverse
// All byte offsets (not DWORD indices unless noted)
// ============================================================

// --- CFrostbite member offsets (byte) ---
// Derived from IDA pseudocode (this + N*4 = byte this + N*4)
namespace FB
{
    // Player pointer
    constexpr int m_pPlayer          = 208;  // this+52*4

    // Ammo
    constexpr int m_iPrimaryAmmoType = 332;  // this+83*4
    // Player ammo array base: player + 4148

    // Timing
    constexpr int m_flNextPrimaryAttack = 316; // this+79*4
    constexpr int m_flTimeWeaponIdle    = 396; // this+99*4
    constexpr int m_flNextThink         = 316; // shared slot in FROSTBITE_prop

    // Charge / shield state
    constexpr int m_flChargeState    = 508;  // this+127*4
    // If m_flChargeState > gpGlobals->time → charged mode active

    // Shot counter (used in cubic idle formula)
    constexpr int m_iShotsFired      = 404;  // this+101*4

    // AddToPlayer / config
    constexpr int m_iConfigID        = 472;  // this+118*4

    // Holster reset targets
    constexpr int m_iClip            = 348;  // this+87*4
    constexpr int m_iClipB           = 492;  // this+123*4

    // Think timer
    constexpr int m_flThinkTimer     = 1968; // this+492*4 (FROSTBITE_prop)
    constexpr int m_flShieldActive   = 2032; // this+508*4 (FROSTBITE_prop)
    constexpr int m_flShieldExpiry   = 2048; // this+512*4
    constexpr int m_iFreezeTimer     = 1392; // this+348*4 (FROSTBITE_prop)
    constexpr int m_iTriggerFlag     = 1744; // this+436*4
    constexpr int m_flBuffListHead   = 2080; // this+520*4 (linked list)

    // Player-side offsets (relative to player entity)
    constexpr int pl_stamina         = 3744; // recoil applied here
    constexpr int pl_recoil2         = 3752;
    constexpr int pl_anim_time       = 11676;// idle gate timer
    constexpr int pl_dead_flag       = 6237; // byte
    constexpr int pl_team            = 584;  // == 6 → specific team check
    constexpr int pl_ammo_base       = 4148; // int array, index by ammo type
    constexpr int pl_nextattack      = 372;  // player-side attack time
}

// --- FROSTBITE_prop config defaults (decoded IEEE 754) ---
namespace FBCfg
{
    // A-Mode (LMB)
    constexpr float AModeDamage         =  8.0f;
    constexpr float AModeDamageZombie   = 27.0f;
    constexpr float AModeDamageScenario = 40.0f;
    constexpr float AModeCycleTime      =  0.1f;   // fire rate per projectile
    constexpr float Accuracy            =  0.02f;
    constexpr float AccuracyDiv         =  8.0f;
    constexpr float AModeFanAttackMeter =  6.0f;   // 6 projectiles in fan
    constexpr float AModeFanAttackAngle = 180.0f;  // full hemisphere
    constexpr float AModeFanAttackDmg   = 12.0f;
    constexpr float AModeFanAttackDmgZ  = 100.0f;
    constexpr float AutoAimDegree       =  5.0f;
    constexpr float AutoAimMaxDist      = 32.0f;
    constexpr float ReloadTime          =  2.0f;
    constexpr float AModeRangeMod       =  0.8f;

    // B-Mode (RMB shield)
    constexpr float BModeCycleTime    =  1.0f;
    constexpr float BModeLifeTime     =  4.0f;   // shield lives 4 seconds
    constexpr float BModeSpeed        =  800.0f; // shield travels at 800 u/s
    constexpr float BModeRadius       =  3.0f;
    constexpr float BModeHeight       =  2.5f;
    constexpr float BModeHitboxSize   =  0.13f;
    constexpr float BModeDamage       = 20.0f;
    constexpr float BModeDamageZombie = 200.0f;
}

// --- Weapon timing (from reverse) ---
namespace FBTiming
{
    constexpr float FireCooldown    = 1.97f;  // PrimaryAttack: NextPrimaryAttack += 1.97
    constexpr float ActualFireRate  = 1.03f;  // sub_10B37470: next allow = time + 1.03
    constexpr float ShieldCooldown  = 80.3f;  // SecondaryAttack cooldown (shield duration)
    constexpr float ThinkInterval   = 1.03f;  // Think: m_flNextThink += 1.03
}

// --- Animation indices ---
namespace FBAnim
{
    constexpr int Idle          = 0;
    constexpr int FireNormal    = 4;   // LMB uncharged animation
    constexpr int PlayerFire    = 5;   // player-body fire animation
    constexpr int PlayerSpecial = 9;   // player special-attack animation
    constexpr int FireCharged   = 10;  // LMB charged animation
}

// --- Playback event flags ---
namespace FBEvent
{
    constexpr int Normal  = 1;
    constexpr int Charged = 7;
    constexpr int FEV_FLAG = 32;       // flags passed to PLAYBACK_EVENT_FULL
}

// --- Vtable slot numbers in CFrostbite's vtable ---
// Slots are 4-byte pointers. Slot 0 = vtable[0] = first entry.
// These are relative to the CFrostbite vtable pointer itself (0x115D4C80 in client IDB).
// The vtable at 0x115D4F00 is CSimpleWpn<CFrostbite> wrapper — do NOT use that one.
//
// From IDA, CFrostbite vtable (0x115D4C80):
//   [0x00] slot 0  — destructor
//   [0x04] slot 1  — Spawn
//   [0x08] slot 2  — Precache
//   [0x0C] slot 3  — (base)
//   [0x10] slot 4  — (base)
//   [0x14] slot 5  — WeaponIdle   sub_10B30C80
//   [0x18] slot 6  — PrimaryAttack sub_10B30E90
//   [0x1C] slot 7  — SecondaryAttack sub_10B30F60
//   [0x20] slot 8  — AddToPlayer  sub_10B30FE0
//   [0x24] slot 9  — Holster      sub_10B31040
//   [0x28] slot 10 — TakeDamage   sub_10B31060
//   [0x2C] slot 11 — (base)
//   [0x30] slot 12 — Think        sub_10B312C0
namespace FBSlots
{
    constexpr int Slot_WeaponIdle      = 0x14 / 4;  // slot 5
    constexpr int Slot_PrimaryAttack   = 0x18 / 4;  // slot 6  — sub_10B30E90
    constexpr int Slot_SecondaryAttack = 0x1C / 4;  // slot 7  — sub_10B30F60
    constexpr int Slot_AddToPlayer     = 0x20 / 4;  // slot 8  — sub_10B30FE0
    constexpr int Slot_Holster         = 0x24 / 4;  // slot 9  — sub_10B31040
    constexpr int Slot_TakeDamage      = 0x28 / 4;  // slot 10 — sub_10B31060
    constexpr int Slot_Think           = 0x30 / 4;  // slot 12 — sub_10B312C0
}

// --- Recoil values applied on fire (player offsets) ---
constexpr int FB_RECOIL_STAMINA  = 1000;
constexpr int FB_RECOIL_2        = 512;
