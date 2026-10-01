# Resource loading investigation

## Opt-in discovery layer

A build configured with `SC13_ENABLE_DISCOVERY_TRACE=ON` instruments these Windows APIs through
their resolved Kernel32 targets:

- `CreateFileW` and `CreateFileA`;
- `ReadFile`;
- `SetFilePointer` and `SetFilePointerEx`;
- `CreateFileMappingW` and `CreateFileMappingA`;
- `MapViewOfFile`;
- `CloseHandle` for bounded tracking cleanup.

Only the canonical `SimCityDLCEP1-Scripts_287520926.package` filename becomes tracked. Package
opens include a short module-relative stack. Ordinary reads are capped at 16 log entries per
tracked file, while any read overlapping target offset `0x59A7E` and its 879 stored bytes is
always logged.

The exact build resolves the `PFRecordRead` constructor at RVA `0x0052B9B0`, a parsed-resource
publisher at RVA `0x000099F0`, the stream-read wrapper at RVA `0x004E9630`, and the concrete PROP
deserializer at RVA `0x00009710`. The early PID 27996 experiment proved that the target preload
bypasses the first two candidates. The stream trace led to the runtime object, whose vtable then
identified the deserializer. All four discovery addresses are exact-fingerprint gated and resolve
from unique signatures. The normal M3 build does not install these completed investigation hooks
and does not require their signatures to resolve.

## Historical M2 production layer

This section records the evidence that established the interception primitive. M3 retains the
same verified deserializer boundary but replaces the single hardcoded patch with typed
`PatchRegistry` lookup and `RuntimeResourceCache` rebuilds.

The normal build resolves only the concrete PROP deserializer at RVA `0x00009710` and installs
one hook. A no-match resource pays only the original call plus a 12-byte identity check. Detailed
validation, copying, logging, and allocation occur only for the exact Sanitizer TGI.

## Evidence progression

1. Capture which packages are opened by the actual process and from which module-relative
   callers.
2. Resolve the first stable game-owned caller above Win32 I/O in a disassembler/debugger.
3. Trace index parsing, the RefPack call, and propagation of TGI fields.
4. Identify the narrowest function that accepts or constructs TGI and returns resource data.
5. Observe cache behavior and the corresponding release/destructor path.
6. Record a signature only after the function purpose and ABI are supported by caller/callee and
   debugger evidence.
7. Install a log-only TGI filter hook.
8. Patch only the validated scalar value and retain enough identity for safe rollback.

Steps 1 through 7 are complete for the Sanitizer target. M2 deliberately patches the existing
game-owned scalar instead of returning a cloned vtable-bearing object whose allocator and
destructor contracts are unknown.

## Earlier runtime evidence collected

- executable and loaded `.text` fingerprints are exact-match gated;
- the target TGI exists three times in live memory, but only one occurrence owns a valid
  89-record property vector;
- the verified target record changed from `00 00 96 43` (`300.0`) to `00 80 AC 43` (`345.0`);
- EAWebKit received a separate `ID+345` presentation copy;
- the selector and actual simulation both used the new total of `545 $/h`;
- the placed-building detail panel retained a stale display of `500 $/h`.

## M2 deserializer gate

The parsed-object validator requires all of these conditions before reporting a runtime baseline:

- exact TGI `00B1B104:61EFC000:719436BD`;
- reserved field zero and equal vector end/capacity;
- exactly 89 records with 24-byte stride and strictly increasing IDs;
- scalar-float runtime metadata `0x000D0000` for all four selected properties;
- values `0x09AE19D7=300`, `0x0AFB9882=72`, `0x0C09DA83=2`, and `0x0FD16C15=200`.

On PID 35348, the hook at RVA `0x00009710` reported the exact target TGI after a successful
deserialization, a structurally valid table, and all four original values `300/72/2/200`. This is
the earliest verified target-specific point before the resource is exposed to downstream caches.

The patch implementation creates or reuses a loader-owned validated snapshot, then writes only
the four-byte `valueBits` field of property `0x09AE19D7` in the game-owned table. It records the
resource identity, vector base, value address, and old/new bits. Shutdown restores `300.0` only if
the object still has the exact TGI and vector identity and the value remains `345.0`; otherwise it
skips the stale site. On PID 30180 the watcher-before-game workflow repeated the exact baseline
and patch, the changed value was visible after map load, and controlled detach logged
`status=restored-300` before unloading the DLL. The remaining runtime acceptance gate is the full
functionality checklist plus confirmation of vanilla UI after a post-detach map reload.
