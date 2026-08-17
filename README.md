# SC13 Mod Loader

SC13 Mod Loader is an experimental runtime modding framework for SimCity (2013) and
Cities of Tomorrow. Milestone M2 implements and verifies one exact-build runtime PROP patch at
the game's concrete synchronous deserializer.
It does not modify, regenerate, or merge any game `.package` file.

## Current state

- The local SimCity executable is confirmed as x86, version `10.3.6.0`.
- A bounds-checked DBPF v3 reader and RefPack decompressor can locate the known Sewage
  Sanitizer TGI.
- A byte-preserving PROP parser reads all 89 properties in the real resource and reads
  `0x09AE19D7` as `300.0`.
- The x86 DLL fingerprints the process, inventories modules, and installs only the uniquely
  resolved PROP deserializer hook in a normal build.
- Earlier package-I/O, stream, publisher, `PFRecordRead`, and executable-image instrumentation is
  retained only behind the explicit `SC13_ENABLE_DISCOVERY_TRACE` CMake option.
- The exact runtime path is verified as TGI owner -> 89-entry property vector ->
  `0x09AE19D7`. The patch changes its aligned float from `300.0` to `345.0` only after every
  build, TGI, layout, count, order, type, and old-value check succeeds.
- Earlier in-game validation showed `545 $/h` in the building selector and an actual
  simulation charge of `545 $/h` (`345` for the Sanitizer plus `200` for its spawned tank).
  The already-placed building panel continued to display the stale vanilla total `500 $/h`;
  this is a presentation-cache discrepancy, not the simulation result.
- A watcher-before-game run on PID 30180 reached the deserializer patch and showed the changed
  value after map load. Controlled detach atomically restored `300.0` and unloaded the DLL while
  the game stayed open.

The scalar interception primitive and the complete M2 gameplay checklist are proven, including
controlled detach and post-detach restoration of the vanilla UI.
General patch declarations, multiple mods, conflict handling, Lua, overlays, and legacy-package
importing remain outside this milestone.

## Build

Visual Studio 2022:

```powershell
cmake --preset vs2022-x86
cmake --build --preset vs2022-release --parallel
ctest --preset vs2022-release
```

This machine also has Visual Studio 2026, so the verified local build used:

```powershell
cmake --preset vs2026-x86
cmake --build --preset vs2026-release --parallel
ctest --preset vs2026-release
```

Runtime artifacts are placed in `build/<preset>/bin/<configuration>/`.

## Read-only resource inspection

```powershell
sc13-inspect.exe scan `
  "<SimCity-install>" `
  "00B1B104:61EFC000:719436BD" `
  "0x09AE19D7"
```

The tool reads indices and selected resource bytes in memory. It never exports or writes a
package.

## First runtime test

Follow [docs/first-test.md](docs/first-test.md). The loader log is written beside the DLL in
`logs/sc13modloader.log`, not into `SimCityData`.

For the verified local workflow, double-click `Start-Simoder.bat`. It starts or reuses the elevated
watcher first and then launches SimCity through the installed EA App.

## SimCity startup troubleshooting

The base game can intermittently remain after the intro with a permanent loading spinner. On the
validated machine, two affected sessions eventually recovered without loader intervention, while
three consecutive launches reached the menu normally after disabling the EA App in-game overlay.
The successful no-overlay process also produced no EA IGO log. This is a practical workaround, not
proof that the overlay is the underlying cause. If the symptom returns, disable
**EA App -> Settings -> Application -> In-game overlay** before launching through
`Start-Simoder.bat`.

## Repository policy

Proprietary EA binaries and package files are excluded. Tests use minimal synthetic fixtures.
No license has been selected for this new repository yet; choose one before public release.
