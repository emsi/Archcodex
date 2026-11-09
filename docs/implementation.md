# Implementation: 0.3.0

This is an installable development version for host testing, not completion of every
stage in the project design. The implementation uses C++20, Qt Widgets, KDE
Frameworks, Konsole KPart, Boost.Beast and SQLite. There is no JavaScript/TypeScript
application UI.

## Delivered behavior

- Multiple named connections to existing local Unix-socket app-servers. A blank
  socket selects Codex's host default; explicit socket paths remain supported.
- Startup discovery of existing host-visible local Codex servers, with inotify
  events for new normal sockets and an explicit Discover action. No status polling.
- Separate roots for every server, with meaningful configuration/user/host names
  and compact folder paths derived from conversation working directories. PID is
  confined to details; no manual project setup is required.
- Persisted **View → Hide unsaved sessions**, enabled by default, with an exception
  for open terminal views so retained errors remain accessible in Active.
- An IDE-style left navigator with All, Active, Running and Waiting for input tabs, recent-use ordering,
  expandable sessions, backend labels and accessible status descriptions.
- Running, waiting (including idle prompts), saved, error and unknown states from protocol events.
  Initial/reconnect inventory queries are finite. There is no recurring status
  query, terminal-output heuristic or filesystem-based status inference.
- Konsole terminal views created only when explicitly opened. Existing views are
  reused by backend/thread ID and retained until closed. One is visible at a time.
- A new-session action launches the host Codex TUI against the selected backend
  and working directory. No task prompt or approval-policy override is injected.
- Server-side conversation search on Enter, across all servers or within the
  selected server/folder subtree. Every result page is considered before filtering
  is complete; the result summary reports entries hidden by the saved-state filter.
- Persistent backend profiles, legacy display names, list metadata and local recent
  use. Full conversation bodies and authentication tokens are not cached.
- Automatic reconnect after transport failure with bounded backoff, explicit
  disconnect, and manual connection controls. Reconnect takes a fresh snapshot.

The first history page contains 200 sessions per backend. Load more history
fetches further pages on demand. Loaded sessions are also enumerated at connect,
so a live older conversation need not occur on the first history page to be
visible. Projects beyond the loaded history are discovered as further pages are
requested. This is deliberately not presented as an exhaustive disk scan.

## Structure and boundaries

| Code | Responsibility |
| --- | --- |
| `src/discovery.*`, `src/discover.py` | Host socket discovery and filesystem-event subscription. |
| `src/backend.*` | Shared Codex request/event handling, inventory, recovery and search. |
| `src/localbackend.cpp` | Local Unix adapter's transport launch arguments. |
| `src/bridge.cpp` | Beast WebSocket framing over a native socket or host Codex proxy. |
| `src/store.*`, `src/types.*` | SQLite metadata, validation and state interpretation. |
| `src/sessionmodel.*` | Server/folder/session tree and shared state filtering. |
| `src/terminal.*` | Konsole embedding and structured TUI launch arguments. |
| `src/window.*` | Native interface, automatic connection reconciliation and connection dialogs. |

The internal backend contract allows later adapters to reuse the protocol and
models. There is no public plugin ABI or installed companion service. The packaged
discovery script runs with host Python 3 through the existing host-command boundary;
it lasts only as long as the GUI. See [discovery details](discovery.md).
The GUI never sends
turn/start, approval responses, cancellation or server-stop requests. New
conversation interaction happens through the explicitly launched TUI.

The bridge is a separate small process per connected backend so blocking host
process setup cannot stall the GUI. Beast implements the WebSocket protocol;
there is no handwritten WebSocket parser. GUI-to-bridge messages are compact
JSON lines; bridge-to-GUI messages use a four-byte big-endian length followed by
JSON bytes. Individual messages and outgoing queues are bounded at 16 MiB.
Metadata reads run with four requests in flight, with a 4,096-entry pending bound.

Connection generations discard stale replies. Status notifications override
concurrent inventory snapshots, and removal events prevent older snapshots from
reintroducing archived sessions. Saved metadata becomes unknown after disconnect.
Request deadlines and reconnect delays are one-shot transport controls, not
session polling.

## Flatpak contents and permissions

The package targets x86_64 Linux and uses KDE Platform 6.11, Konsole 26.08.2 and
Boost 1.90.0 headers. Dependency source hashes are recorded in the manifest. The
GUI and bridge are built against that SDK; the host does not need matching Qt/KDE
libraries or a separate Konsole installation.

