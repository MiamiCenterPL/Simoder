# Reverse-engineering record

Facts in this document are scoped to the local installation inspected on 2026-08-16 and
2026-08-17. Every item is labeled by confidence. Addresses are valid only for the exact hashes
recorded below.

## M1/M2 status matrix

| Item | Status | Evidence |
| --- | --- | --- |
| DLL attached to SimCity | DONE | `src/bootstrap/launcher_main.cpp`; runtime initialization and unload records in the ignored `logs/sc13modloader.log` |
| safe detach | DONE | `SC13_Shutdown`, `UninstallTraceHooks`, and `RestoreRuntimePatches`; PID 30180 logged `rollback ... status=restored-300` followed by clean shutdown on 2026-08-17 |
| game architecture identified | DONE | PE machine `0x014C` and x86-only CMake gate in `src/reverse/game_build.cpp` and `CMakeLists.txt` |
| game build fingerprinting | DONE | file/version/loaded-`.text` fingerprinting in `src/reverse/game_build.cpp` |
| module enumeration | DONE | bounded Toolhelp inventory in `src/reverse/game_build.cpp` and bootstrap logs |
| pattern scanner | DONE | unique/not-found/ambiguous resolver and tests in `src/memory/signature.*` and `tests/test_main.cpp` |
| hook infrastructure | DONE | MinHook lifecycle and fail-open cleanup in `src/hooks/file_trace.cpp` |
| DBPF parser | DONE | bounds-checked DBPF v3 reader and synthetic tests in `src/formats/dbpf` |
| PROP parser | DONE | byte-preserving parser and one-field-diff tests in `src/formats/prop` |
| known Sanitizer PROP parsed correctly | DONE | offline resource SHA and 89-property facts below; runtime baseline `300/72/2/200` |
| actual game resource loading path identified | DONE | concrete PROP deserializer record at RVA `0x00009710` below |
| known TGI observed at runtime | DONE | exact filter and `[SC13][PROP-DESERIALIZE]` record in `src/hooks/resource_trace.cpp` and runtime log |
| runtime PROP replacement achieved | DONE | four-byte `300 -> 345` record, signal handshake, and changed in-game value confirmed after map load on PID 30180 |

`DONE` denotes implemented behavior backed by the cited source plus either deterministic tests or
the ignored local runtime log. Gameplay acceptance beyond the scalar value is tracked separately
in `first-test.md` and is not inferred from this implementation matrix.

## Current executable

### SimCity main image

- **Verified:** the main executable at `<SimCity-install>\SimCity\SimCity.exe`.
- **Verified:** PE machine `0x014C` (x86), Windows GUI subsystem.
- **Verified:** file and product version `10.3.6.0`.
- **Verified:** size `10,980,128` bytes; PE `SizeOfImage` is `0x00C6E000`.
- **Verified:** SHA-256
  `4E5F136A7BF58A52C4B97ED9C9A615A118D4A696E13AA8CAA6A0A51FD4516D35`.
- **Verified:** the copy under `SimCityUserData\Patches\UpdatedApp` is byte-identical by SHA-256.
- **Verified:** Windows reports the executable's Electronic Arts Authenticode signature as valid.
- **Verified:** the static dependency table exposed by `dumpbin /dependents` contains only
  `Core/Activation.dll`.
- **Verified:** the PE debug directory names `SimCity.pdb`, GUID
  `{3515CCCA-2EB7-40FB-81BA-B29660519882}`, age 1. The exact symbol-server key returned HTTP 404
  from Microsoft's public symbol server on 2026-08-16, so no matching public PDB is available
  from that source.
- **Verified:** the on-disk `.text` bytes disassemble as high-entropy invalid control flow, both
  at the entry point and at addresses stored in otherwise coherent function-pointer tables near
  `Resource/DatabasePackedFile`. The entry point begins with an indirect jump through an address
  associated with the activation boundary.
- **Strong conclusion:** the executable code is protected on disk and useful static xrefs require
  a post-activation memory snapshot. The runtime module inventory is also required before
  evaluating any proxy DLL candidate.
