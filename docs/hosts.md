# Hosts

Archcodex has one host configuration for ordinary Codex servers and myCodex
containers. **Connections → Hosts…** replaces **myCodex hosts…**. Add an SSH alias
once; both discovery options default to on. This Computer is implicit, always
enabled and cannot be removed. Its ordinary Codex discovery is always enabled;
local myCodex discovery can be switched off independently.

Every host has one Projects root. Ordinary servers and myCodex container servers
appear beneath it, with the existing compact project paths and session filters.
The internal server/backend identity remains separate: identical socket paths or
conversation IDs on different hosts or containers do not identify the same session.

## Transport and discovery

Local ordinary servers keep direct local socket access and their existing scanner.
They do not depend on Docker or the systemd-managed container transport. On an SSH
host, the existing Python host helper runs ordinary endpoint discovery alongside
Docker discovery. One foreground SSH connection carries inventory, the GUI's
protocol streams and local TUIs for both types of server. Each attached container
keeps its existing single helper. No additional per-session SSH/helper process,
remote listener, installed service or myCodex modification is required.

Ordinary remote servers use the same procfs/current-user/socket-namespace scanner
as local servers, including the default CODEX_HOME or ~/.codex control socket.
The scanner runs outside the transport event loop. Inotify invalidations trigger
fresh snapshots; initial connection, recovery and explicit Refresh also reconcile
inventory. Session updates come from app-server events. An arbitrary new custom
socket location outside the watched paths may need Refresh.

Only existing servers are discovered. Their protocol exposes loaded sessions and
saved history; conversation files alone do not create a server. Other accounts'
servers are excluded. Docker and container failures are independent of ordinary
host discovery, so a host without Docker can still serve Codex sessions.

Remote host endpoints use registered, unguessable tokens. New opens verify the
socket's owner, device/inode and peer UID/PID. The existing private desktop relay
and SSH security/cleanup policy also applies to these endpoints. The local Codex
executable setting selects the TUI; a discovered remote executable is never run
on the desktop. Container WORKDIR filtering and execution evidence stay specific
to containers.

## Recovery and Refresh

Native and Docker inventories report completeness and refresh revisions separately.
An incomplete native scan retains registered routes; opens still revalidate their
identity. Only fresh, complete evidence authorizes retirement in that scope. A
local scan cannot retire a remote server, nor can Docker absence retire native
host servers. Open, pending and saved terminal references keep their existing
retirement protections.

Changing one host's discovery options preserves the shared SSH connection and
unaffected streams. Disabling a remote host disconnects its streams and stops its
retries, retaining terminal output. Removing a remote host closes its terminal
views; existing cached history retains the prior connection-management semantics.
Neither action stops the independent Codex app-server. Reconnection rediscovers
relay endpoints before restoration. SSH liveness keeps the existing 15-second,
three-missed-reply defaults; normal session operation remains event-driven.

## Compatibility and implementation choices

Host settings are read from `window.ini`'s `hosts` array, falling back to the old
`containerHosts` array. Existing host IDs, SSH aliases and remote enabled states are
preserved. A legacy disabled local entry becomes an enabled This Computer with
only myCodex discovery disabled. Missing local entries are supplied automatically;
saved attempts to disable the local host or its ordinary discovery are normalized.
The existing command ID `mycodex_hosts` is retained to preserve custom shortcuts.

Existing backend IDs and workspace identity calculations remain unchanged. New
ordinary remote profiles use `ssh-unix`, a host-scoped endpoint ID and transient
relay paths that are not persisted. Profiles remain JSON in the existing SQLite
tables; there is no schema migration. **Older releases reject saved `ssh-unix`
profiles**, so downgrading requires a pre-test application-data backup or a separate
`--data-dir` for testing. Existing myCodex and local profiles remain compatible.

The local candidate uses the established Flatpak app ID and stable installation
slot. Package permissions and dependencies are unchanged. Test fixtures cover
mixed native/container routing, Docker failure, slow scans, partial inventory,
endpoint replacement, migration, local-host protection and local TUI argument
selection. Real SSH/Docker and sandboxed KDE acceptance remain desktop checks.
