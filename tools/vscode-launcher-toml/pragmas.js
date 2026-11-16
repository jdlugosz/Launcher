'use strict';

// Decorations supply the background that TextMate token colors cannot.
// Pragmas are physical lines, independent of TOML strings and comments.
function pragmaRanges(source) {
    const ranges = [];
    const lines = source.split(/\r?\n/);
    for (let lineNumber = 0; lineNumber < lines.length; ++lineNumber) {
        const line = lines[lineNumber];
        const start = lineNumber === 0 && line.startsWith('\uFEFF') ? 1 : 0;
        if (/^%\S/.test(line.slice(start))) {
            ranges.push({ line: lineNumber, start, end: line.length });
        }
    }
    return ranges;
}

module.exports = { pragmaRanges };
