# SSH connections and authentication

Use **Connections → Hosts… → Add SSH host…** or **Edit…** to specify an SSH
alias, hostname or `user@host`. User, port and identity-file fields are optional;
blank values inherit the local computer's OpenSSH configuration. Specify a user
in either the target or the User field. Identity paths refer to the local
computer, including `~/.ssh/…`, rather than to the Flatpak filesystem. Jump hosts,
certificates and other advanced options remain in `~/.ssh/config`.

Select **Connect / retry** to authenticate. Archcodex displays OpenSSH's password,
key-passphrase and keyboard-interactive challenges in a native, nonmodal dialog.
Answers are masked by default, with an explicit Show answer checkbox. Multiple
challenges are answered in sequence. The host stays identified in the title;
other hosts and terminal views remain usable. A hardware-key touch notice can
also be displayed. Hardware tokens and real PAM/MFA providers require desktop
acceptance testing.

Startup and automatic recovery first try without user interaction. If OpenSSH
needs an answer, that attempt stops and the host reports **Authentication requires
your input. Select Connect.** Cancelling or rejected authentication stops retries
until another explicit Connect. Network failures retain the existing bounded
reconnection backoff, but recovery never replays permission to prompt. Healthy
session status still comes from app-server events.

Host-key checking follows OpenSSH configuration. With the normal `ask` policy,
an unknown host's full OpenSSH confirmation is shown; enter `yes` only after
checking its fingerprint. A stricter configured policy remains strict. Changed
host keys are not automatically accepted. OpenSSH maintains its normal
`known_hosts` record; Archcodex does not maintain a separate trust store.

## Implementation and credential lifetime

The existing foreground SSH process remains owned by the desktop transport's
transient systemd user service. One SSH connection per host carries discovery,
observers and local Codex TUIs. Authentication does not add persistent remote
processes, TCP listeners, tunnels, a container service or a runtime dependency.

The desktop transport stages a small, credential-free Python askpass executable
and an owner-only Unix socket inside its existing mode-0700 instance directory.
`SSH_ASKPASS_REQUIRE=force` routes OpenSSH challenges there while SSH stdin/stdout
remain the transport protocol. The bridge checks peer UID and a random attempt
token. Its short-lived process exists only while OpenSSH requests an answer.
Prompts and replies cross the existing local GUI/helper pipe, never the remote
application protocol. A full snapshot retains unanswered prompts until completion,
so normal snapshot coalescing cannot lose a challenge. Replies include host,
attempt and prompt IDs; an obsolete answer cannot authenticate a new attempt.

Passwords, passphrases and challenge answers are never put in settings, files,
process arguments, environment variables, logs, notifications or terminal output.
They pass through bounded in-memory widgets, messages and pipes and are discarded
after submission. There is no credential cache, wallet integration or automatic
key addition. Existing SSH agents can supply keys. The host launcher forwards its
`SSH_AUTH_SOCK` to this service only, without importing environment globally.
Core dumps are disabled for the service, askpass bridge, and GUI when it presents
an authentication prompt. This also disables later GUI crash dumps for that run;
it is not a guarantee of memory zeroization or protection against a compromised
same-user process or administrator.

Connection establishment retains a ten-second network connect timeout and allows
120 seconds for authentication and initial protocol startup. The control pipe
acknowledges state updates immediately; waiting for a person does not consume its
30-second request deadline. Host disable/removal/edit, cancellation, application
exit and expired connection attempts withdraw prompts and reap owned children.
The existing systemd cgroup and ExecStopPost cleanup also remove this instance's
askpass executable/socket after abnormal termination. The selected local host must
allow execution in the protected temporary instance directory.

SSH still disables inherited forwarding and uses an owned foreground connection,
not an unrelated ControlMaster. Existing externally managed masters are left
alone. Accounts that only work through an already-authenticated external master
must authenticate a fresh connection for Archcodex.

## Host edits and compatibility

Editing connection parameters disconnects that host's old routes and assigns a
new discovery generation. Cached sessions and retained terminals cannot silently
retarget to a different user, port or identity file. Rediscovery creates new server
identities for that generation. Retained old entries are grouped separately under
Previous connection, keeping their original host label. This is intentionally conservative even for an
identity-file-only edit. Other hosts are unaffected.

`window.ini` now stores authoritative `hostsV2` metadata, migrating the existing
`hosts`/`containerHosts` records on first save. Default profiles retain their IDs.
A compatibility `hosts` snapshot disables profiles with explicit SSH options or
an edited generation so an older client does not silently use the wrong target.
Changing that older snapshot after downgrading does not override an existing
`hostsV2` configuration when upgrading again. No passwords are part of either
format. Existing backend/workspace compatibility limitations still apply; see
[Hosts](hosts.md).

## Validation

Standard-library tests cover repeated challenges, cancellation, stale responses,
background authentication suppression, waits beyond the old ten-second handshake
limit, and the existing multi-client transport routing/cleanup. Qt tests exercise
nonmodal prompts, preserving typed answers across snapshots, clearing replies,
host withdrawal, option validation and persistence/downgrade behavior.

`uv run --with paramiko python -B tests/test_sshauth.py` additionally uses the real
local OpenSSH client against temporary loopback SSH fixtures for password,
encrypted private key, two keyboard-interactive challenges, unknown-host
confirmation and background suppression. Paramiko is an optional test dependency,
not shipped in the Flatpak. The fixtures use temporary keys/configuration and do
not access a user's SSH accounts, agent, credentials or known_hosts. Real desktop
Flatpak activation, agent availability, PAM and hardware-token behavior must still
be tested on the intended desktop.
