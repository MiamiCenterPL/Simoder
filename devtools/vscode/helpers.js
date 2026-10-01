'use strict';

/** @summary Finds a bounded source position from production parser errors or quoted symbol identities. */
function diagnosticLocation(text, message) {
    const lines = text.split(/\r?\n/);
    const exact = /(?:mod|overrides|symbols)\.toon:(\d+)(?::(\d+))?/.exec(message);
    if (exact) return { line: Math.min(lines.length - 1, Math.max(0, Number(exact[1]) - 1)), column: Math.max(0, Number(exact[2] || 1) - 1) };
    const identity = /(?:name|Symbol|property) '([^']+)'/i.exec(message);
    if (identity) {
        const line = lines.findIndex(value => value.includes(identity[1]));
        if (line >= 0) return { line, column: lines[line].indexOf(identity[1]) };
    }
    return { line: 0, column: 0 };
}

/** @summary Collects both global and per-resource validation failures for editor diagnostics. */
function reportErrors(report) {
    return [...(report.errors || []), ...(report.resources || []).filter(resource => resource.error)
        .map(resource => `${resource.tgi}: ${resource.error}`)];
}
/** @summary Finds a full quoted or unquoted TOON name cell so completion cannot duplicate quotes. */
function completionBounds(line, column) {
    const nameField = /^\s*name\s*:\s*/.exec(line);
    let begin = nameField ? nameField[0].length : 0;
    let quoted = false, escaped = false, end = line.length;
    for (let index = begin; index < line.length; ++index) {
        const character = line[index];
        if (escaped) { escaped = false; continue; }
        if (quoted && character === '\\') { escaped = true; continue; }
        if (character === '"') quoted = !quoted;
        if (!quoted && character === ',' && !nameField) {
            if (index < column) begin = index + 1;
            else { end = index; break; }
        }
    }
    while (begin < end && /\s/.test(line[begin])) ++begin;
    while (end > begin && /\s/.test(line[end - 1])) --end;
    return { begin, end };
}
module.exports = { diagnosticLocation, reportErrors, completionBounds };
