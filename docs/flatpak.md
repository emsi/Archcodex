# Flatpak distribution and host integration

Status: primary distribution direction adopted; permission baseline and host
integration below describe the full-project design. The
[0.5.0 implementation](implementation.md) retains the original host-command permission
set and a [buildable manifest](../packaging/io.github.emsi.Archcodex.json).
Last updated: 2026-11-09.

## 1. Packaging model

Distribute Archcodex as a Flatpak using the KDE Platform and KDE SDK. Build the
C++/Qt Widgets application and its required Konsole component against the same
runtime generation. Source releases and checksums are pinned in the manifest.

The [Konsole Flathub manifest](https://github.com/flathub/org.kde.konsole/blob/master/org.kde.konsole.json)
is the starting reference. On 2026-11-08 it selected KDE runtime **6.11** and
Konsole **26.08.2**, with DRI, IPC, network, Wayland, fallback X11 and host-command
D-Bus access. Version 0.2.0 adopts those runtime/component versions with the
narrower permissions documented in [implementation and testing](implementation.md).

Bundle the Konsole KPart and any dependencies missing from the chosen runtime.
Installing the separate Konsole Flatpak does not supply Archcodex's plugin. Do
not load a host-distribution Qt/KDE plugin into the Flatpak runtime. Verify plugin
discovery, supporting resources, profiles, translations and license notices in
the built artifact.

The host desktop baseline remains Debian 13 and Ubuntu 26.04 or newer. Host Qt
versions are not the application's library baseline; the Flatpak runtime is.
Versions 0.2.0–0.3.0 select KDE 6.11; an explicit upgrade policy remains to be defined.
A `.deb` is not a primary deliverable in this design.

Version 0.4.0 uses application ID `io.github.emsi.Archcodex` and GPL-3.0-or-later.
The distribution repository remains open. Flathub is a possible publication
destination; the local test bundle does not publish the application there.

## 2. Host execution is an explicit architectural boundary

The proposed initial package contains the GUI, its libraries, terminal component
and application metadata. It uses an installed **host Codex CLI** for terminal
clients and local socket relays, and **host OpenSSH** for SSH connections. Docker
and Python 3 are host dependencies for container discovery/integration. No new
myCodex command interface is required.

This keeps the GUI attached to the user's existing tools and SSH configuration.
It also means the first Flatpak is not a self-contained Codex installation. On
connect/open, identify a configured host executable and verify its capabilities.
Do not silently install or update it. Permit per-profile executable selection
when compatibility requires it; avoid assuming the sandbox's `PATH` describes
the host's tools.

Create one internal host-process abstraction with structured executable,
arguments, environment and file-descriptor handling. Prefer KDE's supported
`KSandbox` facilities for ordinary launches. Konsole already uses these facilities
for its Flatpak terminal path:
[Session.cpp](https://github.com/KDE/konsole/blob/master/src/session/Session.cpp).
Do not add a second `flatpak-spawn --host` wrapper blindly around a launch that
Konsole already sends to the host. If host-command access is unavailable, fail
with a clear connection error; do not silently launch a sandbox-local Codex
instance instead.

Use a non-PTY host process for protocol relays. Its stdout/stdin are transport
bytes and its stderr is diagnostic. Use the Konsole PTY path for the interactive
TUI. Handle process completion through exit notifications, not repeated process
queries. Terminal titles and Codex status come from application/protocol metadata;
do not enable terminal activity/silence tracking as a substitute.

The underlying
[Flatpak host-command interface](https://github.com/flatpak/flatpak/blob/main/data/org.freedesktop.Flatpak.xml)
accepts argument, environment and file-descriptor mappings and emits process exit
signals. Direct use is an option if a required operation cannot be expressed
correctly through KDE helpers. Exact process/PTY ownership and secret delivery
must be proved in the integration spike before choosing the final wrapper.

The host-command permission allows arbitrary execution as the user on the host.
Flatpak is therefore a distribution/runtime boundary here, not a security
boundary against Archcodex itself. Narrow direct filesystem grants still avoid
unnecessary mounts, but do not revoke the power of host execution. This follows
the trust model already used by Konsole's Flatpak; its
[permission rationale](https://github.com/flathub/org.kde.konsole/blob/master/README.md)
explicitly identifies host command execution.

## 3. Proposed permissions

Start from Konsole's manifest, retain permissions with a concrete need, and
validate the actual package before finalizing the manifest.

| Permission | Proposed treatment | Reason |
| --- | --- | --- |
| `--socket=x11` | Include in the initial X11 package. | X11 is mandatory; native Wayland is not an initial dependency. |
| `--share=ipc` | Include with the initial X11 configuration. | Supports the conventional X11 shared-memory path; verify with the selected Qt runtime. |
| `--share=network` | Include. | Direct TCP/TLS app-server connections. |
| `--talk-name=org.freedesktop.Flatpak` | Include. | Host Codex, OpenSSH and Docker command execution. |
| `--device=dri` | Retain as a provisional Konsole-derived rendering permission. | Validate its need with the chosen rendering/runtime configuration; it does not promise GPU terminal rendering. |
| `--socket=wayland` and `--socket=fallback-x11` | Alternative display configuration after native Wayland qualification. | Matches Konsole's current arrangement while preserving operation in an X11 desktop session. |
| `--talk-name=org.kde.StatusNotifierWatcher` | Only if a tray feature is selected. | Tray integration is pending UI design. |

Do not combine `x11` and `fallback-x11` expecting unrestricted X11 on a Wayland
desktop: fallback X11 is conditional. Start with the mandatory X11 path, then
choose the dual-display configuration deliberately. The
[Flatpak permissions reference](https://docs.flatpak.org/en/latest/sandbox-permissions.html)
describes these display and D-Bus rules.

Do not request blanket `--filesystem=host`, `--filesystem=home`, full session or
system bus access, all devices, direct Docker socket access, or an SSH agent
socket by default. Host OpenSSH uses the host's agent; a second sandbox SSH stack
is unnecessary. Use portals for ordinary document export/import and desktop
integration where supported.

Do not grant access to `~/.codex/auth.json` or copy it into sandbox storage.
Codex manages its own credentials in its execution environment. App-server
connection credentials are a separate concern and must not be confused with
Codex account credentials.

## 4. Connection paths inside the package

### Existing local Unix app-server

```text
Flatpak protocol client
  -> host command: codex app-server proxy --sock HOST_SOCKET_PATH
  -> host app-server Unix socket

Flatpak Konsole KPart
  -> host Codex TUI with explicit remote Unix endpoint and thread ID
  -> the same host app-server
```

The GUI need not mount the directory containing the host socket, nor the
project's source tree, just to speak the protocol. The host relay opens the
socket. The GUI must implement WebSocket framing over that relay; raw stdout is
not JSONL. See the [Codex proxy source](https://github.com/openai/codex/blob/main/codex-rs/stdio-to-uds/src/lib.rs).

Direct Unix-socket access can be an optional future route with a narrow path
grant. It is not the default design because arbitrary user-selected sockets
would otherwise demand changing filesystem grants. A file chooser portal is
not a general solution for Unix-socket access.

### Remote TCP app-server

The GUI uses its network permission to connect to the configured WebSocket/TLS
endpoint. Konsole launches the host TUI against the same endpoint. Both clients
must satisfy authentication and version compatibility. No local filesystem grant
is needed for the remote project's source directory.

### SSH-forwarded socket

The host OpenSSH process creates a private forwarded Unix socket in a host
runtime directory. The GUI reaches it through the host Codex proxy; the host TUI
uses it directly. This keeps SSH keys, aliases, known hosts and jump-host behavior
in the existing host OpenSSH environment. See [backend design](backends.md).

Runtime socket paths must use the host's runtime location, not a sandbox path
mistakenly sent to the host. Resolve this through the host boundary. Use short,
unique names within Unix-socket path length limits. Remove only owned resources
when a connection ends.

### myCodex

The provider runs packaged Python through the existing host-command boundary,
locally or over host OpenSSH. It discovers Docker containers and executes their
native Codex proxy without a PTY. A separate PTY runs the selected container's
TUI. Docker engine access belongs to the host user; no Docker socket is mounted
into the Flatpak. No new network, filesystem, SSH-agent or D-Bus grant is added.
No myCodex command family or host-installed helper is required. See
[myCodex integration](mycodex.md).

## 5. Process lifetime, credentials and application data

The app-server is outside the Flatpak's process lifetime. A sandbox-scoped
app-server or a foreground child killed with a host bridge would fail the
requirement that running tasks survive GUI exit.

Host relay, tunnel and TUI processes are client resources. Their cleanup must
not send cancellation or termination to the execution server. Test both normal
window close and abrupt sandbox termination, including process groups and PTY
hangup behavior. Konsole's standalone Flatpak working correctly is evidence for
the approach, not proof that every embedding path has the required semantics.

Pass secrets through a suitable environment/FD or credential mechanism, never
through logged launch strings or literal token arguments. The inspected
[KSandbox implementation](https://github.com/KDE/kcoreaddons/blob/master/src/lib/util/ksandbox.cpp)
serializes environment differences into `flatpak-spawn --env=...` arguments.
Injecting a token through that helper would therefore put it in the command
arguments. Use a supported FD or direct host-command environment path for secrets
and validate its integration with the terminal PTY. This remains an explicit
release gate; no credential mechanism is implemented by these documents.

Application profiles, SQLite metadata and caches live in the Flatpak's
application-specific XDG locations. Use Qt's path facilities instead of embedding
host paths. Source directories and Codex history remain in their owning backend
environment. Export/import should use portals. An export must not silently
include credentials or cached conversation bodies.

## 6. Packaging and integration gates

Before calling the package supported:

1. Build from pinned sources with `flatpak-builder`; verify all required plugins,
   assets and license notices are in the artifact. Confirm the intended license
   is compatible with Konsole's applicable GPL terms.
2. Launch from KDE on Debian 13 and Ubuntu 26.04 under X11 with the declared
   permissions and no broad local overrides. Treat Wayland as a separate matrix
   entry if offered.
3. Embed the packaged KPart and exercise Unicode, input methods, clipboard,
   selection, font changes, resize, scrolling, mouse reporting and sustained
   Codex TUI output.
4. Verify host executable discovery, working directories, PTY control, signals
   and process exit notification. Verify that credentials are absent from
   command arguments and ordinary logs.
5. Connect to multiple existing host sockets through non-TTY relays. Test
   partial I/O, backpressure and clean separation of stderr from protocol bytes.
6. Verify the TCP and SSH paths when their backend stages are reached, including
   authentication, private trust roots where configured and SSH prompts.
7. Trace a quiet connected session: no periodic status RPCs or process-based
   session-state inference. Measure CPU, memory and wakeups with 40–80 active
   backend sessions and only the user-opened terminals.
8. Close and forcibly terminate the GUI during active work. Reopen and reconnect
   to the same conversation, with the backend turn still running or completed.
9. Inspect permission use and remove inherited permissions that have no tested
   purpose. Document the host-execution trust model in release information.

The original documentation work created none of these runtime artifacts. The
later implementation adds the manifest, build tools and a local test bundle;
see [implementation and testing](implementation.md). No host service or public
publication is created.
