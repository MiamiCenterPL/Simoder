# Developer workflow and integrations

## Offline CLI

`simoder-dev.exe` is built alongside `sc13-inspect`. With `SC13_INSTALL_DEVTOOLS=ON`,
it installs into `simoder/DevTools`. All analysis commands are read-only. `init` creates
a new directory with a complete, valid sample mod and never installs or enables it.

```powershell
build/vs2026/bin/Release/simoder-dev.exe init example.sanitizer work/MyMod
build/vs2026/bin/Release/simoder-dev.exe explain work/MyMod
build/vs2026/bin/Release/simoder-dev.exe symbols work/MyMod sanitizer
build/vs2026/bin/Release/simoder-dev.exe validate work/MyMod --game '<package-directory>' --json
build/vs2026/bin/Release/simoder-dev.exe dry-run work/MyMod --game '<package-directory>' --with work/OtherMod
```

`explain` checks definitions and displays resolved ownership, identities and arithmetic operands.
It deliberately reports `checkedGame=false` and null computed values without package data.
`validate` and `dry-run` require `--game`, inspect package indexes and validate scalar property
existence/type, finite arithmetic and ordered conflicts. `dry-run` and `validate` use the same
read-only evaluator; dry-run emphasizes the printed operation chain. `--with` can be repeated;
arguments define activation order. For example, `300 -> set 345 -> add 5 -> 350` is evaluated
with the runtime patch arithmetic, including float32 rounding. Nothing writes a package.

All matching package paths are reported. Multiple matches for a TGI, including duplicates inside
one index, are an error: package precedence has not been proven. For a focused check, supply a
directory containing only the intended source package. Scan/parse failures are also errors;
the tool never treats an incomplete scan as validation success. Unique resources include their
uncompressed SHA-256. A successful offline result does not prove the game chose that package.

JSON reports use `schemaVersion: 1`, `valid`, `checkedGame`, `errors`, `resources`, `steps`
and `conflicts`. Exit codes: 0 success, 1 validation/I/O failure, 2 command usage error.
`symbols` always outputs JSON, searches case-sensitively by name or hexadecimal identity,
and can read a catalog while a syntactically valid document has incomplete patch definitions.

## OpenSC5 and registry catalogs

```powershell
python tools/symbols/import_symbols.py opensc5 Properties.txt work/MyMod/symbols.toon --allow-skipped
python tools/symbols/import_symbols.py s3db database_main.s3db work/MyMod/symbols.toon --user database_user.s3db
```

OpenSC5 numeric declarations preserve declared type and source line; unresolved hash expressions
are reported. s3db import preserves descriptor comments and applies the user database by ID,
matching OpenSCP's main/user semantics. Ambiguous names fail. The converter never overwrites
an existing output and rejects output exceeding the runtime TOON size limit.

Symbol entries can carry `type`, `description`, `unit` and `origin`. A declared non-float type
cannot be referenced by a float override. Units are never inferred. Imported OpenSC5 entries
use `reverse_engineering`; s3db descriptors use `registry`; authors can label their own entries
`local_alias`. These labels describe provenance, not a guarantee of original EA symbol names.

## OpenSCP before/after export

```powershell
python tools/symbols/openscp_diff.py before.json after.json work/ExportedMod `
  --id example.exported --tgi 00B1B104:61EFC000:719436BD
