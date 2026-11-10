# Local TUI connections: current plan

Updated: 2026-11-10. Status: adopted; the shared myCodex transport and desktop
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

Select the desktop Codex executable independently of the container executable.
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
