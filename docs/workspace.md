# Remembered terminal workspace

Implemented for issue [#3](https://github.com/emsi/Archcodex/issues/3), with the
explicitly agreed limitation that stock-TUI conversation switches are not yet
tracked. Version 0.8.0 is a local development/test build, not a published release.
No Codex patch or new transport proxy is required by this implementation.

## What the user sees

- **Restore opened terminals on startup**, in Settings → Application, defaults
  to enabled. Its change takes effect on the next application startup. The
  workspace is still saved when disabled; the next run starts empty and its
  workspace replaces the previous one when saved. This is not a workspace archive.
- Remembered views appear immediately as lightweight placeholders, including in
  **Opened**, regardless of unsaved-session and myCodex WORKDIR filters there.
  Normal history/activity views retain their existing filtering rules.
- Ready sources restore independently. Offline, disabled and missing sources
  retain their placeholders; nothing starts an app-server or enables a source.
  Use Connections to connect a source. A previously disconnected/failed client
  requires **Reconnect client**, even after its source becomes ready.
- The selected view and terminal MRU order survive. Restoring a background view
  does not focus it, record a visit, or reorder the explorer. Actual user visits
  retain the existing ordering behavior. Ctrl+Tab also reaches placeholders.
- Missing, unsaved, changed-context and otherwise failed targets remain visible
  with an explanation and explicit retry. There are no automatic retry loops
  after a preflight or client failure, and no replacement conversations.
- Closing a view explicitly removes its remembered intent. Removing its source
  or remote myCodex host also closes associated views. Temporary discovery gaps,
  cache reconciliation and disconnections do not remove them.
- An exited client's terminal output remains available during this run. Only a
  short status/error summary survives restart. Raw terminal output does not.
- **Terminal workspace…** in the status bar opens nonmodal details; activate a
  row to select its terminal. Pending/failed terminals offer Connections and
  Reconnect client. The existing Close command/shortcut removes a placeholder.

## Current conversation-binding limitation

A terminal view remembers the exact thread ID that **Archcodex opened**. It does
not claim that this remains the TUI's currently displayed conversation:

| Action | Remembered target |
| --- | --- |
| Open a saved session from Projects | Exact selected thread ID |
| Fork using Archcodex's menu/command | Actual ID returned by the server for the fork |
| `/rename` inside the TUI | Existing `thread/name/updated` handling updates that thread's title, including the workspace |
| `/resume`, `/fork`, `/new`, agent navigation inside the TUI | May leave the previous launch target saved; restart can reopen that previous conversation |
| Archcodex **New** | Unknown thread identity; remembered as an unresolved placeholder, never automatically replayed |

The global `thread/started` and name/status notifications are not used to guess
which terminal changed its displayed conversation. Two clients can share an
app-server and independently create or rename conversations. Open the desired
saved conversation from Projects when you need an unambiguous restore target.
The current UI still allows one view per backend/thread pair; separate stable
view IDs make future reassociation possible without changing workspace identity.

### Future option: observe each TUI's protocol stream

Archcodex already arranges each client's socket connection. A future version can
observe request/response pairs on that specific client stream, carrying the
stable view ID as local connection context. Candidates include successful
`thread/start`, `thread/resume` and `thread/fork` replies. This can build on the
existing relay rather than introducing one helper/tunnel per terminal. Ordinary
app-server status delivery should remain on the existing shared event connection.

This work is **deferred**, and it needs acceptance tests before enabling automatic
rebinding. Transparent proxying does not itself prove what the TUI displays:

- Auxiliary/background requests must not be mistaken for navigation.
- Failed/cancelled switches must retain the previous binding. With Codex 0.166.0,
  an injected history-read failure after a successful resume response left the
  real TUI on its previous conversation in an isolated test. Rebinding on the
  resume response alone would have been wrong.
- Cached agent navigation may not send a new resume request. Such navigation
  needs another reliable observation or must remain an explicit limitation.
- Each observation must carry the originating view and connection generation;
  reconnects and late replies must not rebind a closed/replaced terminal.
- Forward protocol bytes unchanged, bound buffering, store only the confirmed
  thread association, and never log conversation traffic or credentials.

This approach requires **no modifications to the Codex TUI**, but full coverage
must be demonstrated rather than assumed. No proxy observation is implemented
in 0.8.0.

## Persistence and compatibility

`metadata.sqlite` now has database schema version **2** and a separate singleton
`workspace` row containing JSON record version **1**. It has no foreign keys to
the transient session cache or discovered backends. The record stores:

- A stable view UUID and backend/launch-thread IDs; unknown New targets have no
  thread ID.
- Last known title/cwd, a logical source fingerprint, and same-container execution
  evidence tied to the existing environment identity.
- Automatic/disconnected/unresolved disposition and a bounded failure summary.
- Selected view and explicit MRU view-ID list, with an increasing snapshot revision.

It stores no terminal output, credentials, PIDs, live relay socket names, or SSH
control paths. Snapshot validation bounds the record to 512 views and 4 MiB,
rejects duplicate/malformed identities and navigation, and rejects unsupported
versions. The limits guard corrupt input and accidental unbounded restore work;
40–80 views are covered by scheduling tests.

A single SQLite upsert replaces the complete workspace atomically. Writes run
on the existing `AsyncStore` worker, coalesce by workspace key, and ignore stale
revisions. UI changes also coalesce per event-loop turn. Shutdown snapshots intent
and freezes terminal lifecycle updates **before** disconnecting transports;
already accepted writes drain during the existing worker shutdown. An abrupt
process kill can lose the latest queued change; the prior committed snapshot
remains intact.

An unreadable or future workspace record is preserved and workspace saving is
disabled for that run; details report the problem. Other cache operations can
continue if the database schema itself is supported. A future database schema
is rejected entirely. An empty or partial startup never erases a readable
pending workspace while restoration is enabled.

**Downgrade:** 0.7.1 and earlier reject database schema 2. Before first testing,
quit Archcodex and back up its data directory, normally
`~/.var/app/io.github.emsi.Archcodex/data/archcodex/` (or the custom
location passed with `--data-dir`). To return
to an older build, restore that pre-upgrade database while the application is
stopped, or use a separate empty data directory. Do not merely lower the SQLite
schema version. There is no automatic destructive downgrade migration.

## Opening and source validation

`OpeningCoordinator` is a small event-driven state machine, separate from
widgets, persistence and transport ownership. Explicit opens, retries and
restoration use the same path. It queues intent, waits for the selected backend,
then verifies that exact ID with `thread/read` and `includeTurns:false`. It does
not scan full history to find a target or poll for status. Ordinary initial
inventory queries and existing backend request deadlines remain unchanged.

Automatic opens require saved history and a matching initialized source
fingerprint. Container opens additionally require observed execution in that
container/environment, either fresh evidence or saved evidence with the same
identity. Shared history alone cannot authorize execution in a sibling container.
Missing execution evidence waits for the existing initial inventory, or produces
an actionable failure if that inventory fails. A changed container ID, mounts,
CODEX_HOME or endpoint blocks opening; close the old view and open the freshly
discovered source explicitly. Container process epochs and regenerated relay paths
are excluded from the fingerprint, so ordinary GUI/transport reconnects work.

Known archive/removal events fail verification. Codex 0.166.0's metadata-only
read can also return archived history without an explicit archive flag. If a
thread was archived while Archcodex was offline, its TUI may be the component
that rejects resuming it; its error is retained and it is not retried. There is
no unarchive request, history mutation or new-session fallback. No multi-client
protocol can eliminate a deletion/archive race after preflight.

The queue allows at most four outstanding verification requests globally and
two per SSH-host key (local sources share the local key); remote requests can
use at most three slots to leave local capacity. Cancelled requests still count
until their reply or transport disconnection. Explicit opens get priority;
automatic requests begin with the saved selection and MRU. Dispatched entries
rotate, and one widget launch is allowed per event-loop turn. Offline views
consume no slots. Tokens and source generations discard obsolete replies.
The coordinator creates no threads, helper processes, tunnels or listeners.

## Verification

Core tests cover strict record validation, v1 database migration, atomic complete
replacement, cache independence, protected corruption/future records, coalesced
writer shutdown, exact-ID verification, 80 queued views, local capacity,
cancellation, duplicate opens, late replies, source replacement/reconnect,
explicit retry, unsaved/unknown targets and changed container evidence.

GUI tests exercise real packaged Konsole parts, placeholders with no processes,
MRU/selection/focus during background launches, shutdown freeze, exact targets
outside the first history page, rename persistence, offline restarts, and the
startup setting. Packaging and runtime verification results for this build are
recorded in `docs/implementation.md`. Actual desktop/SSH/container acceptance
remains separate from controlled fixtures.
