# Logging

## Pipeline

Every producer emits the same semantic fields:

```text
timestamp | source type | source name | level | thread | message
```

Source types are `GAME`, `LOADER`, and `MOD`. A MOD source name is its stable manifest ID. Producers
never own files, console handles, or Win32 output. The central bounded queue feeds:

- the always-on file sink at `simoder/logs/sc13modloader.log`;
- the optional external DeveloperConsole sink;
- future sinks without changing producers.

The queue holds 2048 fixed-size events. A game/hook thread copies a bounded record and returns. The
worker drains up to 64 records per batch and flushes once per batch. Overflow drops new events and
emits a dropped-count warning instead of blocking or growing without bound.

## DeveloperConsole

The console is disabled by default. `simoder/config.toon` is loaded once at process startup:

```text
developerConsole:
  enabled: false
  captureGameLogs: true
  captureLoaderLogs: true
  captureModLogs: true
  level: info
```

Supported levels are `trace`, `debug`, `info`, `warn`, and `error`. Source filters and the minimum
level affect the console sink; the diagnostic file remains complete. Console allocation or
`CONOUT$` failure is non-fatal and file logging continues.

## Game channels

Read-only import inventory of the validated SimCity `10.3.6.0` executable confirms
`OutputDebugStringA`. Simoder hooks `OutputDebugStringA` and `OutputDebugStringW`, calls the original
function first, preserves `LastError`, copies at most 4096 bytes through checked current-process
reads, and uses a thread-local recursion guard. Captured events are tagged `GAME` and name the API.

The executable also imports generic `WriteFile` and C-runtime formatting functions, but that does
not establish that stdout/stderr is an active logging channel. M3 therefore does not redirect
standard streams or hook `printf`, `fprintf`, or `vsnprintf`. No safe higher-level internal game
logger has yet been verified. Those channels remain explicitly uncaptured rather than guessed.

Diagnostic package-I/O tracing remains opt-in through `SC13_ENABLE_DISCOVERY_TRACE`; it is not a
normal logging dependency.
