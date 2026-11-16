# 2. Configuration becomes a search

## Native wildcards in an ordinary target

The configuration interface remains an ordinary target path. Launcher first expands its macros and environment variables and resolves a relative path against the configuration directory. Only a filesystem Shell target containing a real `*` or `?` becomes a generic version search. URLs, Shell namespace targets and non-filesystem handlers retain their own interpretation.

```toml
# Illustrative configuration
{ target = '$P\Blender Foundation\Blender*\blender.exe' }
{ target = 'Tools\Suite *\Build *\bin\editor-*.exe' }
{ target = 'Documents\Report-202?.pdf' }
```

There is no Blender-specific directory parser. `Blender*` can match the installed directory names `Blender 5.2`, `blender-5.1.0-windows-x64` and `blender-5.3.0-beta+v53...` on the ordinary case-insensitive Windows filesystem. `Blender *` requires the literal space and therefore excludes the dash-named examples. A dash or plus sign has no special meaning here.

Windows performs each individual wildcard match. The traversal algorithm supplies the sequence of path components, rather than introducing recursive glob syntax. A pattern with two changing directory levels enumerates the matching branches at each level. A `**` sequence does not create a separate recursive-search operator.

The question mark in an extended Windows path prefix is literal. `has_target_wildcards()` excludes that prefix when deciding whether the target contains wildcard syntax. This prevents a concrete extended-length path from accidentally becoming a version family.

@source src/launch.cpp | std::optional<fs::path> version_target_pattern(
@source src/target_versions.cpp | bool has_target_wildcards(

## Normalization without filesystem interpretation

Search identity needs a stable lexical representation. It must not probe a slow drive just to decide whether two icons share work. The normalizer therefore manipulates path syntax without resolving directory links or asking the filesystem for a canonical target.

The combination of a wildcard component and `..` deserves care. A generic lexical collapse can erase the requirement that a matching intermediate directory actually exist. The normalizer preserves that semantic boundary instead of reducing the pattern as if every component named one concrete directory.

The provider adapter converts the expanded absolute pattern to the normalized, case-insensitive generic identity used by the service. The service itself compares owned `VersionSearchKey` values. It does not independently perform Windows path normalization every time it receives a request.

@source src/target_versions.cpp | std::filesystem::path normalize_target_version_pattern(
@source src/main.cpp | std::optional<launcher::VersionSearchKey> version_search(

## What belongs in the key

The key contains the provider kind, its canonical location vector and a semantic revision. A generic search normally has one location: its absolute pattern. The current Adobe adapter uses a typed shared default-catalog key. The vector permits a provider to describe a catalog spanning more than one root without confusing provider identity with an icon's configuration.

Arguments, working directory, show mode, Adobe product/beta selection, explicit label, explicit icon, item index and activation token remain outside discovery identity. They affect how a binding uses the discovered files, rather than which files its provider enumerates. Generic targets have no configured older-version selector in this edition; their version menu supplies explicit older-file choices.

@code include/launcher/version_discovery.hpp | struct VersionSearchKey { | 7 | Owned discovery identity

@figure identity | Separate icon configurations share discovery while retaining their own actions and selectors.

This separation prevents duplicate scans when two icons target the same installation family. It also prevents one icon's launch options from leaking into another icon's action. A new provider option belongs in the key only if it changes discovery semantics. Changing a presentation preference alone should not multiply workers or fragment complete catalogs.

## A discovered path is already concrete

Once discovery selects a file, that file's path is an owned observation. Later preparation must not expand a literal percent sequence in its filename again or reinterpret it as another wildcard pattern. The exact-target preparation helpers validate and convert the copied action options while retaining the selected file.

The same rule applies to menu rows. Selecting an older installation is an explicit choice of one concrete path. The eventual action must not silently resolve the original family pattern back to the newest installation. If the selected file disappears, the action can fail against that file. It must not switch to another installation merely because another file is now available.

@source src/launch.cpp | PreparedLaunch prepare_exact_target_action(

## Executables, documents and shortcuts

Generic discovery accepts final regular files. Executables can contribute numeric Windows version metadata. Documents and shortcuts often have no such resource, so the scanner derives readable labels from the matched path components and applies natural ordering.

The common data model deliberately supports both cases. Some member names preserve earlier executable-oriented terminology, but the type's concrete path is the target file. Launcher-owned Open, Reveal and Properties use that exact path. In particular, Properties for a shortcut concerns the shortcut file itself, rather than silently substituting the shortcut's destination.
