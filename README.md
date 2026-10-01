# Simoder / SC13 Mod Loader

Simoder is an experimental in-game runtime mod manager for SimCity (2013) and Cities of Tomorrow. `SC13ModLoader` is its low-level x86 injection and resource-interception layer.
M3 discovers declarative mods, persists explicit ON/OFF state, applies owned patches in memory, and provides an ImGui management panel. It never modifies, merges, or generates game `.package` files.

## Administrator requirement

The current EA App launch path on the validated machine starts SimCity with elevated rights.
Simoder must run at the same integrity level to open and inject into that process. Consequently:

- `simoder.exe` contains a Windows `requireAdministrator` manifest;
- `Start-Simoder.bat` displays a UAC prompt before starting the watcher;
- cancelling UAC means Simoder cannot attach, although the unmodified game may still be launched
  separately;
- this is an expected runtime requirement, not a request to weaken the game directory ACLs.

## Installed layout

```text
SimCity/
|-- mods/
|   `-- BetterSanitizer/
|       |-- mod.toon
|       `-- overrides.toon
|-- simoder/
|   |-- config.toon
|   |-- state.toon              # generated user state
|   |-- logs/                   # generated logs
|   |-- cache/                  # metadata only; never packages
|   |-- simoder.exe
|   |-- sc13modloader.dll
|   |-- DevTools/                # optional; SC13_INSTALL_DEVTOOLS=ON
|   |   `-- AI/
|   |       |-- Simoder.DevBridge.exe
|   |       |-- Simoder.McpServer.exe
|   |       |-- Register-Simoder-MCP.ps1
|   |       `-- Unregister-Simoder-MCP.ps1
|   `-- Uninstall-Simoder.bat
|-- Start-Simoder.bat
`-- Start-Simoder.sh            # experimental Wine/Proton wrapper
```

`mods/` belongs to the user. The uninstaller preserves mods, `config.toon`, and `state.toon` by
default. Optional reverse-engineering utilities install under `simoder/DevTools` only when the
`SC13_INSTALL_DEVTOOLS` CMake option is enabled.

When enabled, DevTools also include the Windows-only AI DevBridge. Its non-elevated stdio MCP
server can inspect and operate only signed/allowlisted EA and SimCity windows through a separately
elevated broker. Registration is always explicit; see `simoder/DevTools/AI/README.md` after
installation. Normal builds, `Start-Simoder.bat`, and end-user runtime behavior do not load it.

The source repository intentionally does not contain root-level `mods/` or `simoder/` runtime
directories. Read-only distribution defaults live under `packaging/`; `cmake --install` places
them in the game root without overwriting an existing user config or BetterSanitizer definition.

## Build and install

The game process is x86, so configure a Win32 build. The verified local generator is Visual
Studio 2026:

```powershell
cmake --preset vs2026-x86
cmake --build --preset vs2026-release --parallel
ctest --preset vs2026-release
cmake --install build/vs2026 --config Release --prefix '<SimCity-install>'
```

Build and install the optional developer payload with:

```powershell
cmake --preset vs2026-x86 -DSC13_INSTALL_DEVTOOLS=ON
cmake --build --preset vs2026-release --parallel
ctest --preset vs2026-release
cmake --install build/vs2026 --config Release --prefix '<SimCity-install>'
```

Visual Studio 2022 presets are also provided. Dear ImGui `v1.92.9` and MinHook `v1.3.4` are
fetched from their official repositories and pinned by tag.

Run a non-mutating installation check with:

```powershell
& '<SimCity-install>\Start-Simoder.bat' --check
```

Then double-click `Start-Simoder.bat`, approve UAC, and let the EA App launch the game. A small
`Simoder` overlay button opens the mod table. `Refresh` rescans `mods/` but never enables newly
discovered mods. Active files changed on disk are marked `Changed` and require OFF/ON.

## Better Sanitizer

The included test mod owns one proven runtime patch:

```text
TGI       00B1B104:61EFC000:719436BD
Property  0x09AE19D7 (float)
Operation set 345
Vanilla   300
```

It defaults to OFF on first discovery. ON and OFF rebuild retained live PROP resources immediately
from vanilla plus the current ordered patch set, and future loads use the same generation. Enabling
also rereads `mod.toon` and `overrides.toon`, so edits made while OFF do not require a separate
Refresh. A game subsystem that already copied a PROP value may still require its panel or object to
be recreated. The earlier M2 manual validation confirmed the patched `545 $/h` simulation total
and vanilla `500 $/h` after controlled detach.

The TOON reader is a strict, bounded subset of the official TOON 4.1 syntax needed by M3. Parsed
documents are immediately converted into typed C++ models; the runtime hook never receives a
TOON DOM. See the [official specification](https://github.com/toon-format/spec/blob/main/SPEC.md).

## Logging and DeveloperConsole

File logging is always centralized at `simoder/logs/sc13modloader.log`. Events carry a source:
`GAME`, `LOADER`, or `MOD`; mod events also carry the stable mod ID. SimCity's verified imported
`OutputDebugStringA` channel is captured while preserving original debugger behavior. Arbitrary
C-runtime formatting functions are not hooked.

The external DeveloperConsole is disabled by default. Enable it in `simoder/config.toon`:

```text
developerConsole:
  enabled: true
  captureGameLogs: true
  captureLoaderLogs: true
  captureModLogs: true
  level: info
```

Console creation failure does not disable file logging, mods, or the game.

## Startup troubleshooting

The base game can intermittently remain after the intro with a permanent loading spinner. On the
validated machine, disabling **EA App -> Settings -> Application -> In-game overlay** produced
three consecutive successful starts. This is a practical workaround, not proof of the underlying
cause, and is unrelated to Simoder's in-game ImGui overlay.

Detailed behavior is documented in [architecture](docs/architecture.md),
[mod lifecycle](docs/mod-lifecycle.md), [logging](docs/logging.md), and the
[manual M3 test](docs/first-test.md).

## Repository policy

Proprietary EA binaries and package files are excluded. Tests use generated text and synthetic
binary fixtures. No license has been selected yet; choose one before public distribution.
