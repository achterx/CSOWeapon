#include "frostbite_fix.h"
#include "givefnptrs.h"
#include "pattern_scan.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>

// ============================================================
// Logging
// ============================================================
static FILE* g_log = nullptr;

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

// ============================================================
// VirtualProtect write helper
// ============================================================
static bool WriteProtected(void* addr, const void* data, size_t len)
{
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old))
        return false;
    memcpy(addr, data, len);
    VirtualProtect(addr, len, old, &old);
    return true;
}

static bool PatchSlot(void** vtable, int slot, void* newFunc)
{
    void** target = vtable + slot;
    return WriteProtected(target, &newFunc, sizeof(void*));
}

// ============================================================
// Engine function pointers — resolved from hw.dll / swds.dll
// (not in mp.dll — these are engine-side exports)
// ============================================================
struct edict_t;  // full definition in spawn helpers below
typedef int  (*PFN_DispatchSpawn)(edict_t* pent);
typedef void (*PFN_SetOrigin)(edict_t* pent, const float* org);
static PFN_DispatchSpawn g_pfnDispatchSpawn = nullptr;
static PFN_SetOrigin     g_pfnSetOrigin     = nullptr;

// ============================================================
// Runtime state — filled by FrostbiteFix_Init()
// ============================================================
static HMODULE g_hMpDll          = nullptr;
static void**  g_pWeaponVtable   = nullptr;  // CFrostbite vtable in mp.dll
static void**  g_pPropVtable     = nullptr;  // FROSTBITE_prop vtable in mp.dll

// ============================================================
// Vtable resolution — mirrors CSOWeapon(1).dll approach exactly:
//   1. Scan mp.dll memory for the "FROSTBITE\0" string
//   2. Find the PUSH opcode referencing it (config constructor)
//   3. Walk back to find MOV [ecx], imm32 writing the vtable ptr
//   4. That imm32 IS the vtable address
// ============================================================
static void* FindWeaponVtable(HMODULE hMp)
{
    auto* base = reinterpret_cast<uint8_t*>(hMp);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    size_t imgSize = nt->OptionalHeader.SizeOfImage;

    // Step 1: find "FROSTBITE\0" string in the whole image
    const char* target = "FROSTBITE";
    uint8_t* found = nullptr;
    for (size_t i = 0; i < imgSize - 10; ++i) {
        if (memcmp(base + i, target, 10) == 0) {
            found = base + i;
            break;
        }
    }

    if (!found) {
        Log("[frostbite_fix] ERROR: 'FROSTBITE' string not found in mp.dll\n");
        return nullptr;
    }
    Log("[frostbite_fix] 'FROSTBITE' string at mp.dll+%X\n",
        (uintptr_t)found - (uintptr_t)base);

    // Step 2: scan .text for a PUSH that references this address
    // PUSH imm32: opcode 0x68 followed by the 4-byte address
    uint32_t strAddr = (uint32_t)found;
    uint8_t pushPat[5] = { 0x68,
        (uint8_t)(strAddr & 0xFF),
        (uint8_t)((strAddr >> 8) & 0xFF),
        (uint8_t)((strAddr >> 16) & 0xFF),
        (uint8_t)((strAddr >> 24) & 0xFF)
    };

    uint8_t* pushInstr = nullptr;
    auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* secBase = base + section->VirtualAddress;
        size_t secSize   = section->Misc.VirtualSize;
        for (size_t j = 0; j + 5 <= secSize; ++j) {
            if (memcmp(secBase + j, pushPat, 5) == 0) {
                pushInstr = secBase + j;
                break;
            }
        }
        if (pushInstr) break;
    }

    if (!pushInstr) {
        Log("[frostbite_fix] ERROR: PUSH 'FROSTBITE' not found\n");
        return nullptr;
    }
    Log("[frostbite_fix] PUSH 'FROSTBITE' at mp.dll+%X\n",
        (uintptr_t)pushInstr - (uintptr_t)base);

    // Step 3: walk backwards up to 256 bytes to find function prologue
    // then scan forward for MOV [ecx], imm32 (C7 01 [4 bytes]) — vtable write
    // The constructor writes the vtable ptr as the very first thing after prologue.
    uint8_t* scanStart = pushInstr - 256;
    if (scanStart < base) scanStart = base;

    uint32_t vtableAddr = 0;
    for (uint8_t* p = scanStart; p < pushInstr - 5; ++p) {
        // C7 01 xx xx xx xx  = MOV DWORD PTR [ecx], imm32
        if (p[0] == 0xC7 && p[1] == 0x01) {
            vtableAddr = *reinterpret_cast<uint32_t*>(p + 2);
            Log("[frostbite_fix] Candidate vtable ptr = %08X at mp.dll+%X\n",
                vtableAddr, (uintptr_t)p - (uintptr_t)base);
            // Validate: the vtable must be in .rdata (not executable, has data)
            // Quick check: it must point back into the module
            if (vtableAddr >= (uint32_t)base &&
                vtableAddr <  (uint32_t)(base + imgSize)) {
                break;
            }
            vtableAddr = 0;
        }
    }

    if (!vtableAddr) {
        Log("[frostbite_fix] ERROR: vtable write not found\n");
        return nullptr;
    }

    Log("[frostbite_fix] CFrostbite vtable resolved: %08X\n", vtableAddr);
    return reinterpret_cast<void*>(vtableAddr);
}

