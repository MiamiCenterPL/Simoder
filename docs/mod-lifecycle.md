# Mod lifecycle

## Discovery

`Refresh` scans only immediate real directories under the game-root `mods/`. Symlinks/reparse
targets are rejected. `mod.toon` and `overrides.toon` are parsed into typed models and fingerprinted
with SHA-256 before and after parsing; a file that changes during the read is rejected.

Directory names are display locations, not identity. The manifest `id` is the stable key. Duplicate
IDs fail with all conflicting paths. A malformed entry does not stop valid mods.
Optional `symbols.toon` participates in the same stable-read fingerprints. A runtime worker
polls approximately once a second; it detects changes without silently applying pending versions.

Newly discovered valid mods always enter `Inactive`; Refresh never enables them.

## States and transitions

```text
discovery -> Inactive -> Loading -> Active -> Unloading -> Inactive
                         |                      |
                         `-> Failed             `-> Failed

Active + changed files -> Changed --OFF--> Inactive --ON--> Active
Active + removed folder -> Missing + registry removal
```

ON validates the complete typed definition, atomically registers all owned patches, invalidates
only affected TGIs, rebuilds retained live resources, and persists activation order. Immediately
before ON, discovery rereads the source files so an edit performed while OFF does not require a
separate Refresh. A registry or persistence failure leaves no partial patch set. OFF removes
ownership and immediately rebuilds retained live resources from vanilla plus remaining active mods.

The user activation order in `simoder/state.toon` is the deterministic M3 patch order. Persistence
uses a sibling temporary file and `MoveFileExW` replacement with write-through. A new installation
starts with an empty enabled list.

## Refresh policy

- inactive added mod: appears OFF;
- inactive removed mod: disappears;
- active valid files changed: `Changed`, old committed definition retained;
- active files become malformed: `Changed` with diagnostic, old definition retained;
- active directory removed: patches removed, cache invalidated, persisted ID removed, UI `Missing`;
- duplicate active ID introduced: old definition remains until the user switches it OFF.

Refresh does not silently reload active mods. The overlay can preview complete current/pending
operations and explicitly Reload. Reload checks the displayed source fingerprint, preflights
retained vanilla tables, and replaces one registry generation while preserving activation order.
Invalid or stale previews retain the old active definition; live transactions remain per resource.
OFF applies a valid pending changed definition to the
inactive record; the following ON registers it. If changed files are invalid, OFF safely removes
the old patch but leaves the entry `Failed` until a later Refresh validates corrected files.

## Runtime refresh boundary

After registry publication, every retained matching PROP instance is rebuilt transactionally from
its process-lifetime vanilla snapshot. Invalid or stale addresses are skipped, structural drift is
never overwritten, and future deserialization uses the same registry generation. A subsystem that
copied a PROP value into another game-owned object may still require its panel or object to be
recreated; Simoder does not guess unverified consumer layouts.

## Override scope

M3 supports finite scalar `float` operations `set`, `add`, `subtract`, `multiply`, and `divide`.
Division by zero, duplicate properties inside one mod, invalid uint32 identifiers, unsupported
types, and non-finite results fail safely. Property existence and the verified runtime metadata
type are checked when the exact vanilla resource becomes available.
