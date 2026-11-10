# myCodex integration: 0.6.0

The adopted [local TUI plan](local-tui-connections.md) and
[security design](container-transport-security.md) are implemented for local and
SSH myCodex hosts. Existing containers remain the primary workflow. No myCodex
command, image update, container recreation, socket mount or permanent remote
service is required. Direct TCP and generic SSH host-socket profiles remain
future adapters.

## Connection and execution

```text
Konsole PTY → desktop Codex TUI --remote unix://PRIVATE_LOCAL_RELAY
Archcodex observer → existing WebSocket bridge → host stdio adapter → same relay

Local:  shared desktop helper → one Docker exec attachment → container helper → app-server
Remote: shared desktop helper → one SSH connection → host helper
                                      → one Docker exec attachment/container → container helper → app-server
```

The helpers multiplex independent raw WebSocket streams and discovery events.
They do not interpret or merge the clients' JSON-RPC. There is one observer per
app-server and one local TUI per opened terminal; listed sessions add no TUI
processes. Neither path starts an app-server. The server, model credentials,
agent commands and workspace stay in the original container. The desktop TUI
uses the host Codex executable and local clipboard. Image contents travel through
Codex's existing remote protocol; no SCP service or shared image directory is used.

Launch is `codex --remote unix://LOCAL_RELAY resume -- THREAD_ID`, or
`codex --remote unix://LOCAL_RELAY --cd REMOTE_WORKDIR` for New. A remote path is
never used as a desktop chdir, and the container's CODEX_HOME is not imported.
Missing/old desktop Codex produces a retained terminal error, never a fallback to
the desktop's default app-server. The explicit Fork action still sends native
`thread/fork` after validating execution provenance, then opens the returned ID.