// ============================================================
// Replacement functions — __thiscall via __fastcall trick
// (MSVC: __fastcall with dummy 2nd param = ecx,edx calling conv)
// ============================================================

// Helper: read member from 'this' at byte offset
#define MEMBER(type, base, off)  (*reinterpret_cast<type*>((uint8_t*)(base) + (off)))

// gpGlobals->time shorthand
#define NOW()  g_globals->time

// ============================================================
// FROSTBITE_prop entity spawn helpers
// ============================================================
// entvars_t layout — only the fields we write at spawn time.
// Offsets from GoldSrc SDK (identical in CSNZ mp.dll).
// ============================================================
struct entvars_t
{
    // +0   string_t  classname
    // +4   string_t  globalname
    // +8   string_t  model
    // +12  string_t  target
    // +16  string_t  targetname
    // +20  string_t  netname
    // +24  string_t  message
    // +28  int       flags
    // +32  float[3]  origin    ← x,y,z
    // +44  float[3]  angles
    // +56  float[3]  velocity
    // +68  float[3]  avelocity
    // +80  float[3]  punchangle
    // +92  float[3]  v_angle
    // +104 float[3]  endpos
    // +116 float[3]  startpos
    // ...
    // +192 int       owner     ← edict index of owner
    // ...
    // +456 int       flags2 / solid / movetype area
    uint8_t _raw[512];

    float* origin()   { return reinterpret_cast<float*>(_raw + 32); }
    float* angles()   { return reinterpret_cast<float*>(_raw + 44); }
    float* velocity() { return reinterpret_cast<float*>(_raw + 56); }
    int&   owner()    { return *reinterpret_cast<int*>(_raw + 192); }
};

struct edict_t
{
    int        free;        // +0
    int        freetime;    // +4
    void*      pvServerData;// +8  = CBaseEntity private data ptr
    entvars_t  v;           // +12 = entvars (pev)
};

// Get edict from entity ptr (the pvServerData -> back to edict)
// In GoldSrc: pev is at entity+8, and entity ptr = pev - offsetof(edict_t, v)
// But simpler: the entity base class stores pev ptr at entity+8
inline entvars_t* GetPev(void* entity)
{
    return *reinterpret_cast<entvars_t**>(reinterpret_cast<uint8_t*>(entity) + 8);
}

// UTIL_AngleVectors — we call it via engfuncs
static void FB_AngleVectors(float* angles, float* forward, float* right, float* up)
{
    typedef void(*AngleVectors_t)(const float*, float*, float*, float*);
    reinterpret_cast<AngleVectors_t>(g_engfuncs->pfnAngleVectors)(angles, forward, right, up);
}

