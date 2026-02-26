#include "frostbite_fix.h"
#include "givefnptrs.h"
#include "pattern_scan.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <stdarg.h>

// ============================================================
// Logging — writes to file AND OutputDebugString
// ============================================================
static FILE* g_log = nullptr;

static void Log(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    OutputDebugStringA(buf);

    if (!g_log) {
        const char* paths[] = {
            "frostbite_fix.log",
            "C:\\frostbite_fix.log",
            "C:\\Temp\\frostbite_fix.log",
            nullptr
        };
        for (int i = 0; paths[i]; ++i) {
            g_log = fopen(paths[i], "a");
            if (g_log) break;
        }
    }
    if (g_log) { fputs(buf, g_log); fflush(g_log); }
}

// ============================================================
// VirtualProtect write helper
// ============================================================
static bool WriteProtected(void* addr, const void* data, size_t len)
{
    DWORD old;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "[FB] VirtualProtect FAILED at %p err=%lu\n",
            addr, GetLastError());
        Log(buf);
        return false;
    }
    memcpy(addr, data, len);
    VirtualProtect(addr, len, old, &old);
    return true;
}

static bool PatchSlot(void** vtable, int slot, void* newFunc)
{
    Log("[FB] Patching vtable[%d] at %p  old=%p  new=%p\n",
        slot, vtable + slot, vtable[slot], newFunc);
    return WriteProtected(vtable + slot, &newFunc, sizeof(void*));
}

// ============================================================
// Engine-side helpers from hw.dll / swds.dll
// ============================================================
struct edict_t;
typedef int  (*PFN_DispatchSpawn)(edict_t*);
typedef void (*PFN_SetOrigin)(edict_t*, const float*);
static PFN_DispatchSpawn g_pfnDispatchSpawn = nullptr;
static PFN_SetOrigin     g_pfnSetOrigin     = nullptr;

static void ResolveEngineHelpers()
{
    HMODULE hEng = GetModuleHandleA("hw.dll");
    if (!hEng) hEng = GetModuleHandleA("swds.dll");
    if (!hEng) hEng = GetModuleHandleA("engine.dll");
    if (!hEng) { Log("[FB] WARNING: engine DLL not found for DispatchSpawn/SetOrigin\n"); return; }

    g_pfnDispatchSpawn = (PFN_DispatchSpawn)GetProcAddress(hEng, "DispatchSpawn");
    g_pfnSetOrigin     = (PFN_SetOrigin)    GetProcAddress(hEng, "SetOrigin");
    Log("[FB] DispatchSpawn=%p  SetOrigin=%p\n", g_pfnDispatchSpawn, g_pfnSetOrigin);
}

// ============================================================
// Runtime state
// ============================================================
static HMODULE g_hMpDll        = nullptr;
static void**  g_pWeaponVtable = nullptr;
static void**  g_pPropVtable   = nullptr;

