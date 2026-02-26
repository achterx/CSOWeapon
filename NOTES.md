# frostbite_fix — Build & Deployment Notes

## Quick Start

```bash
# Clone and build (Windows, 32-bit required)
git clone <your-repo>
cd frostbite_fix
cmake -B build -A Win32
cmake --build build --config Release

# Deploy
copy build\Release\frostbite_fix.dll  C:\hlds\

# Launch
C:\hlds\hlds.exe -useful frostbite_fix.dll -game czero +maxplayers 32 +map de_dust2
```

## GitHub Actions
Push to main/master → CI builds both Release and Debug x86 DLLs automatically.
Artifacts available for 30 days (Release) / 14 days (Debug) under Actions → latest run.

---

## What This Patches

- **CFrostbite::PrimaryAttack** — 6-projectile 180° fan freeze attack, 1.97s cooldown
- **CFrostbite::SecondaryAttack** — ice shield deploy, 80.3s cooldown
- **CFrostbite::Holster** — state reset (clip=0, idletime=0)
- **FROSTBITE_prop::Think** — freeze timer tick, shield expiry, buff list cleanup stub

## How vtable resolution works

1. Scan mp.dll for `"FROSTBITE\0"` string
2. Find the `PUSH imm32` referencing it in .text
3. Walk back ≤256 bytes for `MOV [ecx], imm32` → that imm32 is the vtable ptr
4. FROSTBITE_prop vtable = weapon vtable − 28 bytes (7 pointers, matches client IDB diff)

No hardcoded addresses. Works across mp.dll builds as long as the string survives.

## How engine function resolution works

- `GiveFnptrsToDll` — named export in mp.dll, fills full `enginefuncs_t` in one call
- `DispatchSpawn`, `SetOrigin` — named exports in hw.dll (client) / swds.dll (dedicated)

---

## Open Items

### 1. Vtable slot numbers
Slots in `FBSlots::` are from the CLIENT IDB. Verify against server mp.dll:
in IDA, open CFrostbite vtable → count entries before first Frostbite-unique function.
Adjust `FBSlots::Slot_*` constants if server has extra base class virtuals.

### 2. Owner edict index
`SpawnFrostbiteProjectile` currently passes `pfnEntOffsetOfPEntity(playerPev)`.
This returns a **byte offset**, not an edict index. If projectiles don't recognise
their owner, swap to:
```cpp
reinterpret_cast<int(*)(entvars_t*)>(g_engfuncs->pfnIndexOfEdict)(playerPev)
```

### 3. Precached event index
`Hook_PrimaryAttack` passes event index `0` to `PLAYBACK_EVENT_FULL`.
The real index comes from `PRECACHE_EVENT(1, "events/frostbite.sc")` in Precache().
Capture it at Precache time and store it, or pattern-scan for the stored value at
`this + eventIndex_offset` (unknown offset — check sub_10B30B50 in IDA).

### 4. Think buff list cleanup
`Hook_Think` has a stub where expired buff nodes should be removed from `self+520`.
Node layout: `node+20` = expiry float. Remove if `< NOW()`.
Full impl needs the list node struct from `sub_10633A10`.

### 5. Eye height for projectile spawn
Currently uses `origin[2] + 36.0f` as eye height approximation.
Real value: `player->pev->view_ofs[2]` which is at `playerPev + 104 + 8 = pev+112`.
```cpp
float eyeZ = playerPev->origin()[2] + *reinterpret_cast<float*>((uint8_t*)playerPev + 112);
```

### 6. build.yml artifact path
If the DLL lands in `build/frostbite_fix.dll` instead of `build/Release/frostbite_fix.dll`,
the upload-artifact step covers both paths with a glob. Check Actions output to confirm.
