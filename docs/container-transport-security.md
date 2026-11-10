# Container transport: security and helper analysis

Analyzed: 2026-11-10 against Archcodex `b52aaff`, Codex `rust-v0.166.0`,
Docker `docker-v29.1.3` source and current myCodex Compose configuration.
Status: recommendation for discussion, not an adopted implementation. The
[connection plan](local-tui-connections.md) records the agreed direction.
No containers, mounts, listeners, credentials or SSH settings were changed.

## 1. Agreed trust boundary

The user explicitly selected: trust programs running as the selected desktop
and SSH/host user; exclude other users, other ordinary containers and network
clients. Do not claim isolation from host root, privileged containers with host
access, or accounts controlling the Docker daemon. Those actors already have
authority outside this transport; do not grant it to new actors.

An app-server connection is a powerful control capability, not a read-only
status feed or a per-thread authorization boundary. WORKDIR filtering and
execution-provenance checks remain necessary but do not restrict what an
authorized native client can request from the server.

Owner-only Unix sockets fit the selected user boundary. Still preserve Codex's
existing restrictions on sandboxed tools, including tools running under the same
UID. Do not expose a protected control socket through a project, shared home or
sandbox-visible alias.

## 2. Existing helpers can be reused

`src/containers.py` already runs one host inventory helper per discovery host,
uses Docker events, and launches one `docker exec -i` endpoint-discovery helper
inside each observed container. `src/discover.py` waits on inotify and stdin;
input closure ends it. These are connection-scoped foreground processes.

A shared transport can evolve those host/container discovery helpers rather
than add a second set of persistent processes. This is a proposal, not a claim
that today's discovery protocol already carries TUI connections.

In Codex 0.166.0, the advertised socket is a symlink to a listener under
`/tmp/codex-daemon-<uid>`. Sharing the home does not expose that listener.
Publishing its physical directory also exposes the other sockets/locks there
to the authorized host user and requires mount, UID and lifetime qualification.
Shared history does not establish shared execution identity.

## 3. Alternatives

| Route | Process scaling | Exposure and complexity | Assessment |
| --- | --- | --- | --- |
| Publish the physical socket directory; native SSH forwarding | No per-client exec relay | Deliberate mount of privileged socket state; UID mapping, other sockets, lifetime and sandbox aliases need qualification | Efficient when explicitly provisioned, not the default for existing private containers |
| Docker exec proxy per client | Grows with opened clients and observers | No new remote listener, but many processes and difficult EOF ownership | Compatibility route |
| Shared helpers over SSH and Docker stdio | Grows with attached containers | No new remote listener or mount; needs bounded stream multiplexing | Recommended candidate |
| Host-private gateway sockets plus native SSH forwarding | Shared host/container helpers | Avoids custom multiplexing over SSH but still needs it through Docker; introduces remote socket paths and cleanup | Viable alternative |
| SSH daemon inside each container | Extra daemon/client/authentication machinery | Host keys, another server and image integration | Unnecessary for an existing private socket |
| Merge native clients into one JSON-RPC connection | Fewer app-server connections | Must virtualize initialization, IDs, subscriptions, approvals and recovery | Avoid; solves the wrong problem |

Do not use `/proc/<container-pid>/root`, relaxed `/var/lib/docker` permissions,
privileged namespace entry, unauthenticated TCP or a Docker socket mounted into
the container to avoid a small helper.

## 4. Recommended candidate: pipe-only remote transport

```text
Desktop
  Archcodex observer / independent local TUIs
       | independent WebSocket byte streams
  private local Unix endpoints + shared local transport helper
       | one framed stream through one foreground SSH command per host profile
Remote Docker host
  host helper (extend existing container inventory helper)
       | one docker exec -i stdio connection per attached container/runtime user
Container
  helper (extend existing endpoint discovery helper)
       | one Unix connection per accepted client stream
  existing app-server(s)
```

The host helper routes streams to registered container endpoints; the container
helper connects to the actual app-server sockets in its own filesystem namespace.
Both use inherited pipes. Neither needs a new TCP or Unix listening socket.
Only the desktop needs new listening endpoints for unmodified local TUIs.
No SSH/Docker PTYs are allocated for transport.

Ordinary remote host sockets keep native SSH Unix forwarding. Container backends
use the shared helper command instead of an SSH command per client. SSH supplies
encryption and host authentication; no new remote network service is introduced.
One helper command consumes one SSH command-session channel, avoiding a channel
per container or terminal.

For local containers, omit SSH and merge the desktop and Docker-host roles in
one host helper. Flatpak uses its existing host-command boundary, without adding
blanket filesystem/network permissions for this route.

Route the observer through this shared transport too. Keeping its old per-server
SSH/Docker proxy chain would retain avoidable overhead. The existing Beast
protocol adapter may remain separate; it must acquire a shared transport stream
instead of launching another container connection. Container helpers open Unix
sockets directly rather than spawning a stock Codex proxy for each stream.

