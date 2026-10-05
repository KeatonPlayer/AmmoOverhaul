# DiverseCasings — Build 1 (.308 test)

An experimental xNVSE DLL that selects a casing NIF from the ammunition fired by
each actor. Includes C++ source, a preconfigured INI and a GitHub Actions build.
This is a test build, not an in-game-validated release.

## Build on GitHub

1. Extract this source ZIP.
2. Upload the **contents** of `DiverseCasings` to the root of a GitHub repository.
   `CMakeLists.txt` must be at repository root, and the workflow must be at
   `.github/workflows/build.yml`. Include that dot-prefixed folder.
3. Open **Actions → Build DiverseCasings**. A push starts the build automatically;
   the workflow also supports **Run workflow**.
4. Open the successful run and download **DiverseCasings-Build1-Win32** from
   **Artifacts**. The artifact includes `NVSE/Plugins/DiverseCasings.dll` and INI.

The workflow uses Windows 2022, Visual Studio's Win32 compiler, CMake, and a
statically linked MSVC runtime. There are no SDK downloads or external C++
dependencies. The native target is 32-bit; selecting x64 is rejected.

Local Visual Studio build (Desktop development with C++ installed):

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist
```

## Install the test

Requires Fallout New Vegas **1.4.0.525, standard non-NoGore runtime**, and xNVSE 6
with messaging interface v4. Use your existing working xNVSE installation.
JIP/JohnnyGuitar are not required by this DLL itself.

Install the generated artifact as a mod so `NVSE/Plugins` appears under the
game's `Data` directory. Enable the actual `GrandAmmoOverhaul.esp` from Overwrite.
The `(3)` upload suffix is not part of the INI plugin name. If your installed
plugin really has another filename, change `Plugin=` accordingly.

Both meshes must be present at:

```text
Data\Meshes\GrandAmmoOverhaul\.308\Brass 308.nif
Data\Meshes\GrandAmmoOverhaul\.308\Polymer 308.nif
```

These NIFs/textures are user-supplied and are not included. By default a missing
loose NIF disables its individual rule and is logged. MO2 virtual files should
be visible when launching the game through MO2. `RequireLooseMeshes=0` allows
engine BSA resolution instead; it does not verify that the BSA contains a mesh.

Disable the old casing-changing quest/UDF or ScriptRunner implementation for
this test, so its shared-record setter does not interfere with comparisons.
Keep any dependencies used by your other mods. Restart after INI edits.

## Configured mapping

| Priority | Origin plugin | Local FormID | EditorID | Model relative to Data\Meshes |
|---|---|---|---|---|
| 1 | GrandAmmoOverhaul.esp | 001673 | Ammo308Polymer | GrandAmmoOverhaul\.308\Polymer 308.nif |
| 2 | FalloutNV.esm | 1537E9 | AmmoList308 | GrandAmmoOverhaul\.308\Brass 308.nif |

Verified in the supplied Overwrite ESP: the polymer record's file ID is
`03001673`; `03` is that file's master/self index, not a permanent game load
index. The DLL resolves the plugin's actual runtime index. The list originates
in FalloutNV.esm; the DLL reads the live winning override, which includes polymer.

All `Type=Ammo` rules precede all `Type=List` rules. Within a type, the first
matching rule wins. Add consecutively numbered sections up to `Rule64` for
additional mappings. FormLists may nest up to eight levels. A direct-AMMO
weapon works too: matching uses the native selected ammo, not the weapon's
AMMO-versus-FLST field type. No matching rule means original casing.

## What the DLL does

The supplied executable was inspected without executing it. The three call
sites below were verified in its decoded code section:

| Address | Original callee | Purpose |
|---|---|---|
| 00523ABE | 00525980 | Capture the ammo selected by the native weapon-fire routine |
| 00524E0B | 004600D0 | Select a per-ejection model at the native casing routine's first model read |
| 00524F31 | 004600D0 | Return the same model at the subsequent path read |

The firing hook forwards to the original ammo selector, preserves its returned
ammo, and queues one snapshot per call, keyed by actor pointer, actor FormID
and weapon FormID. Ejection consumes a snapshot before the native visibility
checks, so culled casing visuals normally consume their corresponding snapshot
too. The second model read uses the same native stack-frame context.

A temporary, borrowed TESModel view carries the chosen path. The shared weapon
record is never modified. The engine continues its own casing creation,
physics and animation-event timing. This plugin does not call EjectCasing,
spawn extra references, poll every frame, or edit animations. An originally
empty shell-casing model stays empty.

All three original CALL instructions must match before any are changed.
Unknown runtime versions and already-patched call sites fail closed. The log
reports the failed address. This does not prove compatibility with every mod:
a plugin that changes surrounding code or patches later can still conflict.

## In-game validation still required

Use a separate test save. Start with a .308 weapon that already ejects a visible
casing normally and whose animations provide the usual ejection event.

1. Fire ordinary .308: check for the brass mesh.
2. Switch to Ammo308Polymer: check for the polymer mesh.
3. Switch back: check that brass returns immediately.
4. Give an NPC the **same base weapon** with different ammo. Alternate shots
   between player and NPC and verify their casings stay independent.
5. Test a bolt-action weapon, automatic weapon, first/third person, and VATS.
   Include the final round of a magazine and firing immediately after reloading.
6. Test a delayed ejection followed by an ammo switch, then save/reload and cell
   transitions. Confirm timing matches baseline and no additional casings appear.
7. Fire an unmatched caliber: its original casing should remain.

For the two-actor test, make sure the NPC is actually firing polymer; the SHOT
log line confirms the native ammo ID. Possessing ammo alone is not proof.

`DiverseCasings.log` is written beside `FalloutNV.exe`. Expect **Installed three
verified call-site hooks**, two **Active** rules, and paired **SHOT/EJECT** lines.
Rule 1 is polymer, Rule 2 is the .308 list, and Rule 0 means original casing.
Send that log plus the tested weapon/plugin and observed behavior if it fails.
Set `DebugLog=0` after testing to remove per-shot file I/O.

## Limits of Build 1

- Event pairing assumes one native ejection event per captured firing call.
  A custom animation that omits, duplicates or batches ejection events can
  desynchronize the queue. Reload-ejection/revolver behavior is not yet proven.
  Logs and the test matrix are needed before calling this universally compatible.
- The queue holds 32 shots per actor/weapon and 128 actor/weapon combinations.
  Overflow uses original casings for that slot until an inactivity timeout.
  Expired or missing snapshots use the original casing; the DLL does not guess
  from newly equipped ammo. Default timeout is 30 seconds of wall-clock time,
  including pauses, so long pauses/VATS sequences can expire pending snapshots.
- Pending shots are cleared on load/new-game/main-menu transitions and are not
  stored in saves. An already-pending ejection after a load may use the original.
- This is a casing **model** test. It does not choose distinct ejection velocity,
  direction, sounds or material physics for each ammo type.
- No weapon blacklist or hot reload is implemented in this first test build.
- The NIFs were not supplied, so their scale, collision and textures have not
  been inspected or validated.

## Validation performed here

- Parsed the supplied Overwrite ESP and verified the two rule records and list
  membership; the ESP and game executable were not modified.
- Verified hook bytes and the actor/weapon stack locations in the supplied
  executable's native routines.
- Compiled and linked the source as a Windows x86 DLL using Zig/Clang 0.16.0;
  verified PE machine `0x14C` and the `NVSEPlugin_Query` / `NVSEPlugin_Load` exports.
- Ran the portable C++ queue tests covering actor isolation, mixed-ammo FIFO
  ordering, unmatched shots, expiry, duplicate pops, resets and overflow.
- GitHub's MSVC workflow and the DLL inside Fallout New Vegas have **not** been
  run here. The source package contains no prebuilt DLL; Actions builds it.

## Source references

These public sources informed ABI/engine-layout verification. The implementation
is a small independent plugin, not a redistributed JIP or JohnnyGuitar build.

- [xNVSE PluginAPI.h](https://github.com/xNVSE/NVSE/blob/0ccd23ad885ddae533c1790a3fc56cd073e38de3/nvse/nvse/PluginAPI.h)
- [xNVSE form lookup](https://github.com/xNVSE/NVSE/blob/0ccd23ad885ddae533c1790a3fc56cd073e38de3/nvse/nvse/GameAPI.cpp)
- [xNVSE map layout](https://github.com/xNVSE/NVSE/blob/0ccd23ad885ddae533c1790a3fc56cd073e38de3/nvse/nvse/NiTypes.h)
- [JohnnyGuitar TESObjectWEAP](https://github.com/carxt/JohnnyGuitarNVSE/blob/1fd2c43a44a714c566e54c013cbeb973c4b46e28/JG/internal/Game/Bethesda/TESObjectWEAP.cpp)
- [JIP form layouts](https://github.com/jazzisparis/JIP-LN-NVSE/blob/5a30ac4356ea0e93b9ff357b5031b1e420240a4d/nvse/GameForms.h)

Input hashes (for reproducibility):

```text
FalloutNV.exe SHA-256:
518c87f58a6c4d9826e9ef8fbb7f4213882fa70822675610d45aea2464502a57
Overwrite ESP uploaded as GrandAmmoOverhaul(3).esp SHA-256:
7dbbbee0937d53095ab9c868492590b7268f8e3ef58a625fa81a3fa5dd96a6b2
```
