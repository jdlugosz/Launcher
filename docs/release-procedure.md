# Launcher release procedure

The two license sources stay in version control:

- `licenses/Prerelease.txt`: restricted evaluation terms, selected by default.
- `licenses/Release.txt`: community terms for the official Beta and Release tracks
  and their corresponding source.

Every binary download contains the selected document under the common name
`LICENSE.txt`, alongside the unchanged `THIRD-PARTY-NOTICES.txt`. The source-root
`LICENSE.txt` identifies the current source terms; it is not the file copied
into binary downloads. Both canonical documents remain in source control.

The build configuration `Release` means optimized binaries. The distribution
kind chooses the legal terms. `LAUNCHER_DISTRIBUTION_KIND=Beta` selects Beta,
and `LAUNCHER_RELEASE_PACKAGE=ON` selects the final Release workflow.

## Packaging entry points

The CMake targets use the
[configured Release build](build.md#build-from-the-command-line).

```text
cmake --build --preset release --target LauncherPackagePrerelease
```

`LauncherStage` builds the package executables and stages the explicit payload
under `build/windows-x64/shipping/Release` for the default Release preset.
`LauncherZip` depends on that stage and creates the ZIP beside it.
`LauncherShippingCheck` depends on the ZIP and runs package, isolated installer
and wizard checks. Run the application test suite separately with CTest. These
targets call
`tools/stage-shipping.ps1` with `-Action Stage|Zip`; the configured
`LAUNCHER_RELEASE_PACKAGE` option supplies the final-release switch.

The stage is regenerated from the install payload. Store personal configuration
and any edits from a trial run in a separate directory before restaging. Stage
and ZIP builds require the Release configuration and reject instrumentation.

The explicit distribution targets are `LauncherPackagePrerelease`,
`LauncherPackageBeta`, and `LauncherPackageRelease`, with a shared driver taking
`-Kind Prerelease|Beta|Release`.
These names distinguish distribution type from the optimized `Release` compiler
configuration used by all three. They call `tools/package-distribution.ps1`, which
reserves the identity before building in a separate tree under `build/packages`.
It runs the shipping checks and prints the completed ZIP path and SHA-256.
The invoking Ninja build tree is not reconfigured recursively. The source's
three-part version stays in CMake and vcpkg. Numbered prerelease and Beta packages
share the reservation sequence for the fourth File version component.
The native Product version is the major and
minor series, and Launcher.exe carries the package's Git provenance. The SVG
Extractor has its own File version. The signed ZIP identifies the precise files
that ship together.

The counter and reservation receipts live in `launcher-packages` under Git's
common directory, shared by linked worktrees and independent of build cleanup.
Reservations are serialized and never reused, including failed attempts. Back up
that directory when moving the packaging role to another machine. The direct
driver accepts `-StateDirectory` for a separately preserved/shared state location.
A special update uses `-Kind Prerelease -DistributionKind Special` with an explicit
third-component source version; it uses evaluation terms and retains the old EXE.

Packaging is one step in the release process. It does not decide that a candidate
is final, change source versions, create a tag, publish a download or advance
development to the next version.

## Shared Updater Client

**About → Check now** uses the installed updater in Launcher's scope. For
missing-helper downloads, set both `LAUNCHER_UPDATER_SETUP_URL` to the published
signed Setup executable and `LAUNCHER_UPDATER_SIGNER_SHA256` to the SHA-256 of
its DER-encoded signing certificate in the calling CMake build. The package
driver carries those values into its isolated build. Update the fingerprint
when the publisher changes signing certificates.

To include archive recovery, set `LAUNCHER_ARCHIVE_SIGNING_CERTIFICATE` to the
DER public certificate used to verify Launcher ZIP signatures. The package
includes its immutable catalog build ID and publisher pin. By default a
prerelease ZIP uses `launcher-development-` followed by the version with
hyphens in place of dots. Set `LAUNCHER_ARCHIVE_BUILD_ID` for a directly
configured package when using a different catalog ID. Publish the same ID in
the Atom feed, and sign the completed ZIP with the matching private key.
Keep its detached `.zip.sig` available for recovery.
Set `LAUNCHER_ARCHIVE_ADDITIONAL_SIGNING_CERTIFICATE` to a second DER RSA
public certificate if the current and previous archives can have different
signers. The package driver forwards both files to its isolated build.
Configure both approved archive signers in Launcher packages that participate
in cross-signer recovery.

`LAUNCHER_UPDATER_INFO_URL` sets the HTTPS page to open when the bootstrap is
blocked or fails. With the download settings empty, Check now opens this page
directly when the updater is missing. The current default is the published
Launcher beta page. Portable Launcher also opens the Launcher beta page.

Installed Launcher starts a background check about one second after startup
and normally invokes the updater every 23½ hours while it stays open. The
updater decides whether a network check is due. If the request fails, Launcher
uses a one-hour retry or the longer delay requested by the server.

## Candidate iteration

1. Build a candidate from the reviewed source. Run the full CTest suite and
   shipping checks, including the relevant native and installer scenarios.
   See [Build and test Launcher](build.md#automated-checks).
2. Use `LauncherPackagePrerelease` for evaluation packages. Numbered package
   identities have a fourth component, such as `1.1.0.8`. The driver reserves
   the identity before compiling the binaries that carry it.
3. For the official public Beta track, build the numbered package using
   `cmake --build --preset release --target LauncherPackageBeta`. The target
   runs the full Release test suite and shipping checks against that numbered
   build before reporting its package as complete. Verify the ZIP includes the
   community `LICENSE.txt`, preserve its SHA-256, and publish the corresponding
   source alongside the signed artifact. Assign the resulting build to `beta`
   in the catalog.
4. When the candidate is accepted, perform the final version/commit/tag steps
   below. Full testing and beta feedback precede this promotion; the rebuilt
   final distribution still needs artifact and final-mode validation.

## Prepare and verify the release commit

1. Confirm the community terms in `licenses/Release.txt` and identify the source
   corresponding to the Release package.
2. Set the same three-part version in `CMakeLists.txt` and `vcpkg.json`. An
   official release uses an even minor version: for example `1.2.0`, following
   development `1.1.0`. A nonzero patch version remains part of its release
   series. Do not create a tag merely to advance the version.

   The mechanical source-version step is:

   ```powershell
   .\tools\set-release-version.ps1 -Action PrepareRelease
   ```

   This selects the next even minor and updates both files together. For a patch
   release on a release branch, pass an explicit version, such as
   `-Action PrepareRelease -Version 1.2.3`. It does not commit or tag anything.
3. Confirm the candidate testing and any beta feedback are complete. Keep
   `LAUNCHER_RELEASE_PACKAGE=OFF` during candidate preparation. If code changes
   are needed, return to candidate iteration before designating the final release.
4. Commit the approved version, license, source, and documentation changes.
   Ensure the working tree is clean, including untracked source files. Preserve
   other work separately rather than discarding it to satisfy this check.
5. Create an annotated, signed tag on that exact commit using the authorized
   release signing identity. For the example version:

   ```text
   git tag -s v1.2 -m "Launcher 1.2"
   ```

   A tag named `v1.2.0` is also accepted for project version `1.2.0`; for a
   nonzero patch use the full version, such as `v1.2.1`. Git signature
   verification must succeed at minimum trust `fully` on the build machine.
   A lightweight tag, unsigned tag, untrusted signature, older commit's tag,
   differently named alias, or unrelated version tag is insufficient.

## Build the public distribution

In the initialized x64 developer terminal, select the release package and
build it after the tag exists:

```text
cmake --build --preset release --target LauncherPackageRelease
```

The driver selects `licenses/Release.txt`, prepares matching download README text,
builds Launcher with the tagged commit identity, stages the explicit
CMake install payload, makes the ZIP, and verifies it. Packaging checks the
final license, even minor version, matching source/manifest versions, clean
tree, and exact trusted signed tag again before replacing any prior output.
Launcher and LauncherDrop File versions must match the selected release. The
SVG Extractor keeps its own File version.

Run the appropriate automated checks against this final rebuilt mode, including
startup, version/Git provenance and final-release installer behavior. Candidate
beta results do not by themselves verify the changed final metadata and
installation policy. A metadata-only promotion need not repeat external beta
testing. If final validation finds a code problem, return to the candidate cycle;
do not move or reuse a published release tag for different contents.

The driver reports the ZIP under its isolated build's
`shipping/Release/Launcher-R<version>-windows-x64.zip`. The EXE inside has the
matching `Launcher-R<version>.exe` name.
The ZIP opens onto the versioned payload folder plus the generated outer
`Install.cmd` and `Portable.txt`. Check that both outer files name the packaged
version and that setup still resolves its helpers inside the payload folder.
Explicit compiled distribution status
controls installer retention; neither the optimized build configuration nor an
even minor number by itself authorizes deleting an earlier executable.
After successful activation, a final release removes verified older Launcher
executables in the selected installation. Registered Setup retains one exact
copy of the previous executable recorded in the scoped update receipt when its
numeric version and SHA-256 match. A matching same-byte repair also retains that
copy if it is still present. After **Keep this version**, Updater can retire
that copy if both exact signed ZIPs remain available in the catalog. Launcher
maintenance verifies both ZIPs again before removing the retained executable.
**Launcher Update History and Recovery** can then restore the previous version
from the signed archives. Setup preserves equal or newer versions and personal
settings.
Confirm the downloaded-layout license and build details, retain its SHA-256,
and preserve the verified ZIP outside the regenerable staging directory before
publishing it. Publish the intended commit and signed tag together under the
authorized repository/release workflow. Packaging does not create tags, push
commits, or publish releases on its own.

## Resume development

After preserving the release, advance both project version files to the next
odd minor series, such as `1.3.0`, and explicitly restore the default mode:

```text
.\tools\set-release-version.ps1 -Action NextDevelopment
cmake --preset windows-x64 -DLAUNCHER_RELEASE_PACKAGE=OFF -DLAUNCHER_DISTRIBUTION_KIND=Prerelease
```

Then use the normal build and development acceptance workflow. This restores
the prerelease evaluation document in generated builds and distributions.
Do not rewrite or reuse the release tag for later development commits.