// DispatchSpawn — called on newly created entities to run their Spawn()
// Located via export or pattern; for now we call through g_engfuncs->pfnCreateNamedEntity
// which already returns a ready-to-use entity. We then call DispatchSpawn on it.
// In GoldSrc the sequence is:
//   edict = CREATE_NAMED_ENTITY(MAKE_STRING("frostbiteB_projectile"))
//   DispatchSpawn(edict)         ← triggers entity's Spawn() virtual
//   SET_ORIGIN(edict, pos)
//   SET_VELOCITY(edict->v, vel)
static void ResolveEngineHelpers()
{
    HMODULE hHw = GetModuleHandleA("hw.dll");
    if (!hHw) hHw = GetModuleHandleA("swds.dll"); // dedicated server uses swds
    if (!hHw) { Log("[FB] WARNING: hw.dll / swds.dll not found\n"); return; }

    g_pfnDispatchSpawn = reinterpret_cast<PFN_DispatchSpawn>(
        GetProcAddress(hHw, "DispatchSpawn"));
    g_pfnSetOrigin = reinterpret_cast<PFN_SetOrigin>(
        GetProcAddress(hHw, "SetOrigin"));

    Log("[FB] DispatchSpawn=%p SetOrigin=%p\n", g_pfnDispatchSpawn, g_pfnSetOrigin);
}

// ============================================================
// SpawnFrostbiteProjectile
// Spawns one FROSTBITE_prop at `origin` traveling in `direction`
// at the given speed. Owner is the weapon's player edict index.
// ============================================================
static void SpawnFrostbiteProjectile(float* origin, float* direction,
                                     float speed, int ownerEdictIndex)
{
    if (!g_engfuncs || !g_engfuncs->pfnCreateNamedEntity) return;

    // CREATE_NAMED_ENTITY("frostbiteB_projectile")
    // pfnAllocString gives us a string_t from a C string
    // pfnCreateNamedEntity takes string_t and returns edict_t*
    typedef int (*AllocString_t)(const char*);
    typedef edict_t* (*CreateNamedEntity_t)(int);

    int classStr = reinterpret_cast<AllocString_t>(
        g_engfuncs->pfnAllocString)("frostbiteB_projectile");

    edict_t* pEdict = reinterpret_cast<CreateNamedEntity_t>(
        g_engfuncs->pfnCreateNamedEntity)(classStr);

    if (!pEdict) {
        Log("[FB] SpawnFrostbiteProjectile: CREATE_NAMED_ENTITY returned null\n");
        return;
    }

    // Set owner before DispatchSpawn so the entity knows who fired it
    pEdict->v.owner() = ownerEdictIndex;

    // Position
    pEdict->v.origin()[0] = origin[0];
    pEdict->v.origin()[1] = origin[1];
    pEdict->v.origin()[2] = origin[2];

    // Velocity = direction * speed
    pEdict->v.velocity()[0] = direction[0] * speed;
    pEdict->v.velocity()[1] = direction[1] * speed;
    pEdict->v.velocity()[2] = direction[2] * speed;

    // Run Spawn() on the entity
    if (g_pfnDispatchSpawn) {
        g_pfnDispatchSpawn(pEdict);
    }

    // Ensure origin is set after spawn (some entities reset it)
    if (g_pfnSetOrigin) {
        g_pfnSetOrigin(pEdict, origin);
    }
}

