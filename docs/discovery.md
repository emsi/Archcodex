# Existing local sessions first

Archcodex 0.2.0 starts by finding existing Codex servers, then requests their
loaded sessions and history. Projects are groups of those sessions, not objects
the user must create. One server can contain multiple sessions; the number of
TUIs is not used to infer the number of execution servers.

## Host discovery

The GUI executes its packaged discovery script using `/usr/bin/python3 -I -u`.
Inside Flatpak this goes through `flatpak-spawn --watch-bus --host`, using the
already granted `org.freedesktop.Flatpak` permission. Python uses its standard
library only. The script is passed as a structured argument, not interpolated
shell code, and is not installed on the host as a service.

At startup and on explicit Discover:

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

A new arbitrary custom location outside these watches requires the Discover
action or a manually configured connection. New ordinary Codex sockets in the
known runtime directory are found through events. A socket bound before the
server calls listen is a candidate; the existing finite connection/backoff logic
handles readiness without treating the filesystem as a session-state source.

Discovery is bounded to 4,096 endpoints and reports reaching the limit. Transport
messages remain bounded to 16 MiB. The helper exits when its GUI input pipe
closes; no daemon or login service is created.

## Grouping and attachment

Servers verified to share the host mount namespace use the `local-host`
environment identity. Their sessions with the same exact working-directory path
share a project row. Other namespaces/endpoints retain separate environment
identities. A conversation still retains `(backend ID, thread ID)` for routing;
equal thread IDs across separate servers are not silently merged.

Loaded-thread inventory is independent of the first history page. An older
running or idle loaded conversation can appear even if its saved timestamp puts
it beyond that page. Opening its TUI attaches to the existing backend. Discovery
itself sends no thread/start, thread/resume, turn/start or approval response.

Project search includes every backend in that project's environment. New-session
creation remains a secondary menu action; if a shared project has several
connected servers, the user chooses its target server.

## Evidence and remaining host check

Tests cover fake procfs plus real Unix socket nodes, symlink aliases, updater
exclusion, current-user filtering, private namespaces, inotify creation/deletion,
an idle loop without a timeout and helper exit on pipe closure. An optional real
Codex test starts an isolated server after the helper and verifies event-driven
discovery. A window test feeds two discovered fixture servers through the real
bridge, verifies automatic session loading/grouping and searches their shared
project. These tests use no account credentials or model turn.

The host-side Flatpak command boundary still requires desktop validation because
the build container cannot execute a full Flatpak sandbox. Run installation and
testing from a host KDE terminal, not inside myCodex.

The installed CLI used for the isolated tests is Codex 0.166.0. Current protocol
documentation describes [loaded threads, metadata reads and status events](https://learn.chatgpt.com/docs/app-server)
and the [shared-server CLI](https://learn.chatgpt.com/docs/cli/reference).
