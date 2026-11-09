# Backend design

Status: target contracts for the adopted architecture. Local Unix and local/SSH myCodex adapters are
implemented in [0.4.0](implementation.md); direct TCP and generic SSH socket profiles remain planned.
Last updated: 2026-11-09.

## 1. Separation of responsibilities

A **backend** identifies an execution environment and an app-server endpoint. It
is not a terminal tab, a project directory, or a copy of conversation storage.
Several backends can be configured on one host, and a backend can contain many
projects and conversations. Each local server has a separate explorer root.
Container roots include the friendly container name and local/SSH host; the shared tree keeps paths and sessions scoped to the backend.

Keep three concerns separate:

| Layer | Responsibility |
| --- | --- |
| Backend adapter | Resolve an endpoint, establish its transport, describe its execution environment and construct terminal launch plans. |
| Shared Codex protocol client | Initialize, send RPCs, receive events and server requests, and translate supported protocol versions. |
| Session/project services | Maintain application models, recent use, attention and search across backends. |

The UI consumes the same model regardless of connection type. It must not contain
Docker commands, SSH forwarding logic, socket framing or myCodex-specific paths.
Initial adapters are independently testable internal C++ modules. A stable
third-party plugin ABI is unnecessary for the first release.

## 2. Common contract

The following describes responsibilities, not a finalized C++ header.

| Contract element | Required behavior |
| --- | --- |
| Profile | Persistent backend ID, display name, adapter kind, endpoint settings and non-secret credential references. |
| Execution identity | Host/environment identity and the endpoint currently reached; optional server identity when the protocol supplies one. |
| Connect/disconnect | Asynchronous, cancellable connection setup and transport disposal. Disconnect never means stop the app-server. |
| Transport | Reliable bidirectional byte stream or WebSocket connection; distinguish raw relays from direct stdio JSONL. |
| Capabilities | Record event, search, project, terminal and optional lifecycle support without inventing unsupported APIs. |
| Initial state | Shared protocol service reads inventory and current state after connect/reconnect. |
| Events | Deliver normalized events with backend ID and connection generation. No polling fallback. |
| Terminal launch plan | Executable, argument vector, environment/credential references, working directory, PTY requirements and explicit target thread/endpoint. |
| Optional operations | Explicit lifecycle or environment actions, available only where actually supported. |
| Errors | Separate permission, authentication, transport, incompatible-protocol and server failures. |

Profiles contain configuration, not live secrets. Paths belong to an explicit
environment. A local host path, a sandbox path, a remote path and a container path
must not be interchangeable string fields.

Connections to different sockets on the same machine remain separate backends.
Conversely, two profiles might be aliases for one server. Do not auto-merge them
solely because thread IDs or storage paths match. Shared history does not prove
shared running execution. Alias handling requires reliable identity evidence or
an explicit user association.

The common protocol service owns reconnect reconciliation and capability checks.
Transport adapters must not run their own periodic thread-list/status loop.

## 3. Stage one: existing local Unix sockets

Connect to one or more explicitly configured, already-running app-server Unix
sockets. Preserve support for custom paths and multiple Codex homes; the default
daemon path is a convenience, not an assumption baked into the model.

In a native development build, the protocol client can open a Unix stream and
perform the WebSocket handshake directly. In the Flatpak, use the host-command
boundary described in [Flatpak design](flatpak.md): launch the host's
`codex app-server proxy --sock PATH` without a PTY and carry the WebSocket exchange
over its pipes. An asynchronous stream adapter is needed for those pipes.

