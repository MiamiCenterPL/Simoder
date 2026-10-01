'use strict';
const assert = require('node:assert/strict');
const { test } = require('node:test');
const { diagnosticLocation, reportErrors, completionBounds } = require('./helpers');
/** @summary Verifies bounded parser positions and per-resource failures without VS Code dependencies. */
test('diagnostics locate parser errors and named symbols', () => {
    assert.deepEqual(diagnosticLocation('a\nb', 'overrides.toon:2:3: invalid'), { line: 1, column: 2 });
    assert.deepEqual(diagnosticLocation('first\n  cost,float,set,345', "Unknown property name 'cost'"), { line: 1, column: 2 });
    assert.deepEqual(reportErrors({ errors: ['invalid'], resources: [{ tgi: 'ABC', error: 'missing' }] }), ['invalid', 'ABC: missing']);
});
/** @summary Verifies quoted CSV names and object names are replaced as complete tokens. */
test('completion preserves TOON quoting and cell boundaries', () => {
    const line = '      "cost, hourly",float,set,345';
    const bounds = completionBounds(line, 12);
    assert.equal(line.slice(bounds.begin, bounds.end), '"cost, hourly"');
    const objectLine = '      name: "sanitizer"';
    const objectBounds = completionBounds(objectLine, 18);
    assert.equal(objectLine.slice(objectBounds.begin, objectBounds.end), '"sanitizer"');
});