// ---- PrimaryAttack ----------------------------------------
// Replaces sub_10B30E90
// Fires the 6-projectile 180° freeze fan with 1.97s cooldown.
// Confirmed from IDA:
//   AModeFanAttackMeter = 6   (projectile count)
//   AModeFanAttackAngle = 180 (spread degrees)
//   BModeSpeed          = 800 (projectile speed)
//   Event normal=1, charged=7; Player anim=5; Fire anim 4 vs 10
// ============================================================
static int __fastcall Hook_PrimaryAttack(void* ecx, void* /*edx*/)
{
    uint8_t* self = reinterpret_cast<uint8_t*>(ecx);

    // --- 1. Get player ---
    uint8_t* player = *reinterpret_cast<uint8_t**>(self + FB::m_pPlayer);
    if (!player) return 0;

    // --- 2. Check ammo ---
    int ammoType = MEMBER(int, self, FB::m_iPrimaryAmmoType);
    int ammo     = MEMBER(int, player, FB::pl_ammo_base + ammoType * 4);
    if (ammo <= 0) return 0;

    // --- 3. Determine charge state ---
    float chargeState = MEMBER(float, self, FB::m_flChargeState);
    bool  charged     = chargeState > NOW();

    // --- 4. Vtable helpers ---
    void** vt       = *reinterpret_cast<void***>(ecx);
    void** playerVt = *reinterpret_cast<void***>(player);

    // SendWeaponAnim (vtable+716)
    typedef int(__thiscall* SendWeaponAnim_t)(void*, int, int);
    auto sendAnim = reinterpret_cast<SendWeaponAnim_t>(vt[716 / 4]);
    if (sendAnim) sendAnim(ecx, charged ? FBAnim::FireCharged : FBAnim::FireNormal, 0);

    // Player SetAnimation (vtable+548)
    typedef void(__thiscall* SetAnimation_t)(void*, int);
    auto setAnim = reinterpret_cast<SetAnimation_t>(playerVt[548 / 4]);
    if (setAnim) setAnim(player, FBAnim::PlayerFire);

    // --- 5. Playback event (muzzle flash, sound) ---
    // pfnPrecacheEvent index is stored at this+eventIndex (unknown offset — use 0 for now)
    // In sub_10B37470: PLAYBACK_EVENT_FULL(32, pPlayer, eventIdx, 0, origin, angles, 0,0, eventParam, 0, 0, 0)
    // eventParam = charged ? 7 : 1
    // We call it via g_engfuncs->pfnPlaybackEvent
    entvars_t* playerPev = GetPev(player);
    if (playerPev && g_engfuncs->pfnPlaybackEvent) {
        // Event index 0 = placeholder; server-side playback event needs precached index
        // For now fire the event with the correct iparam1 so client knows charge state
        PLAYBACK_EVENT_FULL(FBEvent::FEV_FLAG, player, 0, 0.f,
            playerPev->origin(), playerPev->angles(),
            0.f, 0.f,
            charged ? FBEvent::Charged : FBEvent::Normal,
            0, 0, 0);
    }

    // --- 6. Apply recoil to player ---
    MEMBER(int, player, FB::pl_stamina)  = FB_RECOIL_STAMINA;
    MEMBER(int, player, FB::pl_recoil2)  = FB_RECOIL_2;

    // --- 7. Set next attack timers ---
    MEMBER(float, self, FB::m_flNextPrimaryAttack) = NOW() + FBTiming::FireCooldown;
    MEMBER(float, self, FB::m_flTimeWeaponIdle)    = NOW();

    // --- 8. Decrement ammo ---
    MEMBER(int, player, FB::pl_ammo_base + ammoType * 4) = ammo - 1;

    // --- 9. Spawn the 6-projectile fan ---
    // Fan: 6 projectiles spread evenly over 180° in front of the player
    // We use the player's v_angle (eye angles) as the base direction
    //
    // AModeFanAttackMeter = 6 projectiles
    // AModeFanAttackAngle = 180° total spread
    // BModeSpeed = 800 u/s
    //
    // Spread formula:
    //   startAngle = -90° (half of 180°)
    //   step       = 180° / (6-1) = 36° per projectile
    //   projYaw[i] = baseYaw + startAngle + i * step
    //
    // Confirmed by AModeFanAttackAngle=180 and AModeFanAttackMeter=6 in sub_10B31520

    if (playerPev) {
        float* eyeAngles = playerPev->angles(); // pitch, yaw, roll

        // Spawn origin = player eye position (approximate: origin + view offset)
        float spawnOrigin[3];
        spawnOrigin[0] = playerPev->origin()[0];
        spawnOrigin[1] = playerPev->origin()[1];
        spawnOrigin[2] = playerPev->origin()[2] + 36.0f; // eye height approx

        constexpr int   PROJ_COUNT   = (int)FBCfg::AModeFanAttackMeter; // 6
        constexpr float TOTAL_SPREAD = FBCfg::AModeFanAttackAngle;       // 180.0f
        constexpr float PROJ_SPEED   = FBCfg::BModeSpeed;                // 800.0f
        constexpr float START_OFFSET = -(TOTAL_SPREAD / 2.f);            // -90°
        constexpr float STEP         = TOTAL_SPREAD / (PROJ_COUNT - 1);  // 36°

        constexpr float DEG2RAD = 3.14159265f / 180.f;

        float baseYaw   = eyeAngles[1]; // yaw = index 1
        float basePitch = eyeAngles[0]; // pitch = index 0

        for (int i = 0; i < PROJ_COUNT; ++i)
        {
            float projYaw = baseYaw + START_OFFSET + i * STEP;

            // Convert spherical to direction vector
            float yawRad   = projYaw   * DEG2RAD;
            float pitchRad = basePitch * DEG2RAD;

            float direction[3];
            direction[0] =  cosf(pitchRad) * cosf(yawRad);
            direction[1] =  cosf(pitchRad) * sinf(yawRad);
            direction[2] = -sinf(pitchRad);

            // Add small accuracy spread (Accuracy = 0.02f from config)
            direction[0] += RANDOM_FLOAT(-FBCfg::Accuracy, FBCfg::Accuracy);
            direction[1] += RANDOM_FLOAT(-FBCfg::Accuracy, FBCfg::Accuracy);
            direction[2] += RANDOM_FLOAT(-FBCfg::Accuracy, FBCfg::Accuracy);

            // Normalize
            float len = sqrtf(direction[0]*direction[0] +
                              direction[1]*direction[1] +
                              direction[2]*direction[2]);
            if (len > 0.001f) {
                direction[0] /= len;
                direction[1] /= len;
                direction[2] /= len;
            }

            SpawnFrostbiteProjectile(spawnOrigin, direction, PROJ_SPEED,
                /* ownerEdictIndex — pfnIndexOfEdict(playerPev's edict) */
                reinterpret_cast<int(*)(entvars_t*)>(
                    g_engfuncs->pfnEntOffsetOfPEntity)(playerPev));
        }
    }

    Log("[FB] PrimaryAttack: fired %d projectiles (charged=%d, ammo=%d->%d)\n",
        (int)FBCfg::AModeFanAttackMeter, charged, ammo, ammo - 1);

    return 1;
}

