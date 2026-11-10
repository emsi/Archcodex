# myCodex transport options and adopted route

Update, 2026-11-10: the [local TUI connection plan](local-tui-connections.md)
supersedes the future terminal-placement direction in this earlier discussion.
The [new security analysis](container-transport-security.md) recommends evaluating
shared helpers without remote listeners or socket mounts. That recommendation
has not been adopted for implementation. The description below still records
the route shipped in 0.5.1 and the earlier alternatives.

Status: the implementation request adopts direct container integration for 0.4.0.
Use Docker exec and native `codex app-server proxy` without a PTY for protocol
traffic; launch the container's Codex TUI with a PTY. Remote hosts use the same
route through host OpenSSH. No mandatory myCodex integration command, container
recreation, new mount or public helper protocol is introduced.

Published physical Unix sockets and native SSH socket forwarding remain future
optimizations. The analysis below records the alternatives considered; unresolved
wording there describes that earlier discussion. The current implementation,
requirements and restrictions are authoritative in [mycodex.md](mycodex.md).

## 1. Separate discovery from connection

The earlier `myCodex integration watch/proxy/tui` proposal bundled three jobs:
discovering environments, reaching an app-server, and choosing where its TUI
runs. A command namespace is not an architectural requirement for any of them.

If a container's actual Unix listener is accessible to the Docker-host user,
Archcodex can reuse its local Unix adapter. If accessible to the SSH user on a
remote Docker host, it can reuse native SSH Unix-socket forwarding. A container
does not intrinsically require `docker exec` in the connection path.

Docker event/metadata discovery remains necessary to associate endpoints with
containers and list containers with no server. Remote socket discovery also
needs an event channel, independent of the forwarded protocol connection. This
could be Archcodex-owned discovery code using Docker and Linux notifications;
it need not be a new public myCodex command or permanent service. Exact tooling
and dependencies remain open.

## 2. What is actually in the shared volume?

Reviewed source snapshots:

