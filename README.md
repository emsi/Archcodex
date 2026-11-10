# Archcodex

Archcodex is a native Linux desktop application for working with Codex sessions.
Version **0.6.0** supports **existing local Unix-socket app-servers** and
**myCodex containers locally or on configured SSH hosts**. Direct TCP and generic
SSH socket profiles remain planned.

Its primary workflow is returning to **existing Codex sessions**. On startup it
discovers the current user’s host-visible local Codex servers and loads their
sessions. Projects groups local servers under **username @ hostname**, local
containers under **myCodex @ hostname**, and remote myCodex connections under
their own host groups. Short server/container names sit below these roots, with
compact project folders underneath. No project creation is needed; process IDs
remain confined to details and terminal headers retain their full context.
It provides All, Active, Running, Waiting and Opened views, conversation search
and the existing Codex TUI. Active includes live sessions at an idle prompt.
Exited terminal clients retain their output, remain in Active, and offer an explicit restart.
Restart rechecks the selected session and uses the current connection profile.
**Opened** lists all open terminal views, including retained exited/error output,
regardless of history visibility settings. **Ctrl+Tab** switches through recently
used terminals; **Ctrl+Shift+Tab** reverses direction. Hold Ctrl to cycle through
the list and release it to make the selected terminal most recent.
Right-click a session for **Fork**, **New** in the same directory, or **Close**
its terminal view. Folders offer **New** and **Expand All**; host/server nodes
offer **Expand All** for their branch. Activity transitions update recent-use
ordering immediately from server events.
**View → Hide unsaved sessions** is enabled by default and remembered; open
terminal entries remain accessible even when their conversation is unsaved.
**Settings → Configure Archcodex…** organizes preferences into Application and
Backends pages. myCodex lists only sessions in each container's WORKDIR and
subdirectories by default, in history/activity tabs and search. Enable **Show sessions outside
the container's working directory** under Backends to include other shared history.
Each view compresses its own folder paths. Running, Waiting and Opened open expanded;
waiting sessions use green indicators, running sessions blue.
Selecting a row preserves its status colour. A red retained-client error clears
when its terminal closes; the server's waiting state does not prove a successful reconnect.
Session and project indicators reflect **server events, without status
polling**. Multiple local and container servers connect independently.
Discovery issues appear in the status bar; click **Discovery limited** or use
**Connections → Discovery details** for explanations, technical details and
retry/add-connection/copy-report actions. Discovery never opens a popup on its own.

The adopted foundation is **C++20, Qt 6 Widgets, KDE Frameworks 6 and Konsole
KPart**, distributed primarily as a **Flatpak**. Linux is the only target; KDE on
Debian 13 and Ubuntu 26.04 or newer is the initial desktop baseline. X11 support
is required. This first package uses X11, including XWayland where available.

## Install and test

The local build produces `dist/Archcodex-0.6.0-x86_64.flatpak` and
`dist/SHA256SUMS`. Run these commands in your **host KDE terminal**, outside
myCodex or any other container:

```sh
sha256sum -c dist/SHA256SUMS
flatpak install --user --or-update ./dist/Archcodex-0.6.0-x86_64.flatpak
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
It does not read account credentials. Container discovery uses a separate Docker adapter; other users' host
processes remain outside host discovery. See [local discovery](docs/discovery.md).

Local myCodex containers are detected automatically. Use **Connections → myCodex
hosts…** to add an SSH alias, retry a connection or disable discovery. Docker CLI
access and Python 3 are required on each host; remote background connections need
an accepted host key and non-interactive authentication through host OpenSSH.
No myCodex update, container recreation or additional Flatpak permission is required.

The Codex TUI runs **on the desktop host**, using local clipboard/image input
and an explicit private relay endpoint. App-server, tools and workspaces remain
in the selected container. Observers and TUIs share one pipe-only transport per
host, with one Docker attachment per container and one SSH connection per remote
host profile. No remote listener or socket mount is created. The desktop needs
**Codex 0.166.0 or newer** in PATH and a **running systemd user manager** for
transport supervision. SSH keepalives use 15 seconds / three missed replies;
session status remains event-driven. See the [transport design](docs/container-transport-security.md).

A shared conversation appears as
**History only** unless Archcodex has observed it loaded in that execution
environment. Shared files alone never authorize resuming it there. Private
storage without explicit mode metadata is labeled **Custom storage**.
See [myCodex integration](docs/mycodex.md) for requirements and boundaries.

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
  roadmap. Local Unix and local/SSH myCodex are implemented; direct TCP and
  generic SSH socket profiles remain planned.
- [myCodex integration](docs/mycodex.md): agreed identity, discovery,
  responsiveness and shared-history requirements.
- [Local TUI connection plan](docs/local-tui-connections.md): desktop clients,
  shared transports, process counts and disconnect requirements; myCodex route implemented.
- [Container transport security](docs/container-transport-security.md): adopted
  shared helpers, socket exposure boundaries and lifecycle tradeoffs.
- [myCodex transport discussion](docs/mycodex-transport-options.md): volume/socket
  exposure and SSH forwarding compared with an optional container exec relay.
- [Flatpak design](docs/flatpak.md): runtime, terminal integration, host commands,
  proposed permissions and packaging checks.
- [Local discovery](docs/discovery.md): host discovery, namespace boundaries and event-driven updates.
- [Initial UI design](docs/ui.md): a simple IDE layout, host groups, expandable projects and
  sessions, and the five session views.
- [Implementation and testing](docs/implementation.md): what 0.6.0 delivers and
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