// ============================================================
// Vtable resolution
// ============================================================
// CSOWeapon approach confirmed from IDA:
//   1. Scan mp.dll image for the weapon classname string
//   2. Find PUSH imm32 referencing it (constructor uses it)
//   3. Scan nearby code for MOV [ecx], imm32 — that imm32 is the vtable ptr
//
// CRITICAL: The classname used in CSNZ mp.dll is NOT just "FROSTBITE".
// CSNZ weapon classnames follow the pattern "weapon_frostbite" (lowercase).
// We try multiple candidates and log every attempt.
// ============================================================
static void* FindVtableByClassname(HMODULE hMp, const char* classname)
{
    auto* base    = reinterpret_cast<uint8_t*>(hMp);
    auto* dos     = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt      = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    size_t imgSz  = nt->OptionalHeader.SizeOfImage;
    size_t cnLen  = strlen(classname) + 1;  // include null terminator

    Log("[FB] Searching for classname \"%s\" in mp.dll image (%zu bytes)\n",
        classname, imgSz);

    // --- Step 1: find the string ---
    uint8_t* strPtr = nullptr;
    for (size_t i = 0; i + cnLen <= imgSz; ++i) {
        if (memcmp(base + i, classname, cnLen) == 0) {
            strPtr = base + i;
            Log("[FB]   String \"%s\" found at mp.dll+0x%X  (VA=%p)\n",
                classname, (unsigned)i, strPtr);
            break;
        }
    }
    if (!strPtr) {
        Log("[FB]   String \"%s\" NOT found\n", classname);
        return nullptr;
    }

    // --- Step 2: find PUSH imm32 that references this string in .text ---
    uint32_t strVA    = (uint32_t)(uintptr_t)strPtr;
    uint8_t  pat[5]   = { 0x68,
        (uint8_t)(strVA),
        (uint8_t)(strVA >> 8),
        (uint8_t)(strVA >> 16),
        (uint8_t)(strVA >> 24)
    };

    uint8_t* pushInstr = nullptr;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* sb = base + sec->VirtualAddress;
        size_t   ss = sec->Misc.VirtualSize;
        for (size_t j = 0; j + 5 <= ss; ++j) {
            if (memcmp(sb + j, pat, 5) == 0) {
                pushInstr = sb + j;
                Log("[FB]   PUSH \"%s\" at mp.dll+0x%X\n",
                    classname, (unsigned)(pushInstr - base));
                break;
            }
        }
        if (pushInstr) break;
    }
    if (!pushInstr) {
        Log("[FB]   PUSH for \"%s\" NOT found in .text\n", classname);
        // Also try MOV EAX/ECX, imm32 patterns (0xB8/0xB9 opcodes)
        // Some compilers use those instead of PUSH
        uint8_t movPat1[5] = { 0xB8, pat[1], pat[2], pat[3], pat[4] }; // MOV EAX
        uint8_t movPat2[5] = { 0xB9, pat[1], pat[2], pat[3], pat[4] }; // MOV ECX
        sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            uint8_t* sb = base + sec->VirtualAddress;
            size_t   ss = sec->Misc.VirtualSize;
            for (size_t j = 0; j + 5 <= ss; ++j) {
                if (memcmp(sb+j, movPat1, 5)==0 || memcmp(sb+j, movPat2, 5)==0) {
                    pushInstr = sb + j;
                    Log("[FB]   MOV reg,\"%s\" at mp.dll+0x%X\n",
                        classname, (unsigned)(pushInstr - base));
                    break;
                }
            }
            if (pushInstr) break;
        }
        if (!pushInstr) return nullptr;
    }

    // --- Step 3: scan ±512 bytes for MOV [ecx], imm32  (C7 01 xx xx xx xx) ---
    //             or MOV [reg], imm32 variants
    uint8_t* scanFrom = (pushInstr > base + 512) ? pushInstr - 512 : base;
    uint8_t* scanTo   = pushInstr + 512;
    if (scanTo > base + imgSz) scanTo = base + imgSz - 5;

    uint32_t vtableAddr = 0;
    for (uint8_t* p = scanFrom; p < scanTo - 5; ++p) {
        bool isMov = false;
        // C7 01 xx xx xx xx = MOV DWORD PTR [ecx], imm32
        // C7 00 xx xx xx xx = MOV DWORD PTR [eax], imm32
        // C7 06 xx xx xx xx = MOV DWORD PTR [esi], imm32
        if (p[0] == 0xC7 && (p[1]==0x01 || p[1]==0x00 || p[1]==0x06))
            isMov = true;
        if (!isMov) continue;

        uint32_t candidate = *(uint32_t*)(p + 2);
        // Must point into the mp.dll image (vtable is in .rdata)
        if (candidate < (uint32_t)(uintptr_t)base ||
            candidate >= (uint32_t)(uintptr_t)(base + imgSz))
            continue;

        // Must NOT be in an executable section (vtables are .rdata)
        uint32_t off = candidate - (uint32_t)(uintptr_t)base;
        bool inExec = false;
        auto* s2 = IMAGE_FIRST_SECTION(nt);
        for (WORD k = 0; k < nt->FileHeader.NumberOfSections; ++k, ++s2) {
            if (off >= s2->VirtualAddress &&
                off <  s2->VirtualAddress + s2->Misc.VirtualSize) {
                if (s2->Characteristics & IMAGE_SCN_MEM_EXECUTE) inExec = true;
                break;
            }
        }
        if (inExec) continue;

        vtableAddr = candidate;
        Log("[FB]   Candidate vtable ptr=0x%08X at mp.dll+0x%X\n",
            candidate, (unsigned)(p - base));
        break;
    }

    if (!vtableAddr) {
        Log("[FB]   Vtable write MOV not found near \"%s\"\n", classname);
        return nullptr;
    }

    Log("[FB] Vtable for \"%s\" resolved: 0x%08X\n", classname, vtableAddr);
    return reinterpret_cast<void*>((uintptr_t)vtableAddr);
}

