# Archcodex

Archcodex is a native Linux desktop app for managing multiple Codex instances
and their sessions in one workspace. Connect to local Codex app-servers and
[myCodex](https://github.com/emsi/myCodex) containers on your computer or remote
hosts over SSH, browse their projects, and work in the existing Codex terminal
interface.

## Install from Flatpak

Download **`Archcodex-0.11.0-x86_64.flatpak`** and **`SHA256SUMS`** from the
[latest GitHub release](https://github.com/emsi/Archcodex/releases/latest) into
the same directory.

With Flatpak installed, quit any older Archcodex instance and run the following
in your **local host terminal**, outside myCodex or any other container:

```sh
sha256sum -c SHA256SUMS
flatpak install --user --or-update ./Archcodex-0.11.0-x86_64.flatpak
flatpak run io.github.emsi.Archcodex
```

The package targets **x86_64 Linux with X11 or XWayland**. KDE on Debian 13 and
Ubuntu 26.04 or newer is the initial desktop baseline. Flatpak may prompt to
download the KDE 6.11 runtime; matching host Qt/KDE libraries are not required.

To update an older version, download a newer release and repeat the installation
command with its filename. Starting with **0.10.0-rc.1**, Archcodex also checks
GitHub once at startup and offers **Help → Check for Updates…** (also in the
command palette). Click **Download and Install** to update the existing per-user
Flatpak, then restart when ready. **Settings → Application → Check for updates
on startup** controls the automatic check; installation always needs your approval.
These standalone bundles come directly from GitHub Releases.
See [update behavior and limitations](docs/updates.md) and
[all releases](https://github.com/emsi/Archcodex/releases) for notes and checksums.

### Requirements

- **Codex on the local computer** and an independently running app-server.
  Codex 0.166.0 was used for protocol validation; myCodex connections require a
  local Codex CLI version of **0.166.0 or newer**. Archcodex does not install
  Codex or start app-servers.
- **Host Python 3** at `/usr/bin/python3` for local discovery. No third-party
  Python packages are required.
- **Host Bash** at `/bin/bash` and GNU `env` at `/usr/bin/env` for terminal
  startup. Archcodex initializes Bash's interactive configuration before
  starting the local Codex TUI; see [external editors](docs/local-tui-connections.md#external-editors-and-terminal-startup).
- **For myCodex:** Docker access and Python 3 on each selected host, plus a
  running systemd user manager on the local computer. Remote hosts also need
  an accepted SSH host key and non-interactive authentication, such as an SSH
  agent.

## Remember opened terminals

Archcodex 0.8.0 remembers open terminal views, selection
and recent-terminal order. **Settings → Application → Restore opened terminals
on startup** is enabled by default. Offline views remain in **Opened**; select
one to reconnect or inspect **Terminal workspace…** in the status bar.
Previously exited or failed clients remain disconnected, with a short summary.
Terminal output itself is not saved across application restarts.

**Known limitation:** restoration uses the conversation opened from Archcodex.
Switching inside the Codex TUI with `/resume`, `/fork`, `/new` or agent navigation
can leave that saved target unchanged. `/rename` updates the bound conversation's
title. Archcodex's **New** command now creates a named, saved conversation on the
selected server and opens its exact ID, so it also supports rename, status and
restoration. Older **New session terminal** views with unknown IDs remain
disconnected placeholders; open their saved conversations from Projects.
See [workspace restoration](docs/workspace.md) for behavior, migration and the
planned protocol-observation option that requires no Codex changes.

## Features

- **Automatic discovery:** find existing local Codex servers and running
  myCodex containers without creating projects manually.
- **Organized sessions:** browse hosts, servers, compact project folders and
  conversations, with recent-use ordering.
- **Live status:** distinguish running and waiting sessions using app-server
  events, without status polling.
- **Desktop notifications:** open the right session from a Running → Waiting
  alert. Configure these in **Settings → Notifications**; see
  [notification behavior and compatibility](docs/notifications.md).
- **Embedded terminal:** use the Codex TUI in Konsole, with local clipboard and
  image input even when the workspace is in a remote myCodex container.
- **Search and filters:** search full conversations across connected servers,
  or filter live and opened sessions as you type.
- **Keyboard navigation:** use a command palette, configurable shortcuts and
  recent-terminal/project switchers.
- **Retained output:** keep exited or failed terminal views available to
  inspect and restart explicitly.

## Using Archcodex

### Connect and open a session

1. Start Codex normally on your local computer, or start your myCodex containers.
2. Launch Archcodex. Local servers and containers are discovered automatically.
3. For remote myCodex containers, open **Connections → myCodex hosts…** and add
   an SSH host alias from your existing SSH configuration.
4. Expand a host, server and project in **Projects**. Click once or press Enter on
   a session to open its terminal.

Projects are groups of existing working directories; no project setup is needed.
Local Codex, local myCodex and remote myCodex hosts have separate roots, and
identical paths on different servers remain separate.

Use **Discover** to refresh local discovery explicitly, or
**Connections → Manage connections → Add** for a custom local socket.
An empty socket path selects Codex's default endpoint.

Right-click a session for **Fork**, **New** in the same directory, or **Close**
its terminal view. Directories offer **New** and **Expand All**; other parent
nodes offer **Expand All**. Closing a terminal does not delete its conversation
history. An exited client retains its output and offers **Restart client**.

**New** uses the selected server's configuration and the clicked working
directory. It initially names the conversation **New session** to save even an
empty conversation before the TUI attaches; use `/rename` to change that name.
If creation loses its connection, check Projects after reconnecting before
trying again. Archcodex never automatically repeats a creation request.

### Choose a view and search

| View | What it shows |
| --- | --- |
| **All** | Saved and live sessions; search full conversations by pressing Enter. |
| **Active** | Live sessions, including idle prompts, plus retained exited/error terminals. |
| **Running** | Sessions currently executing a turn. |
| **Waiting** | Sessions at an input prompt or waiting for input or approval. |
| **Opened** | All open terminal views, including retained exited/error output. |

Each view keeps its own query and scope. In **All**, choose all servers or a
selected server/folder to scope conversation search. **Load more history**
retrieves older session entries.

In **Active**, **Running**, **Waiting** and **Opened**, typing filters cached
titles, working-directory paths and session IDs without making server search
requests. Each view compresses its own folder paths. Running, Waiting and Opened
start expanded; waiting indicators are green and running indicators are blue.

### Navigate with the keyboard

| Shortcut | Action |
| --- | --- |
| Ctrl+Shift+P | Open the command palette |
| Ctrl+Shift+F | Search terminal scrollback |
| Ctrl+Alt+P / Ctrl+Alt+F | Focus Projects / its search or filter field |
| Alt+1–5 | Select All / Active / Running / Waiting / Opened |
| Alt+0 | Return to the terminal |
| F6 / Shift+F6 | Switch pane |
| Ctrl+Tab / Ctrl+Shift+Tab | Cycle recent terminals |
| Ctrl+backtick / Ctrl+Shift+backtick | Cycle projects with open terminals |
| Ctrl+Shift+W | Close the current terminal |
| Ctrl+Shift+, | Open Settings |
| Menu / Shift+F10 | Open the selected project/session row's context menu |

For either recent-item switcher, keep Ctrl held to choose an entry, release it
to activate, or press Escape to cancel. Project cycling selects that project's
most recently activated open terminal.

Change bindings in **Settings → Configure Archcodex… → Keyboard Shortcuts**.

### Settings and connection details

**Settings → Configure Archcodex…** organizes preferences into Application,
Backends and Keyboard Shortcuts:

- **Hide unsaved sessions** is enabled by default and also available in the
  View menu. Open terminal views remain accessible.
- **myCodex visibility** defaults to each container's WORKDIR and subdirectories.
  Enable **Show sessions outside the container's working directory** under
  Backends to include other shared history. Visibility alone does not authorize
  resuming a session in a different container.
- **Local Codex executable**, under Application, lets you choose an absolute
  executable path when automatic discovery or the GUI's PATH finds the wrong
  version or none at all.

If discovery is incomplete, click **Discovery limited** in the status bar or
open **Connections → Discovery details**. The details include explanations,
technical information and retry/add-connection/copy-report actions. Use
**Connections → myCodex hosts…** to retry or disable a container host.

The Codex TUI runs on the **local computer running Archcodex**. For myCodex
connections, the app-server, tools and workspace stay in the selected container.
Transports are shared per host/container, and no remote listening socket is
created. A conversation marked **History only** cannot be resumed in a container
unless Archcodex has observed it loaded in that execution environment.
See [myCodex integration](docs/mycodex.md) for details.

### Current scope

The current source supports existing local Unix-socket
app-servers and local or SSH-hosted myCodex containers. Direct TCP connections and generic SSH forwarding
to non-container app-servers remain planned. Native Wayland is not yet qualified;
the Flatpak uses X11/XWayland.

Conversation search depends on the server's experimental search API. See
[implementation and host testing](docs/implementation.md) for compatibility,
permissions, validation and known limitations.

## Development

Archcodex uses **C++20, Qt 6 Widgets, KDE Frameworks 6, Konsole KPart,
Boost.Beast and SQLite**. Linux is the only target; the application UI contains
no JavaScript or TypeScript.

### Build a Flatpak

On a Linux machine with Git, Flatpak, flatpak-builder and working user namespaces:

```sh
git clone https://github.com/emsi/Archcodex.git
cd Archcodex
./scripts/build-flatpak.sh
```

The script keeps build runtimes in a separate `.build/flatpak` installation and
reads the committed `VERSION`, and produces the bundle, checksums and source
receipt directly in `dist/`, with versioned filenames. Builds require clean
committed source and an unused version; see the
[candidate and release process](docs/releases/process.md).
For the current x86_64 candidate:

```sh
version=$(cat VERSION)
cd dist
sha256sum -c "Archcodex-$version-x86_64.flatpak.sha256"
flatpak install --user --or-update "./Archcodex-$version-x86_64.flatpak"
flatpak run io.github.emsi.Archcodex
```

The build script itself does not install Archcodex into your normal user
installation. Dependency sources and hashes are pinned in the
[Flatpak manifest](packaging/io.github.emsi.Archcodex.json).

### Build and test the core

Install CMake 3.22 or newer, Ninja, a C++20 compiler, Qt 6 development packages
(Core, Network, SQL, Test and WebSockets), Qt's SQLite driver, Boost headers
and Python 3, then run:

```sh
cmake -S . -B build -G Ninja -DARCHCODEX_GUI=OFF -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

A full native GUI build additionally needs Qt Widgets and KDE Frameworks 6
CoreAddons, Parts, XmlGui and ConfigWidgets. At runtime it requires the Konsole
KPart with the patches in `packaging/`; an unpatched system component is rejected
with a capability error. The Flatpak supplies the matching components.

### Architecture and contributing

Read the implementation notes before changing connection or session behavior.
Session status must remain event-driven; backend work must not block the GUI.
Run the checks relevant to your change and use the documented host acceptance
steps for terminal, Docker, SSH or Flatpak integration changes.

- [Project design](docs/design.md): requirements, architecture and technology choices.
- [Backend design](docs/backends.md): adapter contracts and planned connection types.
- [Local discovery](docs/discovery.md): host discovery and namespace boundaries.
- [myCodex integration](docs/mycodex.md): discovery, identity and shared history.
- [Local TUI connections](docs/local-tui-connections.md): desktop clients and shared transports.
- [Container transport security](docs/container-transport-security.md): socket boundaries and cleanup.
- [Transport alternatives](docs/mycodex-transport-options.md): socket forwarding and container relays.
- [Flatpak design](docs/flatpak.md): packaging, runtime and host commands.
- [UI design](docs/ui.md): project navigation, session views and interaction behavior.
- [Implementation and testing](docs/implementation.md): delivered behavior and validation boundaries.
- [Release history and publishing](docs/releases/README.md): notes, provenance and publication procedure.

Design documents also describe future stages; their presence does not imply
that every proposed feature is implemented.

## License

Application code is licensed under **GPL-3.0-or-later**; see [LICENSE](LICENSE).
Bundled components retain their own licenses. The Flatpak ID is
`io.github.emsi.Archcodex`.
