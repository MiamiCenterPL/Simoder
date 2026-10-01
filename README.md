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
discovered mods. Active files changed on disk are marked `Changed`; previewed Reload or OFF/ON
applies the pending definition.

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

## Named overrides

Named resource and property references are supported in `overrides.toon`. Declare explicit
aliases in the document's `symbols.resources` and `symbols.properties` arrays; every entry
requires a non-empty `source` describing its provenance. See the packaged BetterSanitizer
example. `sanitizerOperatingCost` is a Simoder alias backed by the verified cost effect,
not a confirmed original EA field name.

Names are case-sensitive and scoped to one mod document. Resource aliases map to exact
`type/group/instance` values; property aliases map to exact `id` values. No implicit hashing
is performed. Numeric-only overrides remain supported. A named reference may also carry
its numeric identity, in which case the identities must agree (a resource must provide all
three TGI fields). Unknown names, duplicate declarations, missing provenance, mismatches,
and duplicate resolved patch targets reject the mod before activation. Multiple distinct
aliases may describe one identity, but cannot bypass same-property conflict validation.
Resolution runs during definition loading; runtime hooks continue to receive numeric typed
patches. Editing symbols participates in the existing override fingerprint and OFF/ON reload.

An optional `symbols.toon` beside `overrides.toon` supplies reusable imported aliases. Both
catalogs are merged; duplicate names are rejected rather than silently overridden. Catalog
creation, deletion and edits participate in stable-read fingerprint checks and the normal
Changed/OFF/ON lifecycle. Runtime/UI snapshots retain selected names and provenance; the
overlay's Resolved overrides inspector shows names, IDs, operations and sources. Committed
runtime patch logs additionally show vanilla and final resource values (the final value may
include operations from several mods).

Convert local dictionaries with Python 3 (standard library only):

```powershell
python tools/symbols/import_symbols.py opensc5 Properties.txt path/to/mod/symbols.toon
python tools/symbols/import_symbols.py s3db database_main.s3db path/to/mod/symbols.toon
build/vs2026/bin/Release/sc13-inspect.exe mod path/to/mod
```

The OpenSC5 adapter accepts explicit hexadecimal/decimal property declarations. Hash expressions
are reported as skipped; inspect the report and add `--allow-skipped` to accept a partial catalog.
The s3db adapter reads the same SQLite `Properties(id,name,comments)` table as OpenSCP's
`sc-registry`, read-only. It imports property descriptors only; group/instance descriptors cannot
identify a complete resource TGI. Empty names are reported, ambiguous names fail, and output is
sorted with source SHA-256 provenance. Existing outputs are never overwritten. Catalogs are pinned
per mod rather than a mutable global database. This is dictionary interoperability, not support
for arbitrary OpenSCP packages or its planned mod-project format. Imported names do not expand
the loader's supported scalar-float patch types.

External dictionaries are not bundled. Adapter formats were checked against
[OpenSC5 Properties.txt](https://github.com/TornadoCookie/OpenSC5/blob/main/Properties.txt) and
[OpenSCP sc-registry](https://github.com/FluffyChi-Xing/fluffy-open-scp/blob/master/crates/sc-registry/src/lib.rs).

## Logging and DeveloperConsole

The developer workflow now includes `simoder-dev` validation/dry-run/scaffolding and JSON symbol
search, OpenSCP before/after conversion, richer catalog metadata, a runtime inspector, previewed
reload preserving mod order, bounded MCP evidence and a local VS Code extension. See the
[developer experience guide](docs/developer-experience.md) for commands, integration contracts
and verification limits.

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
