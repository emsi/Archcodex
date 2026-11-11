# Signed Flatpak repository on GitHub Pages

Archcodex can publish a native Flatpak update repository from this GitHub
repository using a deployment-only Pages workflow. The intended address is
`https://emsi.github.io/Archcodex/flatpak/`. This tooling does not make that
address live: the initial signing configuration and deployment are release
operations. Until those are completed, use the existing GitHub Release bundles.

## Publication model

1. Build and accept a final Flatpak **locally**, following [the release
   process](process.md). Its versioned build receipt identifies the exact bytes.
2. Locally import that accepted bundle, sign its OSTree commit, generate and
   sign AppStream/summary metadata and static deltas, and prepare a signed Pages
   archive. Import/signing does not change the bundle or its OSTree commit ID.
3. Include that archive and signature in the same GitHub Release as the bundle.
4. Explicitly dispatch the Pages workflow with the final release tag and the
   archive's locally verified SHA-256. It downloads, verifies and deploys those
   files. GitHub does not compile the application, generate the Flatpak
   repository, or hold a private signing key.

This uses the existing repository and an Actions artifact, with no `gh-pages`
branch or package files in Git. Releases retain standalone bundle downloads.
The Pages remote enables normal `flatpak update` and desktop software-manager
updates. An Archcodex update dialog/check-on-startup is separate work.

## One-time release-operator setup

Local tools: Python **3.11+**, Flatpak, OSTree, GnuPG, and GitHub CLI. The
deployment runner only needs Python, GnuPG and GitHub CLI, already available on
the selected Ubuntu 24.04 runner. There is no new application/runtime dependency
or sandbox permission.

Choose a maintained **dedicated OpenPGP release-signing key**, store its private
material and backups locally, and record its complete 40-character **primary**
fingerprint. A signing subkey under that primary key is supported. This key is
separate from any SSH Git commit-signing key. Use normal GnuPG pinentry/agent
handling for protected private keys; never put passphrases/private keys in this
repository, workflow inputs, release assets or Actions secrets.

When initial publication is authorized:

- In repository **Settings → Pages**, select **GitHub Actions** as the source.
- Restrict the `github-pages` deployment environment to the default branch.
- Set the repository Actions variable `FLATPAK_SIGNING_FINGERPRINT` to the
  verified full primary fingerprint. It is public configuration, not a secret.
- Publish that fingerprint in reviewed installation documentation before
  inviting users to trust the remote. The `.flatpakrepo` embeds the public key;
  the workflow checks it against this independent repository variable.

The workflow fails if the fingerprint is missing or wrong. Creating an issue,
building a candidate, or merging the tooling does not configure Pages or
authorize the first deployment.

## Prepare a final repository snapshot locally

Replace `VERSION`, `PRIMARY_FINGERPRINT` and `/path/to/release-gnupg` with the
accepted final version and local signing identity. Do not use an RC version.

```sh
python3 scripts/flatpak_repository.py prepare \
  --bundle dist/Archcodex-VERSION-x86_64.flatpak \
  --receipt dist/Archcodex-VERSION-x86_64.BUILD-INFO.json \
  --fingerprint PRIMARY_FINGERPRINT \
  --gpg-homedir /path/to/release-gnupg
```

**For every later publication**, also supply:

```text
--previous dist/Archcodex-PREVIOUS_VERSION-x86_64.pages.tar.gz
```

Keep/download that previous archive and its adjacent `.asc` signature from the
last deployed release. The tool verifies its signature, URL and version before
extending it. Previous objects are retained so clients with an older cached
summary can finish fetching them. The deployment checks that this is the
actual predecessor currently on Pages, or a retry with the same signed publication
manifest (build identity, URL, signing fingerprint and predecessor).
It rejects rollback, missing history and conflicting redeployments.

The command validates the bundle's size/hash/ref, imported commit, embedded
AppStream version and runtime against its receipt, and performs a native signed
OSTree pull into an isolated repository. The receipt comes from the existing
accepted-build process; preparing a repository does not replace desktop testing.
It writes only these new files, directly in `dist/`:

- `Archcodex-VERSION-x86_64.pages.tar.gz`
- `Archcodex-VERSION-x86_64.pages.tar.gz.asc`
- `Archcodex-VERSION-x86_64.pages.tar.gz.sha256`

Existing outputs are never overwritten. Preparation uses a temporary directory
and removes it afterward. A failed final filesystem copy can leave partial
output files; inspect and remove only undistributed outputs from that attempt
before retrying. Preserve the accepted bundle and its receipt in all cases.

The archive includes the `flatpak/` repository, public key,
`archcodex.flatpakrepo`, `archcodex.flatpakref`, a small installation landing
page and `publication.json` containing the build receipt and predecessor hash.
Only the public key is exported. The complete archive has a detached signature
as well as the native repository signatures that Flatpak clients verify.

Verify into a new staging directory before publication:

```sh
python3 scripts/flatpak_repository.py verify \
  --archive dist/Archcodex-VERSION-x86_64.pages.tar.gz \
  --signature dist/Archcodex-VERSION-x86_64.pages.tar.gz.asc \
  --fingerprint PRIMARY_FINGERPRINT \
  --output .build/pages-review
```

`verify` checks the archive signature, pinned key, repository identity and safe
archive structure before extracting files. `prepare` additionally validates the
native Flatpak repository. Neither command publishes, changes the normal user's
Flatpak installation, nor executes application code from the archive.

## Publish and deploy

