# Local TUI connections: current plan

Updated: 2026-11-11. Status: adopted; the shared myCodex transport and desktop
TUI launch are implemented in 0.6.0. Generic SSH socket profiles and direct TCP
remain future adapters. See [current integration](mycodex.md) and the
[security design](container-transport-security.md).

## Agreed connection model

Run the Codex TUI on the desktop host, inside the existing Konsole view. Keep the
selected app-server, its credentials, workspace and agent tool execution in the
original host or container. Always supply the explicit selected endpoint; never
fall back to the desktop's default server after a connection failure.

Keep these quantities separate:

| Quantity | Relationship |
| --- | --- |
| Sessions/threads | Many conversations can belong to one app-server. |
| Protocol connections | One client connection can address many threads; multiple clients can connect to the same listening socket. |
| TUI processes | Each independently opened terminal runs its own client. Discovering a session does not launch a TUI. |
| SSH transports | Native forwarding can carry many independent client streams and multiple server sockets through one SSH connection. |
| Helpers | Their count should follow actual transport boundaries, not the number of listed sessions. |

Preserve one Archcodex observer connection per backend, receiving events for its
sessions. Separate native TUIs keep independent protocol connections. Do not
merge their initialized JSON-RPC streams into the observer connection or build a
protocol broker merely to reduce SSH connections.

```text
Archcodex observer ---- connection A ---+
Desktop Codex TUI 1 --- connection B ---+--- shared SSH transport --- app-server
Desktop Codex TUI 2 --- connection C ---+
```

For one remote app-server with 40 sessions and three opened TUIs, the target is
one observer plus three TUI connections carried by one SSH transport. Even 40
opened TUIs do not inherently require 40 SSH connections. Existing discovery,
Flatpak launch and observer bridge processes are separate overhead, not included
in this example. Multiple containers remain distinct execution environments even
when their conversation files or IDs overlap.

## Transport selection

- Existing local host socket: direct access for the host TUI.
- Remote host-visible socket: native OpenSSH Unix-socket forwarding, shared by
  clients and owned by Archcodex; planned adapter, not yet implemented.
- Container-private socket: one shared, pipe-only helper attachment per container
  and runtime user. No remote listener, published socket or recreated container.

A per-client `ssh -> docker exec -> codex app-server proxy` chain is compatible
with existing private sockets but is an expensive fallback, not the desired
architecture for every terminal. Forty opened remote TUIs using independent
chains could add about 120 connector roles (SSH client, Docker client and proxy
per TUI), excluding launch wrappers and SSH server processes. This is not a cost
imposed by 40 running conversations or by the app-server protocol.

The adopted route extends the existing event-driven discovery helpers with raw
stream multiplexing. One desktop transport helper owns the local endpoints; one
foreground SSH command carries all remote container traffic for a host profile.
One Docker exec attachment per container carries discovery, observers and TUIs.
Local containers omit SSH and merge the desktop/host roles. The protocol carries
independent WebSocket bytes, without merging Codex requests or credentials.
The [security design](container-transport-security.md) documents the alternatives,
limits and cleanup contract. No myCodex command, image change or mount is needed.

The agreed access boundary trusts the selected desktop and SSH/host user while
excluding other users, ordinary containers and network clients. Preserve Codex's
existing restrictions on sandboxed tools as well as this account boundary.

## Desktop and server responsibilities

Select the local computer's Codex executable independently of the container
executable. In 0.6.1 an optional Application setting overrides automatic live
local discovery, with local PATH as the final fallback. “Desktop” in the diagrams
means this local computer, not the selected SSH host.
Do not assign the remote `CODEX_HOME` to the local process or copy credentials.
Resume the exact thread on the validated backend. New-session working-directory
paths refer to the remote environment; do not resolve or change into them on the
desktop. Retain the existing server-side Fork operation and validate that local
client defaults do not override saved remote settings unexpectedly.

Codex 0.166.0 source enables local-image serialization for explicit remote
endpoints, including Unix sockets. The local TUI reads the desktop clipboard and
transfers image contents through the protocol. No SCP upload service, shared
temporary directory, remote X11 forwarding or automatic workspace sync is planned.
Actual desktop/client/server compatibility still requires testing.

## External editors and terminal startup