The application-specific permissions are:

- X11 and shared IPC, with the Qt xcb platform selected.
- Access to `org.freedesktop.Flatpak` for host commands.

The KDE runtime also provides its normal desktop-settings integration. This
local-only package does not add direct network, DRI, home/host filesystem, SSH
agent, Docker socket or unrestricted bus access. Later backends must justify any
additional permission.

Host-command access can execute programs as the user outside the sandbox.
Flatpak therefore provides packaging/runtime isolation, not a security boundary
against the application itself. The GUI uses structured argument vectors and a
constant shell `exec "$@"` wrapper for the TUI; paths and thread IDs are never
interpolated into shell code. Existing-thread IDs follow the CLI's `--` argument
separator.

Host Codex remains responsible for its normal credential handling. The GUI does
not open or copy Codex account credential files. Local socket ownership/access
and the selected server are part of the user's existing trust configuration.

Konsole's activity/silence/process tracking actions are disabled in the embedded
view. A small, pinned [Konsole patch](../packaging/konsole-event-only.patch) also
disables its activity-triggered process/title snapshots when built with
`ARCHCODEX_EMBEDDED_TERMINAL`. Archcodex already owns the titles and status, so
that process inspection has no purpose here. The patch does not change PTY
rendering or input and must be re-reviewed when updating Konsole.
A second [lifecycle patch](../packaging/konsole-embedded-lifecycle.patch) exposes
Konsole's existing hold-open setting and a process-exit signal. Archcodex opts in
to retain exited clients and displays an explicit Restart client action. That
also avoids Konsole's default nonzero-exit notification lookup using the embedding
application's name. No global Qt/KDE warning suppression is used. A matching
patched KPart is now required; native builds report a clear capability error if
an unpatched plugin is loaded.

Application data is stored under the Flatpak's application-specific XDG data
directory. The SQLite file has owner-only permissions. A Linux `flock` prevents
two instances from modifying the same app data concurrently, including across
PID namespaces; the lock is released automatically on process exit. Removing a
connection deletes only its local cache/configuration, not Codex history.

## Validation performed

- Core protocol tests use a real Unix stream/WebSocket fixture and the actual
  bridge executable. They exercise pushed updates, no steady-state polling,
  snapshot/event races, archive races, pagination and disconnect state.
- Storage tests verify persistence, disconnected cache state, permissions and
  deletion of dependent local metadata.
- Launch-argument tests exercise shell metacharacters and the CLI argument
  separator without executing embedded input.
- Model tests cover 80 sessions across two backends, Active filtering, waiting
  and unknown states, separate server roots, compact folder paths, backend routing, empty-folder removal
  and preservation of legacy display names.
- Discovery tests cover aliases, updater exclusion, same-user visibility, namespace
  isolation, inotify creation/deletion and an idle loop with no rescan timeout.
- A real isolated Codex server started after discovery is picked up by an inotify
  event. No model turn or account credentials are involved.
- Window integration tests discover two fixture servers without project setup,
  load a session outside the history page, preserve separate roots, validate
  selected-server search/open routing, and persist the unsaved-entry setting.
- Discovery, core, model and window integration suites pass in the KDE 6.11 SDK. Core tests also pass with address
  and undefined-behavior sanitizers; leak detection was disabled for that run.
- An isolated real Codex 0.166.0 app-server passes initialize, inventory, search
  and observer-disconnect checks. It uses a temporary Codex home and a minimal
  environment, with no account credentials or model turn.
- A native X11 smoke test loads the compiled Konsole component, creates a real
  PTY, runs a shell command and captures the application window. This also passes
  using only the KDE Platform files and packaged application, without SDK libraries.
- The bundle imports and deploys into an isolated project-local Flatpak
  installation. Its application ref and effective permissions are inspected.

The build environment prohibits the namespaces used by Flatpak's sandbox. The
initial artifact is compiled in a separate root containing the actual KDE SDK
files, then finalized/exported with Flatpak's packaging tools. The icon validator
runs without its sandbox for this local export of the repository's own SVG.
The normal build script uses standard flatpak-builder on a host with namespaces.
The installation check uses a temporary D-Bus session because the container has
no system bus; no desktop services or policy settings are changed. Flatpak's
deployment succeeds, while sandboxed cache-update triggers report the same
namespace restriction. Full sandbox execution remains a host acceptance test.