static void* FindWeaponVtable(HMODULE hMp)
{
    // Try every reasonable classname variant — log all attempts.
    // CSNZ uses lowercase "weapon_" prefix in most builds;
    // some older builds use all-caps.
    const char* candidates[] = {
        "weapon_frostbite",     // CSNZ standard
        "WEAPON_FROSTBITE",     // some older builds
        "Weapon_FrostBite",     // mixed case seen in some i64 labels
        "frostbite",            // bare name fallback
        "FROSTBITE",            // what v1-v3 searched for
        "CFrostbite",           // class name used in some binaries
        nullptr
    };

    for (int i = 0; candidates[i]; ++i) {
        void* vt = FindVtableByClassname(hMp, candidates[i]);
        if (vt) return vt;
    }

    Log("[FB] ERROR: All classname candidates failed. Dumping first 32 strings found near 'rost':\n");
    // Last-ditch: dump anything containing "rost" (case-insensitive) to find the real name
    auto* base   = reinterpret_cast<uint8_t*>(hMp);
    auto* dos    = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt     = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    size_t imgSz = nt->OptionalHeader.SizeOfImage;
    int found = 0;
    for (size_t i = 4; i + 4 < imgSz && found < 32; ++i) {
        uint8_t b0=base[i], b1=base[i+1], b2=base[i+2], b3=base[i+3];
        // look for "rost" or "ROST" (case-insensitive)
        if ((b0|0x20)=='r' && (b1|0x20)=='o' && (b2|0x20)=='s' && (b3|0x20)=='t') {
            // walk back to find start of string (printable ASCII run)
            int start = (int)i;
            while (start > 0 && base[start-1] >= 0x20 && base[start-1] <= 0x7E) --start;
            // walk forward to end
            int end = (int)i + 4;
            while (end < (int)imgSz && base[end] >= 0x20 && base[end] <= 0x7E) ++end;
            if (end - start >= 4 && end - start < 64) {
                char s[65] = {};
                memcpy(s, base + start, end - start);
                Log("[FB]   mp.dll+0x%X: \"%s\"\n", (unsigned)start, s);
                found++;
                i = (size_t)end;
            }
        }
    }

    return nullptr;
}

// ============================================================
// Member access helper
// ============================================================
#define MEMBER(type, base, off)  (*reinterpret_cast<type*>((uint8_t*)(base) + (off)))
#define NOW()  (g_globals ? g_globals->time : 0.f)

// ============================================================
// Entity structures (GoldSrc layout)
// ============================================================
struct entvars_t {
    uint8_t _raw[512];
    float* origin()   { return reinterpret_cast<float*>(_raw + 32); }
    float* angles()   { return reinterpret_cast<float*>(_raw + 44); }
    float* velocity() { return reinterpret_cast<float*>(_raw + 56); }
    int&   owner()    { return *reinterpret_cast<int*>(_raw + 192); }
};