- **Verified at runtime:** the decrypted loaded `.text` SHA-256 is
  `D187C19201972ABEBCAA2FA5D8E38263D7A0B0FC3B6803D646636AFB14C49D39`.

### Static resource-layer landmarks

- **Verified:** `.rdata` contains `Resource/DatabasePackedFile`, `ResourceMan/PFRecordRead`,
  `Resource/PFReadBuffer`, `Resource/Mgr/DBList`, `Resource/PackedRecord`, `Resource/Raw`,
  `Resource/DDFRecord`, `Refpack`, and `game://localhost/resource/%ls`.
- **Verified:** the target TGI components and property ID have no plain little-endian occurrence
  in the protected on-disk `.text` section.
- **Verified:** direct VA, RVA, section-offset, and file-offset searches do not connect those
  names to the protected code bytes. This negative result is not evidence that the runtime code
  lacks such references because `.text` is encrypted.
- **Implemented as opt-in discovery evidence:** a build configured with
  `SC13_ENABLE_DISCOVERY_TRACE=ON` captures the initial loaded `.text`,
  samples it for mutations, and writes each stable changed state to `logs/captures` beside the
  DLL. For the first five seconds it samples every 10 ms and also captures the first mutation
  immediately, so a short-lived activation transition is not lost. Captures are digest-addressed
  and never written into the game installation.

### Bootstrap runtime evidence

- **Verified:** creating the process fully suspended is too early for a Toolhelp module snapshot;
  `kernel32.dll` is not yet available to the injector.
- **Verified:** attempting remote `LoadLibraryW` at the system loader breakpoint deadlocks on the
  loader lock.
- **Verified:** the launcher can instead arm a one-byte breakpoint at the executable entry point,
  continue past loader initialization, restore the original byte and instruction pointer, detach
  the debugger, inject, and resume the first game instruction. The successful observation run was
  PID 7632 on 2026-08-16.
- **Verified:** modern Windows may return a forwarded `LoadLibraryW` address owned by a module
  other than the requested `kernel32.dll`; the launcher now maps the RVA from the actual local
  owner module to the matching remote module.
- **Observed:** direct standalone execution exited immediately after bootstrap, consistent with
  requiring the normal EA client launch path. Attaching to an EA-launched process works when the
  launcher has the required process rights.
- **Verified:** `--detach` calls `SC13_Shutdown` and then `FreeLibrary`, permitting a controlled
  DLL replacement without restarting the city.

### Verified runtime resource functions

- **Verified:** `PFRecordRead` constructor RVA `0x0052B9B0`, `__thiscall`, three stack arguments,
  `ret 0x0C`. After saving four registers it reads `[esp+0x18]`, which is the second stack
  argument, and copies its 12-byte instance/type/group key into `self+0x0C`.
- **Verified:** its build resolver uses the unique signature
  `53 55 56 8B F1 57 C7 06 44 A8 CF 00 33 C0 8D 4E 04 87 01 8B 44 24 18 C7 06 5C 58 D3 00 C7 46 08 91 A8 E4 12`.
- **Verified:** the only direct constructor call is at VA `0x008EE387` (RVA `0x004EE387`),
  in a factory that allocates a `0x7C`-byte object.
- **Verified:** the RefPack decompressor begins at RVA `0x0054BEF0`.
- **Verified:** the runtime resource-object vtable is `0x00CF8A20`; its constructor at
  `0x00408700` initializes the property vector stored at object offsets `0x18..0x20`.
- **Verified:** the parsed-resource publication function is VA `0x004099F0`, RVA `0x000099F0`.
  Four direct callers clean three arguments (`0x0C`) after the call, establishing `cdecl` for
  the observed call sites. The first argument is the parsed resource; the second remains an
  opaque context/token and the third is an observed flag whose exact semantics remain unknown.
- **Verified:** the publisher checks `[resource+0x0C]` for type `0x00B1B104`. For that type it
  obtains a manager through VA `0x00405DA0` and invokes virtual slot `+0x34` with the resource,
  instance from `+0x08`, and group from `+0x10`.
- **Verified:** the four direct publisher calls are VA `0x0041694C`, `0x0041B767`,
  `0x004507AC`, and `0x004F7782`.