The isolated Python helpers are packaged as resources and supplied through argv.
They are not installed on the host or added to myCodex. The small framing protocol
is internal to the application and versioned separately from Codex. See the
[security design](container-transport-security.md#9-concrete-implementation-and-operational-limits)
for queue bounds, private paths, process accounting and cleanup.

## Requirements and defaults

- Local discovery is enabled by default. Use **Connections → myCodex hosts…**
  to disable it, retry it or add/remove an SSH host. Host entries are persisted
  in Archcodex's `window.ini`; discovered server profiles and metadata use SQLite.
- Each selected Docker host needs `/usr/bin/python3` and the Docker CLI in PATH,
  with existing engine access as the SSH/local user. There is no automatic sudo.
  The selected container also needs Python 3 and an existing compatible Codex
  app-server. No fallback installs or upgrades them. The desktop requires Codex
  0.166.0 or newer in its host PATH and a running systemd user manager. One
  transient user service supervises the shared transport; no unit is installed.
  The manager's login environment must provide Docker/OpenSSH and any SSH agent.
- SSH uses the host's OpenSSH configuration, aliases, agent and known-host files.
  Background connections use `BatchMode=yes`, a 10-second connect timeout and no
  PTY. Set up/verify SSH authentication in a host terminal first. Keys and account
  credentials are never copied into the Flatpak; agent forwarding is not enabled.
  All traffic shares one foreground SSH command per host profile, without a PTY
  or control master. Agent, X11 and configured port forwarding are explicitly
  disabled. `ServerAliveInterval=15` / `ServerAliveCountMax=3` are the approved
  transport-liveness defaults; they do not query Codex sessions.
- Enumerate local Unix Docker contexts on each selected host, deduplicated by
  Docker engine ID. Do not automatically follow TCP/SSH Docker contexts. Context
  additions need an explicit refresh. Rootless Unix contexts are eligible under
  the same access rules; actual rootless-host qualification remains outstanding.
- Identify existing myCodex containers using the Compose label
  `io.infrasecture.mycodex.gui`, independent of its value. Read the runtime UID
  and home from the existing myCodex environment metadata; resolve its real
  account name inside the container. Missing UID/home produces a diagnostic,
  never an automatic root fallback. Generic or unlabeled containers are excluded.
- Explicit Private/Shared/Custom mode metadata is used when present. For legacy
  containers, volume `codex_state` identifies the conventional shared mode;
  other storage is labeled **Custom storage**, rather than guessing private mode
  from a project-looking volume name. This label never grants resume permission.

These legacy fields were checked against the existing
[myCodex Compose source](https://github.com/emsi/myCodex/blob/ec3133efb53795586232aad0305aa10ca53dfd10/docker-compose.yaml).
Actual containers with modified configuration may report incomplete discovery.
No Flatpak permissions or myCodex repository files were changed for this release.
Existing 0.3.x data loads without migration. Older binaries do not understand
new container profiles; use a separate `--data-dir` when testing a downgrade.

## Discovery and recovery

| Layer | Initial/recovery action | Normal operation |
| --- | --- | --- |
| Docker engines | Enumerate accessible Unix contexts and identify engines. | Keep one lifecycle event subscription per engine. |
| Containers | List/inspect running labeled containers; reconcile on lifecycle events. | React to start, die, destroy, pause, unpause, rename and restart. Ignore exec events generated by our own helpers. |
| App-servers | Run the existing endpoint-discovery code inside the actual runtime namespace. | Inotify socket events; snapshots after invalidation, overflow or explicit refresh. |
| Sessions | Read history and loaded-thread inventory after connection. | Codex notifications; no periodic status requests. |

Subscribe with Docker event replay overlapping startup, then reconcile the
inventory. Docker replay is bounded, so recovery always obtains a new inventory.
Running containers with no accessible server remain visible in All with an
explanation. Paused/inaccessible containers and failed hosts report coverage
limits; partial results remain usable.

Failed transports/watchers retry with backoff from 1 to 30 seconds. This is
reconnection of a failed subscription, not a timer querying healthy sessions.
Disable/disconnect cancels the relevant retry path. Normal idle discovery sends
no repeated Docker list/inspect commands. Failure detection uses stream closure,
request deadlines and the approved SSH keepalives. A silent loss is detected
from the desktop after roughly 45 seconds; the remote side can persist until
its own SSH/TCP detection. Remote SSH-server settings are not changed.

Explicit Discover/Connect-retry requests refresh healthy inventories without
replacing their helper attachments or dropping terminals. Disable/remove a host
closes its shared connection and all its client streams while retaining terminal
output and cached history. Re-enable/reconnect, then Restart client to attach
again. This differs from 0.5.1's independent remote TUI connections.

## Shared history and resume authority

Keep the host, engine, actual container ID, container lifetime, server endpoint
and thread ID separate. Labels and container names are display metadata. A live
handle is backend plus thread ID; equal conversation IDs on separate servers are
never merged. Roots show container name, host and storage mode, with compact
per-filter folder trees underneath. IDs and socket details remain in details.

A server listing a saved conversation establishes **history visibility only**.
It does not establish that its workspace, mounts or tools are suitable for
resuming it. This applies to both the explorer and global search.

By default, the explorer and search list only sessions whose cwd is the
container's Docker `Config.WorkingDir` (WORKDIR) or a subdirectory. Container
WORKDIR is separate from the app-server process cwd and host bind-mount paths.
Paths are normalized lexically in the container namespace, with directory
boundaries respected; the desktop does not resolve remote paths or symlinks.
An empty Docker WorkingDir means `/`. Older cached profiles with no recorded
WORKDIR hide sessions until discovery supplies it; the root's details show this.
**Settings → Configure Archcodex… → Backends → Show sessions outside the
container's working directory** disables this restriction for local and SSH
myCodex backends. Hidden metadata remains cached, so changing the setting does
not require refetching or polling. Other backend types are unaffected. An already
open terminal remains open even if changing this setting hides its explorer row.
The independent unsaved-session filter still applies. This is a display filter,
not execution authority; showing shared history does not authorize launching it.
Opened is a separate inventory of GUI terminal views and bypasses both history
visibility filters, including for retained exited clients.

The initial policy deliberately uses one concrete piece of evidence: this
specific server's loaded-thread inventory or live-session event. Archcodex
persists that observation bound to its execution-environment fingerprint
(container, UID, home and mount metadata, scoped to host/engine). Metadata reads
and search results cannot manufacture this app-owned evidence.

- An observed live conversation can be attached to its server.
- A previously observed conversation can be resumed in the same established
  environment after it unloads or Archcodex restarts.
- Other saved conversations within the visibility settings appear as **History only**.
  Opening one explains why this container cannot be selected automatically.
  Open it in its correct environment first; subsequent inventory/events establish
  the association. There is no “resume anywhere” override.
- Recreation creates a new container identity. Changed mount/environment identity
  invalidates cached execution evidence, including across a partially written
  cache. A successful metadata read or matching cwd string cannot restore it.
- Before attaching the shared helper, check the selected Docker context is local
  Unix, the actual engine ID, full container ID, running/unpaused state,
  `StartedAt` lifetime and environment fingerprint. Mount order is canonicalized.
  Every subsequent stream is pinned to that attachment and a registered endpoint
  token; the container rechecks socket identity and peer UID/PID on open. Docker
  events invalidate changed contexts. No Docker inspect command is added per TUI.
  A shared history file cannot retarget a stream to a different container.

Observed execution is not proof of original authorship or of unchanged files
inside a mount. It is a conservative routing rule, not a complete workspace
provenance system. An explicit alternative-environment workflow and a deduplicated
shared-history library can be designed later.

## Responsiveness and limits

Protocol/discovery QProcesses and JSON decoding run on worker event loops grouped
by host, with discovery separate from protocol work. SQLite has its own worker
and connection. The GUI consumes coalesced session batches of at most 64 rows;
cache restoration is also chunked. Qt models are updated only on the GUI thread.
No Docker/SSH operation or database lock is waited on by the GUI.

One protocol connection serves all conversations on a server. Terminals are
created on demand. Discovery starts up to eight container watchers concurrently;
metadata reads have four requests in flight per backend. Full snapshots are
coalesced while the GUI handles its current update. Protocol messages, discovery
output, pending events and write queues have explicit bounds. Per-request
failures do not routinely disconnect an otherwise healthy backend.

Closing facades queues cleanup to workers. Owned watcher stdin is closed, then
remaining helpers are terminated/killed after a grace period. Application exit
services completion events for up to ten seconds to flush accepted cache writes;
a deadline violation is reported rather than silently treated as a clean flush.
The window itself closes without waiting on remote SSH or Docker.

Remote hosts fail independently. Background failures stay in host status/details;
interactive launch failures retain terminal output. Existing All/Active/Running/
Waiting semantics, blue Running and green Waiting indicators are preserved.

## Validation and remaining host acceptance

Automated fixtures cover Docker lifecycle updates without idle polling, watcher
cleanup, changed lifetime/mount/engine rejection, shell argument boundaries, raw
proxy transport, separate container roots, empty containers, shared history,
80 live records, stalled SSH and blocked database responsiveness. Native and KDE
SDK tests cover protocol/model/window behavior; actual Konsole PTY checks use the
packaged KDE libraries under X11.

This development environment has no accessible Docker daemon and cannot launch
a full Flatpak sandbox. Therefore real private/shared myCodex containers, remote
SSH authentication/jump hosts, rootless engines, systemd supervision, desktop
clipboard/image paste and active task survival across disconnect still need host
qualification. Automated transport tests use fake SSH/Docker entry points with
real helper subprocesses, byte streams, 40 clients and two containers; native
observer tests also exercise real WebSocket handshakes and notifications.
No credential files or live model turns were used in automated tests.

Direct TCP and generic SSH-forwarded host socket profiles remain future adapters.
Published container socket directories are an optional future optimization;
`codex app-server daemon` is not needed to connect to an already-running server.