The relay does not provide protocol conversion or server lifecycle management.
Its stdout contains only transport bytes; stderr is diagnostic output. Partial
reads/writes, backpressure, EOF and cancellation must be handled correctly.
Reference: [Codex proxy implementation](https://github.com/openai/codex/blob/main/codex-rs/stdio-to-uds/src/lib.rs).

For terminal interaction, launch the host Codex TUI with an explicit
`unix://PATH` remote endpoint and the chosen thread ID. Use the supported CLI
argument ordering for the selected Codex version. A bare `codex resume` that may
resolve a different default daemon is not an acceptable substitute.

Attaching must not create a server, install a service, alter Codex configuration
or upgrade the executable. Connection errors should identify the supplied host
socket and distinguish missing path, denied access, no listener and incompatible
protocol where the underlying error makes that distinction possible.

Acceptance: two independent local servers, several projects each, a GUI and TUI
attached to the same active thread, event-driven status, GUI exit and successful
return to the same conversation.

## 4. Stage two: remote TCP endpoints

Here TCP carries the Codex WebSocket protocol: `ws://` or `wss://`, not an
unspecified newline-JSON socket. The GUI's protocol client connects directly from
the Flatpak. The host TUI connects to the same endpoint.

Profiles describe the URL, authentication mechanism supported by that server,
credential reference and TLS expectations. Non-loopback connections should use
authenticated TLS. Native server support and external TLS termination must be
distinguished; do not promise a native `wss` listener merely because clients can
connect to one. Do not expose a local server publicly as a connection shortcut.

The inspected Codex CLI supports WebSocket authentication options, while public
documentation still describes WebSocket support as experimental. Pin a tested
configuration and validate both GUI and TUI authentication before supporting a
server release. References:
[app-server documentation](https://learn.chatgpt.com/docs/app-server) and
[app-server command implementation](https://github.com/openai/codex/tree/main/codex-rs/app-server).

Use the system trust configuration appropriate to the runtime, with an explicit
profile mechanism for private trust roots if required. Never silently disable
certificate or hostname verification. Authentication failure is not a reason to
retry anonymously.

Loss of a GUI/TUI TCP connection must not stop an independently running server.
Apply the common reconnect/snapshot logic, and keep unrelated backends usable.

Acceptance: loopback development endpoint, authenticated remote endpoint, TLS
failure, rejected credentials, abrupt disconnect, reconnect during an active
turn and isolation from a slow backend.

## 5. Stage three: sockets over SSH

Use host OpenSSH rather than a separate embedded SSH implementation. Respect
the user's host aliases, SSH configuration, known hosts, agent and jump hosts.
Do not copy private keys into the Flatpak or change SSH configuration on attach.

The preferred route is native OpenSSH Unix-socket forwarding:

```text
GUI protocol client
  -> host non-PTY Codex proxy
  -> private local forwarded Unix socket
  -> OpenSSH connection
  -> existing remote Unix app-server socket

Host Codex TUI
  -> the same local forwarded Unix socket
  -> the same remote app-server
```

Use a private host runtime directory and a distinct socket for each connection
generation. Set restrictive socket permissions, request forwarding setup failure
reporting, and run the forwarding connection without a remote command or PTY.
Do not remove a pre-existing socket unless it is known to belong to this
connection. Disconnect cleanup removes only the application's own forwarding
resources.

OpenSSH supports local/remote Unix socket forwarding directly; no `socat` bridge
is needed for a host-visible remote socket. See
[ssh(1)](https://man.openbsd.org/ssh.1). Forwarding permissions on the SSH server
still apply. A path inside an unexposed container is not a host-visible socket;
that case belongs to the later myCodex adapter.

Treat the tunnel as transport, not as the server supervisor. An independently
running remote app-server continues after the tunnel closes. Reconnect establishes
a new tunnel and refreshes state once. Do not enable agent forwarding merely to
connect to Codex.

Host-key and interactive authentication handling must be designed deliberately:
preserve OpenSSH verification and support the user's existing agent/askpass
workflow where possible. An implementation spike must verify that authentication
prompts are usable from the packaged GUI without hanging the event loop.

Acceptance: an SSH alias, jump host, encrypted key through an agent, a changed
host key, rejected forwarding, paths requiring careful quoting, SSH loss during
a turn and return to the same conversation. The raw protocol channel never gets
a PTY; only the interactive TUI gets one.

## 6. Stage four: myCodex

The current implementation request brings myCodex forward ahead of the remaining
direct adapters. It composes native proxy transport and shared protocol machinery with container/environment discovery and
terminal launch behavior. Docker and myCodex dependencies must not leak into
other adapters or into the shared UI model.

The [agreed myCodex direction](mycodex.md) covers private/shared environments,
local and configured remote hosts, automatic container discovery, event-driven
updates, failure isolation and independent server lifetime. It separates host,
Docker engine, container, app-server and history-store identities.

A visible saved conversation is not proof that it can be restarted in the
container whose server returned it. Keep history visibility, actual live
execution and established resume context separate. Never select a container by
shared volume, thread ID or cwd alone. Existing saved-session filtering and a
successful metadata read do not establish execution eligibility.

The adopted route uses `docker exec -i` and native `codex app-server proxy`
inside the selected container. The TUI uses a separate `docker exec -it` there.
Remote containers use host OpenSSH with the same commands. The provider is owned
by Archcodex and requires no new public myCodex command. Published physical sockets
may later reuse local Unix or native SSH forwarding. See the
[transport comparison](mycodex-transport-options.md).

Discovery uses Docker lifecycle events plus endpoint events and initial/recovery
snapshots. List running myCodex containers even when they have no app-server;
report coverage limits. Use the host-command boundary and SSH as appropriate,
without mounting the Docker socket into the Flatpak. Do not turn discovery or
attachment into a pull, update, recreation, server start or history resume.

Acceptance: distinct execution contexts sharing the same history, unknown or
unavailable resume contexts, private/shared modes, zero/multiple servers,
container/socket lifecycle events, GUI responsiveness during remote failure,
explicit GUI/TUI endpoint targeting and continuity after GUI exit. The chosen
transport must additionally pass the qualification cases in the separate analysis.

## 7. Shared release requirements

Every adapter must demonstrate the same initial-state/event/reconnect contract,
correct backend identity, explicit endpoint targeting, cancellation of its own
transport work, and no normal-operation status polling.

Only supported features are exposed. Lack of native search can use the documented
indexing strategy; lack of status events cannot be hidden by periodic queries.
Unavailable optional lifecycle control must not prevent connection to an
already-running server.