- [myCodex Compose](https://github.com/emsi/myCodex/blob/ec3133efb53795586232aad0305aa10ca53dfd10/docker-compose.yaml)
  mounts the state volume at the runtime home and a separate workspace bind.
  It does not publish the Codex physical socket directory.
- [Codex Unix transport](https://github.com/openai/codex/blob/4f9e7171a1f3334ab22123aba61daa5af37f289e/codex-rs/app-server-transport/src/transport/unix_socket.rs)
  uses the advertised socket path as a symlink. The actual listener is placed at
  a hashed path under a protected directory. This behavior also applies to an
  explicit Unix listen path in that implementation.
- [Codex socket directory](https://github.com/openai/codex/blob/4f9e7171a1f3334ab22123aba61daa5af37f289e/codex-rs/uds/src/daemon_directory.rs)
  chooses `/tmp/codex-daemon-<effective-uid>` independently of `HOME`, `CODEX_HOME`
  and `TMPDIR`, and requires user ownership and mode 0700.

Thus a shared home may contain:

```text
CODEX_HOME/app-server-control/app-server-control.sock
  -> /tmp/codex-daemon-UID/HASH
```

The target is in the container's filesystem namespace. Looking up the same
absolute symlink on the host resolves it in the host namespace. Sharing the
symlink does not share the listening socket; successful host lookup would not
by itself prove it reached the selected container. Do not access account files
to establish this distinction.

These are source findings, not an inspection of the user's live volumes or a
claim that every installed Codex version uses this layout. Discovery must resolve
the real endpoint for the installed version, not hardcode a hash or infer a
listener from a pathname alone.

## 3. Alternatives

| Option | Local connection | Remote connection | Main consequence |
| --- | --- | --- | --- |
| Actual listener already accessible through a volume | Existing Unix adapter opens that host path. | SSH forwards that same remote-host path. | Works conditionally; named-volume internals and access rights are not a portable host-access contract. A shared symlink alone is insufficient. |
| Dedicated per-container host-visible socket directory | Existing Unix adapter opens the published physical listener. | Existing SSH adapter forwards that listener. | Reuses transports; requires deliberate myCodex mount/metadata support for new or explicitly recreated containers. |
| Private socket plus exec relay | Host-side `docker exec -i` runs the container's native Codex proxy. | SSH runs the same relay on the Docker host. | Can serve existing unexposed containers; adds subprocesses and correct runtime-user/environment handling. |

Linux pathname sockets use filesystem paths and permissions. Docker recommends
bind mounts for data deliberately accessed by both host and container; a named
volume's storage location is Docker-managed. Treat direct named-volume access as
a detected capability, not a reason to change `/var/lib/docker` permissions or
require root. Rootless/user-namespace UID mappings also need qualification.
See [Linux Unix sockets](https://man7.org/linux/man-pages/man7/unix.7.html),
[Docker volumes](https://docs.docker.com/engine/storage/volumes/) and
[bind mounts](https://docs.docker.com/engine/storage/bind-mounts/).

## 4. Candidate: publish the real socket directory

A promising design is a separate host directory per container instance, mounted
at that container's physical Codex socket directory before its server starts.
Do not mount all containers onto one shared runtime-socket directory.

For the reviewed Codex layout, the conceptual mapping is:

```text
Docker host: HOST_SOCKET_ROOT/INSTANCE/
    bind-mounted into this container as /tmp/codex-daemon-UID/

Container: /tmp/codex-daemon-UID/HASH
Host:      HOST_SOCKET_ROOT/INSTANCE/HASH
```

Each container can have the same internal path and hash while retaining a
different host-visible socket. Discover the actual control endpoint; the runtime
directory can also contain updater sockets and locks, which must not appear as
Codex session servers.

Constraints to settle before selecting this option:

- The directory must meet Codex ownership/mode rules under the actual UID mapping.
  Host access must not be achieved by making control sockets world-accessible.
- Bind the directory, not a single socket inode, so server socket replacement
  remains visible. Do not rewrite a shared home symlink for one container.
- The host path and metadata must identify the container and current lifetime.
  Keep Unix paths short. A readable directory entry is not proof of ownership,
  a live listener or the correct protocol endpoint.
- Directory lifetime belongs to myCodex/container lifecycle, not GUI lifetime.
  A container can outlive a login session; selecting `XDG_RUNTIME_DIR` requires
  checking logout, lingering, reboot and restart behavior. No final host root
  or instance-token format is chosen here.
- Publishing the whole reserved directory also exposes its other control
  sockets to the authorized host user. Keep each directory private and qualify
  the actual security/sandbox behavior; do not mount another container's directory.
- Adding the mount to existing containers normally requires their recreation.
  Discovery must not do that automatically. Decide whether legacy containers
  remain listed but unavailable, or use an optional exec-relay fallback.
- Changing only `CODEX_HOME`, `TMPDIR` or the advertised listen pathname does
  not relocate the reviewed implementation's physical socket directory.

Exact source paths/mounts need testing against the supported Codex and myCodex
versions. No Codex patch is presumed necessary, and no mount is authorized by
this document.

## 5. Reuse the same Unix/SSH connection path

Once the physical listener is accessible on its Docker host:

```text
Local:
  Flatpak GUI -> existing host Codex byte proxy -> published Unix listener
  Host Codex TUI ------------------------------> published Unix listener

Remote:
  Flatpak GUI -> existing host Codex byte proxy -> local forwarded Unix socket
  Host Codex TUI ------------------------------> local forwarded Unix socket
    -> OpenSSH -> remote host's published Unix listener -> container app-server
```

The local proxy is the existing Flatpak host-access boundary, not a proposed
`myCodex integration` command. Native builds can open the socket directly.
Both clients still use the Codex WebSocket handshake/framing over Unix streams.
Container sockets require no extra TCP port or Docker-daemon network exposure.

Illustrative command shape, assuming paths with no forwarding-syntax ambiguity:

```text
ssh -NT -o ExitOnForwardFailure=yes \
  -L /LOCAL/PRIVATE/forward.sock:/REMOTE/PUBLISHED/INSTANCE/control.sock HOST_ALIAS

codex --remote unix:///LOCAL/PRIVATE/forward.sock resume -- THREAD_ID
```

These are placeholders, not commands executed during this analysis. The remote
path must identify the actual exposed listener (often a hash, not `control.sock`).
Implementation supplies short, private owned paths and handles SSH forwarding
syntax/quoting. The SSH user must be able to connect to the remote endpoint.
Forwarding setup success does not prove the destination server is reachable;
complete the Codex handshake before reporting connected. See
[OpenSSH Unix-socket forwarding](https://man.openbsd.org/ssh.1).

This arrangement also permits the local host's TUI to serve container sessions,
as with ordinary SSH servers. The execution server remains in the container.
Client/server compatibility and any client-local facilities must be qualified.
Running the TUI inside the container remains an alternative if required, not an
accepted prerequisite. Terminal placement must not change the selected server
or turn an unknown resume context into a valid one.

## 6. What a helper would still buy

An exec relay avoids publishing private sockets and can work with existing
containers, using the installed container Codex proxy without a PTY. A myCodex
machine interface could hide runtime-user and version differences. Those are
advantages, not evidence that the interface is mandatory.

Archcodex could also issue the bounded Docker operations itself through its
host boundary. Whether runtime metadata is best described by labels, a small
descriptor or a command is independent of the byte transport. Ordinary myCodex
launch/update/interactive-attach paths must not be reused as discovery calls.

If an exec fallback is selected, it must be explicit and capability-based, not
a silent way to resume on another container. It needs cancellation, diagnostics,
non-PTY framing and actual runtime-user/environment handling. Neither route
changes shared-history ownership or validates a historical resume target.

## 7. Direction to evaluate, not a settled decision

Prefer reusing the existing Unix/SSH adapters when the real listener can be
exposed with a small, well-defined per-container mount/metadata change. Evaluate
that first rather than making a new `myCodex integration` command mandatory.
Keep exec relay as a separately assessed compatibility option.

Before choosing, verify: physical socket visibility and permissions; isolation
between containers sharing the same home; socket replacement and stale entries;
rootless/rootful behavior; GUI exit, container restart and host-session lifetime;
native SSH forwarding and host-TUI compatibility; discovery without polling;
and refusal to resume shared history in an unestablished execution context.

Shared history is the stricter invariant: a transport can successfully reach
container B while B remains the wrong environment for a conversation visible
there. Transport success must never be used as proof of resume eligibility.
