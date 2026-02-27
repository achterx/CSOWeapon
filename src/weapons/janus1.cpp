// janus1.cpp — CJanus1, Janus-1 grenade launcher
// v3: factory now swaps vtable after calling original constructor

#include "janus1.h"
#include "../hooks.h"
#include "../logger.h"
#include <cstring>
#include <cstdint>
#include <windows.h>

// -----------------------------------------------------------------------
// mp.dll function types
// -----------------------------------------------------------------------
typedef int   (__cdecl* pfnPrecacheModel_t)(const char*);
typedef int   (__cdecl* pfnPrecacheSound_t)(const char*);
typedef int   (__cdecl* pfnPrecacheEvent_t)(int type, const char* name);

static pfnPrecacheModel_t g_PrecacheModel = nullptr;
static pfnPrecacheSound_t g_PrecacheSound = nullptr;
static pfnPrecacheEvent_t g_PrecacheEvent = nullptr;
static void*              g_engfuncsTable = nullptr;

static void ResolveEngineFns()
{
    if (!g_engfuncsTable) return;
    void** tbl = (void**)g_engfuncsTable;
    g_PrecacheModel = (pfnPrecacheModel_t)tbl[0];
    g_PrecacheSound = (pfnPrecacheSound_t)tbl[1];
    g_PrecacheEvent = (pfnPrecacheEvent_t)tbl[140];
    Log("[janus1] PrecacheModel=0x%08zX PrecacheSound=0x%08zX PrecacheEvent=0x%08zX\n",
        (uintptr_t)g_PrecacheModel, (uintptr_t)g_PrecacheSound, (uintptr_t)g_PrecacheEvent);
}

// -----------------------------------------------------------------------
// Vtable
// -----------------------------------------------------------------------
static void*  g_vtable[220]  = {};
static bool   g_vtableReady  = false;
static void** g_m79Vtable    = nullptr;

// CM79 vtable RVA (??_7CM79@@6B@ @ 0x1159FA04)
static const uintptr_t RVA_CM79_vtable = 0x159FA04;

// -----------------------------------------------------------------------
// Our virtual methods — __fastcall so MSVC puts 'this' in ECX
// -----------------------------------------------------------------------

// Slot 3: Precache
static int __fastcall J1_Precache(void* self, void*)
{
    Log("[janus1] Precache self=%p\n", self);
    if (g_PrecacheSound) {
        g_PrecacheSound("weapons/janus1-1.wav");
        g_PrecacheSound("weapons/janus1-2.wav");
        g_PrecacheSound("weapons/janus1_reload.wav");
    }
    if (g_PrecacheModel) {
        g_PrecacheModel("models/v_janus1.mdl");
        g_PrecacheModel("models/p_janus1.mdl");
        g_PrecacheModel("models/w_janus1.mdl");
    }
    if (g_PrecacheEvent) {
        uint16_t ev = (uint16_t)g_PrecacheEvent(1, "events/janus1.sc");
        *reinterpret_cast<uint16_t*>((uint8_t*)self + F_usFireEvent) = ev;
        Log("[janus1] event=%d\n", ev);
    }
    return 1;
}

// Slot 102: Deploy — delegate to M79
static int __fastcall J1_Deploy(void* self, void*)
{
    Log("[janus1] Deploy\n");
    if (g_m79Vtable) {
        typedef int(__thiscall* Fn)(void*);
        return reinterpret_cast<Fn>(g_m79Vtable[102])(self);
    }
    return 1;
}

// Slot 165: WeaponIdle — delegate to M79
static void __fastcall J1_WeaponIdle(void* self, void*)
{
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[165])(self);
    }
}

// Slot 167: PrimaryAttack — delegate to M79
static void __fastcall J1_PrimaryAttack(void* self, void*)
{
    Log("[janus1] PrimaryAttack t=%.3f\n", GetTime());
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*);
        reinterpret_cast<Fn>(g_m79Vtable[167])(self);
    }
}

// Slot 175: AddToPlayer — delegate to M79
static int __fastcall J1_AddToPlayer(void* self, void*, void* player)
{
    Log("[janus1] AddToPlayer player=%p\n", player);
    if (g_m79Vtable) {
        typedef int(__thiscall* Fn)(void*, void*);
        int r = reinterpret_cast<Fn>(g_m79Vtable[175])(self, player);
        Log("[janus1] AddToPlayer ret=%d\n", r);
        return r;
    }
    return 0;
}

// Slot 191: Holster — delegate to M79
static void __fastcall J1_Holster(void* self, void*, int skip)
{
    Log("[janus1] Holster\n");
    if (g_m79Vtable) {
        typedef void(__thiscall* Fn)(void*, int);
        reinterpret_cast<Fn>(g_m79Vtable[191])(self, skip);
    }
}

