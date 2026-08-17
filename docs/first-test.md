# M2 runtime acceptance test

This procedure verifies the exact-build Sewage Sanitizer runtime patch. It changes only property
`0x09AE19D7` in process memory and never writes a `.package` file.

## 1. Build and verify the offline baseline

From the repository root:

```powershell
cmake --preset vs2026-x86
cmake --build --preset vs2026-release --parallel
ctest --preset vs2026-release

& '.\build\vs2026\bin\Release\sc13-launcher.exe' --self-check

& '.\build\vs2026\bin\Release\sc13-inspect.exe' scan `
  '<SimCity-install>' `
  '00B1B104:61EFC000:719436BD' `
  '0x09AE19D7'
```

Every canonical current-package match must decompress to resource SHA-256
`49E10D6E3512E34821B54C75DE978D398E23347695442A9F1C077B37AF65886B`, parse as 89 properties,
and report `300` for the target property. Do not use an unrelated custom package as the vanilla
baseline and never alter EA's canonical packages.

## 2. Start the watcher and game

Double-click `Start-Simoder.bat` and approve its UAC prompt. The batch starts one elevated watcher,
reuses it if it is already active, and asks the installed EA App to launch SimCity. It intentionally
starts the watcher before the game so the deserializer hook is active before the target preload.

For a manual fallback after an EA-started `SimCity.exe` exists:

```powershell
& '.\build\vs2026\bin\Release\sc13-launcher.exe' --watch-attach 4294967295 `
  '.\build\vs2026\bin\Release\sc13modloader.dll'
```

The watcher exits successfully only after the DLL signals that the exact runtime write occurred.

## 3. Verify runtime evidence

The ignored log at `build/vs2026/bin/Release/logs/sc13modloader.log` must contain:

```text
Resolved PROP deserializer uniquely: module=SimCity.exe RVA=0x00009710 ...
[SC13][PROP-DESERIALIZE] ... TGI=00B1B104:61EFC000:719436BD ...
    bounds=valid table=valid baseline=verified ...
[SC13][PROP-DESERIALIZE] id=0x09AE19D7 float=300.000
[SC13][PROP-DESERIALIZE] id=0x0AFB9882 float=72.000
[SC13][PROP-DESERIALIZE] id=0x0C09DA83 float=2.000
[SC13][PROP-DESERIALIZE] id=0x0FD16C15 float=200.000
[SC13][PATCH] ... id=0x09AE19D7 old=300.000 new=345.000 ... bytes=4 ... status=applied
```

The normal build installs only the critical deserializer hook. Package-I/O, stream, publisher,
PFRecordRead, and executable-image tracing exist only in an explicit
`SC13_ENABLE_DISCOVERY_TRACE=ON` diagnostic build.

## 4. Verify gameplay

Load a city that can construct the Sewage Sanitizer and check every item:

1. the utility selector shows a `545 $/h` total (`345` Sanitizer plus unchanged `200` tank);
2. placing the Sanitizer succeeds and does not expose raw localization/property strings;
3. the placed-building hourly-cost UI is recorded as either `545` or a reproducible stale value;
4. the city budget decreases by `545 $/h` while the building is operating;
5. the Sanitizer can be disabled and enabled again;
6. its spawned tank and add-on modules continue to work;
7. the building can be bulldozed without a crash;
8. saving/reloading the city does not corrupt the building or save.

The budget delta is the authoritative simulation assertion. A stale placed-building presentation
value must be documented separately rather than treated as evidence that the simulation remained
vanilla.

## 5. Verify controlled detach and vanilla restoration

While the game is still running, execute from an elevated terminal:

```powershell
& '.\build\vs2026\bin\Release\sc13-launcher.exe' --detach <pid> `
  '.\build\vs2026\bin\Release\sc13modloader.dll'
```

The log must contain:

```text
[SC13][PATCH] rollback ... status=restored-300
SC13 Mod Loader shutting down
```

Reload the map after detach. A fresh Sanitizer load must return to the vanilla `500 $/h` total,
and the game must remain functional. Starting the game normally without the watcher provides the
same vanilla control.

## 6. Verify package integrity

Hash each canonical `SimCityDLCEP1-Scripts_287520926.package` before and after the run. On the
validated installation all current copies remain:

```text
SHA-256 0A3F66804D9A99A626509B42C76D3C308A53F9FB2D84529F621A88BF85043FF9
size    409297 bytes
```

No `.package`, executable-memory capture, minidump, or runtime log belongs in source control.

## Validated result

The complete checklist passed on 2026-08-17 against SimCity `10.3.6.0`:

- the active runtime patch produced `545 $/h` and the expected `300.0 -> 345.0` log record;
- the simulation charge, enable/disable flow, spawned tank, add-on modules, bulldoze, and
  save/reload behavior were manually verified;
- controlled detach logged `restored-300`, unloaded the DLL, and left the game responsive;
- reloading the map after detach restored the vanilla `500 $/h` UI;
- both canonical `287520926` package copies remained 409297 bytes with the documented SHA-256.
