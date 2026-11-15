# Existing local sessions first

Archcodex 0.3.2 starts by finding existing Codex servers, then requests their
loaded sessions and history. Projects are groups of those sessions, not objects
the user must create. One server can contain multiple sessions; the number of
TUIs is not used to infer the number of execution servers.

## Host discovery

The GUI executes its packaged discovery script using `/usr/bin/python3 -I -u`.
Inside Flatpak this goes through `flatpak-spawn --watch-bus --host`, using the
already granted `org.freedesktop.Flatpak` permission. Python uses its standard
library only. The script is passed as a structured argument, not interpolated
shell code, and is not installed on the host as a service.

At startup and on explicit Refresh:

1. Read the host's `/proc/net/unix` named Unix stream endpoints.
2. Match socket descriptors to current-user Codex processes using procfs UID,
   executable, command arguments, namespace and descriptor metadata. Exclude
   app-server updater/proxy/generator commands.
3. Require an accessible socket owned by the current user. For another mount
   namespace, require the socket node to be the same inode/device through the
   process root and the host path. Otherwise omit the inaccessible endpoint.
4. Resolve advertised symlinks to canonical socket paths and reuse existing
   configured profiles, including the default-socket profile, where matched.
5. Connect through the normal Codex protocol adapter. Successful initialization
   establishes protocol compatibility; discovery alone never claims a session
   is running, waiting or idle.

There is no account-file, credential, conversation-file or repository scan.
Procfs restrictions can make coverage incomplete; the UI reports inaccessible
processes rather than claiming exhaustive discovery. Other users' sessions are
excluded. Container-private sockets are not made reachable by discovering their
processes. This does not implement a myCodex/Docker adapter.

## Events and boundaries

The helper subscribes to Linux inotify changes in `/tmp/codex-daemon-<uid>`, the
default `$CODEX_HOME/app-server-control` socket location (falling back to
`~/.codex`), and parents of known/configured socket paths. If a directory does
not exist, its nearest existing ancestor is watched for its creation. Creation,
rename, deletion and event-queue overflow invalidate the discovery snapshot.

When idle, the helper blocks in `selector.select()` with no timeout. There is no
background scan interval, session query timer, process-statistics collection or
terminal-output interpretation. A socket event leads to a discovery snapshot;
an established app-server supplies session updates through protocol events.

A new arbitrary custom location outside these watches requires the Refresh
action or a manually configured connection. New ordinary Codex sockets in the
known runtime directory are found through events. A socket bound before the
server calls listen is a candidate; the existing finite connection/backoff logic
handles readiness without treating the filesystem as a session-state source.

Discovery is bounded to 4,096 endpoints and reports reaching the limit. Transport
messages remain bounded to 16 MiB. The helper exits when its GUI input pipe
closes; no daemon or login service is created.

## Actionable discovery diagnostics

The helper reports structured issues rather than a generic warning string. It
separates ownership checks, unidentified same-user processes, confirmed Codex
processes, procfs access failures, directory-watch failures and the endpoint limit.
Permission failures include the operation, exact path and errno name/message.
Each affected process contributes its first failed operation, avoiding duplicate
warnings for its many descriptors. Reports contain up to 12 examples per issue
category and an explicit omitted-entry count. Normal process-exit races remain
ordinary omissions, not warnings. Directory-watch errors are reported from the
final watch configuration, avoiding duplicate reports from the two setup passes.

An executable link can be inaccessible before the scanner knows whether the
process is Codex. The UI says so explicitly and does not claim missing sessions.
Known Codex inspection failures are separate; directory-watch failures explain
that manual Refresh still works and connected session events continue normally.
No additional process environment, configuration content or credential reads are
introduced to produce these diagnostics.

The status bar links to an on-demand nonmodal details window; discovery does not
open it automatically or place reports in the project tree. See [UI behavior](ui.md).
Helper startup/exit failures include available OS errors, exit status and a bounded
4 KiB stderr tail. A one-shot 15-second response deadline covers each explicit
refresh as well as startup, preventing a permanently pending retry. This is a
request timeout, not a recurring scan or status query. Reports are not persisted.
The report and helper protocol are internal to the matching application package;
there is no new public integration API or dependency.

## Grouping and attachment

