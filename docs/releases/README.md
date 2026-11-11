# Releases

Download the [latest release](https://github.com/emsi/Archcodex/releases/latest)
or browse [all releases](https://github.com/emsi/Archcodex/releases).
Each release has an x86_64 Flatpak, a `SHA256SUMS` file containing its basename,
and its own release notes. GitHub also provides source archives from the tag.
The versioned build manifest pins the dependency sources and includes the
Konsole patches needed to build the bundled component.

These 0.x versions are development builds for Linux host testing. GitHub's
Latest label identifies the recommended download; it does not mean every
desktop acceptance scenario has been qualified. Earlier versions remain
available for reference; the newest recommended release carries Latest.
Standalone GitHub bundles do not create a Flatpak update repository or a Flathub
listing.

## History

| Version | Changes |
| --- | --- |
| [0.8.1](v0.8.1.md) | Fix New-session rename, status and restoration with real conversation IDs |
| [0.8.0](v0.8.0.md) | Restore opened terminals, selection and recency, with documented TUI limits |
| [0.7.1](v0.7.1.md) | Restore terminal scrollback shortcuts with Konsole 26.04.3 |
| [0.7.0](v0.7.0.md) | Configurable keyboard navigation and deferred recent-terminal/project switching |
| [0.6.2](v0.6.2.md) | Independent conversation search and per-tab local filters |
| [0.6.1](v0.6.1.md) | myCodex startup discovery and local Codex executable lookup |
| [0.6.0](v0.6.0.md) | Local TUIs over shared myCodex transports |
| [0.5.1](v0.5.1.md) | Session context menus and event-driven activity ordering |
| [0.5.0](v0.5.0.md) | Host groups and Opened terminals |
| [0.4.1](v0.4.1.md) | Stable container identity, WORKDIR filtering and Settings |
| [0.4.0](v0.4.0.md) | Local and SSH myCodex integration |
| [0.3.2](v0.3.2.md) | Actionable, unobtrusive discovery diagnostics |
| [0.3.1](v0.3.1.md) | Per-filter compact paths, expansion and waiting indicators |
| [0.3.0](v0.3.0.md) | Separate server roots, compact folders and unsaved-session visibility |
| [0.2.1](v0.2.1.md) | Live-session filters and retained terminal output |
| [0.2.0](v0.2.0.md) | Automatic discovery of existing local sessions |
| [0.1.0](v0.1.0.md) | Initial native Linux workspace |

## Historical source and artifact verification

[catalog.json](catalog.json) records the exact source commit, source commit date,
previous tag, bundle filename/size/SHA-256 and Flatpak OSTree commit for each
version. Tags point at historical source commits, not the later documentation
commit that publishes this catalog. Release publication dates are the actual
publication dates, not backdated build dates.

Before publication, all 14 existing bundles were imported into a separate local
OSTree repository. The embedded application was run with `--version` using the
existing KDE runtime, without discovery or connections; its version and the
AppStream version matched the catalog. All effective Flatpak metadata matched
across the historical bundles. The six saved source archives for 0.1.0 through
0.3.2 matched their corresponding Git trees. Later associations use the version
commits, recorded build history and matching packaged versions; the saved 0.6.2
and 0.7.0 build records also identify their exact Git/OSTree commits and hashes.

This establishes the historical association and artifact identity, not a
reproducible-build attestation. The original bundles were not rebuilt or changed.
Full Flatpak sandbox launch remains unverified in the build container because
user namespaces are prohibited. Per-version notes link to the original test
history and preserve known limitations.

## Publishing another version

Use the [local candidate and final-release process](process.md) for version
allocation, build checks, receipts and test acceptance. The build and publication
steps are separate; a local test-build request does not authorize publication.

Issue fixes must first follow the dedicated-branch and PR-merge process in
[AGENTS.md](../../AGENTS.md). Publishing a release does not bypass that process.

1. Select the already built and accepted final-version bundle and its
   `BUILD-INFO.json` receipt. Ensure its hash, executable and AppStream version
   match. Publication uses these exact bytes and must not rebuild the package.
2. Add its catalog entry. Generate release notes using GitHub's
   `repos/emsi/Archcodex/releases/generate-notes` API, with explicit `tag_name`,
   `previous_tag_name` and the built source commit as `target_commitish`.
   Save the returned `body` verbatim in the version's notes file and use that
   same body for the GitHub release. Verify that the expected merged issue PRs
   appear; investigate missing PRs or an incorrect comparison range before
   publishing. Do not append lists of unchanged or unimplemented features.
   Installation, compatibility and implementation details belong in README
   and project documentation. This process applies to new releases; rewriting
   published notes requires an explicit request for that release.
3. Create the tag at the recorded source commit and push the required source
   commits and that tag. Never move an existing release tag to a different build.
4. Create a draft with `gh release create --draft --verify-tag --latest=false`,
   the explicit title, notes via `--notes-file`, and the Flatpak plus its own
   `SHA256SUMS`. Checksums use filenames relative to the download directory.
5. Verify the remote tag's peeled commit, draft body and exact asset set. Compare
   uploaded SHA-256 values with the catalog, and download the assets to check
   their bytes before publication. A partial upload stays a draft; retry only
   missing assets, without overwriting an existing different asset or body.
6. Publish the verified draft. Set Latest explicitly for the newest recommended
   normal release, and false for historical releases. A prerelease cannot be
   Latest. Recheck published downloads and the Latest endpoint afterward.
7. Verify that resolved issues were closed through their linked PR merges into
   the default branch, as required by [AGENTS.md](../../AGENTS.md). Both issue and
   PR must document decisions, divergences and accepted limitations. Publication
   does not replace the PR workflow or independently justify manual issue closure.

The 0.x releases are normal GitHub releases explicitly described as
development builds. Future prereleases should
use GitHub's prerelease flag when they should not replace the recommended build.
Publication always needs task authorization; this procedure is not an unattended
release workflow.

## Initial history publication — 2026-11-10

All 14 versions, v0.1.0 through v0.7.0, were published after draft verification.
Each has exactly two uploaded assets: its original Flatpak and `SHA256SUMS`.
All 28 uploaded files were downloaded and hashed before publication. Published
release bodies, asset IDs/digests and peeled tag commits were checked against
the catalog afterward. The v0.7.0 published assets were downloaded and checked
again, and GitHub's Latest endpoint resolved to v0.7.0 at that time.

The repository was private at the initial publication. Releases and
downloads inherit repository access; that publication did not change repository
visibility. No application tests were
rerun because this task changed only release documentation and distribution;
the artifact checks above supplement the version-specific recorded test history.

Commit `563e6e0` adds the notes, catalog and installation links. The source
branch and annotated historical tags were pushed atomically without force.
Publication receipts and verification downloads remain in ignored
`.build/release-audit/`; the application, existing bundles and desktop installation
were not modified.

## Decisions not agreed in advance

Publishing all historical bundles, matching tags, reviewed notes, checksums and
one Latest release was authorized. Supporting choices for this task are:

- Store the notes and a machine-readable provenance catalog in `docs/releases/`
  and link downloads from `README.md`; no CI publisher or new application code.
- Use annotated `v`-prefixed tags at the existing version commits and commit the
  publication documentation separately. Push the missing source commits and the
  documentation with a normal fast-forward; do not rewrite existing history.
- Keep the original bundles and source dates. Publish as development-labelled
  normal GitHub releases, with only v0.7.0 Latest. Do not backdate GitHub publication.
  Preserve the repository's existing private visibility and inherited release access.
- Upload exactly the Flatpak and per-release `SHA256SUMS`; GitHub's source archives
  come from the matching tag. No separate duplicate source assets or build-info
  assets are added. Existing local `dist/SHA256SUMS` remains unchanged.
- Use explicit reviewed notes instead of GitHub's sparse built-in output, with
  exact adjacent-tag comparisons and the first release's history link.
- Use local Flatpak/OSTree tools and GitHub CLI because the connector lacks
  release APIs. An initial Python GI probe was unavailable; no dependency was
  installed. Temporary audit imports, extracted executables, checksums, downloaded
  verification files and publication receipts stay under ignored
  `.build/release-audit/`. The normal desktop installation is untouched.
- Stage releases as drafts and verify tags, bodies and downloaded asset hashes
  before publishing. No release overwrite, forced tag, signing-policy change,
  issue comment, Flathub submission or remote machine configuration is included.
