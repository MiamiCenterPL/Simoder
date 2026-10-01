'use strict';
const vscode = require('vscode');
const path = require('path');
const { execFile } = require('child_process');
const { diagnosticLocation, reportErrors, completionBounds } = require('./helpers');

/** @summary Runs the configured CLI directly with bounded output and no shell interpolation. */
function run(document, args) {
    if (!vscode.workspace.isTrusted) return Promise.reject(new Error('Trust the workspace before running Simoder tools.'));
    const config = vscode.workspace.getConfiguration('simoder', document.uri);
    const root = vscode.workspace.getWorkspaceFolder(document.uri)?.uri.fsPath;
    const executable = config.get('executable') || (root && path.join(root, 'build/vs2026/bin/Release/simoder-dev.exe'));
    if (!executable || !path.isAbsolute(executable)) return Promise.reject(new Error('Set simoder.executable to an absolute simoder-dev.exe path.'));
    return new Promise((resolve, reject) => execFile(executable, args, { windowsHide: true, timeout: 30000, maxBuffer: 2 * 1024 * 1024 },
        (error, stdout, stderr) => {
            try {
                if (stdout.trim()) resolve(JSON.parse(stdout));
                else reject(new Error(stderr.trim() || error?.message || 'CLI returned no report'));
            } catch (parseError) { reject(parseError); }
        }));
}

/** @summary Registers symbol completions, a validation command, and version-safe on-save diagnostics. */
function activate(context) {
    const diagnostics = vscode.languages.createDiagnosticCollection('simoder');
    const output = vscode.window.createOutputChannel('Simoder');
    const selector = { language: 'simoder-toon', scheme: 'file' };
    /** @summary Validates saved files and attaches errors to their named TOON source when available. */
    async function validate(document, explicit = false) {
        if (document.isDirty) {
            if (explicit) vscode.window.showInformationMessage('Save the mod files before validating their disk contents.');
            return;
        }
        const version = document.version;
        const directory = path.dirname(document.uri.fsPath);
        try {
            const game = vscode.workspace.getConfiguration('simoder', document.uri).get('gameDirectory');
            const report = await run(document, game ? ['validate', directory, '--game', game, '--json'] : ['explain', directory, '--json']);
            if (document.version !== version || document.isDirty) return;
            for (const name of ['mod.toon', 'overrides.toon', 'symbols.toon']) diagnostics.delete(vscode.Uri.file(path.join(directory, name)));
            const grouped = new Map();
            for (const message of reportErrors(report)) {
                const namedFile = /\b(mod|overrides|symbols)\.toon/.exec(message)?.[0] || 'overrides.toon';
                const uri = vscode.Uri.file(path.join(directory, namedFile));
                const target = await vscode.workspace.openTextDocument(uri);
                const location = diagnosticLocation(target.getText(), message);
                const line = target.lineAt(location.line);
                const column = Math.min(location.column, line.text.length);
                const range = new vscode.Range(location.line, column, location.line, Math.max(column, line.text.length));
                const items = grouped.get(uri.toString()) || [];
                items.push(new vscode.Diagnostic(range, message, vscode.DiagnosticSeverity.Error));
                grouped.set(uri.toString(), items);
            }
            for (const [uri, items] of grouped) diagnostics.set(vscode.Uri.parse(uri), items);
            output.appendLine(JSON.stringify(report, null, 2));
            if (explicit) {
                output.show(true);
                vscode.window.showInformationMessage(report.valid ? (report.checkedGame ? 'Simoder: packages validated.' : 'Simoder: definitions valid; game data unchecked.') : 'Simoder: validation failed; see diagnostics.');
            }
        } catch (error) { output.appendLine(String(error)); if (explicit) vscode.window.showErrorMessage(String(error)); }
    }
    context.subscriptions.push(diagnostics, output,
        vscode.languages.registerCompletionItemProvider(selector, {
            /** @summary Completes documented aliases using the loader's catalog resolution rules. */
            async provideCompletionItems(document, position) {
                try {
                    const report = await run(document, ['symbols', path.dirname(document.uri.fsPath)]);
                    return report.symbols.map(symbol => {
                        const item = new vscode.CompletionItem(symbol.name, symbol.kind === 'resource' ? vscode.CompletionItemKind.Module : vscode.CompletionItemKind.Field);
                        item.insertText = JSON.stringify(symbol.name);
                        const bounds = completionBounds(document.lineAt(position.line).text, position.character);
                        item.range = new vscode.Range(position.line, bounds.begin, position.line, bounds.end);
                        item.detail = `${symbol.kind} ${symbol.id || symbol.tgi} ${symbol.type || ''}`;
                        item.documentation = [symbol.description, symbol.unit, symbol.origin, symbol.source].filter(Boolean).join('\n');
                        return item;
                    });
                } catch (error) { output.appendLine(String(error)); return []; }
            }
        }),
        vscode.workspace.onDidSaveTextDocument(document => { if (document.languageId === 'simoder-toon') void validate(document); }),
        vscode.commands.registerCommand('simoder.validateMod', () => {
            const document = vscode.window.activeTextEditor?.document;
            if (document?.languageId === 'simoder-toon') return validate(document, true);
            return vscode.window.showInformationMessage('Open a Simoder TOON file first.');
        }));
}
module.exports = { activate };
