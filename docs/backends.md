# Backend design

Status: target contracts for the adopted architecture. The local Unix adapter is
implemented in [0.3.0](implementation.md); TCP, SSH and myCodex remain planned.
Last updated: 2026-11-09.

## 1. Separation of responsibilities

A **backend** identifies an execution environment and an app-server endpoint. It
is not a terminal tab, a project directory, or a copy of conversation storage.
Several backends can be configured on one host, and a backend can contain many
projects and conversations. Each local server has a separate explorer root.
Future adapters will provide meaningful SSH host or myCodex container context
for those roots; the shared tree keeps paths and sessions scoped to the backend.

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

myCodex comes after the three direct connection adapters. It composes their
transport and protocol machinery with container/environment discovery and
terminal launch behavior. Docker and myCodex dependencies must not leak into
other adapters or into the shared UI model.

The current [myCodex repository](https://github.com/emsi/myCodex) provides a
persistent container workstation and tmux/byobu attachment. Its launcher
currently has human-readable information and interactive execution paths. Those
are useful for people, but a raw protocol relay needs machine-readable metadata
and a non-TTY stream. Relevant sources:
[launcher](https://github.com/emsi/myCodex/blob/master/bin/myCodex),
[Compose configuration](https://github.com/emsi/myCodex/blob/master/docker-compose.yaml).

Proposed small integration additions, to be designed and authorized separately:

| Proposed operation | Purpose and constraints |
| --- | --- |
| `info --json` | Versioned descriptor for the instance, execution user, workdir, storage identity, Codex version and reachable endpoint information. |
| `app-server proxy` | Non-TTY, bidirectional relay to the intended existing server; protocol bytes on stdout and diagnostics on stderr. |
| `app-server ensure --json`, if needed | Explicit lifecycle action that reports the resolved endpoint; never silently recreate a running container merely to attach. |

These names are proposals, not claims that the commands exist today. First
determine whether existing interfaces can meet the contract without changes.
Use the intended container user and Codex environment, not an accidental root
session from an unqualified container exec.

Shared Codex history and home volumes need particular care: two containers may
see the same files without sharing a usable daemon socket or process namespace.
Discover the actual execution owner. Do not identify an active backend only by
`CODEX_HOME`, and do not redesign authentication/state volumes as a side effect
of GUI integration.

Preserve myCodex's existing attach and image lifecycle semantics. The GUI should
not turn a connection into an implicit pull, update, recreation or restart.
Conversation persistence can rely on the independent app-server; preserving an
existing tmux screen is optional and is not required for the common backend
contract.

myCodex can be local or reached through SSH. The adapter must state which host
owns the launcher, which environment owns the socket, and where the TUI runs.
Use the host-command bridge for local launcher commands; do not mount the Docker
socket into the Flatpak just to discover instances.

Acceptance: distinct instances sharing history, private and shared state modes,
running-container attachment without mutation, correct execution user, protocol
relay without PTY corruption, remote instances and continuity after GUI exit.

## 7. Shared release requirements

Every adapter must demonstrate the same initial-state/event/reconnect contract,
correct backend identity, explicit endpoint targeting, cancellation of its own
transport work, and no normal-operation status polling.

Only supported features are exposed. Lack of native search can use the documented
indexing strategy; lack of status events cannot be hidden by periodic queries.
Unavailable optional lifecycle control must not prevent connection to an
already-running server.
