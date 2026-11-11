# Local candidates and public releases

Building, accepting a build, and publishing are separate operations. A request to
build a test Flatpak authorizes a local package, not a GitHub release or draft.

## Version identity

`VERSION` is the single source for the application's About/`--version` display,
generated AppStream metadata, bundle filename and build receipt. CMake keeps the
numeric part separately because its `project(VERSION)` accepts only numbers.

Use the [SemVer prerelease notation](https://semver.org/#spec-item-9):

| Stage | Example | Purpose |
| --- | --- | --- |
| Local candidate | `0.9.0-rc.1`, then `0.9.0-rc.2` | Test deliveries; never a public release by implication |
| Final package | `0.9.0` | Built after candidate acceptance and issue PR merges; validate before publication |
| Next changes | `0.9.1-rc.1` or `0.10.0-rc.1` | Fixes or new features respectively |

The notification feature targets **0.9.0**, following published 0.8.1. Candidate
numbers start at 1 and increase for every changed delivery, including packaging
changes. Allocate them on the release's source branch by committing `VERSION`;
do not maintain independent counters in scripts, filenames or README examples.
Patch releases carry fixes; minor releases carry features. The project remains
under development at major version zero.

Each completed local build reserves its version with a local `test/vVERSION`
tag at the exact source commit. Keep these tags even after cleaning build caches.
Public releases use separate `vVERSION` tags. Never push `test/*` tags or use
`git push --tags`. Multiple release operators must coordinate reservations;
local tags cannot reserve a name globally in other clones.

## Build and test a candidate

1. Inspect the latest GitHub release, public tags, `docs/releases/catalog.json`,
   local `test/*` tags and `dist/`. Choose the next unused candidate and commit
   `VERSION` with the intended changes. Untracked source files also make a tree
   unsuitable for a package.
2. Run `./scripts/build-flatpak.sh`. It builds locally, using the manifest's pinned
   dependency sources. It neither installs the app into the normal user
   installation nor pushes or publishes anything.
3. The checks reject dirty source, versions at/below the published catalog, used
   public/local version tags, existing artifacts for that version, source changes during
   the build, mismatched application/AppStream versions and mismatched bundle
   payloads. All files stay directly in `dist/`: `Archcodex-VERSION-ARCH.flatpak`,
   its `.flatpak.sha256` checksum and `Archcodex-VERSION-ARCH.BUILD-INFO.json`.
   The receipt records the full source commit/tree, manifest hash, runtime,
   Flatpak commit, build method and bundle hash. There is no mutable shared
   `dist/SHA256SUMS` for these builds.
4. On the desktop host, quit Archcodex, change into `dist/`, verify
   `sha256sum -c Archcodex-VERSION-x86_64.flatpak.sha256`, then install its exact bundle with
   `flatpak install --user --or-update ./Archcodex-VERSION-x86_64.flatpak`.
   Check `flatpak run io.github.emsi.Archcodex --version` before testing.
5. Record acceptance against the candidate version **and bundle SHA-256**, with
   the tested host, failures and unverified scenarios. Keep the bundle and receipt
   until the final release is accepted. Any changed candidate receives a new number.

Candidates intentionally use the existing application ID and Flatpak `stable`
ref, so installation replaces the installed app and uses its existing settings
and workspace. `stable` here identifies the Flatpak install slot, not acceptance
or publication status. Installing an older saved bundle restores the older
application; it does not roll back user data. Changes to data compatibility must
be assessed separately before testing or reverting. Running side by side with
isolated data would require a separate application ID and is not this process.

The version and receipt identify a build; they are not a claim of reproducible
bytes, a signature, or successful desktop acceptance.

### Retained SDK fallback in this build container

This container cannot launch Flatpak's user-namespace sandbox. Reuse the existing
KDE SDK chroot and pinned, patched Konsole build instead of downloading a second
SDK. This is a packaging fallback, not a sandbox test:

1. Run `python3 scripts/release.py prepare` to reserve that version's filenames and
   source identity, then copy that clean source into the retained SDK. Set
   `SOURCE_DATE_EPOCH` to the source commit's Unix timestamp for configuration.
2. Build/install Archcodex and the manifest-pinned Konsole into a temporary stage.
   Include the dependency licenses and the notification event configuration.
   Use the manifest's runtime, application ID and finish arguments. Strip the
   staged binaries, finalize with `flatpak build-finish`, export with
   `flatpak build-export`, and write the versioned bundle directly into `dist/`.
3. Run `python3 scripts/release.py finish --build-dir STAGE --repository REPOSITORY
   --sdk-root .build/sdk-root`. This checks the staged executable's version using
   the SDK loader, verifies the AppStream version, imports the actual bundle into
   a temporary repository and compares its commit and executable, then writes the
   receipt/checksums and reserves the local tag. Its temporary verification import
   is removed automatically. Verify packaged Konsole startup separately.
4. Remove the temporary stage/export repository after validation, retaining the
   single SDK, output bundle, receipt and local tag. Report real Flatpak/KDE
   acceptance as unverified until it is tested on a host.

A failed build leaves `dist/.Archcodex-VERSION.building.json` and may leave
incomplete files for that version. Inspect the failure first. If that attempt was
never distributed and has no completed receipt or version tag, remove only that
version's marker and incomplete files, then retry the same committed source.
Keep every other version's files. Never delete a reservation to reuse a delivered number.

## Cut and publish the final release

1. Obtain explicit authorization to cut/publish. Complete the issue PR workflow
   and merge the accepted source into the default branch. Record any accepted
   limitations in both issue and PR. Do not mark incomplete criteria complete.
2. Change `VERSION` to the final number, such as `0.9.0`, and commit. Build that
   final package **locally** using the same checks, then verify its version,
   checksum, installation and smoke behavior. Changing `0.9.0-rc.N` to `0.9.0`
   changes the embedded version: an RC cannot become a final package merely by
   renaming the file. Code or dependency changes require another candidate cycle.
3. Once the final package is accepted, freeze its bytes. Publish that existing
   bundle; publication must not rebuild it locally or on GitHub Actions.
4. Follow [Publishing another version](README.md#publishing-another-version):
   verify that the public tag/release does not already exist remotely, add the
   exact receipt values to the release catalog, generate GitHub notes with an
   explicit previous **public final** tag, and verify the expected merged PRs.
   Create the public tag at the receipt's source commit and push only the intended
   branch and public tag. Preserve all published notes and artifacts.
5. Upload to a draft, verify the downloaded bytes against the accepted receipt,
   and publish only the verified draft. A partial upload stays a draft. Mark the
   newest accepted normal release Latest. Any explicitly requested public RC
   would instead be a GitHub prerelease, never Latest.

For releases distributed through the update remote, also follow the
[signed Flatpak repository procedure](flatpak-repository.md): prepare/sign the
Pages snapshot locally from the same accepted bundle, include its archive,
signature and checksum in the verified draft, then explicitly dispatch the
deployment-only workflow after publication. First-time signing/Pages setup is
required. Repository preparation and deployment never rebuild the application.

The catalog describes published releases. Local candidates stay in `dist/` and
local `test/*` tags; they do not consume public release numbers or require public
release notes. Gaps in RC numbers are harmless and preferable to reusing an identity.
