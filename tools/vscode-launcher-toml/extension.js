'use strict';

const vscode = require('vscode');
const { pragmaRanges } = require('./pragmas');

function activate(context) {
    const decoration = vscode.window.createTextEditorDecorationType({
        color: '#FFFFFF',
        backgroundColor: '#C00000',
        fontStyle: 'normal',
        rangeBehavior: vscode.DecorationRangeBehavior.ClosedClosed
    });

    function update(editor) {
        const ranges = editor.document.languageId === 'launcher-toml'
            ? pragmaRanges(editor.document.getText()).map(range => new vscode.Range(range.line, range.start, range.line, range.end))
            : [];
        editor.setDecorations(decoration, ranges);
    }

    function updateVisible() {
        vscode.window.visibleTextEditors.forEach(update);
    }

    context.subscriptions.push(
        decoration,
        vscode.window.onDidChangeVisibleTextEditors(updateVisible),
        vscode.workspace.onDidOpenTextDocument(updateVisible),
        vscode.workspace.onDidChangeTextDocument(event => {
            for (const editor of vscode.window.visibleTextEditors) {
                if (editor.document === event.document) update(editor);
            }
        })
    );
    updateVisible();
}

module.exports = { activate };
