'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { pragmaRanges } = require('./pragmas');

test('pragmas require a name immediately after a column-one percent', () => {
    const lines = ['%STOP', ' %STOP', '\t%STOP', '% STOP', '%', '# %STOP', 'name="%STOP"'];
    assert.deepEqual(pragmaRanges(lines.join('\n')), [{ line: 0, start: 0, end: 5 }]);
});

test('pragma lines are recognized inside multiline strings and arrays', () => {
    const lines = ['name="""', '%INCLUDE "novel.txt"', '"""', 'items=[', '%STOP', ']'];
    assert.deepEqual(pragmaRanges(lines.join('\n')), [
        { line: 1, start: 0, end: lines[1].length },
        { line: 4, start: 0, end: 5 }
    ]);
});

test('quotes and brackets on a pragma line do not affect subsequent pragmas', () => {
    const lines = ['%STOP """ [{', '%INCLUDE "novel.txt"'];
    assert.deepEqual(pragmaRanges(lines.join('\n')), [
        { line: 0, start: 0, end: lines[0].length },
        { line: 1, start: 0, end: lines[1].length }
    ]);
});

test('a file BOM is excluded from the first pragma range and CRLF is supported', () => {
    assert.deepEqual(pragmaRanges('\uFEFF%STOP\r\n%INCLUDE "novel.txt"\r\n'), [
        { line: 0, start: 1, end: 6 },
        { line: 1, start: 0, end: 20 }
    ]);
    assert.deepEqual(pragmaRanges('\n\uFEFF%STOP\n'), []);
});