// -----------------------------------------------------------------------
// Build vtable: copy M79's 220 slots, override ours
// -----------------------------------------------------------------------
static bool BuildVtable(uintptr_t mpBase)
{
    void** m79 = reinterpret_cast<void**>(mpBase + RVA_CM79_vtable);
    uint32_t check = 0;
    __try { check = (uint32_t)(uintptr_t)m79[0]; }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[janus1] Bad M79 vtable\n"); return false; }

    Log("[janus1] M79 vtable @ 0x%08zX  slot[0]=0x%08X\n", (uintptr_t)m79, check);

    __try { memcpy(g_vtable, m79, 220 * sizeof(void*)); }
    __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_m79Vtable = m79;

    // Override slots with our implementations
    g_vtable[3]   = (void*)J1_Precache;
    g_vtable[102] = (void*)J1_Deploy;
    g_vtable[165] = (void*)J1_WeaponIdle;
    g_vtable[167] = (void*)J1_PrimaryAttack;
    g_vtable[175] = (void*)J1_AddToPlayer;
    g_vtable[191] = (void*)J1_Holster;

    g_vtableReady = true;
    Log("[janus1] Vtable ready\n");
    return true;
}

// -----------------------------------------------------------------------
// Factory
//
// From IDA decompile of weapon_janus1 (same pattern as weapon_frostbite):
//   void __cdecl weapon_janus1(int edict_ptr)   <- edict_t* passed as int
//   {
//     if (!edict_ptr) edict_ptr = CreateEntity() + 132
//     entvars = *(edict_ptr + 568)              <- pev
//     if (!entvars || !*(entvars + 128))        <- if no private data yet
//     {
//       obj = AllocEntPrivateData(entvars, SIZE)
//       if (obj)
//         *(Constructor(obj) + 8) = edict_ptr   <- obj->edict = edict
//     }
//   }
//
// So the object pointer is *(entvars + 128) after the factory runs.
// We call the original, then read that pointer and swap vtable[0].
// -----------------------------------------------------------------------
typedef void(__cdecl* pfnOrigFactory_t)(int);

static uint8_t          g_savedBytes[5] = {};
static bool             g_savedOk       = false;

static void CallOrigAndSwap(int edict)
{
    uintptr_t fnAddr = GetMpBase() + RVA_weapon_janus1;

    // 1. Temporarily restore original bytes
    DWORD old = 0;
    VirtualProtect((void*)fnAddr, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)fnAddr, g_savedBytes, 5);
    VirtualProtect((void*)fnAddr, 5, old, &old);

    // 2. Call original factory — allocates and constructs object
    reinterpret_cast<pfnOrigFactory_t>(fnAddr)(edict);

    // 3. Re-patch immediately
    WriteJmp5(fnAddr, (uintptr_t)Janus1_Factory, nullptr);

    // 4. Find the object:
    //    entvars_t* pev = *(edict_t*)(edict + 568)  [edict is edict_t* here]
    //    void* obj      = *(pev + 128)
    void* obj = nullptr;
    __try
    {
        uint8_t* edictPtr = reinterpret_cast<uint8_t*>((uintptr_t)edict);
        uint8_t* pev      = *reinterpret_cast<uint8_t**>(edictPtr + 568);
        obj               = *reinterpret_cast<void**>(pev + 128);
    }
    __except(EXCEPTION_EXECUTE_HANDLER)
    {
        Log("[janus1] Exception reading object ptr from edict 0x%08X\n", edict);
        return;
    }

    if (!obj)
    {
        Log("[janus1] obj is null after factory\n");
        return;
    }

    Log("[janus1] obj=%p  current vtable=0x%08X\n",
        obj, *reinterpret_cast<uint32_t*>(obj));

    // 5. Swap vtable pointer at obj[0]
    DWORD old2 = 0;
    if (VirtualProtect(obj, 4, PAGE_EXECUTE_READWRITE, &old2))
    {
        *reinterpret_cast<void**>(obj) = (void*)g_vtable;
        VirtualProtect(obj, 4, old2, &old2);
        Log("[janus1] Vtable swapped -> 0x%08zX\n", (uintptr_t)g_vtable);
    }
    else
    {
        Log("[janus1] VirtualProtect failed on obj\n");
    }
}