// ---- SecondaryAttack (shield deploy) ----------------------
// Replaces sub_10B30F60
// 80.3s cooldown. Deploys ice shield. Sets charge flag.
static void __fastcall Hook_SecondaryAttack(void* ecx, void* /*edx*/)
{
    uint8_t* self = reinterpret_cast<uint8_t*>(ecx);

    // Call through base SecondaryAttack (vtable+648)
    typedef void(__thiscall* BaseSecondary_t)(void*);
    void** vt = *reinterpret_cast<void***>(ecx);
    auto baseSecondary = reinterpret_cast<BaseSecondary_t>(vt[648 / 4]);
    if (baseSecondary) baseSecondary(ecx);

    float next = MEMBER(float, self, FB::m_flNextPrimaryAttack);
    if (NOW() <= next) return;

    // Set cooldown
    MEMBER(float, self, FB::m_flNextPrimaryAttack) = NOW() + FBTiming::ShieldCooldown;

    // Check if charged (for shield strength param)
    float chargeState = MEMBER(float, self, FB::m_flChargeState);
    bool  charged     = chargeState > NOW();

    // Check if player has shield active (vtable+688)
    typedef int(__thiscall* HasShield_t)(void*);
    auto hasShield = reinterpret_cast<HasShield_t>(vt[688 / 4]);
    int  shieldOn  = hasShield ? hasShield(ecx) : 0;

    // Deploy shield (vtable+652, params: shieldParam, hasShield)
    typedef void(__thiscall* DeployShield_t)(void*, int, int);
    auto deployShield = reinterpret_cast<DeployShield_t>(vt[652 / 4]);
    if (deployShield) {
        int shieldParam = charged ? 6 : 0;
        deployShield(ecx, shieldParam, shieldOn);
    }

    Log("[FB] SecondaryAttack — shield deployed (charged=%d)\n", charged);
}

// ---- Holster ----------------------------------------------
// Replaces sub_10B31040
static void __fastcall Hook_Holster(void* ecx, void* /*edx*/)
{
    uint8_t* self = reinterpret_cast<uint8_t*>(ecx);
    MEMBER(int,   self, FB::m_iClip)  = 0;
    MEMBER(int,   self, FB::m_iClipB) = 0;
    Log("[FB] Holster — state reset\n");
}