Every server has its own root, including servers on the same host or sharing a
Codex configuration. Session identity remains `(backend ID, thread ID)`; shared
storage does not imply that live instances are interchangeable. Folder paths are
compacted relative to the common root of the visible working directories on that
server. Only branching folders and folders containing sessions are retained.
Full paths remain in details; paths are compared lexically in the server's
namespace, without resolving them through the GUI sandbox.

Automatic server names use the following evidence:

- The discovered default control socket is **Main · user@host**.
- A different `codexHome` returned by `initialize` is **~/.codex-work · user@host**,
  or its full path when it lies outside that user's home.
- Other instances use **Codex · launch-directory · user@host** where available.
- Identical automatic names gain a numeric suffix ordered by persisted profile ID.
  Manual names are preserved. Very long automatic labels are truncated; details
  retain their full values. PID is shown only in details.

Discovery reads the matching process's cwd and PID and obtains user/home/hostname
from standard Linux account/host interfaces. The handshake supplies the server's
actual Codex home and user agent. No process environment, configuration content or
account credential file is read. A launch directory is context, not an assertion
that the server owns only that project.

Loaded-thread inventory is independent of the first history page. Opening a
session first requests `thread/read(includeTurns=false)` on its selected server;
the TUI then uses that profile's executable and explicit socket:

```text
<profile executable> --remote unix://<profile socket> resume -- <thread ID>
```

Discovery itself sends no thread/start, thread/resume, turn/start or approval
response. Folder search stays within its selected server and includes descendant
working directories. A new session targets that same selected server.

## Evidence and remaining host check

Tests cover fake procfs plus real Unix socket nodes, symlink aliases, updater
exclusion, current-user filtering, private namespaces, inotify creation/deletion,
an idle loop without a timeout and helper exit on pipe closure. An optional real
Codex test starts an isolated server after the helper and verifies event-driven
discovery. A window test feeds two discovered fixture servers through the real
bridge, verifies automatic session loading, separate server roots, and server-scoped
search. These tests use no account credentials or model turn.

The host-side Flatpak command boundary still requires desktop validation because
the build container cannot execute a full Flatpak sandbox. Run installation and
testing from a host KDE terminal, not inside myCodex.

The installed CLI used for the isolated tests is Codex 0.166.0. Current protocol
documentation describes [loaded threads, metadata reads and status events](https://learn.chatgpt.com/docs/app-server)
and the [shared-server CLI](https://learn.chatgpt.com/docs/cli/reference).

## Explicit inventory reconciliation

Refresh reconciles local servers and each enabled myCodex host independently. Startup
and filesystem/Docker events continue to update discovery without retiring saved profiles.
The Refresh action retains its existing internal command ID (`discover`), preserving
configured keyboard shortcuts.

An automatic profile can be retired only after a fresh inventory confirms its endpoint
absent. Manual profiles, disabled hosts, explicitly disconnected connections and profiles
with automatic connection disabled are retained. Open terminal views (including retained
output), pending starts/forks/verification, and successfully saved workspace references
protect their connection. An unreadable workspace prevents retirement.

Local scans must report no access/truncation problems. Container reports carry separate
freshness for Docker enumeration and each endpoint scan. Successful enumeration can prove
that a container is gone; a running container needs its own complete endpoint inventory.
Paused, initializing, failed or malformed scopes are unknown. Healthy scopes reconcile
without waiting for another host or container. No path names receive special treatment.

One request token travels through discovery and the shared host/container transport.
Superseded helper reports are rejected. Repeated Refresh requests coalesce into one pending
refresh, and a 60-second one-shot deadline reports unfinished scopes as unchecked. This
deadline does not query servers. Existing client streams survive a transport refresh.

Retirement is an ordered metadata-store operation. Later stale cache saves are blocked,
and the UI removes the connection only after database confirmation. Rediscovery or an
open request cancels retirement and restores the cached source if the deletion already
committed. Removing an eligible connection cascades through its local project/session
cache, including hidden history. It does not touch Codex files or add a manual ignore
marker. A returning endpoint is discovered normally.

The status bar shows progress/removals; its tooltip and bounded Discovery details report
new/updated/removed counts and unchecked scopes. Storage failures retain the UI entry and
appear in those details. Each host applies results asynchronously; Refresh does not wait
on remote work in the GUI thread.