- **Verified:** its build resolver uses the unique signature
  `56 8B 74 24 08 81 7E 0C 04 B1 B1 00 75 19 E8 ?? ?? ?? ?? 8B 4E 10 8B 10 8B 52 34 51 8B 4E 08 51 56 8B C8 FF D2`.
- **Verified at attach on PID 29248:** both signatures resolved exactly once at their expected
  RVAs. The temporary hook at `0x004099F0` is read-only and runs before the original function.
- **Negative runtime result on PID 27996:** early instrumentation was active before the canonical
  package opened and before its target payload was read, but the Sanitizer preload produced no
  event at either `PFRecordRead` or `0x004099F0`. Those functions are therefore not the
  interception boundary for this observed preload path.
- **Verified:** the concrete PROP deserializer is VA/RVA `0x00409710`/`0x00009710`, a
  `__thiscall` method with one reader argument and `ret 0x04`. It reserves the 24-byte record
  vector from the serialized count and invokes the property reader once per record.
- **Verified:** its build resolver uses the unique signature
  `83 EC 10 53 55 8B 6C 24 1C 56 57 33 F6 56 8D 44 24 14 50 55 8B F9 E8 ?? ?? ?? ?? 8A D8 8B 44 24 1C 83 C4 0C 84 DB 74 5E`.
- **Verified on PID 35348:** after the original method returned success, the exact Sanitizer TGI
  owned a valid 89-record table and exposed `300/72/2/200` for the four required property IDs.

### Runtime function records

```text
Name: PFRecordRead constructor
Hypothesized purpose: construct one packed-file read request
Module: SimCity.exe
Signature: exact 36-byte signature listed above
Calling convention: __thiscall, callee removes 0x0C
Arguments: self; stack1 opaque owner/context; stack2 verified 12-byte key; stack3 opaque options
Return value: treated as opaque constructor result by the hook
Evidence: constructor instructions, self+0x0C key copy, one direct factory call
Callers: SimCity.exe RVA 0x004EE387
Callees: not used as evidence for the current hook
Game build: file SHA 4E5F...16D35, loaded .text SHA D187...9D39
Confidence: verified except the two explicitly opaque arguments
```

```text
Name: parsed-resource publisher candidate
Hypothesized purpose: route some complete parsed resources to a type-specific manager, then notify
Module: SimCity.exe
Signature: wildcarded call target with stable surrounding instructions, listed above
Calling convention: cdecl at all four direct callers
Arguments: parsed resource; opaque context/token; flag with unknown exact semantics
Return value: ignored at the observed call sites; the hook does not synthesize a value
Evidence: type check, TGI field loads, manager virtual call, four caller cleanup sequences
Callers: RVAs 0x0001694C, 0x0001B767, 0x000507AC, 0x000F7782
Callees: manager getter VA 0x00405DA0; type-specific virtual slot +0x34
Game build: file SHA 4E5F...16D35, loaded .text SHA D187...9D39
Confidence: verified for ABI and behavior visible in disassembly; disproven as the Sanitizer
            preload boundary observed on PID 27996
```

```text
Name: concrete PROP deserializer
Hypothesized purpose: deserialize one PROP resource into its game-owned property vector
Module: SimCity.exe
Signature: exact wildcarded signature listed above
Calling convention: __thiscall, one reader argument, callee removes 0x04
Arguments: self; opaque reader/serializer object
Return value: boolean success in AL
Evidence: vector reservation, per-record reader loop, ret 0x04, live target postcondition
Callers: virtual dispatch through the target resource vtable
Callees: property reader VA 0x008F15A0; vector reservation VA 0x00409450
Game build: file SHA 4E5F...16D35, loaded .text SHA D187...9D39
Confidence: verified for ABI, table construction, and live target postcondition
```

### Parsed-resource representation and lifetime boundary

- **Verified:** at publisher entry the object contains vtable `+0x00`, instance `+0x08`, type
  `+0x0C`, group `+0x10`, reserved zero at `+0x14`, and vector begin/end/capacity at
  `+0x18/+0x1C/+0x20`.