// ---- Think (FROSTBITE_prop) --------------------------------
// Replaces sub_10B312C0
// Ticks freeze timer, expires shield, processes buff list.
static void __fastcall Hook_Think(void* ecx, void* /*edx*/)
{
    uint8_t* self = reinterpret_cast<uint8_t*>(ecx);

    float thinkTimer = MEMBER(float, self, FB::m_flThinkTimer);

    // Tick down freeze timer
    if (NOW() > thinkTimer) {
        // Accumulate toward cap
        float delta = RANDOM_FLOAT(0.f, 1.f); // sub_10576750(0) equivalent
        int& freezeTimer = MEMBER(int, self, FB::m_iFreezeTimer);
        float cap = RANDOM_FLOAT(0.f, 1.f);
        int newVal = (int)((float)freezeTimer + delta);
        if (newVal > (int)cap) newVal = (int)cap;
        freezeTimer = newVal;

        MEMBER(float, self, FB::m_flThinkTimer) = RANDOM_FLOAT(0.f, 1.f) + NOW();
    }

    // Shield expiry
    float shieldActive = MEMBER(float, self, FB::m_flShieldActive);
    if (shieldActive > 0.f && NOW() > shieldActive) {
        uint8_t* player = *reinterpret_cast<uint8_t**>(self + FB::m_pPlayer);

        MEMBER(int, self, FB::m_iTriggerFlag) = 0;
        MEMBER(float, self, FB::m_flShieldActive) = -1.0f;
        MEMBER(float, self, FB::m_flShieldExpiry) = -1.0f;

        // Team check — if team == 6, reset think immediately
        if (player) {
            void** playerPev = *reinterpret_cast<void***>(*reinterpret_cast<void**>(player + 8) + 584);
            // simplified: just reset next think
            MEMBER(float, self, FB::m_flNextThink) = NOW();
        }
    }

    // Buff list cleanup at this+520
    // Iterate linked list, remove expired entries
    // (full impl requires knowing the list node layout — stub for now)
    // TODO: walk self+FB::m_flBuffListHead list, remove nodes where node+20 < NOW()
}

// ============================================================
// Apply patches to the vtable
// ============================================================
static bool ApplyPatches()
{
    if (!g_pWeaponVtable) {
        Log("[frostbite_fix] ERROR: weapon vtable not resolved, cannot patch\n");
        return false;
    }

    // Weapon vtable patches
    bool ok = true;
    ok &= PatchSlot(g_pWeaponVtable, FBSlots::Slot_PrimaryAttack,   (void*)Hook_PrimaryAttack);
    ok &= PatchSlot(g_pWeaponVtable, FBSlots::Slot_SecondaryAttack, (void*)Hook_SecondaryAttack);
    ok &= PatchSlot(g_pWeaponVtable, FBSlots::Slot_Holster,         (void*)Hook_Holster);

    // Prop vtable patches (Think shared)
    if (g_pPropVtable) {
        ok &= PatchSlot(g_pPropVtable, FBSlots::Slot_Think, (void*)Hook_Think);
    }

    Log("[frostbite_fix] Patches applied: %s\n", ok ? "OK" : "PARTIAL FAILURE");
    return ok;
}

// ============================================================
// Public init — called from dllmain.cpp after mp.dll loads
// ============================================================
bool FrostbiteFix_Init(HMODULE hMpDll)
{
    g_log = fopen("frostbite_fix.log", "a");
    Log("\n=== FrostbiteFix_Init ===\n");
    Log("mp.dll base: %p\n", hMpDll);

    g_hMpDll = hMpDll;

    // 1. Get engine function table via GiveFnptrsToDll (named export)
    if (!GiveFnptrs_Init(hMpDll)) {
        Log("ERROR: GiveFnptrs_Init failed\n");
        return false;
    }

    // 1b. Resolve engine-side helpers (DispatchSpawn, SetOrigin) from hw.dll/swds.dll
    ResolveEngineHelpers();

    // 2. Resolve vtable addresses via string scanning
    g_pWeaponVtable = reinterpret_cast<void**>(FindWeaponVtable(hMpDll));
    if (!g_pWeaponVtable) {
        Log("ERROR: Weapon vtable resolution failed\n");
        return false;
    }

    // FROSTBITE_prop vtable is 7 pointers (28 bytes) BEFORE the weapon vtable
    // (0x115D4C64 vs 0x115D4C80 in client IDB — exactly 28 bytes / 7 slots apart)
    g_pPropVtable = g_pWeaponVtable - 7;
    Log("FROSTBITE_prop vtable: %p\n", g_pPropVtable);

    // 3. Patch vtable slots
    if (!ApplyPatches()) {
        Log("WARNING: Some patches failed\n");
        // Non-fatal — partial patching is better than nothing
    }

    Log("FrostbiteFix_Init COMPLETE\n");
    return true;
}

void FrostbiteFix_Shutdown()
{
    Log("FrostbiteFix_Shutdown\n");
    if (g_log) { fclose(g_log); g_log = nullptr; }
}