Follow the existing [public-release procedure](README.md#publishing-another-version),
including a public tag at the receipt's source commit and GitHub-generated notes.
For a release intended for the update remote, upload the three `.pages.tar.gz*`
files alongside the bundle and checksum to its draft. Download and verify those
assets before publishing the draft. Do not replace any previously published
release's assets or notes implicitly.

After the verified final release is public and marked **Latest**, dispatch:

```sh
gh workflow run flatpak-pages.yml --ref master \
  -f release=vVERSION -f sha256=LOCALLY_VERIFIED_ARCHIVE_SHA256
```

For the explicitly authorized **first** publication, add `-f bootstrap=true`.
It defaults to false; an unexpected missing site must not silently reset history.

The workflow must already be merged into the default branch. It checks that the
release is final, public, Latest, has the expected assets, and that the peeled
public tag identifies the build receipt's exact source commit. It compares the
downloaded bundle with that receipt, verifies the signed Pages archive against
both the supplied checksum and pinned key, and checks the deployed predecessor.
Unexpected HTTP/network errors fail deployment; only a 404 with the explicit
bootstrap flag permits initial publication when the payload also declares no
predecessor.

Verification has read-only repository access. Only the separate deployment job
receives `pages: write` and `id-token: write`. Actions are pinned to commits;
release inputs are passed as quoted environment variables. The entire workflow
is serialized with cancellation disabled. GitHub can replace a pending run with
a newer pending run; inspect run status and redispatch the current Latest if
needed. There is no build job or signing secret in Actions.

After success, check the served `publication.json`, add the remote in an isolated
installation, verify the advertised commit and test install/update on the KDE
desktop. A local fixture test is not proof of a successful real Pages deployment
or sandboxed desktop launch.

## User installation and migration once Pages is live

New users can open the `.flatpakref` in their software manager, or run:

```sh
flatpak install --user https://emsi.github.io/Archcodex/archcodex.flatpakref
```

It specifies the KDE runtime's Flathub repository. To explicitly configure the
remote (including for existing bundle installations):

```sh
flatpak remote-add --user --if-not-exists archcodex \
  https://emsi.github.io/Archcodex/archcodex.flatpakrepo
flatpak install --user --reinstall archcodex io.github.emsi.Archcodex//stable
flatpak info --user --show-origin io.github.emsi.Archcodex
```

The last command should print `archcodex`. Adding a remote alone does not migrate
an existing bundle's origin. Reinstallation preserves application data; it does
not require uninstalling or deleting settings. Quit Archcodex first and confirm
the version afterward. A local RC can be newer than the public stable release;
deliberate migration replaces it with the public version. Keep using the local
bundle while testing that RC if this is not intended. Existing unused bundle
origins can be reviewed separately; the tooling does not remove other remotes.

Later updates use:

```sh
flatpak update --user io.github.emsi.Archcodex
```

A system-wide installation uses `--system` consistently instead of `--user`.
The remote supplies only Archcodex; it does not mirror the KDE runtime. An
already running application continues running its old deployment until restarted.

## Operational boundaries and recovery

- **Current architecture/channel:** x86_64, existing application ID and `stable`
  ref. RCs remain local test bundles. Additional architectures/channels require
  an explicit extension, not silently repurposing stable.
- **Retention:** complete successive snapshots retain previous repository objects.
  Full static deltas accelerate current installs. Archives and expanded sites
  must remain below 900 MiB and 100,000 entries; preparation/verification reject
  larger payloads. This leaves margin under Pages' 1 GB site limit. No scheduled
  pruning service is introduced. Assess storage/hosting or a deliberate retention
  policy before reaching the cap. GitHub Releases retain the frozen snapshots.
- **Failure:** verification failure leaves the live site alone. Interrupted or
  failed deployment can be retried with the same tag/checksum. If a different
  predecessor or a newer deployment is detected, investigate; do not override the
  guard. Fixing a bad published application uses a newer final release. Existing
  local bundles remain available for explicit per-machine recovery.
- **Key continuity:** key rotation, including introducing a new signing subkey,
  needs a reviewed overlap/key-distribution procedure for existing installations;
  this tooling does not automate it. A new primary key is deliberately rejected
  by the previous-archive check. Changing only the workflow fingerprint or the
  downloadable descriptor is not a client key migration.
- **Hosting:** Pages replaces the whole site, so this deployment owns the project's
  Pages contents. Do not add another independent Pages publisher. CLI `--url`
  permits another HTTPS host ending in `/flatpak/`; adopting one also requires
  updating the workflow expectation and migrating installed remotes.
- **Updates:** repository hosting permits native Flatpak updates. It does not
  implement an application update prompt, force automatic desktop updates,
  modify session notification behavior, or add a background worker to Archcodex.

References: [Flatpak repository hosting](https://docs.flatpak.org/en/latest/hosting-a-repository.html),
[custom Pages workflows](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages),
[Pages limits](https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits).

## Tests

```sh
python3 -B tests/test_release.py
python3 -B tests/test_flatpak_repository.py -v
```

The repository suite includes real signed fixture bundles, an HTTP-served update,
bundle-origin migration, tampering rejection, publication chain checks and mocked
GitHub release validation. It uses temporary keys, installations and a private
D-Bus session, downloads no runtime and builds no GUI. Native integration tests
are skipped if Flatpak/OSTree/GnuPG are absent; install those tools plus
`dbus-run-session` when validating publication changes. Full desktop/Pages
acceptance remains a release-operator check.