### Why stream framing is justified

Each Docker exec attachment supplies one stdio stream. Carrying independent
native client connections requires stream separation. Reuse one small versioned
framing contract across the SSH and Docker hops. This is byte multiplexing, not
an app-server broker: each client's WebSocket handshake, messages and request IDs
remain unchanged. No parsing or rebuilding of Codex JSON-RPC is required.

The contract needs negotiation, endpoint/inventory events, open/open-result,
data, flow-control credit, close/reset and bounded diagnostics. Exact encoding,
limits and library selection remain implementation choices requiring review.
Do not create a general remote shell service or stable third-party protocol.

Required properties:

- Validate versions, frame lengths, stream IDs and generations before allocation.
  Bound frames, stream counts, per-stream queues and total memory.
- Chunk transfers instead of buffering entire images/WebSocket messages. Do not
  add base64 encoding to the raw transport payload.
- Use per-stream flow control and fair scheduling. Reserve control capacity so
  stalled terminals/full data queues cannot block inventory, close or credit
  messages. Qualify flow control independently on both hops.
- Make close/reset idempotent. Parent EOF closes its streams. Server EOF closes
  the affected stream promptly rather than waiting forever for the other copy
  direction. Allow only a bounded drain of pending bytes.
- Keep diagnostics off binary stdout and bound their storage. Never block the
  GUI or serialize all hosts behind one blocked worker.

This protocol needs malformed-input and failure tests; it is not cost-free.
Prefer an existing framing implementation if it fits the helper/dependency
constraints. The current isolated Python 3 helper path is a reasonable starting
candidate, subject to throughput tests; no additional language or network stack
is selected. An exact codec/library decision remains open.

### Process budget

For one desktop instance connected to remote Docker hosts:

- One shared desktop transport helper.
- One foreground SSH client and one host helper per remote SSH profile.
- One Docker exec client and one container helper per attached container/runtime
  identity, replacing the existing discovery role rather than duplicating it.
- One Codex TUI per opened terminal and independent app-server connections.
- Existing observer bridges, Docker-event clients, Flatpak/launcher and SSH-server
  processes remain additional overhead to include in actual measurements.

For 40 opened TUIs in one container on one remote host, the principal transport
roles are desktop helper + SSH client + host helper + Docker client + container
helper: five, not 120. This is a design count, not a measured total OS process
count. Forty distinct attached containers still need 40 container helpers and
40 Docker clients. Opening another terminal in an attached container adds a TUI,
streams and buffers, not another Docker or SSH connection.

One helper per container means per attached Archcodex instance and runtime user,
not a global singleton shared by every desktop. Different runtime users need
different helpers; do not elevate privileges to combine them.

## 5. Security constraints

### Remote side

No new remote listener, port mapping, socket-directory mount, credential copy,
SSH-agent forwarding or X11 forwarding is needed. The selected SSH user uses its
existing Docker authority. The container helper runs as the validated runtime
user, not root, and receives no Docker socket.

Retain SSH host-key verification and existing account authentication. Own the
foreground SSH process; do not detach it or reuse an unrelated user-owned master
whose lifetime Archcodex cannot control. Identify a transport by its configured
SSH profile/authentication context, not only a displayed hostname.

Open operations use endpoints registered by discovery, bound to Docker engine
ID, full container ID, lifetime, runtime user, environment fingerprint and
connection generation. Native clients cannot supply arbitrary host paths,
commands, Docker contexts or container names.

Keep routing tables separate by container. Check the target socket's ownership
and current endpoint evidence. Treat socket replacement/server restart as
identity/reconnection events. Revalidate before attaching and consume Docker
lifecycle events. A stop between validation and connect must fail closed, never
retarget by friendly name; no snapshot eliminates all races.

Container metadata and frame contents are untrusted input to host code. They
must not select another host path or cause unbounded allocation. Successful
transport does not establish historical resume/fork provenance.

### Desktop endpoints and Codex sandbox protection

The unmodified TUI needs an explicit local Unix endpoint. Use a short owner-only
directory and mode 0600; check the peer UID where the listener permits. Do not
share this directory with containers or place it in a project/shared home.
Same-user desktop clients are trusted by agreement.

Codex masks its privileged daemon directory from filesystem-restricted tools.
A newly invented socket pathname is not automatically covered. Preserve that
restriction for desktop relay endpoints: mode 0700 alone does not exclude a
same-UID sandboxed command. One candidate is an Archcodex-owned subdirectory
beneath the installed version's protected daemon root, after verifying ownership,
layout and sandbox masking. This is not a permanent public upstream API.
Qualification of this location is a security gate; unsupported protection must
be reported rather than silently bypassed. Do not assume arbitrary aliases are
masked or rewrite Codex's own sockets.

No new bearer-token scheme is needed merely to distinguish trusted same-user
programs. A deliberately exposed desktop endpoint grants access to its backend;
the helper does not offer read-only or per-thread authorization.

