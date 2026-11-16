# Launcher TOML for VS Code

Extends Even Better TOML's syntax highlighting with percent pragma comments.
Lines beginning with `%` immediately followed by a directive name use the TextMate scope
`comment.line.percentage.pragma.launcher-toml` and white text on a red background.
`%` must be in column one, with no leading whitespace or space before the name.
Pragmas occupy the entire physical line and are recognized even inside TOML
multiline strings or arrays; quotes and brackets on a pragma line do not affect
TOML highlighting. Inline percent signs and indented lines stay TOML text.

`Launcher*.toml` files automatically use **Launcher TOML**. Other TOML files keep
their normal language mode. To use the extension for a differently named file,
click its language in the status bar and select **Launcher TOML**. Select **TOML**
to return to standard TOML support.

This language mode provides syntax highlighting and basic editing behavior.
Even Better TOML supplies the underlying `source.toml` grammar, but its standard
TOML language server does not validate, format or complete this dialect. No
diagnostics are hidden in ordinary TOML mode. Launcher itself currently
implements `%STOP`; `%INCLUDE` is planned and not implemented. Highlighting
other `%` commands does not implement them. See the project's README for the
preprocessing syntax and future include example.
Only the pragma line is styled as a comment; following examples keep normal
TOML highlighting.

The upstream definition is in the installed `tamasfe.even-better-toml-<version>`
extension: `toml.tmLanguage.json` defines tokens, `language-configuration.json`
defines commenting/bracket behavior, and `package.json` registers them.
This companion extension leaves those installed files intact and reuses the
current upstream grammar after updates.

Build the local VSIX from the project root (no Node/npm installation needed):

```powershell
.\tools\vscode-launcher-toml\Package.ps1
code --install-extension .\build\vscode-extension\launcher-toml-0.1.1.vsix
```

If VS Code requests it, run **Developer: Reload Window**. To rebuild the same
version, choose a fresh `-OutputDirectory`; packaging never overwrites a VSIX.
The files to edit are `syntaxes/pragma.tmLanguage.json` (comment recognition),
`pragmas.js` (column-one decoration ranges), and `extension.js` (inverse colors).
