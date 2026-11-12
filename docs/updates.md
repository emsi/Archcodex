# Updates from GitHub Releases

Archcodex uses the same standalone `.flatpak` bundle as a manual download and
`flatpak install --user --or-update`. This replaces the Pages/OSTree repository
proposal in [#15](https://github.com/emsi/Archcodex/issues/15) and unmerged
[PR #16](https://github.com/emsi/Archcodex/pull/16); implementation is tracked in
[#17](https://github.com/emsi/Archcodex/issues/17).

## User workflow

- A Flatpak launch checks GitHub once, asynchronously. An available update appears
  in the status bar. Startup errors appear there too, with details on click.
- **Help → Check for Updates…**, also available in the command palette, checks
  explicitly and opens a nonmodal dialog. The dialog shows the running version,
  release notes as plain text, and a link to the release page.
- **Download and Install** is explicit consent to download that release and
  install it into this computer's existing per-user Flatpak. Codex servers and
  remote computers are not updated. Flatpak may download required runtimes.
- Continue working while downloading/installing. Cancel is available while
  checking/downloading. Once the Flatpak transaction starts, wait for it to finish
  before quitting; normal window close is deferred during this phase.
- After installation, **Restart Now** closes normally and reopens the same
  workspace, or **Later** keeps the running version until the next launch.
  Restoration follows **Restore opened terminals on startup**. Terminal output
  is not persisted; the existing in-TUI conversation-switch limitations apply.
- **Skip This Version** hides its startup indicator for this workspace. Manual
  checks still show it. A newer release can notify again. Disable startup checks
  under **Settings → Application**; manual checking remains available.

The first version with these controls is **0.10.0-rc.1**. Older versions still
need manual installation. Native builds can check releases but cannot install
them. System/named installations are detected and get an explanation and release
link; the updater does not create a per-user copy alongside them. There is no
privilege escalation or system installation support in this version.

Only GitHub's Latest stable release is considered. Drafts and prereleases are
rejected. Numeric version comparison understands `X.Y.Z-rc.N`: `0.10.0` can
replace `0.10.0-rc.1`, but `0.9.0` cannot. The installed deployment is also checked,
so an old running instance does not reinstall an already installed update.
While public Latest is older than this RC, checking correctly reports up to date.

## Implementation and boundaries

`Updater` owns one asynchronous `QProcess` at a time. The bundled `updates.py`
runs on the local host through the already permitted
`flatpak-spawn --watch-bus --host`, using `/usr/bin/python3` and `/usr/bin/flatpak`.
It exits after each operation. Python's standard library performs HTTPS and
SHA-256 verification; no extra Python packages, host libraries, Flatpak
permissions, GitHub credentials or release assets are required. There is no
recurring check timer or persistent update service.

Checks use the public GitHub API for `emsi/Archcodex`. Downloads allow HTTPS
redirects only to GitHub's API, site and release-asset domains, with the host's
certificate verification and proxy configuration. Requests are unauthenticated;
rate limits and network failures are reported and can be retried explicitly.
Metadata/checksum sizes, download size and operation duration are bounded.

Selection requires the exact architecture-specific bundle filename and release
URL. The helper uses GitHub's asset SHA-256 digest, falling back to the existing
`SHA256SUMS` for older releases. Before downloading it re-fetches the accepted
tag and compares asset ID, size, hash and URL. Changed/replaced assets require a
fresh check and acceptance. Bytes must match both size and SHA-256 before Flatpak
receives them. This trusts the project's GitHub account and HTTPS, like manual
installation; a checksum from the same account is not an independent publisher
signature. Flatpak performs its native bundle validation.

The running `/.flatpak-info` deployment path must match
`flatpak info --user --show-location` for the exact application, architecture and
stable ref. Installed commit/version are rechecked before the transaction.
Downloads use a private owner-only cache under the host's
`$XDG_CACHE_HOME/archcodex-updates`, with a lock across workspaces. Completed,
cancelled and failed downloads are removed; leftovers from a forced process kill
are removed on the next install while holding that lock. The private directory
and empty lock file remain for reuse. This lock coordinates Archcodex updaters;
Flatpak serializes its transactions with other installers.

Before invoking Flatpak, the helper waits for the GUI's final acknowledgement.
The GUI enters the non-cancellable state before sending it. A cancellation
racing the last downloaded bytes therefore cannot start an installation. The
actual command is:

```sh
/usr/bin/flatpak install --user --or-update --noninteractive --bundle PRIVATE_DOWNLOAD
```

An interrupted transaction or timeout is reported as an error; check the installed
version before retrying. No automatic rollback or retry of installation occurs.
Flatpak updates deployment atomically; already running instances keep their old
deployment. Required runtime/remotes must work noninteractively or the operation
fails with Flatpak's error and the manual release link remains available.

Explicit restart creates one bounded host handoff process. It identifies the
current Flatpak instance with `flatpak ps`, holds a Linux pidfd for its child PID,
and waits for that process to exit before executing `flatpak run --user` with the
same architecture, stable branch and data directory. This avoids a fixed delay,
PID reuse and racing the workspace lock. It exits after 30 seconds if shutdown
does not finish. Installation itself never closes terminal views or restarts the
application. Restart failure can always be recovered by launching normally.

## Validation

Python tests cover release/version selection, redirect restrictions, installation
scope, checksums, changed releases/deployments, acknowledgement, cache permissions
and cleanup. Qt tests cover process failures, cancellation, installation/restart
states, GUI responsiveness and the dialog; window tests cover the persisted
startup setting and command registration. Tests use isolated fixtures and do not
update the developer's normal installation.

Build receipts and desktop test instructions accompany each local candidate in
`dist/`. The retained SDK build container cannot run the real Flatpak sandbox;
full KDE/Flatpak host-command, runtime installation and restart acceptance must
be tested on the desktop host. A current/newer RC cannot exercise a real update
from an older public Latest release; the transaction tests use controlled fixtures.
