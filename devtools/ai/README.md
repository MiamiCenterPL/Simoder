# Simoder AI DevBridge

This optional, Windows-only developer module exposes a restricted SimCity startup workflow to
local MCP clients. It is not installed unless `SC13_INSTALL_DEVTOOLS=ON`.

## Security model

- `Simoder.McpServer.exe` is a non-elevated stdio server.
- `Simoder.DevBridge.exe` requests one manual UAC approval and exposes no network listener.
- The Named Pipe uses `CurrentUserOnly`, a per-user/per-session name, and exact client executable
  plus Windows-session verification.
- EA and SimCity targets require expected paths, names, and trusted Electronic Arts Authenticode
  signatures. Simoder executables require exact installed paths.
- Screenshots and input are limited to visible allowlisted windows. A click requires a capture no
  older than ten seconds, unchanged window geometry, a point inside the client area, and the same
  window still being in the foreground.
- UI discovery uses UI Automation first. Image acquisition uses Windows Graphics Capture with a
  bounded `PrintWindow` fallback and a foreground-only screen-copy fallback.
- Arbitrary text, arbitrary keys, process termination, and automatic restart are not exposed.

Windows UAC secure-desktop prompts are always manual. If EA displays another UAC prompt, the MCP
workflow reports `human_action_required` and waits.

## Registration

From the desired trusted project:

```powershell
& '<SimCity>\simoder\DevTools\AI\Register-Simoder-MCP.ps1' -Scope Project -ProjectPath $PWD
```

User-wide registration is explicit:

```powershell
& '<SimCity>\simoder\DevTools\AI\Register-Simoder-MCP.ps1' -Scope User
```

Registration is idempotent and writes only a delimited managed block. It configures stdio,
`default_tools_approval_mode = "writes"`, a 60-second startup timeout, and a 310-second bounded
tool timeout. Restart the Codex host after registration.

Before deleting DevTools, unregister the same scope:

```powershell
& '<SimCity>\simoder\DevTools\AI\Unregister-Simoder-MCP.ps1' -Scope Project -ProjectPath $PWD
```

## AI workflow

1. Call `simcity_start_session` with `continue`, or `play` plus exact visible region and city names.
2. Manually approve UAC.
3. Alternate `simcity_get_startup_state` and `simcity_capture_window`.
4. Use the fresh image and `capture_id` for `simcity_click` or an allowlisted key.
5. Bind visual conclusions to evidence with `simcity_confirm_stage`.
6. Treat `suspected_infinite_loading` as diagnostic only and collect `simcity_get_diagnostics`.

The workflow succeeds only after `in_city` is confirmed from a fresh SimCity capture.

## Mod development evidence

`simoder_get_runtime_state` reads mods, resolved overrides, pending changes, conflicts and bounded
historical resource observations from the loader's atomic snapshot. Check `available` and `fresh`.
A fresh snapshot requires the verified SimCity PID, consistent generation and age <= 5 seconds;
it does not establish consumer/gameplay acceptance. `simoder_get_logs` reads at most 200 lines
from the fixed loader log's last 64 KiB and can filter one exact mod ID. Both tools are read-only,
cause no UAC prompt, and require the existing broker. See `docs/developer-experience.md` in source.
