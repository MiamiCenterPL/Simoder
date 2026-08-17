# Architecture

## M2 boundary

M1 proved one late in-memory PROP change for
`00B1B104:61EFC000:719436BD`, property `0x09AE19D7`, from `300.0` to `345.0`.
M2 moves that exact change to the concrete synchronous PROP deserializer so every downstream
consumer can receive the same value. The
declarative mod format, Lua, overlay, legacy-package importer, and general patch registry remain
future layers.

## Component boundaries

```text
sc13-launcher.exe
    -> LoadLibraryW(sc13modloader.dll)
    -> SC13_Initialize() outside DllMain

sc13modloader.dll
    bootstrap -> build fingerprint -> module inventory
              -> uniquely resolved PROP deserializer hook
              -> exact TGI/property-vector validator
              -> owned snapshot/cache
              -> atomic four-byte pre-consumer 300.0 -> 345.0 patch and detach rollback

SC13_ENABLE_DISCOVERY_TRACE=ON
    diagnostic -> filtered package API and stream trace
               -> PFRecordRead and parsed-publisher trace
               -> digest-addressed executable-image captures

sc13_core
    core       -> TGI, SHA-256
               -> runtime property records, owned resource copies, patched-copy cache
    formats    -> DBPF v3, RefPack, byte-preserving PROP
    memory     -> validated signature parsing and exact-match scanning

sc13-inspect.exe
    read-only package and resource validation

sc13-memory-probe.exe
    read-only live-memory evidence and bounded hexdumps
```

Generic parsers do not depend on game addresses. Reverse-engineered symbols and calling
conventions live under `sc13::reverse`; hook mechanics live under `sc13::hooks`.

## Bootstrap decision

The current executable imports only `Core/Activation.dll` statically. Replacing that signed EA
file with a proxy would violate the non-destructive installation requirement and is not justified
by runtime module evidence. The first bootstrap therefore uses a separate x86 launcher:

1. load the DLL into the selected x86 process;
2. wait for `LoadLibraryW` to return;
3. resolve the RVA of exported `SC13_Initialize` without executing local `DllMain`;
4. invoke that export on a remote thread;
5. let the game continue even when observation hooks cannot be installed.

`DllMain` only stores its module handle and disables thread notifications.

## Runtime patch boundary

The verified M2 object path is:

```text
runtime resource owner
    -> TGI fields in instance/type/group order
    -> vector {begin, end, capacity}
    -> exactly 89 sorted 24-byte property records
    -> property 0x09AE19D7, float metadata 0x000D0000
    -> aligned atomic compare/exchange from 300.0 to 345.0
```

The M2 deserializer hook retains a loader-owned copy for validation and lifetime tests, but does not
fabricate or return a cloned game object: its vtable, allocator, destructor, and reference-count
contract are not established. After clean read-only proof, the mutation is one verified float in
the game-owned vector before downstream caches receive it. The hook
refuses an absent, ambiguous, differently laid-out, wrongly typed, or unexpectedly valued target.
It records the exact value address and atomically restores `300.0` during shutdown only while the
original TGI, vector bounds, aligned address, and replacement bits still match.

## Failure behavior

- Signature absence or ambiguity means no internal hook.
- The normal build resolves and installs only the critical deserializer hook; completed discovery
  hooks cannot become accidental prerequisites for the patch.
- Bootstrap errors are logged and returned to the launcher.
- Hook-boundary functions are `noexcept` and use bounded state.
- Filtered log queues drop records instead of blocking a hot game thread.
- Closing the game and starting it normally removes every loader effect because no game file is
  changed.
