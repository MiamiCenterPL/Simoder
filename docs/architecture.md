# Architecture

## Runtime flow

```text
Start-Simoder.bat (UAC)
    -> simoder/simoder.exe watcher
    -> EA App launches SimCity.exe
    -> LoadLibraryW(sc13modloader.dll)
    -> SC13_Initialize outside DllMain

SimCity PROP deserializer
    -> immutable PatchRegistrySnapshot lookup by exact TGI
    -> retained vanilla RuntimeResourceCache entry
    -> deterministic activation-order patch sequence
    -> transactional value-bit writes to the game-owned table
    -> cache commit only after every write succeeds
```

The normal unpatched hot path loads one atomic shared snapshot, performs one ordered-map TGI
lookup, and returns. It does not scan mods, parse TOON, access the filesystem, or acquire the
`ModManager` lock.

## Component boundaries

- `simoder.exe`: elevated end-user watcher/injector and controlled detach client.
- `sc13modloader.dll`: process bootstrap, build gate, hooks, logging, and UI lifetime.
- `ModManager`: discovery, typed definitions, lifecycle, persistence, and UI snapshots.
- `PatchRegistry`: active ownership, activation sequence, conflict metadata, and immutable
  copy-on-write snapshots.
- `RuntimeResourceCache`: vanilla/last-result retention, targeted invalidation epochs, rebuild
  planning, and controlled-detach restoration snapshots.
- `RuntimeDiagnostics`: owned polling worker for pending-definition detection and atomic JSON
  publication; stops before hook/service shutdown and performs no hot-path filesystem work.
- `simoder-dev`: offline source validation, ordered dry-run, JSON explanations, catalog search
  and new-mod scaffolding using the same typed parser/arithmetic as runtime.
- `Simoder overlay`: rendering and input only; callbacks invoke `ModManager`.
- `AsyncLogger`: the only file/console sinks. Producers emit semantic events.
- `sc13_core`: game-independent TGI, hash, DBPF, RefPack, PROP, TOON, patch, and state logic.
- optional `Simoder.McpServer.exe`: non-elevated stdio MCP surface with read/write annotations.
- optional `Simoder.DevBridge.exe`: separately elevated, same-user Named Pipe broker restricted to
  verified EA/SimCity/Simoder processes and capture-bound input.

`ModManager` does not parse PROP memory, implement hooks, render ImGui, or own log sinks.

## Patch and cache invariants

Activation sequence is the deterministic M3 patch order. Every registered resource block carries
its stable owner ID and sequence. Same-TGI/same-property changes are recorded as conflicts;
different properties of the same resource are not conflicts.

Disabling never reverses arithmetic. Each result is recomputed as:

```text
retained vanilla + currently active ordered patches
```

Targeted invalidation occurs both before and after a registry publication. Per-TGI invalidation
epochs prevent an in-flight old plan from committing across an ON/OFF transition. Existing cache
entries retain owned vectors only; the loader never lends these buffers to the game, so invalidation
cannot create dangling game pointers.

Runtime writes touch only `valueBits` after verifying sorted table structure, record identity,
metadata/type, bounds, alignment, and finite arithmetic. A multi-property update uses compare/
exchange and rolls back earlier writes if any later write fails. Detach restores vanilla only when
the live resource still exactly matches the last committed result.

ON/OFF and active-mod removal reuse the same compare/exchange transaction to rebuild retained live
PROP instances immediately from vanilla plus the newly published registry generation. ON first
rereads stable source bytes, allowing an OFF-state `overrides.toon` edit to take effect without a
manual Refresh.

## Threading and ownership

- `ModManager` serializes discovery and lifecycle with its own mutex.
- `PatchRegistry` serializes writers and atomically publishes immutable reader snapshots.
- `RuntimeResourceCache` has an independent mutex; the hook never takes the manager lock.
- game-memory value transitions use a small exclusive SRW lock separate from registry/cache locks.
- `AsyncLogger` copies events into a bounded queue and drains batches on one worker thread.
- ImGui state stays on the D3D9/window thread; UI snapshots are copied before rendering.

Shutdown order is: disable hooks, restore the original WndProc and destroy ImGui, remove MinHook
hooks, restore retained runtime resources, stop log capture, release mod services, then flush the
logger.

## Graceful degradation

- unknown game fingerprint or ambiguous signature: no resource hook;
- malformed/duplicate mod: that entry is `Failed`; other mods continue;
- UI hook/context failure: runtime mods and game continue without UI;
- DeveloperConsole failure: file logging continues;
- unsafe resource layout/type/value: that resource remains unchanged;
- existing objects that cannot be recreated safely: only future loads receive the new generation.

OpenSCP property-export interchange and registry dictionaries are offline adapters. Native
SimCity UI integration, Lua, legacy package import, dependencies, online distribution, and
arbitrary machine-code patching are outside M3.

## Optional AI DevBridge boundary

AI DevBridge is developer tooling and is not linked into `sc13modloader.dll`. The MCP process has
no direct elevated input capability. Its sibling broker uses `PipeOptions.CurrentUserOnly`, an
exact MCP executable-path check, a Windows-session check, and no TCP listener. EA/SimCity targets
must pass process-name, installation-root, and Electronic Arts Authenticode checks. A coordinate
action additionally requires a fresh image id and unchanged client geometry.

UI Automation invocation is preferred when the pointed element exposes `InvokePattern`; CEF and
DirectX surfaces fall back to foreground `SendInput`. UAC secure-desktop interaction, arbitrary
text/keys/PIDs, termination, and automatic restart are intentionally absent. Infinite Loading is
represented as a two-minute diagnostic suspicion after an AI-confirmed `CityLoading` stage.