Since 0.9.0 ([issue #7](https://github.com/emsi/Archcodex/issues/7)), every terminal
client starts on the **local computer** through `/usr/bin/env`,
then `/bin/bash -ic`, then the selected Codex executable or transport client.
This applies to direct and myCodex connections, including New, resume, reconnect
and workspace restoration. The server endpoint, thread ID and remote working
directory keep their existing meanings.

Existing `EDITOR` and `VISUAL` values in Archcodex are passed as literal arguments
to `env`, before Bash starts. This avoids KDE's Flatpak helper omitting overrides
that match the application's own environment. Unset variables retain ordinary
host inheritance; explicitly empty values remain empty. Bash then reads its
normal interactive startup files, including `~/.bashrc`, which may set or override
those values. For example, export `EDITOR='kate --block'` in the local host's
`.bashrc`. Choose an editor command that waits for editing to finish. Codex still
selects the editor: `VISUAL` takes precedence over `EDITOR`, including an empty
`VISUAL`. An export made only in a different terminal cannot be recovered unless
it also reaches Archcodex, the host launch environment or Bash's startup files.

Bash is required at `/bin/bash`, regardless of `$SHELL` or the account's default
shell. This is an interactive, non-login invocation: login-only configuration is
not added. Startup files run normally and can affect launch time or environment;
Archcodex neither parses nor edits them. It does not forward arbitrary variables
or rewrite `PATH`, display variables, `HOME` or `CODEX_HOME`. The editor runs on
the local computer even for remote myCodex sessions.

The bundled Konsole component exposes `startProgramLiteral`, preserving argv
without its usual environment-variable expansion and disabling shell fallback
for that call. Existing callers of Konsole's ordinary `startProgram` keep their
behavior. A missing launcher, Bash or Codex produces a failed terminal with
retained output; it never opens a different shell. Archcodex rejects an older
component missing this API with a reinstall message.

`env` and Bash replace themselves using `exec`; the relay-backed client also
executes Codex in place. There is no persistent additional launcher process.
No Flatpak permission change is needed.

Regression coverage includes real PTYs with the patched KPart, interactive
startup overrides, unset/empty/literal editor values, both client routes and
terminal lifecycle paths. The KDE host-context fixture checks identical-value
filtering against the actual CoreAddons library with absent/stale helper values.
It does not replace the remaining installed Flatpak desktop acceptance check:
open the chosen editor from Codex, save and return, cancel/fail and recover, and
verify clipboard/image input plus backend routing on the target KDE desktop.

## Ownership, disconnect and recovery requirements

- All connection establishment, forwarding, discovery and cleanup stay off the
  GUI thread, with bounded concurrency, cancellation and buffering.
- Status remains event-driven. Initial/reconnect inventory is allowed; periodic
  session queries and terminal-activity heuristics are not.
- Closing a terminal releases its client and stream, not the app-server or its
  running work. Retained terminal output must not retain dead connectors.
- Shared transports remain only while needed by consumers. GUI exit must release
  its owned clients, helpers, listeners and SSH connections.
- EOF and signals initiate cleanup, followed by a bounded grace period and
  termination of remaining owned children. Signals alone do not establish
  ownership across SSH or Docker.
- A new container lifetime/execution identity invalidates old endpoints. Never
  silently reconnect to another container sharing the same history.
- Coordinate transport recovery; let the native TUI handle protocol recovery.
  Do not replay prompts or other operations whose result is uncertain.
- Use private, application-owned runtime paths. Remove only owned relay sockets,
  never the backend's original socket. Do not use detached tunnels or take
  ownership of the user's unrelated SSH master.

The approved SSH default is `ServerAliveInterval=15` and
`ServerAliveCountMax=3`: roughly 45 seconds to detect a silent connection loss
from the desktop. These are encrypted SSH transport checks, not Codex queries.
No SSH-server configuration is changed: a partition can leave the remote side
alive until its own SSH/TCP failure detection. Immediate remote cleanup cannot
be guaranteed by client keepalives. EOF on either side closes the owned stream
and its helpers. A transient desktop systemd user service owns the helper group
and runs socket cleanup even after the helper is killed.

## Validation and known limits

Source inspection is not an end-to-end qualification. Before delivery, test KDE
X11/Flatpak clipboard and image paste; remote New/Resume/Fork settings; multiple
TUIs and servers sharing a transport; slow links; client/server mismatch;
container replacement; GUI/helper/SSH crashes; silent partitions; and repeated
reconnects without accumulating processes or listening sockets. Accepted remote
work must survive client shutdown. Test the relevant runtime boundaries rather
than only normal window closure.

The inspected Codex 0.166.0 proxy waits for both copy directions to finish;
server EOF can leave it waiting for stdin. Do not assume this proxy guarantees
prompt disconnect propagation. Docker exec stdin closure also does not promise
termination of arbitrary container programs. These findings must inform the
private-container transport choice.

## Evidence

- [App-server protocol](https://learn.chatgpt.com/docs/app-server): connection
  initialization, thread IDs, subscriptions and remote endpoints.
- [Codex 0.166.0 remote client](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/app-server-client/src/remote.rs): independent client connections.
- [Image submission](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/tui/src/chatwidget/image_submission.rs)
  and [remote startup](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/tui/src/app/startup.rs): local image serialization.
- [Codex proxy](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/stdio-to-uds/src/lib.rs): byte relay and EOF behavior.
- [Docker 29.1.3 exec handler](https://github.com/moby/moby/blob/docker-v29.1.3/daemon/server/router/container/exec.go): execution lifecycle is not the local client's process hierarchy.
- [OpenSSH forwarding](https://man.openbsd.org/ssh.1): multiple forwarded streams through one SSH transport.

Implementation was explicitly authorized after the initial design review. No
container changes, SSH-server configuration changes or public deployment are
part of this implementation.