A complete Flatpak launch, its D-Bus host-command boundary, and an authenticated
long-running Codex turn surviving terminal/GUI closure still require host
validation. The application/runtime load and bundle installation checks do not
substitute for those tests. AppStream reports the lack of a public homepage;
none is invented for this unpublished initial package.

## Host acceptance test

1. Install the bundle using the README commands and start it from KDE under X11.
2. Leave existing Codex sessions running before opening Archcodex. Confirm their
   servers and sessions appear without adding connections or creating projects.
   Start another Codex server normally and verify discovery reacts to its socket.
3. Open a conversation; verify that the real Codex TUI accepts keyboard input,
   selection, clipboard operations, scrolling and resizing.
4. Start work in the TUI. Verify the session/project indicators and Active tab
   change when the server reports running, waiting and idle states. An idle prompt
   remains in Active and Waiting for input, but not Running.
5. Search for text from an older conversation, including a project-specific
   search with matches beyond the first result page.
6. Close a terminal during a disposable test task, then reconnect to the same
   conversation. Repeat with GUI exit. Verify the server task was not cancelled.
7. Open a second session whose terminal client exits. Its output and exit status
   must stay visible, with Restart client and Close terminal available. Verify the
   first terminal is still available and that a failed restart remains readable.
8. Disconnect/reconnect one profile and confirm cached status becomes unknown,
   then is refreshed on reconnection; other backends remain usable.

Do not use a foreground server that dies with its launching shell to assess the
independent-server persistence requirement.

## Current limits and defaults

- TCP, SSH and myCodex adapters are not implemented. Native Wayland is not
  qualified; the initial package uses X11/XWayland.
- Conversation search requires the server's experimental search API. Failure is
  reported explicitly; there is no silent title-only fallback or local FTS index.
  Search is capped at 10,000 results per backend with an explicit narrowing notice.
- Active includes loaded idle prompts and errors as well as busy turns. Running
  and Waiting for input require confirmed current state; disconnected last-known
  active rows remain visible only in All/Active with hollow dots.
- Folders map to lexical working-directory paths within each server. Shared
  filesystems do not merge server roots. Linked-worktree grouping is future work.
- Host discovery requires Python 3 and readable same-user procfs metadata. Custom
  sockets outside watched locations may require Discover; private container sockets
  require future integration or a separately configured exposed endpoint.
- Terminal views are retained until explicitly closed; they are not evicted in
  the background. A new-session TUI has no thread ID at launch, so reopening its
  subsequently discovered row can create an additional client to the same server
  conversation. Its launcher stays accessible as **New session terminal** until
  closed, and is never saved as a conversation. It does not create another execution server.
- Cached history can contain stale entries outside the loaded pages. Finite
  metadata validation at inventory/load-more and before attachment hides confirmed
  missing entries; unrelated failures remain explicit errors. Older cache records
  without persistence fields stay visible pending fresh evidence. Archived-history browsing, unread-completion receipts,
  background desktop notifications and a native conversation view are not yet
  implemented.
- The measured 80-session check covers model behavior, not a performance claim
  for 80 simultaneously open terminal renderers or live model turns.
- The bundle is for local installation/testing, not a Flathub submission or public
  release. No remote repository, release tag or publication is created.

Build downloads, SDK roots, test data and logs are kept in ignored `.build/`.
Installable artifacts and checksums are in ignored `dist/`. The full GPL license
and bundled-component notices accompany the package.

## Initial implementation decisions and subsequent changes

The user authorized initial implementation and an installable Flatpak, with the
stack, layout, event-driven operation and backend sequence already agreed. The
following details were chosen during implementation rather than agreed beforehand:

| Choice and reason | Affected files / consequence | Status |
| --- | --- | --- |
| Deliver local Unix connections first, so a complete first adapter can be tested before adding remote transports. | `src/localbackend.cpp`, shared `src/backend.*`; TCP, SSH and myCodex remain later stages. | Implemented; host integration acceptance remains. |
| Initial version 0.1.0 (now 0.2.1), `io.github.emsi.Archcodex`, GPL-3.0-or-later, original SVG icon, KDE 6.11, Konsole 26.08.2 and Boost 1.90.0. | `CMakeLists.txt`, `LICENSE`, `data/`, `packaging/`; GPL-compatible open-source distribution and x86_64 test artifact. Metadata uses CC0-1.0. | Packaged locally; no public repository, tag or release created. |
| X11/xcb for this first package; host commands through Flatpak, without direct network/filesystem/DRI grants. | Manifest and `src/terminal.cpp`, `src/bridge.cpp`; XWayland may work, native Wayland is unqualified. Host-command permission is broad execution authority. | Runtime closure and native X11 validated; sandbox host boundary still needs host testing. |
| Separate Beast helper per backend, bounded buffers/metadata concurrency, finite request deadlines, automatic reconnect and startup connection by default. | `src/backend.*`, `src/bridge.cpp`, `src/types.h`; GUI stays responsive without recurring session queries. Profiles can disable startup connection. | Protocol and reconnect/disconnect mechanisms implemented; tests cover the documented cases. |
| Metadata-only SQLite storage and Linux instance lock, 200-entry history pages and 10,000-result search limit. | `src/store.*`, `src/main.cpp`, `src/backend.cpp`; no offline full-text index or credential cache. Search depends on the experimental server API. | Persistence, permission, pagination and protocol checks pass. |
| Original provisional Active meant running/waiting (superseded in 0.2.1); disconnected last-known activity stays visible, exact working directories identify projects. | `src/sessionmodel.*`, `src/window.*`, `src/terminal.*`; these are provisional interaction defaults, including the new-session client limitation above. | Implemented; tested with an 80-session model fixture, not 80 live tasks. |
| Disable Konsole tracking actions and patch its process/title snapshots, because Archcodex uses server events for that information. | `src/terminal.cpp`, `packaging/konsole-event-only.patch`; upstream changes require patch review. | Patched terminal compiles and passes a real-PTY smoke test. No upstream changes submitted. |
| Install development/packaging tools in the build container and use a separate KDE SDK root when namespaces prevent normal flatpak-builder execution. | Container packages include Flatpak, Qt/KDE development tools, Boost, Xvfb, validators and sanitizers. `.build/` retains approximately 9 GiB of runtimes, sources, roots, fixtures and logs; `dist/` holds local artifacts. | Local bundle exported; icon validation ran without its sandbox. No normal user app installation, host service change or credential access. |
| Commit the authorized source/documentation changes and provide a source archive alongside the bundle. | Repository plus ignored `dist/`; generated outputs are excluded from Git. | Required by the standing commit instruction; the delivered build record identifies the source commit. No push or publication. |


Version 0.2.0 implemented the user's correction that existing sessions are the
primary workflow. Additional implementation choices are: host Python 3 with no
third-party modules; same-user procfs discovery plus inotify; canonical socket
identity; shared-host project grouping; retaining legacy display names without
empty project rows; and remembering automatic endpoints as ordinary profiles.
Disconnect pauses an endpoint for this GUI run. Removal suppresses rediscovery
for this run; a later startup may discover the existing server again. Editing a
discovered profile makes its executable/settings user-managed. Automatically
managed profiles follow the detected server executable when its release changes.
No new Flatpak permissions or installed host service are added. The source,
bundle, source archive and checksums are updated; no public release is made.


## 0.2.1 regression validation

The terminal regression first reproduced both reported warnings and loss of the
new view using a real PTY and a disposable client exiting with code 7. The fix is
tested for retained output through terminal accessibility, explicit restart, clean exits, signals, missing executables,
separate existing terminals and cleanup. No authenticated model turn is involved.
The exact error from the user's host Codex client remains unverified; the new
inline exit message and retained output make it inspectable.

State tests cover ordinary prompts, executing turns, structured input, approvals,
loaded errors, unknown future flags, saved history and disconnected states across
all four views. Reconnect tests cover an old live row outside the history page,
both successful and failed loaded inventories. Closed/restarted event races are
covered. An identical stream of 80 metadata updates now causes zero data/layout
notifications. Window tests exercise filtering, expansion/selection retention and
stable tooltip text while events arrive.

The SDK-root test setup has no /dev/pts, so the PTY suite is run separately with
the same compiled executable and packaged Konsole libraries outside that root.
The full Flatpak sandbox/host-command boundary still needs host acceptance under
the existing container restriction. Tooltip stability on the user's exact KDE
session is not claimed from an offscreen test.


The 0.2.1 core/discovery/model/window suites passed in the KDE SDK. The real-PTY
suite also passed under X11 with no reported Qt/KDE warnings, covering error,
clean exit, signal termination and missing-program cases. An X11 smoke launch
using only the KDE Platform plus packaged application libraries was inspected.
Core/discovery tests also passed under ASan/UBSan (leak detection disabled).
The bundle was imported into the project-local Flatpak installation and its
runtime/ref/permissions were checked; sandboxed deployment triggers still hit the
container's existing namespace restriction. No normal user installation or
external publication was performed.