- **Verified:** the Sanitizer vector is complete at that point: 89 strictly ID-sorted records,
  24 bytes each. This representation is parsed data, not compressed DBPF bytes.
- **Verified:** calls to `0x004099F0` are synchronous at the four direct callers. This fact does
  not establish that the Sanitizer preload uses that function; the early runtime experiment
  established that it does not.
- **Not required for M2:** the resource object's full destructor/reference-count contract. A raw
  clone would duplicate a vtable-bearing object and vector ownership, so the loader does not
  fabricate or return one.
- **Current safety decision:** retain an owned loader-side property snapshot/cache for validation
  and diffing, then change only the validated four-byte scalar in the existing vector immediately
  after deserialization. Do not mutate through the disproven `0x004099F0` preload path.

### Confirmed Sanitizer preload experiment

- **Verified on PID 27996:** the watcher retried while activation still exposed a different
  loaded `.text` hash. Each failed initialization removed all hooks and unloaded the DLL. The
  third attempt matched the supported loaded `.text`, installed the then-enabled discovery hooks
  at 01:16:11, and the
  target package did not open until 01:16:17.
- **Verified:** at 01:18:38 thread 18456 read exactly 879 bytes from offset `0x59A7E` of the
  `EcoGame/SimCityDLCEP1-Scripts_287520926.package` runtime copy. This matches the offline DBPF
  entry's stored range. The immediate Win32 return address was `SimCity.exe+0x004E9653`.
- **Verified:** VA `0x008E9630` (RVA `0x004E9630`) is a `__thiscall` stream-read wrapper. Its
  object stores the Win32 handle at `+0x04`; its arguments are buffer and requested byte count;
  it calls `ReadFile`, returns the byte count or `-1`, and removes eight argument bytes.
- **Verified by external read-only memory inspection after that read:** the target parsed object
  was at `0x2D93B1C8`, with vector begin/end/capacity
  `0x2D941088/0x2D9418E0/0x2D9418E0`. The vector is exactly `0x858` bytes, or 89 records of
  24 bytes. Runtime record `0x09AE19D7` began at `0x2D9411C0` and contained float `300.0`; no
  `ID+345.0` pair existed in the process.
- **Current bounded experiment:** a unique-signature hook at stream wrapper RVA `0x004E9630`
  records its direct return address only when the current package position overlaps the target
  range. This selects the actual higher-level caller from seven static direct callers without
  logging unrelated stream operations.
- **Verified on PID 35348:** the target passed the concrete PROP deserializer at 01:43:27 with
  result success, exact TGI, valid 89-record table, and original values `300/72/2/200`.
- **Verified on PID 30180:** the EA-started watcher attached before the city load, the deserializer
  reported the same baseline, and the exact patch logged `status=applied`. The changed value was
  visible in game after loading the map. Controlled detach later logged `status=restored-300`
  before unloading the DLL while the game remained running.

### Bootstrap implications

- **Verified:** blindly choosing `dinput8.dll`, `version.dll`, or `winmm.dll` is unsupported by
  the static dependency evidence.
- **Verified:** replacing `Core/Activation.dll` would overwrite an original EA file and therefore
  violates the project requirements.
- **Decision:** use the external x86 launcher for the observation milestone. Reconsider a
  non-destructive proxy only if an actual runtime module list proves a naturally loaded candidate
  and deployment can preserve original files.

## DBPF evidence

### Header and index

- **Verified against local files:** archive major version is 3 and the header is 96 bytes.
- **Verified:** entry count is at header offset `0x24`, index byte size at `0x2C`, index minor
  version at `0x3C`, and absolute index offset at `0x40`.
- **Verified:** the inspected package indices use a 32-bit flags word followed by optional fixed
  type, group, and instance-high fields for bits 0, 1, and 2.
- **Verified:** current Cities of Tomorrow script packages use flags `0x00000004`, fixing
  instance-high to zero and leaving 28 bytes per entry.
- **Verified:** compressed entries use marker `0xFFFF`; the target uses 879 disk bytes and 1496
  memory bytes.
- **Verified:** the target RefPack bytes begin `10 FB 00 05 D8`; the three-byte declared output
  size is `0x0005D8` (1496).

