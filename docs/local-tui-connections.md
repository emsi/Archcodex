# Local TUI connections: current plan

Recorded: 2026-11-10. Status: agreed direction for future implementation, not
behavior delivered by 0.5.1. The current container TUI still runs inside its
container, over SSH for remote containers. See [current integration](mycodex.md).

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
  clients and owned by Archcodex.
- Container-private socket: selection remains open pending the security and
  lifecycle analysis. Do not automatically publish it or recreate a container.

A per-client `ssh -> docker exec -> codex app-server proxy` chain is compatible
with existing private sockets but is an expensive fallback, not the desired
architecture for every terminal. Forty opened remote TUIs using independent
chains could add about 120 connector roles (SSH client, Docker client and proxy
per TUI), excluding launch wrappers and SSH server processes. This is not a cost
imposed by 40 running conversations or by the app-server protocol.

Prefer a bounded number of shared transport helpers over per-terminal wrapper
stacks. The [container security analysis](container-transport-security.md)
compares deliberate socket exposure with shared helpers, including the option
of a container-lifetime service. It recommends evaluating a pipe-only helper
route; that recommendation is separate from this agreed plan. No helper protocol,
mount, remote listener or myCodex command has been selected for implementation.

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

Silent network partitions cannot always be distinguished from healthy idle
connections without transport liveness traffic. SSH keepalive policy, remote-side
cleanup deadlines and local systemd supervision were proposed but are not yet
selected defaults. They must be distinguished from session-status polling.

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

No implementation, container change or deployment is authorized by this design
record alone.
