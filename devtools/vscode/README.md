# Simoder Mod Tools for VS Code

This local extension has no npm runtime dependencies. Build `simoder-dev.exe`, then launch
an Extension Development Host with the extension directory and the project workspace:

```powershell
code --extensionDevelopmentPath="C:/Projects/Simoder/devtools/vscode" "C:/Projects/Simoder"
```

Open `mod.toon`, `overrides.toon` or `symbols.toon`. Select **Simoder TOON** as the language
if another TOON extension owns the file association. Trust the workspace before executing tools.
The default executable is `<workspace>/build/vs2026/bin/Release/simoder-dev.exe`.
For another layout set `simoder.executable` to an absolute path.

Use Ctrl+Space for catalog alias completions with identity, type, description and provenance.
Catalogs are read from saved files. `floatpatch` and `propertyalias` snippets provide complete
TOON rows. Saving triggers definition validation. **Simoder: Validate Current Mod** opens the
JSON report in the output channel. Set `simoder.gameDirectory` to check source packages too;
ambiguous package matches are diagnosed rather than guessed. Validation does not enable mods.

Syntax/parser errors use available source positions. Semantic errors locate named identities
where possible, otherwise the start of `overrides.toon`. Failed tool execution is shown in the
output channel. The extension does not install itself or change editor settings automatically.

Helper tests: `node --test devtools/vscode/helpers.test.js`. Visual editor acceptance requires
launching the Extension Host; it is separate from CLI/parser and JavaScript syntax checks.