The bounds-checked implementation is in `src/formats/dbpf`. It rejects out-of-file offsets,
truncated commands, invalid back-references, unsupported index flags, inconsistent sizes, and
unknown compression markers.

### Known TGI location

`00B1B104:61EFC000:719436BD` is verified in these current files:

| Package | Entry | Offset | Compression |
| --- | ---: | ---: | ---: |
| `EcoGame\SimCityDLCEP1-Scripts_277574227.package` | 1265 | `0x000585C0` | `0xFFFF` |
| `EcoGame\SimCityDLCEP1-Scripts_281721150.package` | 1284 | `0x00059951` | `0xFFFF` |
| `EcoGame\SimCityDLCEP1-Scripts_287520926.package` | 1285 | `0x00059A7E` | `0xFFFF` |
| `Packages\SimCityDLCEP1-Scripts_287520926.package` | 1285 | `0x00059A7E` | `0xFFFF` |

All four decompress to SHA-256
`49E10D6E3512E34821B54C75DE978D398E23347695442A9F1C077B37AF65886B`.

`SimCityDataEP1.package` itself does not contain this exact TGI. A pre-existing custom file named
`SimCityData\000_WaterySanitizer.package` was present during initial inventory and contained an
uncompressed copy of the TGI, but it was absent during final tool validation. No SC13 project
operation wrote, renamed, or deleted it. It is not used as vanilla evidence.

## PROP evidence

- **Verified:** the uncompressed target is 1496 bytes and begins with a big-endian property count
  of 89.
- **Verified:** property identifiers, 16-bit types, 16-bit specifiers, array metadata, and numeric
  scalar payloads used by this resource are big-endian.
- **Verified:** scalar float type is `0x000D`; scalar specifier is `0x8000`.
- **Verified:** `0x09AE19D7` decodes as IEEE-754 `300.0`.
- **Verified:** the byte-preserving parser consumes the entire target resource and indexes all 89
  records without trailing data.
- **Verified in automated synthetic tests:** parse followed by serialization with no edit is
  byte-identical; changing a scalar float changes only its four value bytes and reads back as
  `345.0`.
- **Verified in the game:** the parsed runtime form uses 24-byte, ID-sorted property records.
  The target resource owner stores TGI as instance/type/group and points to a vector of exactly
  89 records. Property `0x09AE19D7` has value bits at record offset `+4` and float metadata
  `0x000D0000` at `+20`.
- **Verified in the game:** atomically changing the target value from `300.0` to `345.0` changed
  the building-selector total and the actual hourly simulation charge from `500` to `545` after
  including the unchanged `200` tank cost.
- **Observed UI limitation:** the detail panel for an already placed Sanitizer continued to show
  `500 $/h`, although the budget simulation charged `545 $/h`.

## OpenSC5 assessment

[OpenSC5](https://github.com/TornadoCookie/OpenSC5) was inspected at commit
`0100e3ab15eb7a09fdaa29c8f4616fa86191145f`.

- Its package code corroborates the 96-byte DBPF header, compressed marker `0xFFFF`, index fixed
  fields, and RefPack command families.
- Its PROP code corroborates IDs/types/specifiers, big-endian values, type `0x0D` for float, and
  common vector/string/key layouts.
- Its RefPack routine explicitly performs no bounds checks, so it is unsuitable for direct use in
  an injected framework.
- Its PROP model converts known values and stops on an unrecognized scalar type; it is not a
  lossless serializer. SC13 instead preserves the original buffer and records validated spans.
- OpenSC5 is a reference, not proof of the current game's internal resource-manager ABI.

## Unknowns beyond M2

- whether the earlier deserializer patch eliminates the stale already-placed-building panel;
- the generalized ownership and synchronization contract needed for arbitrary resources;
- stable signatures and ABI records for builds other than the exact verified executable;
- the final declarative patch registry and multi-mod conflict policy.

## Function record template

Use this block only after collecting evidence:

```text
Name:
Hypothesized purpose:
Module:
Signature:
Calling convention:
Arguments:
Return value:
Evidence:
Callers:
Callees:
Game build:
Confidence: verified | strong hypothesis | guess
```
