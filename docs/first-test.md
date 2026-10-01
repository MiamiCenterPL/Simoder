# M3 runtime acceptance test

This procedure validates Simoder discovery, UI, ON/OFF state, runtime rebuilds, logging, and clean
detach without changing a `.package` file.

## 1. Build and stage

```powershell
cmake --preset vs2026-x86
cmake --build --preset vs2026-release --parallel
ctest --preset vs2026-release
cmake --install build/vs2026 --config Release --prefix '<SimCity-install>'
& '<SimCity-install>\Start-Simoder.bat' --check
& '<SimCity-install>\simoder\simoder.exe' --self-check
```

Confirm `config.toon` has `developerConsole.enabled: false` for the default-console test. Delete or
edit `simoder/state.toon` so Better Sanitizer is initially absent from `enabled`.

## 2. Start through EA App

Double-click `Start-Simoder.bat` and approve UAC. The current EA-launched game requires elevation,
so rejecting UAC prevents the watcher from attaching. If base-game infinite loading returns,
disable the EA App in-game overlay and retry.

Load a city that can construct the Sewage Sanitizer. The always-visible `Simoder` button should
open a table with Name, Version, Author, State, and Enabled columns.

## 3. Discovery and inactive baseline

Verify `Better Sanitizer 1.0.0 Dawid` appears as `Inactive` and OFF. With it OFF, reload the map or
otherwise force a fresh Sanitizer resource load. The selector must show vanilla `500 $/h`
(`300 + 200`). No runtime patch event for its mod ID should appear.

Copy a second valid test mod directory into `mods/` while the game runs. It must not appear before
Refresh. After Refresh it must appear `Inactive`/OFF and must not register a patch.

## 4. Enable and disable

Switch Better Sanitizer ON, then recreate the building or reload the map. Verify:

1. its state is `Active`;
2. `state.toon` contains `dawid.better-sanitizer`;
3. the selector and simulation charge are `545 $/h` (`345 + 200`);
4. placement, enable/disable, tank/modules, bulldoze, save, and reload remain functional;
5. the log includes `[MOD] [dawid.better-sanitizer] [PATCH]` and the exact target TGI.

Switch it OFF. The retained live PROP resource must return immediately to `500 $/h` from vanilla
plus the remaining active set. No subtraction/division rollback is involved.

While it is OFF, change property `0x09AE19D7` in `overrides.toon`, then switch it ON without
clicking Refresh. The selector and details must use the new value. A subsystem that copied an
earlier PROP value may require its panel or object to be recreated, but the retained PROP instance
and log must show the new generation immediately.

## 5. Refresh and removal

- Edit an active mod, click Refresh, and verify `Changed`; its old active definition remains until
  OFF/ON.
- Delete an inactive mod and Refresh; its entry disappears.
- Delete an active mod and Refresh; its registry entries and persisted enabled ID disappear, while
  the UI reports `Missing`. Future loads use the remaining active set.
- Add two folders with the same manifest ID; both must be `Failed` and diagnostics must name both
  paths. Other valid mods must continue.

## 6. Logging and optional console

With the default config, no external console window should appear and file logging should continue
under `simoder/logs/sc13modloader.log`. Set `developerConsole.enabled: true`, restart the game, and
verify a separate `Simoder Developer Console` appears. Loader and mod messages must be classified;
any game `OutputDebugStringA/W` message must be tagged `GAME`.

## 7. Controlled detach

From an elevated terminal:

```powershell
& '<SimCity-install>\simoder\simoder.exe' --detach <pid>
```

The original WndProc and ImGui context must be removed, every still-matching retained resource must
be restored to vanilla, the DLL must unload, and the game must remain responsive.

## 8. Integrity

Compare canonical package hashes before and after. The validated
`SimCityDLCEP1-Scripts_287520926.package` baseline is 409297 bytes with SHA-256
`0A3F66804D9A99A626509B42C76D3C308A53F9FB2D84529F621A88BF85043FF9`.
No package, log, dump, capture, generated binary, or local `state.toon` belongs in source control.
