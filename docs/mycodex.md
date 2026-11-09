# myCodex integration: agreed direction

Status: design only, recorded on 2026-11-09 after discussion of local and remote
myCodex environments. Archcodex 0.3.2 implements local host Unix connections;
these requirements do not claim implemented container support. The connection
mechanism, helper interface and terminal placement remain open in the separate
[transport analysis](mycodex-transport-options.md).

## 1. Scope and boundaries

Support private and shared-state myCodex containers on the local machine and on
explicitly configured SSH hosts. Existing containers, app-servers and sessions
are the primary workflow. Projects remain derived groups; users need not create
projects to discover or attach to existing work.

Reuse the common Codex protocol client, session model and event/reconnect
contract. Container discovery and execution context belong to the provider;
socket transport belongs to the existing transport adapters. Docker commands,
SSH commands and container paths must not leak into the UI model. This does not
require a public plugin ABI, permanent management daemon or container orchestrator.

Attaching or discovering must not implicitly install/update software, create or
recreate containers, start replacement app-servers, resume historical threads or
start turns. Optional lifecycle operations require separate explicit actions.
Keep the established backend delivery order: local Unix, TCP, SSH, then myCodex.

## 2. Execution identity and shared history

Keep these identities separate:

| Object | Meaning |
| --- | --- |
| Host | Local machine or configured SSH host; paths belong to that host. |
| Docker engine | The particular accessible engine on that host; aliases need not identify different engines. |
| Container environment | Actual container ID and current lifetime, plus friendly name and optional stable logical instance identity. |
| App-server | Actual endpoint in that environment, with a connection/server generation. |
| Live session | App-server identity plus Codex thread ID. |
| History store | Shared persistent files, when a common store is positively identified; never an execution target. |

Container names, PIDs, identical paths and equal `CODEX_HOME` values are not
sufficient identity. Recreating a container or restarting a server invalidates
old live handles. A logical instance name must not silently transfer running
state to its replacement.

**A server reporting a saved conversation proves visibility of history, not
that the conversation belongs to or can be resumed in that container.**

For example, containers A and B share history. A has `/work/compiler` mounted;
B has `/work/reports`. Both servers may list a compiler conversation, but B is
not thereby a valid restart target. Even an identical cwd string on B is
insufficient: it may name different files, a different worktree or a different
toolchain. Neither a successful metadata read nor the saved-session flag proves
execution eligibility.

Distinguish three questions in application metadata and behavior:

1. **Visible history:** which servers can read the saved conversation?
2. **Live execution:** which server currently has that thread loaded/running,
   according to that server's inventory and events?
3. **Resume context:** which environment has been established as an appropriate
   target for continuing the historical conversation?

Do not derive the second or third answer from the first. If two servers really
load the same thread ID, retain two execution identities; do not collapse them
or route commands by thread ID alone. Loading also does not prove that a server
originally created the conversation.

The routing requirement is agreed; the precise provenance schema and history UI
are still to be designed. A minimal candidate is application-owned metadata
recording observed execution environments separately from history visibility.
Old history with no reliable provenance remains unknown. Do not invent provenance
from filename, title, cwd, shared volume or the first server returning a result.
Observed provenance is evidence of previous execution, not a guarantee that all
mounts, permissions and tools still work after an environment changes.

Normal opening should attach to the known live execution or use an established
resume context. A history-only row must not silently resume through whichever
container returned it. An explicitly chosen alternative environment, if later
supported, is a separate action with target validation. Do not probe eligibility
by resuming the conversation or sending a turn.

Global search can find history even when no suitable runtime is available. Keep
that result readable and explain its execution availability. Presentation-level
deduplication may use verified common storage identity, but must retain the
distinct live execution targets and never deduplicate solely by thread ID.
The exact placement of shared/unassigned history in the explorer remains open.

Private/shared/custom storage mode is descriptive metadata, independent of the
connection method. Preserve the current authentication arrangement; never read,
copy or mount host account credentials as part of this integration.

## 3. Discovery and events

Automatically enumerate running myCodex containers on accessible local Docker
engines, including containers with no running app-server. Report inaccessible
engines or incomplete endpoint coverage instead of claiming complete discovery.
Do not list unrelated workloads as myCodex or acquire additional privileges.

On a configured SSH host, perform equivalent discovery in that host's context.
Adding an SSH host is explicit; connecting remote Docker contexts discovered in
local configuration is not an automatic side effect of local discovery.

| Layer | Initial/recovery action | Normal-operation source |
| --- | --- | --- |
| Containers | Snapshot running instances and reconcile buffered lifecycle events. | Docker lifecycle event stream. |
| Servers | Discover actual reachable control endpoints and their environment association. | Socket filesystem events or an equivalent endpoint event source. |
| Sessions | Read inventory, loaded threads and supported status after connection. | Codex protocol notifications. |