build/vs2026/bin/Release/simoder-dev.exe validate work/ExportedMod --game '<package-directory>'
```

The adapter consumes OpenSCP `export-prop --json` files with the actual upstream contract:
`{name, propertyCount, properties: [{name, hash, type, value}]}`. Hashes are `0x` plus eight
hex digits; types and values are strings; scalar floats use type `Float`. The TGI must be supplied
explicitly from the resource tree because an exported display name alone is insufficient.

Only changed scalar floats become `set` operations. Added/deleted properties, arrays and other
types are reported and block CLI conversion unless `--allow-unsupported` is explicitly supplied.
Non-finite/out-of-range floats are errors. Unchanged unsupported properties are harmless.
Output includes a complete manifest, catalog and overrides in a new directory, with checksums
of both inputs. OpenSCP's property `name` may be registry comments, so generated aliases are
`property_<ID>` and descriptions retain the readable text. Its G7 display values can already
have lost precision; this adapter cannot reconstruct original float bits. Validate against packages.

This integration covers dictionary and property-export interchange. It does not implement
OpenSCP's planned mod-project format, a toolbar inside OpenSCP, or arbitrary overlay-package import.
Upstream contracts were checked against [prop_json.rs](https://github.com/FluffyChi-Xing/fluffy-open-scp/blob/master/crates/sc-exporter/src/prop_json.rs)
and [sc-registry](https://github.com/FluffyChi-Xing/fluffy-open-scp/blob/master/crates/sc-registry/src/lib.rs).
No external game data or upstream source files are shipped.

## Runtime inspector and reload

The overlay's Resolved overrides tree shows symbol provenance, operations, retained instance
counts, outcome, generation, rejection reason and historical vanilla/committed values.
No retained observation does not prove a resource never loaded: evidence is bounded to 128
instances and addresses are historical identity tokens. It never exposes a pointer-writing API.

After successful hook installation, an owned worker polls definitions approximately once a second
and publishes `simoder/logs/runtime-status.json` atomically. New mods remain OFF. Changed active
definitions remain pending. Removed active mods follow the existing unload policy. The worker
stops before hooks/managers are destroyed, removes its snapshot on controlled shutdown, and
does not perform file I/O in the resource hook.

Pending changes show complete current and pending operation lists, including removals. Apply
validated reload checks source stability, binds the click to the displayed fingerprint, preflights
retained vanilla tables, and replaces the registry definition with one new generation while
preserving activation order. Invalid definitions, stale previews and failed preflight keep the
previous active patch set. Resources not yet observed are still checked on future deserialization.

Registry replacement is atomic; memory transactions are per resource. A stale instance can be
skipped while others update. Inspect outcomes after reload. Last committed generation is separate
from the generation of a later rejected attempt. Applied values are historical evidence; copied
consumers, visible simulation effects and object recreation remain separate acceptance steps.

## MCP evidence

The optional DevBridge now exposes read-only `simoder_get_runtime_state` and `simoder_get_logs`.
The runtime response contains mods, resolved operations, pending definitions, conflicts and
resource observations. It wraps these in `available`/`fresh` status. Freshness requires a verified
SimCity PID, consistent generation and a snapshot no older than five seconds. Missing, malformed,
future-dated, stale or oversized snapshots cannot be reported as fresh.

The log tool accepts an exact mod ID and limit 1..200, reads only the fixed log's last 64 KiB,
and never accepts arbitrary paths. Reads do not launch a broker or cause UAC; use the existing
startup workflow when the broker is not running. This adds no remote listener or mutation tool.

## Editor support

See [VS Code extension instructions](../devtools/vscode/README.md). The extension supplies
symbol completion with provenance, snippets, a validate command and on-save errors from the
production CLI. Package validation is enabled by configuring the game directory; otherwise
it clearly labels definition-only validation. It runs executables directly with time/output bounds
and requires a trusted workspace. External editors can use the same versioned JSON CLI contract.

## Verification boundary

Synthetic tests cover arithmetic chains, package ambiguity, missing properties/resources,
reload order and old snapshot immutability, rejected reloads, stale preview fingerprints,
automatic detection and publisher shutdown, converter/parser interoperability, s3db user overlay,
metadata mismatch, malformed/stale MCP evidence, log filtering and MCP tool discovery.
Editor helper tests and JavaScript syntax checks do not establish visual Extension Host acceptance.
In-game overlay/reload/consumer effects require a fresh deployed game session.

The 2026-10-01 verification built x86 runtime/CLI and self-contained DevBridge/MCP, passed all
four CTest groups, and staged the complete developer payload under `build/devexperience-stage`.
A read-only dry-run against the installed `SimCityUserData/Packages` found a unique
`SimCityDLCEP1-Scripts_287520926.package` match, resource SHA-256
`49E10D6E3512E34821B54C75DE978D398E23347695442A9F1C077B37AF65886B`, and computed
`sanitizerOperatingCost: 300 -> set 345 -> 345`. No deployed game files were changed.