void __cdecl Janus1_Factory(int edict)
{
    Log("[janus1] Factory edict=0x%08X t=%.3f vtableReady=%d\n",
        edict, GetTime(), (int)g_vtableReady);

    if (!g_vtableReady || !g_savedOk)
    {
        Log("[janus1] Not ready — calling original directly\n");
        // Just call original without swap if we're not set up yet
        if (g_savedOk)
        {
            uintptr_t fnAddr = GetMpBase() + RVA_weapon_janus1;
            DWORD old = 0;
            VirtualProtect((void*)fnAddr, 5, PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)fnAddr, g_savedBytes, 5);
            VirtualProtect((void*)fnAddr, 5, old, &old);
            reinterpret_cast<pfnOrigFactory_t>(fnAddr)(edict);
            WriteJmp5(fnAddr, (uintptr_t)Janus1_Factory, nullptr);
        }
        return;
    }

    CallOrigAndSwap(edict);
    Log("[janus1] Factory done\n");
}

// -----------------------------------------------------------------------
// PostInit — called after hooks are installed
// -----------------------------------------------------------------------
void Janus1_PostInit(uintptr_t mpBase)
{
    Log("[janus1] PostInit\n");

    // Save original bytes (hooks.cpp already wrote JMP — read current bytes
    // won't work. We need to save BEFORE the patch. We do it here by reading
    // what hooks.cpp saved. Since we don't expose that, re-read from the
    // trampoline: the 5 original bytes are what we need to reconstruct.
    // WORKAROUND: read the current JMP and reconstruct original from IDA known bytes.
    // 
    // Get the original bytes that hooks.cpp saved before patching
    const uint8_t* saved = GetSavedBytes(RVA_weapon_janus1);
    if (saved)
    {
        memcpy(g_savedBytes, saved, 5);
        g_savedOk = true;
        Log("[janus1] Saved bytes from hook: %02X %02X %02X %02X %02X\n",
            g_savedBytes[0], g_savedBytes[1], g_savedBytes[2],
            g_savedBytes[3], g_savedBytes[4]);
    }
    else
    {
        // Fallback: confirmed from IDA weapon_janus1 @ 0x10E96640
        // 55 8B EC 56 8B  (PUSH EBP; MOV EBP,ESP; PUSH ESI; MOV ESI,[ebp+8])
        g_savedBytes[0] = 0x55;
        g_savedBytes[1] = 0x8B;
        g_savedBytes[2] = 0xEC;
        g_savedBytes[3] = 0x56;
        g_savedBytes[4] = 0x8B;
        g_savedOk = true;
        Log("[janus1] Saved bytes from IDA fallback: %02X %02X %02X %02X %02X\n",
            g_savedBytes[0], g_savedBytes[1], g_savedBytes[2],
            g_savedBytes[3], g_savedBytes[4]);
    }

    // Build vtable from M79
    BuildVtable(mpBase);

    // Find engfuncs table by scanning for longest run of hw.dll pointers
    HMODULE hMp = (HMODULE)mpBase;
    HMODULE hHw = GetModuleHandleA("hw.dll");
    if (hHw)
    {
        IMAGE_DOS_HEADER* hdos = (IMAGE_DOS_HEADER*)hHw;
        IMAGE_NT_HEADERS* hnt  = (IMAGE_NT_HEADERS*)((uint8_t*)hHw + hdos->e_lfanew);
        uintptr_t hwLo = (uintptr_t)hHw;
        uintptr_t hwHi = hwLo + hnt->OptionalHeader.SizeOfImage;

        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hMp;
        IMAGE_NT_HEADERS* nt  = (IMAGE_NT_HEADERS*)((uint8_t*)hMp + dos->e_lfanew);
        uint8_t* base = (uint8_t*)mpBase;
        DWORD sz = nt->OptionalHeader.SizeOfImage;

        size_t bestOff = 0; int bestRun = 0, cur = 0; size_t start = 0;
        for (size_t off = 0; off+4 <= sz; off += 4)
        {
            uint32_t v = 0;
            __try { v = *reinterpret_cast<uint32_t*>(base+off); }
            __except(EXCEPTION_EXECUTE_HANDLER) { cur=0; continue; }
            if (v >= hwLo && v < hwHi) { if (!cur) start=off; cur++; }
            else { if (cur>bestRun){bestRun=cur;bestOff=start;} cur=0; }
        }
        if (bestRun >= 20)
        {
            g_engfuncsTable = base + bestOff;
            ResolveEngineFns();
        }
        else Log("[janus1] engfuncs not found (bestRun=%d)\n", bestRun);
    }

    Log("[janus1] PostInit done. vtable=%s savedOk=%d\n",
        g_vtableReady?"OK":"FAIL", (int)g_savedOk);
}

// -----------------------------------------------------------------------
// Static registration
// -----------------------------------------------------------------------
struct Janus1Reg {
    Janus1Reg() {
        RegisterWeaponHook("weapon_janus1", (void*)Janus1_Factory, RVA_weapon_janus1);
    }
};
static Janus1Reg g_reg;