## 0.2.1 choices made during this fix

The user requested the four views, a robust design, investigation/fix of the
terminal disappearance and warning messages, tooltip investigation, and a language
assessment. The following implementation details were selected in that scope:

- Live membership uses server runtime status. Disconnected last-known live rows
  remain in Active as unknown; Running/Waiting require confirmed state. Idle
  prompts, explicit questions and approvals share Waiting, but ordinary prompts
  use a neutral dot and explicit requests use amber. Loaded errors remain Active.
  `src/types.*`, `src/backend.*` and tests implement these rules; connect/reconnect
  inventories and pushed events maintain them without polling.
- The navigator batches changed roles, ignores identical updates, retains view
  expansion and matching selection, and starts with a 390-pixel sidebar. Tooltips
  show a stable escaped snapshot beside a row until reopened. `src/sessionmodel.*`
  and `src/window.cpp` implement this; saved geometry remains respected. Host
  KDE/theme hover behaviour still needs acceptance testing.
- Exited clients (including clean exits) keep their buffers and gain Restart
  client. Existing-conversation restarts reuse the thread ID; new-session
  launchers repeat the new-session action. `src/terminal.*` and the pinned Konsole
  lifecycle patch implement this with exit events. The package must contain the
  matching patched plugin; upgrades require patch review. This fixes disappearing
  diagnostics, but does not establish the underlying cause of the user's Codex
  exit. No global logging suppression or automatic retry was introduced.
- Keep C++/Qt Widgets; document the alternatives in `docs/design.md`. No framework
  migration, new runtime dependency or additional Flatpak permission was added.
- Deliver development version 0.2.1, updating CMake, AppStream, README and the build
  script. Reuse the existing isolated KDE SDK-root build fallback because this
  container cannot run Flatpak namespaces; finalize/export and import the bundle
  with the existing Flatpak tools. Tests, debugger trace and screenshot remain in
  ignored `.build/lifecycle-test/`; bundle/source/checksums remain in ignored
  `dist/`. Disposable fixture processes/directories and Xvfb are cleaned up by
  their test harnesses. No host settings, credentials or services were changed.
- Commit the task's source, tests, patch and documentation under the standing
  commit instruction. No push, tag, upstream submission or public release.


## 0.3.0 server explorer and saved-state correction

The user requested default-on configurable hiding of unsaved entries, retained
error terminals in Active only, separate meaningful server roots without PID in
the visible name, and compact directory chains. This section supersedes the
older shared-host project grouping described in the historical decisions above.

### What the reported Codex error establishes

Codex 0.166.0's remote resume lookup calls the selected app-server's `thread/read`.
Its [TUI implementation](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/tui/src/lib.rs)
(`lookup_session_target_with_app_server`, `no_resume_target_exit`) turns any failed
lookup into the generic “No saved session found” message. The message alone does
not establish a missing file or a wrong configuration directory. The user's
particular underlying server error has not been captured here.

Archcodex launches the configured/discovered executable against that profile's
socket, not a default daemon chosen from the session cwd:

```text
<profile executable> --remote unix://<profile socket> resume -- <thread ID>
```

Only an explicitly blank socket profile uses Codex's default endpoint (`unix://`).
Discovery prefers the server process's executable when host-executable, otherwise
`codex` from the host PATH; manually configured executable choices are preserved.
The shell wrapper is constant and all values remain separate arguments.

