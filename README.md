# Launcher

Launcher is a native Windows 10 and 11 app for x64 PCs. It keeps your
shortcuts on a tabbed dock along a screen edge. Shortcuts can open apps,
documents, folders, and links, or let you choose among installed versions
of an app.

## Download and run

Get Launcher from the [downloads page](https://updates.dlugosz.enterprises/downloads/launcher/index.html)
and extract the entire ZIP.

- **Install or update.** Open **Launcher Setup.exe** inside the versioned folder
  or run the outer **Install.cmd**. See the [Setup and maintenance guide](tools/installer/README.md)
  for installation scope, sign-in startup, and recovery options.
- **Try it without installing.** Read **Portable.txt**, then run
  `Launcher-R<version>.exe` inside the versioned folder.

To check for updates from Launcher, choose **About… → Check now**.

The download includes Launcher Drop and Launcher SVG Extractor. The starter
configuration opens the offline documentation from its **Launcher guide** icon.

## Use the dock

- Point at the tabs to reveal the dock. Click a tab to select its page.
- Let the pointer rest on an icon to see its label. Click to open its target.
  An icon with no target is a placeholder.
- Right-click a tab or an empty part of the dock for **Adjust sizes...**,
  **Edit configuration**, **Open configuration folder**, **About…**,
  **Hide**, **Restart**, or **Quit**.
- To bring the dock back after **Hide**, click its taskbar button.

The starter's **Examples** page includes **Try the many-colors demo**. It opens
a separate dock with its own settings.

## Configure Launcher

Choose **Edit configuration** to open the active TOML file. Save changes, then
choose **Restart** to reload it. The [starter configuration](Launcher.toml)
contains examples below its `%STOP` line. Copy an example into the active
settings above that line before editing it.

A page is a `[[tabs]]` section. Each entry in its `items` array defines a shortcut.

```toml
[[tabs]]
name = "Tools"
color = "green"
items = [
    { label = "Notepad", target = "notepad.exe" },
    { label = "Project files", target = 'C:\Work\Projects' },
]
```

Use [Launcher Drop](tools/drop-target/README.md) to collect filenames and build
item entries. Use [Launcher SVG Extractor](docs/svg-extractor.md) to find SVG
artwork for icons.

## Documentation

- [Configuration](docs/configuration.md) covers profiles, TOML, dock behavior,
  and grid layout.
- [Launch targets](docs/launch-targets.md) covers shortcut fields, icon sources,
  path substitutions, and version selection, including Adobe Creative Cloud.
- [Appearance](docs/appearance.md) covers tabs, colors, labels, sizes, and
  positioning.
- [Configuration and file locations](docs/file-locations.md) explains where
  Launcher reads and saves each kind of file.

## Troubleshooting

Choose **About… → Copy details** when reporting a problem. Launcher writes
`launcher.log` in its profile. External filesystem errors from version
discovery are recorded in `launcher.filesystem-errors.log`. See the
[file search order](docs/file-locations.md#read-search-order) to locate your profile.

## Development

See [Build and test Launcher](docs/build.md) for Windows toolchain prerequisites,
the pinned JMDlib.Cpp dependency, and build and test commands. See the
[release procedure](docs/release-procedure.md) to package a distribution.

## Licensing

This public source and official Beta and Release distributions use the
[Długosz Software Community License](licenses/Release.txt).
Prerelease and Special binary distributions use the
[evaluation terms](licenses/Prerelease.txt). Each download includes its
applicable [license](LICENSE.txt) and [third-party notices](THIRD-PARTY-NOTICES.txt).

The [public source repository](https://github.com/jdlugosz/Launcher) is on GitHub.
