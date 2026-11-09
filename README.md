# Archcodex

Archcodex is a native Linux desktop application for working with Codex sessions.
Version **0.3.1** implements the first backend: **existing local Unix-socket
app-servers**. Remote TCP, SSH and myCodex are later stages.

Its primary workflow is returning to **existing Codex sessions**. On startup it
discovers the current user’s host-visible local Codex servers and loads their
sessions. Each server has its own root with a compact folder tree underneath; no
project creation is needed. Server names use configuration and user/host context,
with process IDs confined to details.
It provides All, Active, Running and Waiting views, conversation search
and the existing Codex TUI. Active includes live sessions at an idle prompt.
Exited terminal clients retain their output, remain in Active, and offer an explicit restart.
**View → Hide unsaved sessions** is enabled by default and remembered; open
terminal entries remain accessible even when their conversation is unsaved.
Each view compresses its own folder paths. Running and Waiting open expanded;
waiting sessions use green indicators, running sessions blue.
Session and project indicators reflect **server events, without status
polling**. Multiple local servers can be connected independently.

The adopted foundation is **C++20, Qt 6 Widgets, KDE Frameworks 6 and Konsole
KPart**, distributed primarily as a **Flatpak**. Linux is the only target; KDE on
Debian 13 and Ubuntu 26.04 or newer is the initial desktop baseline. X11 support
is required. This first package uses X11, including XWayland where available.

## Install and test

The local build produces `dist/Archcodex-0.3.1-x86_64.flatpak` and
`dist/SHA256SUMS`. Run these commands in your **host KDE terminal**, outside
myCodex or any other container:

```sh
sha256sum -c dist/SHA256SUMS
flatpak install --user --or-update ./dist/Archcodex-0.3.1-x86_64.flatpak
flatpak run io.github.emsi.Archcodex
```

The bundle declares the KDE 6.11 runtime and its Flathub runtime source. Flatpak
may prompt to download that runtime. The application also requires a compatible
**host Codex CLI** and an independently running app-server. Codex **0.166.0** was
used for protocol validation. Archcodex does not install or update Codex.

1. Start Codex normally on the host if it is not already running. Archcodex
   discovers existing local servers automatically; it does not start servers.
2. Expand a server and folder, then press Enter or double-click a session to attach
   its TUI. Identical directories on different servers remain separate.
3. **Discover** takes an explicit new snapshot, useful for nonstandard socket
   locations. Normal Codex socket creation is handled through filesystem events.
4. Search conversation text by entering a query and pressing Enter; choose all
   servers or the selected server/folder (including its descendants). **Load more history** retrieves older pages.
5. **Connections → Manage connections → Add** is an advanced fallback for custom
   endpoints. An empty socket path selects Codex's default. The optional **Session
   → New session in selected project** action is secondary to existing sessions.

Discovery requires **host Python 3** at `/usr/bin/python3`, using only its standard
library. It inspects same-user socket/process metadata and waits for Linux
inotify events; session state comes exclusively from app-server notifications.
It does not read account credentials. Private container sockets and other users'
processes are outside automatic discovery. See [local discovery](docs/discovery.md).

See [implementation and host testing](docs/implementation.md) for scope,
permissions, tested behavior and known limitations.

## Build

On a Linux machine with Flatpak, flatpak-builder and working user namespaces:

```sh
./scripts/build-flatpak.sh
```

The script uses a separate `.build/flatpak` installation for build runtimes. It
does not install Archcodex into your normal Flatpak installation. Source releases
and checksums are pinned in the [manifest](packaging/io.github.emsi.Archcodex.json).

Core tests can also be built with Qt 6 development packages, its SQLite driver,
Qt Test/WebSockets, Boost headers and Python 3:

```sh
cmake -S . -B build -G Ninja -DARCHCODEX_GUI=OFF -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The full native build additionally requires KDE Frameworks 6 and the Konsole
KPart with the lifecycle patch in `packaging/` at runtime. The Flatpak supplies
the matching components; an unpatched system KPart is rejected with an explicit
capability error.

## Design documents

- [Project design](docs/design.md): requirements, architecture, event handling,
  session lifetime, search, technology choices and validation.
- [Backend design](docs/backends.md): adapter contracts and the implementation
  sequence: existing local Unix sockets, remote TCP, SSH, then myCodex.
- [Flatpak design](docs/flatpak.md): runtime, terminal integration, host commands,
  proposed permissions and packaging checks.
- [Local discovery](docs/discovery.md): host discovery, namespace boundaries and event-driven updates.
- [Initial UI design](docs/ui.md): a simple IDE layout, expandable projects and
  sessions, and the four session filters.
- [Implementation and testing](docs/implementation.md): what 0.3.1 delivers and
  how to exercise it on your host.

## Project status

Initial implementation for hands-on testing. The design documents also describe
future stages; the implementation document identifies the current boundary.

The documents distinguish agreed requirements from proposed implementation
details and work that still needs validation. Upstream observations were checked
on **2026-11-09**; they are not a promise that every Codex version supports the
same protocol capabilities.

Application code is licensed under **GPL-3.0-or-later**; see [LICENSE](LICENSE).
Bundled components retain their own licenses. The Flatpak ID is
`io.github.emsi.Archcodex`.