struct edict_t {
    int       free_; int freetime; void* pvServerData;
    entvars_t v;
};

inline entvars_t* GetPev(void* entity)
{
    return *reinterpret_cast<entvars_t**>(reinterpret_cast<uint8_t*>(entity) + 8);
}

// ============================================================
// Projectile spawn
// ============================================================
static void SpawnFrostbiteProjectile(float* origin, float* dir, float speed, int ownerIdx)
{
    if (!g_engfuncs || !g_engfuncs->pfnCreateNamedEntity) return;

    typedef int      (*AllocStr_t)(const char*);
    typedef edict_t* (*CreateEnt_t)(int);

    int cls = reinterpret_cast<AllocStr_t>(g_engfuncs->pfnAllocString)("frostbiteB_projectile");
    edict_t* ed = reinterpret_cast<CreateEnt_t>(g_engfuncs->pfnCreateNamedEntity)(cls);
    if (!ed) { Log("[FB] CREATE_NAMED_ENTITY(frostbiteB_projectile) returned null\n"); return; }

    ed->v.owner() = ownerIdx;
    ed->v.origin()[0] = origin[0]; ed->v.origin()[1] = origin[1]; ed->v.origin()[2] = origin[2];
    ed->v.velocity()[0] = dir[0]*speed; ed->v.velocity()[1] = dir[1]*speed; ed->v.velocity()[2] = dir[2]*speed;

    if (g_pfnDispatchSpawn) g_pfnDispatchSpawn(ed);
    if (g_pfnSetOrigin)     g_pfnSetOrigin(ed, origin);
}

// ============================================================
// Hook implementations  (__thiscall via __fastcall trick)
// ============================================================