Subscribe and reconcile without a gap between the inventory and event stream.
Reconnect, event loss/overflow and explicit refresh permit new snapshots.
There is no periodic container listing, process scan or session-status query.
Discovery must not use terminal silence, CPU use or tmux output as session state.
Avoid self-triggering loops from discovery helpers' own Docker exec events.

The location of the socket watcher depends on the unresolved exposure mechanism.
Published host-visible sockets can be watched on the Docker host; private
sockets need observation within the container namespace. On remote hosts,
endpoint events must be carried over a separate SSH event channel. Forwarding a
single socket alone does not enumerate containers or report new endpoints.

Docker's event history is bounded, so reconnect recovery must not assume replay
is durable. See [Docker events](https://docs.docker.com/reference/cli/docker/system/events/).

## 4. Responsiveness and failure isolation

Remote latency or failure must never impose a blocking wait on the GUI. This
also applies to local Docker calls, JSON decoding, large inventories and database
contention. Merely launching SSH through `QProcess` is insufficient.

- Run protocol parsing and bulk work outside the GUI thread. Isolate hosts so
  one stalled host cannot occupy another host's execution context.
- Give database work an owned execution context and connection. Update Qt models
  only on the GUI thread, using bounded batches and affected-branch updates.
- Bound message sizes, accumulated buffers, pending requests and update queues.
  Coalesce replaceable status updates without losing lifecycle/input events;
  invalidate and reconcile if a stream cannot be processed reliably.
- Use asynchronous, cancellable connection setup and cleanup. No GUI-thread
  `waitForFinished` or equivalent synchronous remote/process operation.
- Scope timeouts and errors to the operation/backend. A slow history search
  should not routinely tear down an otherwise healthy status connection.
- Limit connection concurrency. Maintain a protocol connection per server, not
  per conversation, and create TUI clients only when needed.
- Show cached content immediately. Connect hosts independently. Search produces
  progressive results with explicit incomplete/offline coverage and ignores
  replies from cancelled generations.
- Present background failures inline at their host/container. Do not open a
  cascade of modal dialogs or require every host to finish before navigation.

Exact worker/thread topology and numerical performance budgets remain
implementation choices to validate. A thread per session and a custom SSH
multiplexing protocol are not requirements. Full offline conversation indexing
is also outside the initial integration; metadata caching cannot provide it.

Current 0.3.2 still has GUI-thread parsing/persistence and synchronous shutdown
waits in `src/backend.cpp`, `src/discovery.cpp` and `src/window.cpp`. This section
is a delivery requirement, not a claim that those paths have been fixed.

## 5. Presentation and lifetime

Use friendly host and container names. Display private/shared mode as context;
put container IDs, PIDs, socket paths and image/runtime details in details.
Represent multiple app-servers in one container without merging their sessions.
An empty running container remains discoverable in All, with an explanation
that no Codex server is available. Exact nesting is a UI design detail.

Preserve All/Active/Running/Waiting semantics, compression within each filter,
green Waiting and blue Running indicators, and retained failed terminal output.
Container connectivity and conversation activity are separate states. A lost
connection makes activity unknown; cached status is not current status.

Archcodex owns its clients, watchers and tunnels. It does not own the lifetime of
existing containers or app-servers. GUI exit and SSH loss must not terminate
their work. Container stop/recreation is a different boundary and may end work;
do not promise that a saved conversation preserves an executing process.

Preserve host SSH aliases, host-key verification, agent and jump-host settings.
No key copies into the Flatpak or automatic agent forwarding are required.
Transport errors, request deadlines and configured SSH liveness behavior detect
failure; a silent idle network partition is not always detected immediately.
None of this permits a background session-status polling loop.

## 6. Qualification and unresolved decisions

Before claiming support, test private/shared containers, identical history IDs
on multiple servers, known and unknown resume contexts, missing or misleading
cwd mappings, zero/multiple servers, restart/recreation and event recovery.
Verify no discovery/attach side effects and no idle status polling.

Exercise 40–80 sessions while an SSH host is unreachable, Docker is restarting,
history arrives in bulk or one stream is noisy. Local typing, navigation,
cancellation and window close must stay responsive. Validate the actual Flatpak
on X11, including terminal resize, clipboard and task continuity.

Still open: socket exposure versus exec relay; whether any myCodex command is
needed; legacy-container compatibility; client location/version policy; socket
directory lifetime; provenance storage and shared-history presentation. See the
[separate transport analysis](mycodex-transport-options.md). No helper command,
new host dependency, permission change or mount configuration is adopted here.