The server [thread processor](https://github.com/openai/codex/blob/rust-v0.166.0/codex-rs/app-server/src/request_processors/thread_processor.rs)
can read persisted or loaded-only threads. Paginated history need not have a
rollout path. Consequently, filtering uses `ephemeral`, `path`, `historyMode` and
precise missing-thread errors, not a local file-existence guess. Permission,
transport and unrelated server errors never establish that a session is unsaved.
Server configuration identity comes from the `initialize` response's `codexHome`.

### Decisions not agreed in advance

The requested behaviors above were explicitly authorized. The following are the
implementation choices made to deliver them; all are implemented in this version.

| Choice and reason | Affected files/configuration and consequences |
| --- | --- |
| Use `Main`, a shortened custom Codex-home path, or `Codex · launch-directory`, followed by user/host. Preserve manual names; disambiguate duplicate automatic names with a persisted-profile-order numeric suffix. Bound long labels to 150 characters and keep full details. | `src/types.*`, `src/discover.py`, `src/window.cpp`; collects cwd/PID and account/hostname metadata, plus handshake home/user agent, without reading process environments or credentials. Names can refine after connection and duplicate suffixes can change when profiles are added/removed. |
| Keep persistence knowledge separate from terminal presence and server activity. Recognize legacy rollout paths and paginated storage; leave older metadata without evidence visible pending refresh. | `src/types.*`, `src/sessionmodel.*`, SQLite JSON metadata; additive cache fields require no schema migration. Open unsaved/error views override hiding; terminal presence and messages do not survive GUI restart. |
| Validate each loaded history page with bounded metadata reads and validate again on explicit open. Preserve missing-state evidence against older inventories; one follow-up read handles a persistence event overlapping an older read. | `src/backend.*`, `src/window.cpp`; four queued metadata reads in flight, existing 4,096 queue bound, plus an explicit-open request. No periodic query, turn start, approval or file scan. Generic server errors are shown without marking data missing. Only the latest pending activation opens a new view. |
| Retain a synthetic **New session terminal** row until that launcher is closed. Do not infer its eventual thread ID from cwd, timing or text. Focus retained clients even while disconnected. | `src/terminal.*`, `src/window.*`; a discovered conversation may have a separate row/client. Restarting this launcher repeats the existing new-session action. Closing a retained terminal restores the normal visibility/state predicates. |
| Place the saved toggle in View and save immediately; apply it to explorer and search results. Report hidden search-result counts. | `src/window.cpp`, `window.ini`; defaults on as requested, existing preference survives restart. No settings framework or dependency added. |
| Use one root per backend, lexical common-prefix paths, compressed nonbranching folders, stable row keys, recursive expansion/selection restoration and existing MRU ordering. Keep empty configured server roots in All; auto-expand newly seen roots. Preserve legacy folder labels. | `src/sessionmodel.*`, `src/window.*`; same paths on different servers no longer merge. A single cwd keeps its basename below the server; sessions at the common root attach directly to the server. Full common/working paths are in details. Folder keys changed, so old flat-project expansion keys cannot all migrate. |
| Scope local search to the selected server/folder subtree. New-session creation requires selecting a folder or session rather than a server root. | `src/window.cpp`; removes the old shared-project server chooser because the ancestor server now determines routing. Global search still covers all configured servers. |
| Deliver development version 0.3.0 with updated documentation and regression tests. Reuse the existing KDE SDK root and Konsole lifecycle patch. | `CMakeLists.txt`, `data/`, `scripts/build-flatpak.sh`, `README.md`, `docs/`, `tests/`; no dependency, Flatpak permission or host service changes. Local build/test state is retained in ignored `.build/`, with bundle/source archive/checksums in `dist/`. Temporary edit scripts are removed before handoff. |
| Commit completed authorized work, as required by the user's standing instruction. | One local Git commit after checks; no push, tag, Flathub submission or public release. Commit ID is reported at handoff; source archive represents that commit. |

### Validation and remaining boundary

Regression coverage includes both persistence formats, hidden ephemeral/missing
entries, retained error classification and closure, multiple servers sharing
paths/IDs, compressed folder regrouping under Qt's model consistency tester,
80-session filtering, unchanged-update stability, metadata/event races, PID-free
names, selected-server preflight, server-scoped search and setting persistence.
Real-PTY terminal tests exercise normal exit, failure, crash, missing executable,
retained output, refocusing and explicit restart.

The package is validated with the KDE SDK tests, native core sanitizers, an X11
runtime smoke test, and isolated Flatpak import/ref/permission inspection. The
container still cannot run a full Flatpak sandbox; host KDE testing remains
necessary for the actual host-command boundary, authenticated Codex turns,
clipboard and long-running task survival. This change does not prove why the
specific session in the user's report failed its original lookup.


For 0.3.0, all four SDK suites passed (discovery, core, model, window), native
core/discovery passed, and ASan/UBSan core/discovery passed with leak detection
disabled. All four real-PTY exit scenarios passed under X11. The packaged runtime
rendered the two-server synthetic explorer fixture successfully; its SQLite
fixture and screenshot remain under `.build/explorer-test/`. Optional real Codex
integration tests were not rerun for this change. Desktop-file validation passed;
AppStream retains the existing missing-public-homepage warning. Flatpak import
returned success and deployed ref `app/io.github.emsi.Archcodex/x86_64/stable` in
`.build/flatpak`; sandboxed cache triggers reported the known namespace restriction.
Effective permissions match 0.2.1. No normal user installation was modified.