static int __fastcall Hook_PrimaryAttack(void* ecx, void* /*edx*/)
{
    uint8_t* self   = reinterpret_cast<uint8_t*>(ecx);
    uint8_t* player = *reinterpret_cast<uint8_t**>(self + FB::m_pPlayer);
    if (!player) { Log("[FB] PrimaryAttack: m_pPlayer is null\n"); return 0; }

    int ammoType = MEMBER(int, self, FB::m_iPrimaryAmmoType);
    int ammo     = MEMBER(int, player, FB::pl_ammo_base + ammoType * 4);
    Log("[FB] PrimaryAttack: ammoType=%d ammo=%d\n", ammoType, ammo);
    if (ammo <= 0) return 0;

    float chargeState = MEMBER(float, self, FB::m_flChargeState);
    bool  charged     = chargeState > NOW();

    void** vt       = *reinterpret_cast<void***>(ecx);
    void** playerVt = *reinterpret_cast<void***>(player);

    typedef int  (__thiscall* SendAnim_t)(void*, int, int);
    typedef void (__thiscall* SetAnim_t)(void*, int);
    auto sendAnim = reinterpret_cast<SendAnim_t>(vt[716/4]);
    auto setAnim  = reinterpret_cast<SetAnim_t>(playerVt[548/4]);
    if (sendAnim) sendAnim(ecx, charged ? FBAnim::FireCharged : FBAnim::FireNormal, 0);
    if (setAnim)  setAnim(player, FBAnim::PlayerFire);

    entvars_t* pev = GetPev(player);
    if (pev && g_engfuncs->pfnPlaybackEvent) {
        void* invoker = reinterpret_cast<uint8_t*>(pev) - 12;
        PLAYBACK_EVENT_FULL(FBEvent::FEV_FLAG, invoker, 0, 0.f,
            pev->origin(), pev->angles(), 0.f, 0.f,
            charged ? FBEvent::Charged : FBEvent::Normal, 0, 0, 0);
    }

    MEMBER(int,   player, FB::pl_stamina) = FB_RECOIL_STAMINA;
    MEMBER(int,   player, FB::pl_recoil2) = FB_RECOIL_2;
    MEMBER(float, self, FB::m_flNextPrimaryAttack) = NOW() + FBTiming::FireCooldown;
    MEMBER(float, self, FB::m_flTimeWeaponIdle)    = NOW();
    MEMBER(int,   player, FB::pl_ammo_base + ammoType * 4) = ammo - 1;

    if (pev) {
        float* eye = pev->angles();
        float org[3] = { pev->origin()[0], pev->origin()[1], pev->origin()[2] + 36.f };
        constexpr int   N     = (int)FBCfg::AModeFanAttackMeter;
        constexpr float SPREAD= FBCfg::AModeFanAttackAngle;
        constexpr float SPEED = FBCfg::BModeSpeed;
        constexpr float START = -(SPREAD / 2.f);
        constexpr float STEP  = SPREAD / (N - 1);
        constexpr float D2R   = 3.14159265f / 180.f;
        float baseYaw = eye[1], pitch = eye[0];
        for (int i = 0; i < N; ++i) {
            float yaw = (baseYaw + START + i * STEP) * D2R;
            float pr  = pitch * D2R;
            float d[3] = { cosf(pr)*cosf(yaw), cosf(pr)*sinf(yaw), -sinf(pr) };
            d[0] += RANDOM_FLOAT(-FBCfg::Accuracy, FBCfg::Accuracy);
            d[1] += RANDOM_FLOAT(-FBCfg::Accuracy, FBCfg::Accuracy);
            d[2] += RANDOM_FLOAT(-FBCfg::Accuracy, FBCfg::Accuracy);
            float len = sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
            if (len > 0.001f) { d[0]/=len; d[1]/=len; d[2]/=len; }
            uint8_t* edictB = reinterpret_cast<uint8_t*>(pev) - 12;
            int oidx = reinterpret_cast<int(__cdecl*)(const void*)>(
                g_engfuncs->pfnIndexOfEdict)(edictB);
            SpawnFrostbiteProjectile(org, d, SPEED, oidx);
        }
    }

    Log("[FB] PrimaryAttack OK: %d projectiles (charged=%d)\n", (int)FBCfg::AModeFanAttackMeter, charged);
    return 1;
}

static void __fastcall Hook_SecondaryAttack(void* ecx, void* /*edx*/)
{
    uint8_t* self = reinterpret_cast<uint8_t*>(ecx);
    void**   vt   = *reinterpret_cast<void***>(ecx);

    typedef void (__thiscall* BaseSecondary_t)(void*);
    auto base = reinterpret_cast<BaseSecondary_t>(vt[648/4]);
    if (base) base(ecx);

    if (NOW() <= MEMBER(float, self, FB::m_flNextPrimaryAttack)) return;
    MEMBER(float, self, FB::m_flNextPrimaryAttack) = NOW() + FBTiming::ShieldCooldown;

    bool charged = MEMBER(float, self, FB::m_flChargeState) > NOW();
    typedef int  (__thiscall* HasShield_t)(void*);
    typedef void (__thiscall* DeployShield_t)(void*, int, int);
    int shieldOn = 0;
    auto hasShield = reinterpret_cast<HasShield_t>(vt[688/4]);
    if (hasShield) shieldOn = hasShield(ecx);
    auto deploy = reinterpret_cast<DeployShield_t>(vt[652/4]);
    if (deploy) deploy(ecx, charged ? 6 : 0, shieldOn);

    Log("[FB] SecondaryAttack: shield deployed (charged=%d)\n", charged);
}

static void __fastcall Hook_Holster(void* ecx, void* /*edx*/)
{
    uint8_t* self = reinterpret_cast<uint8_t*>(ecx);
    MEMBER(int, self, FB::m_iClip)  = 0;
    MEMBER(int, self, FB::m_iClipB) = 0;
    Log("[FB] Holster\n");
}