## 6. Lifetime: persistent work does not need a permanent relay

Start one helper when a container is attached for discovery/status or terminal
use; retain it while that attachment is wanted. It carries all relevant streams.
GUI/transport closure ends the helper; the existing app-server and accepted work
continue independently.

A helper installed for the container lifetime is possible but not needed first.
After losing its original exec stream, it needs a reconnect mechanism: a private
listener plus an attachment process, or a reverse connection to another listener.
Docker exec is not a general reattachment interface for an already-running exec
process. Permanence therefore does not itself remove connection machinery.

If several independent desktops later need a true container-wide singleton,
consider a myCodex-managed service with version negotiation, a protected control
endpoint and explicit supervision. `init: true` helps reap children but is not a
helper restart policy. Such service/image changes are outside the recommended
initial implementation; existing containers need no recreation or new mounts.

## 7. Failure behavior

| Failure | Required behavior |
| --- | --- |
| One TUI exits | Close its stream; keep discovery and other clients. |
| App-server socket closes | Reset affected streams promptly; preserve other servers. |
| Container helper exits | Close that container's streams, reap Docker client, mark state unknown. |
| Container stops/restarts | Invalidate old identity/generation; rediscover before attachment. |
| Host helper or SSH exits | Close affected desktop streams; host/container pipe EOF initiates cleanup. |
| GUI exits/crashes | Stop recovery; exclusive owner-control closure stops owned local helpers. |
| Silent partition | Cleanup is bounded only after transport failure detection on the relevant side. |

Use exclusive, non-inherited ownership descriptors, EOF handlers, bounded drain
and termination deadlines, and child reaping. Do not rely on SIGTERM crossing SSH
or Docker. Cancellation must not leave blocking reader threads or full writers
preventing exit. Local user-service supervision can provide a crash backstop;
its exact integration remains proposed.

There are no new remote filesystem socket entries in the pipe-only route.
Desktop endpoints still need owned-path cleanup and a crash backstop. Never
remove existing app-server sockets. TCP TIME_WAIT is not an orphan listener.

Coordinate recovery per host/container with backoff, bounded starts and fresh
generations; do not spawn a new helper on every TUI retry or replay uncertain
submissions. During a silent partition the old remote attachment may survive
until remote-side detection expires. Do not promise immediate/zero-overlap
cleanup. SSH keepalive policy remains an explicit decision; no session polling
or terminal-idleness timeout is introduced.

A shared helper crash affects more clients, and one TCP connection has
head-of-line delays during packet loss. Fair scheduling prevents avoidable queue
starvation, not physical network delays. Measure before adding transports.

## 8. Qualification before adoption

1. Connect multiple native TUIs and the observer through one helper path, including
   full WebSocket behavior and approvals.
2. Compare 1, 10 and 40 clients and many containers separately: processes, memory,
   descriptors, throughput and responsiveness during large image transfers.
3. Prove there is no new remote listener/mount. Test another UID and ordinary
   container; verify desktop sandboxed tools cannot reach privileged relay aliases.
4. Test malformed/partial frames, oversized payloads, stalled consumers, exhausted
   credits and abrupt EOF without unbounded allocation or deadlock.
5. Exercise UID mappings, rootless/rootful Docker, multiple Codex homes, shared
   history, socket replacement and container recreation without retargeting.
6. Kill every helper/SSH/client in turn; close the GUI; interrupt networking;
   repeat recovery and inspect both hosts/containers for orphan resources.
7. Confirm remote work survives client loss, uncertain requests are not replayed,
   and clipboard/image paste work on the desktop with Flatpak/X11.

These are proposed acceptance checks, not tests performed during this analysis.
Full implementation awaits acceptance of the security/lifetime constraints and
stream-framing tradeoff.

## Sources

- Current [host/container helper](../src/containers.py),
  [inotify helper](../src/discover.py), [SSH/Flatpak launch](../src/launch.cpp).
- [Codex Unix listener](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/app-server-transport/src/transport/unix_socket.rs)
  and [protected directory](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/uds/src/daemon_directory.rs).
- [Linux sandbox masking](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/linux-sandbox/src/bwrap.rs)
  and [mount aliases](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/linux-sandbox/src/daemon_mounts.rs).
- [Docker exec](https://docs.docker.com/reference/cli/docker/container/exec/),
  [exec lifecycle](https://github.com/moby/moby/blob/docker-v29.1.3/daemon/exec.go),
  [Docker authority](https://docs.docker.com/engine/security/).
- [OpenSSH](https://man.openbsd.org/ssh.1),
  [command-session limits](https://man.openbsd.org/sshd_config.5),
  [Linux Unix socket access](https://man7.org/linux/man-pages/man7/unix.7.html).
- [myCodex Compose](https://github.com/emsi/myCodex/blob/master/docker-compose.yaml):
  current mount/init configuration; moving source, not a version pin.