static void __fastcall Hook_Think(void* ecx, void* /*edx*/)
{
    uint8_t* self = reinterpret_cast<uint8_t*>(ecx);
    if (NOW() > MEMBER(float, self, FB::m_flThinkTimer)) {
        int& ft = MEMBER(int, self, FB::m_iFreezeTimer);
        ft = ft + 1;
        MEMBER(float, self, FB::m_flThinkTimer) = NOW() + 1.f;
    }
    float sa = MEMBER(float, self, FB::m_flShieldActive);
    if (sa > 0.f && NOW() > sa) {
        MEMBER(int,   self, FB::m_iTriggerFlag)   = 0;
        MEMBER(float, self, FB::m_flShieldActive) = -1.f;
        MEMBER(float, self, FB::m_flShieldExpiry) = -1.f;
        uint8_t* pl = *reinterpret_cast<uint8_t**>(self + FB::m_pPlayer);
        if (pl) MEMBER(float, self, FB::m_flNextThink) = NOW();
    }
}

// ============================================================
// Apply vtable patches
// ============================================================
static bool ApplyPatches()
{
    if (!g_pWeaponVtable) {
        Log("[FB] ERROR: weapon vtable null, cannot patch\n");
        return false;
    }

    Log("[FB] Weapon vtable at %p — dumping first 16 slots before patching:\n", g_pWeaponVtable);
    __try {
        for (int i = 0; i < 16; ++i)
            Log("[FB]   vtable[%2d] = %p\n", i, g_pWeaponVtable[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[FB] SEH reading vtable slots — ptr %p is invalid!\n", g_pWeaponVtable);
        return false;
    }

    bool ok = true;
    __try {
        ok &= PatchSlot(g_pWeaponVtable, FBSlots::Slot_PrimaryAttack,   (void*)Hook_PrimaryAttack);
        ok &= PatchSlot(g_pWeaponVtable, FBSlots::Slot_SecondaryAttack, (void*)Hook_SecondaryAttack);
        ok &= PatchSlot(g_pWeaponVtable, FBSlots::Slot_Holster,         (void*)Hook_Holster);
        if (g_pPropVtable)
            ok &= PatchSlot(g_pPropVtable, FBSlots::Slot_Think, (void*)Hook_Think);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[FB] SEH during vtable patching — bad pointer or protected page\n");
        return false;
    }

    Log("[FB] Patches: %s\n", ok ? "ALL OK" : "PARTIAL FAILURE");
    return ok;
}

// ============================================================
// Public API
// ============================================================
bool FrostbiteFix_Init(HMODULE hMpDll)
{
    Log("\n=== FrostbiteFix_Init === mp.dll=%p\n", hMpDll);
    g_hMpDll = hMpDll;

    // Step 1 — engine funcs
    if (!GiveFnptrs_Init(hMpDll)) {
        Log("[FB] FATAL: GiveFnptrs_Init failed\n");
        return false;
    }

    // Step 1b — engine-side exports
    ResolveEngineHelpers();

    // Step 2 — find frostbite vtable
    __try {
        g_pWeaponVtable = reinterpret_cast<void**>(FindWeaponVtable(hMpDll));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[FB] SEH in FindWeaponVtable — mp.dll scan crashed!\n");
        return false;
    }
    if (!g_pWeaponVtable) {
        Log("[FB] FATAL: weapon vtable not found\n");
        return false;
    }

    // Prop vtable is 7 slots (28 bytes) before weapon vtable in client IDB
    g_pPropVtable = g_pWeaponVtable - 7;
    Log("[FB] PropVtable guess: %p\n", g_pPropVtable);

    // Step 3 — patch
    ApplyPatches();

    Log("=== FrostbiteFix_Init COMPLETE ===\n");
    return true;
}

void FrostbiteFix_Shutdown()
{
    Log("FrostbiteFix_Shutdown\n");
    if (g_log) { fclose(g_log); g_log = nullptr; }
}
