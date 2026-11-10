# Implementation: 0.5.0

This is an installable development version for host testing, not completion of every
stage in the project design. The implementation uses C++20, Qt Widgets, KDE
Frameworks, Konsole KPart, Boost.Beast and SQLite. There is no JavaScript/TypeScript
application UI.

## Delivered behavior

- Local and SSH myCodex discovery/attachment, container-native proxy/TUI launches,
  and conservative shared-history routing. See [myCodex integration](mycodex.md).
- Multiple named connections to existing local Unix-socket app-servers. A blank
  socket selects Codex's host default; explicit socket paths remain supported.
- Startup discovery of existing host-visible local Codex servers, with inotify
  events for new normal sockets and an explicit Discover action. No status polling.
- Separate roots for every server, with meaningful configuration/user/host names
  and compact folder paths derived from conversation working directories. PID is
  confined to details; no manual project setup is required.
- Persisted **View → Hide unsaved sessions**, enabled by default, with an exception
  for open terminal views so retained errors remain accessible in Active.
- Nonblocking discovery diagnostics in the status bar and an on-demand details
  window with specific failures, remedies, retry, add-connection and copy actions.
- An IDE-style left navigator with All, Active, Running and Waiting tabs, recent-use ordering,
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

The package targets x86_64 Linux and uses KDE Platform 6.11, Konsole 26.04.3 and
Boost 1.90.0 headers. Dependency source hashes are recorded in the manifest. The
GUI and bridge are built against that SDK; the host does not need matching Qt/KDE
libraries or a separate Konsole installation.
The [Konsole version pin](flatpak.md#1-packaging-model) avoids the 26.08 regression
that intercepts terminal scrollback shortcuts when a client requests the enhanced
keyboard protocol.

The application-specific permissions are:

- X11 and shared IPC, with the Qt xcb platform selected.
- Access to `org.freedesktop.Flatpak` for host commands.

The KDE runtime also provides its normal desktop-settings integration. This
package does not add direct network, DRI, home/host filesystem, SSH
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
   remains in Active and Waiting, but not Running.
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

- Direct TCP and generic SSH socket profiles remain unimplemented; local/SSH
  myCodex containers are supported by the new provider. Native Wayland is not
  qualified; the initial package uses X11/XWayland.
- Conversation search requires the server's experimental search API. Failure is
  reported explicitly; there is no silent title-only fallback or local FTS index.
  Search is capped at 10,000 results per backend with an explicit narrowing notice.
- Active includes loaded idle prompts and errors as well as busy turns. Running
  and Waiting require confirmed current state; disconnected last-known
  active rows remain visible only in All/Active with hollow dots.
- Folders map to lexical working-directory paths within each server. Shared
  filesystems do not merge server roots. Linked-worktree grouping is future work.
- Host discovery requires Python 3 and readable same-user procfs metadata. Custom
  sockets outside watched locations may require Discover. Container sockets are
  discovered separately through the myCodex provider.
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


## 0.3.1 filtered paths, expansion and indicators

The user requested compression within each filtered view, the shorter **Waiting**
tab name, default expansion in Running/Waiting, and green waiting indicators.
Previously the model compressed all sessions and a proxy hid nonmatching leaves;
historical sessions could therefore keep long ancestor chains in filtered views.
Filtering now precedes folder grouping, common-root calculation and aggregation.
All metadata remains in the model's record store; switching tabs changes only its
visible tree. Pushed membership changes recompute that server's tree, while
unchanged membership/title updates retain the existing structure. No protocol,
discovery, session lifetime or polling behavior changes.

### Decisions not agreed in advance

The requested UI changes were explicitly authorized. The following choices support
them; no additional feature area is introduced:

- `src/sessionmodel.*`: keep the view predicate in the tree model, with the proxy
  handling sorting and empty-server visibility. Folder counts and parent colors
  now describe matching sessions only, so hidden waiting history cannot color a
  Running root green. Stable server/path/session keys preserve routing and restore
  navigation as paths compress differently. Search still uses its own All view.
- `src/window.*`: interpret default expansion as expanding every Running/Waiting
  branch, including newly arriving ones, while honoring explicit manual collapses
  per view for the current GUI run. These overrides are not a new persistent
  setting. All/Active retain their existing expansion behavior. Preserve the
  browsing selection and expansion when returning from search as well.
- `src/types.cpp`, `src/sessionmodel.cpp`: use green `#27ae60` for every confirmed
  waiting state, including idle prompts and approvals, and propagate it to visible
  ancestors. Keep existing blue/red/neutral/hollow indicators for running, error,
  history and unknown states. Status text still distinguishes approval from a
  normal prompt; the existing waiting/error/running aggregate priority is retained.
- `tests/test_model.cpp`, `tests/test_window.cpp`: add regressions for historical
  ancestor chains, per-view common roots/counts/colors, event-driven regrouping,
  stable unchanged rows, default expansion, manual collapse and search return.
  Adjust the tooltip test to keep its row present while status changes.
- `CMakeLists.txt`, `data/`, `scripts/build-flatpak.sh`, `README.md`, `docs/`: deliver
  patch version 0.3.1 and document the revised interaction. Reuse the existing KDE
  SDK/runtime and Konsole component, with no dependency or permission changes.
  Build/test artifacts remain in ignored `.build/`; the local bundle, matching
  source archive and checksums are placed in `dist/`. Commit the authorized change
  under the user's standing instruction; no push, tag or public release.

Validation includes model/window regressions in the KDE SDK and X11 visual checks
with synthetic session data. The existing container namespace restriction still
prevents running a full Flatpak sandbox here; actual KDE host launch remains the
final acceptance boundary. No account credentials or live model turns are used.


For 0.3.1, discovery/core/model/window suites all passed in the KDE SDK. The
activity-view integration test also passed under X11 and produced inspected
Running/Waiting screenshots in `.build/filter-view-test/`. The packaged KDE
Platform application loaded its Konsole component and real PTY in the X11 smoke
test and reported version 0.3.1. Its bundle imported successfully into the isolated
`.build/flatpak` installation; cache triggers still reported the known sandbox
namespace restriction. Flatpak metadata/permissions match 0.3.0 exactly. No normal
user installation, host service, credential or remote repository was modified.


## 0.3.2 discovery feedback

The user requested removing the verbose discovery/status banner from the project
explorer and replacing vague warnings with actionable modern UX. The old warning
counted permission failures before the process was necessarily identified as
Codex, so it could not establish that sessions were missing. That distinction is
now explicit in the report. Filter explanations remain available in tab tooltips;
only search-result feedback occupies the explorer while a search is active.

### Decisions not agreed in advance

The UI correction and implementation were authorized. These are the supporting
choices, affected files and consequences:

- `src/window.*`, new `src/discoverydialog.*`: use a small application status-bar
  action and one modeless details window, opened only by the user. This avoids
  interrupting terminal input with recurring modal dialogs. A healthy nonempty
  discovery hides the action; checking, failure, incomplete discovery and zero
  local servers have distinct labels/icons. The Connections menu always offers
  details. Connection readiness uses text without counts when all are ready.
- `src/discoverydialog.*`: show last-check time, issue summaries, explanations and
  next steps, with technical detail collapsed initially. Preserve expanded
  sections by issue code and leave identical reports untouched to prevent flicker
  and lost text selection. Retry, Add connection, Copy report and Close are native
  controls; retries are disabled while pending. Recovery clears old issues and
  helper failure replaces stale diagnostic issues. Report copying is explicit;
  reports and stderr are held in memory rather than written to a new log file.
- `src/discover.py`: replace internal warning strings with structured issue
  records. Distinguish unknown ownership, unidentified same-user processes,
  recognized Codex processes, procfs failure, directory-watch failure and endpoint
  limits. Record the actual failed operation/path/errno from existing inspections,
  including denied descriptor/socket checks that were previously silently skipped.
  Count affected processes once and retain the first failure per process, up to
  12 examples per category with omitted counts. Normal exits are not warnings.
  Report only the final directory-watch configuration to avoid duplicate issues.
  No account credentials, process environments or new privilege changes are used.
- `src/discovery.*`: carry structured reports alongside endpoint snapshots; the
  bundled GUI/helper must match. Include available launch/exit errors and the last
  4 KiB of helper stderr. Reuse the existing one-shot 15-second deadline for every
  explicit refresh so a failed retry cannot remain pending forever. Normal
  discovery remains driven by inotify; session status remains server-event based.
- `tests/test_discovery.py`, `tests/test_core.cpp`, `tests/test_window.cpp`: cover
  accurate process classification, paths/errors, bounded samples, harmless exits,
  watch failures, real helper report parsing, hidden explorer feedback, user-only
  nonmodal opening, stable repeated reports, recovery, copy and action signals.
  The real helper parsing test skips inside a Flatpak build sandbox where host
  command access is not available; synthetic tests remain active there. Optional
  screenshot capture uses synthetic data in ignored `.build/discovery-ux-test/`.
- `CMakeLists.txt`, `data/`, `scripts/build-flatpak.sh`, `README.md`, `docs/`: deliver
  patch version 0.3.2. Link the new Qt Widgets dialog without new dependencies or
  Flatpak permissions. Reuse existing SDK/runtime roots and package tooling;
  `.build/` retains local test/build state, and `dist/` holds the bundle, matching
  source archive and checksums. Commit completed work under the standing user
  instruction; no push, tag or public release.

Full Flatpak sandbox execution still requires the user's host: this container
blocks its namespaces. Tests use fixtures and existing read-only discovery;
no authenticated Codex turn or host configuration change is involved.


For 0.3.2, discovery/core/model/window suites passed in the KDE SDK and native
core/discovery suites passed. The two new discovery-window tests passed under
X11; their inspected screenshots are retained in `.build/discovery-ux-test/`.
The packaged KDE Platform application passed its X11 Konsole/PTY smoke test and
reported version 0.3.2. The bundle imported into the isolated project-local
Flatpak installation; cache triggers reported the existing namespace restriction.
Effective package metadata/permissions match 0.3.1. Optional real Codex integration
tests were not run; no model turn was initiated. No normal user installation or host configuration was changed.


## 0.4.0 direct myCodex integration

The user authorized implementation of the direct-container design. The resulting
routes, requirements and routing policy are documented in [mycodex.md](mycodex.md).
No myCodex changes are required. Direct TCP and generic SSH forwarding remain
future adapters; the user's current request brings myCodex ahead of them.

### Decisions not agreed in advance

- `src/containers.py`, `src/containerdiscovery.*`, `src/launch.*`: use an
  Archcodex-owned Python standard-library adapter, supplied through argv with no
  installed helper or public myCodex command. Docker CLI and Python 3 are required
  on selected hosts; Python 3 and compatible Codex are required in containers.
  Existing Compose labels/UID/home metadata identify eligible containers. Missing
  metadata fails visibly; private-mode names are not guessed. Local discovery is
  on by default, configurable through the host dialog. No containers are created,
  updated, stopped or recreated.
- `src/window.*`, `src/types.*`: persist SSH aliases and enablement in window.ini;
  keep friendly container/host roots within the existing server tree. Empty
  containers remain in All. Explicit host registration prevents automatic access
  to remote Docker contexts. Background SSH requires existing non-interactive
  authentication; no graphical key/password setup is added.
- `src/backend.*`, `src/types.*`, `src/terminal.cpp`: require observed execution
  before normal container resume; retain unknown history with a History only
  suffix. Evidence is cached against execution-environment identity and checked
  at both UI and terminal boundaries. Engine/lifetime/mount/cwd checks reject
  stale targets. No override or automatic migration between containers is added.
- `src/worker.*`, `src/asyncbackend.*`, `src/jsonprocess.*`, `src/asyncstore.*`,
  `src/discovery.*`, `src/sessionmodel.*`, `src/main.cpp`: move process/JSON/SQLite
  work off the GUI, bound/coalesce model delivery, restore cache in chunks and
  drain owned workers at application exit. Shared workers are grouped by host,
  with separate discovery and database loops. Request deadlines are scoped to
  operations; failed subscriptions use bounded reconnect backoff, never idle
  status polling. Cleanup affects only owned clients and watchers.
- `tests/test_containers.py`, `tests/test_runtime.cpp`, model/window/terminal
  regressions: add controlled fixtures for security, routing, event behavior,
  bulk updates and failure isolation. They do not replace real Docker/SSH and
  full host Flatpak acceptance. No credentials or model turns are needed.
- `CMakeLists.txt`, `data/`, `scripts/build-flatpak.sh`, README and design docs:
  version 0.4.0, same runtime and permissions. A local x86_64 Flatpak and checksum
  are generated under ignored dist/. Cached SDK/runtime roots and isolated
  .build/ packaging/test artifacts are reused because namespaces are disabled
  in this container. No host application installation, public tag, push or
  release is part of this task. Source changes are committed under the user's
  standing instruction; artifact build provenance is recorded in dist/BUILD-INFO.txt.

0.4.0 validation: native discovery/container/core/runtime suites pass; KDE SDK
container/core/runtime/model/window suites pass. Core/runtime also pass ASan and
UBSan with leak detection disabled. The final window suite completes in about a
second after fixing an already-finished-worker shutdown race. X11 tests using the
packaged KDE Platform and patched Konsole libraries pass all terminal lifecycle
cases, including history/environment launch guards. The staged application passes
its real-PTY smoke test; a synthetic container/shared-history explorer screenshot
is retained under `.build/mycodex-test/`. Formatting, whitespace, desktop entry,
manifest JSON and shell checks pass. AppStream retains the existing missing
homepage warning. Real Docker/SSH and full Flatpak sandbox acceptance remain
explicitly unverified, as described in [mycodex.md](mycodex.md).

## 0.4.1 container validation and visibility

The container launch fingerprint now canonicalizes mount-list order as well as
object keys. Docker 29.1.3 can enumerate identical mounts in different orders;
that must not cause "Container execution context changed". All mount fields
remain part of the fingerprint, and engine, container lifetime, runtime-user,
configuration-home and execution-evidence checks remain enforced. Discovery and
launch use the same calculation. Cached fingerprints are refreshed by normal
startup discovery; no user data migration or container recreation is required.

Status dots supply explicit Qt icon modes/states, so selection and focus cannot
turn red into magenta. Failed clients keep their output and remain in Active.
Closing that view clears its client error and exposes the server's state; it
does not claim a reconnect succeeded. Distinct forks with identical titles keep
their distinct thread IDs and rows. The earlier report of an entry disappearing
from every tab was not independently reproduced; this update does not claim a
separate cause or speculative deletion fix for it.

Restart client asks the window controller for the current profile and, for an
existing conversation, runs the same selected-server verification as opening it.
It no longer retains/replays launch arguments from before rediscovery. A rejected
restart preserves the old terminal output; a live client cannot be replaced by
this action. Closing the terminal cancels its pending restart. New-session
launchers repeat a new-session launch, retaining their synthetic row identity.

myCodex now shows only sessions in the container's Docker `Config.WorkingDir`
and descendants by default, including in search results and activity views.
The field is recorded separately as `identity.containerWorkdir`, including for
SSH containers; the app-server's process cwd is not used as a substitute.
The shared SessionModel visibility predicate applies before folder compression
and counts, while retaining hidden records for immediate setting changes.
Paths use lexical normalization and directory boundaries, not host filesystem
resolution. A Docker WorkingDir of `/` includes all absolute paths. Older cached
profiles without WORKDIR remain restricted until discovery provides it.
This display filter does not grant execution authority or merge conversations.

Settings → Configure Archcodex opens Application and Backends pages with OK,
Apply and Cancel. It reuses `window.ini`, preserving the existing
`hideUnsavedSessions` key and View shortcut. The new
`backends/myCodex/showAllSessions` key defaults to false and covers both local
and SSH myCodex backends. An open terminal is not closed when its explorer row
is excluded by WORKDIR filtering. New terminal/backend pages can be added when
there are actual settings for them.

### Decisions not agreed in advance

The user authorized the fixes, settings dialog and default WORKDIR filter, and
explicitly chose inclusion of subdirectories. Implementation choices:

| Choice | Reason, affected files and consequences |
| --- | --- |
| Sort full mount records by canonical JSON, retaining all fields. | `src/containers.py`; fixes nondeterministic ordering without relaxing validation or adding a dependency. Startup discovery refreshes older fingerprints. |
| Supply all icon modes/states explicitly. | `src/sessionmodel.cpp`; status colours survive selected, focused, disabled and expanded rendering. Existing status priorities and Active/Running/Waiting meanings are preserved. |
| Reuse the controller's verification path for explicit retries. | `src/terminal.*`, `src/window.*`; removes cached launch commands, requires a ready backend, retains failed output and cancels pending retries when closed. New-session launchers still start a new session. |
| Store container WORKDIR in existing profile JSON; filter only display records. | `src/types.cpp`, `src/sessionmodel.*`, `src/window.cpp`; no SQLite schema migration. Missing cached WORKDIR stays hidden until rediscovery; lexical containment avoids touching remote/host paths. Search uses the same rule; hidden history remains cached. |
| Use a small Qt category-list/stacked-page settings modal. | `src/settingsdialog.*`, `CMakeLists.txt`, `src/window.*`; Application and Backends contain the present settings. No empty Terminal page, public settings framework or new dependency. The existing View shortcut remains synchronized. Apply persists; Cancel discards only unapplied changes. |
| Package as 0.4.1 with the existing runtime, permissions and Konsole patches. | CMake, script, AppStream and documentation version updates; no myCodex changes, container recreation, new host permission or polling. Local build/test/export files remain under `.build/` and the installable bundle/checksum under `dist/`. |

Regression coverage includes mount permutations in both proxy/TUI validation,
rejection of actual mount/runtime changes, stable icon colours, independent
same-title forks, refreshed retry arguments, selected-server retry verification,
retained PTY output, WORKDIR prefix boundaries and cached local/remote profiles,
search-model scope preservation, and settings defaults/Apply/Cancel/persistence.

0.4.1 validation: all seven suites passed across the native and KDE environments
(discovery, containers, core, runtime, model, window and terminal). The real-PTY
terminal suite passed all eight QtTest cases under X11 using the KDE Platform
and packaged Konsole libraries, including refreshed-profile retries and repeating
new-session launchers. The settings/window checks also passed under X11; the
settings dialog screenshot was visually reviewed. The stripped, staged package
reported version 0.4.1 and passed the application/Konsole smoke test.

The bundle was exported and installed into `.build/flatpak`, then its checksum
and effective permissions were verified; permissions match 0.4.0. As before,
this environment blocks user namespaces: compilation used the existing isolated
KDE SDK root, export disabled sandboxing only for the local icon validator, and
installation used a temporary D-Bus session. Sandboxed cache triggers reported
the namespace restriction, although deployment completed. The normal host
Flatpak installation and configuration were not changed. Full sandbox execution,
real Docker/SSH attachment and the reported affected containers still require
host acceptance. AppStream retains its pre-existing missing-homepage warning;
formatting, whitespace, manifest JSON, shell and desktop-entry checks pass.

Builds, fixture data, screenshots, temporary helpers and verification logs are
kept under `.build/fix-0.4.1*` alongside the reused SDK/runtime/package trees.
The installable bundle and SHA-256 file are in `dist/`. Completed source,
regression tests and documentation are committed locally under the standing
repository instruction; no push, tag, external release or host deployment is
performed.

On the host, install 0.4.1 and let startup discovery refresh container metadata.
Check both same-title forks in an affected container, including explicit client
restart and selecting/unselecting an error row. Verify that local and remote
myCodex roots show only WORKDIR/descendant sessions by default; enable the
Backends option to reveal other shared history, then disable it again and check
search and activity views. Previously hidden history must reappear without
changing execution eligibility or introducing polling.

## 0.5.0 host groups and opened-terminal navigation

Projects now separates the local computer (`username @ hostname`), local myCodex
(`myCodex @ hostname`) and each configured remote myCodex connection. These groups
contain short server/container names followed by the existing compact project
folders and sessions. Keeping the backend layer preserves routing when two
servers report the same directory or conversation ID. The local host comes
first, local myCodex second, and remote groups sort by name. Activity does not
reorder these groups; their children retain recent-use ordering.

Automatic child labels omit repeated host/myCodex/storage text; details and the
terminal header retain full context. Manual names are preserved. The existing
container discovery stream now supplies the Docker host's hostname and username,
obtained once on that host. The remote SSH alias is the fallback before discovery
reports its hostname. Cached metadata supplies offline labels; fresh discovery
takes precedence. Remote groups are keyed by configured SSH connection, not
hostname alone, so different execution contexts are not merged by display name.

The additional Opened tab lists every open terminal view, including retained
exited/error output as explicitly requested. It bypasses history visibility
settings and opens expanded by default. Closing a terminal removes that entry;
an idle or running conversation without a GUI terminal does not appear there.
Opened membership is not persisted or inferred from server state.

Ctrl+Tab and Ctrl+Shift+Tab navigate open terminals in recent-use order. Holding
Ctrl freezes a snapshot, successive presses cycle forward/backward, and release
commits the current selection as most recent. Clicking, closing a view, opening
another terminal or deactivating the window finishes the sequence safely. Keys
are intercepted in the owning main window, including the embedded terminal,
without changing normal TUI input or shortcuts inside separate dialogs. Retained
exit output remains in the switcher until closed. Session-menu actions expose
the shortcuts. The terminal header and visible navigator selection follow focus.
Switching never launches a new client or issues a backend request.

Group-level search now targets the group's backend IDs. Existing server/folder
search remains confined to its original backend, and every row keeps its exact
backend/thread identity. Parent counts and dots aggregate only visible children.
Empty configured servers remain in All and disappear from other views unless
they contain matching entries.

### Where the remote TUI runs

For the implemented remote myCodex backend, **Codex TUI runs inside the container
on the remote Docker host**. The local Konsole widget provides rendering and
keyboard input through host OpenSSH (`ssh -tt`). On the remote host, the existing
Python helper validates the selected execution context and runs `docker exec -it`
as the discovered container user. Inside that container, it selects the session's
working directory and launches the discovered Codex executable with
`--remote unix://SOCKET resume -- THREAD_ID` against that container's app-server.
The local computer does not run that Codex TUI.

Status uses a separate non-PTY path: `ssh -T`, `docker exec -i`, then the container's
`codex app-server proxy --sock SOCKET`. It carries protocol traffic and pushed
events to the existing bridge/backend. Local myCodex uses the same container
paths without SSH; ordinary local host sessions run their TUI on the host.
No transport, authentication policy, app-server lifetime or polling behavior was
changed by this UI update.

### Decisions not agreed in advance

The user authorized host/type grouping, shorter explorer labels with unchanged
terminal headers, Opened and Ctrl+Tab navigation. They explicitly selected
inclusion of retained exited/error terminal views. Implementation choices:

| Choice | Files and consequences |
| --- | --- |
| Retain server/container nodes beneath host groups; keep group IDs separate from backend IDs. | `src/sessionmodel.*`, `src/window.*`; avoids merging execution contexts or changing session routing. Adds group-scoped search and aggregate counts/dots. Local/local-myCodex roots precede alphabetically ordered remote groups. |
| Obtain host identity from the existing event-driven discovery helper, cache it in profile metadata, and use SSH aliases until it arrives. | `src/containers.py`, `src/window.*`; no extra host command, DNS lookup, recurring query, secret access or new dependency. Configured SSH connections remain distinct even if their hostnames match. |
| Separate short tree labels from full profile labels and preserve manual names. | `src/types.*`, `src/window.cpp`; terminal titles and connection details keep their existing context; numeric suffixes distinguish otherwise identical automatic names within a group. |
| Append Opened as the fifth tab, expand it by default, and bypass both saved-history and myCodex-WORKDIR filters in that view. | `src/sessionmodel.*`, `src/window.cpp`; every retained terminal stays reachable, while other views keep their visibility policy. Existing tab indices remain unchanged; no schema/settings migration. |
| Keep a transient recent-use list in TerminalArea and freeze it while Ctrl is held. | `src/terminal.*`, `src/window.cpp`; reverse navigation, closed-client cleanup and retained exits are supported without polling, reconnecting or reordering history on status events. Application event filtering is confined to the owning main window. |
| Package as 0.5.0 using existing dependencies, permissions and runtime. | CMake, AppStream, build script and docs; local test/build artifacts under `.build/ui-0.5.0/` plus reused SDK/native/runtime trees, and the bundle/checksum under `dist/`. No myCodex changes or container recreation required. |

0.5.0 validation: all seven suites pass across the native/KDE environments.
New tests cover group aggregation, reparenting/removal and exact backend routing
with QAbstractItemModelTester; local/remote host labels and short child names;
group versus server search scope; Opened visibility for retained errors outside
history filters; and preservation of full terminal titles. The X11/real-PTY
switcher test exercises a held-Ctrl forward/reverse sequence, MRU commit on
release, retained exit output, closed-view cleanup, and normal text input to the
actual terminal display. It verifies that switching keys are absent from the
child process's input. All nine terminal QtTest cases pass.

The host-group screenshot was visually reviewed. The staged application reports
0.5.0 and passes its X11/Konsole smoke test using KDE Platform libraries. The
bundle installs into the project-local `.build/flatpak` installation; installed
application bytes match the staged binary, checksums match, and effective
permissions are identical to 0.4.1. The container still blocks Flatpak namespaces,
so the existing isolated SDK build/export route and temporary D-Bus installation
check were reused. Cache-update triggers report that restriction, while bundle
deployment succeeds. Full sandbox execution and real remote Docker/SSH session
attachment remain host acceptance checks. AppStream retains the existing missing
homepage warning; formatting, whitespace, shell, JSON and desktop-entry checks
pass.

Builds, fixture state, helper scripts, screenshots and logs remain under `.build/`
(principally `ui-0.5.0`, plus `host-groups-*.log` and the reused SDK/native/runtime
trees). Completed code, tests and documentation are committed locally under the
standing instruction. No push, tag, external release, container modification or
normal host Flatpak installation was performed.

## 0.5.1 activity ordering and explorer menus

The confirmed ordering defect was in event handling: `thread/status/changed`
changed state and tab membership but retained the old history timestamp. A saved
session's metadata was not reread on each transition, so a newly running/waiting
conversation could stay in its previous position indefinitely. Activity transitions
now advance recency from event receipt time. Duplicate notifications and thread
closure do not advance it, and stale metadata replies cannot roll it back. No
polling or additional status requests were introduced. The proxy explicitly
declares its recency sort role; host groups keep their established fixed order.

The reported loss of host groups in Waiting was not reproduced in this checkout.
All five tabs use the same source model and grouping; they do not have independent
old/new layouts. Regression tests now check actual group ancestry in Waiting and
Running, including tab changes, disappearing/reappearing activity, local host,
local myCodex and remote myCodex roots. This does not establish why the user's
running instance displayed different organization. Its reported runtime version
and reproduction remain needed if the symptom persists with this bundle.

Right-click menus are scoped to the clicked row. Sessions offer Fork, New and
Close; directories offer New and Expand All; host/server/container parents offer
Expand All. They capture backend/thread/path identities rather than indexes that
can change during a popup. Expansion resolves the branch's current stable key.
New uses the clicked directory and backend. Close releases just that terminal,
including retained exit output, without changing another displayed terminal or
deleting conversation history. It is disabled when no terminal view is open.

Fork is an explicit native `thread/fork` request over the selected backend's
existing asynchronous connection. The local generated protocol schema confirms
the request/response contract. A fresh `thread/read` precedes it. myCodex requires
execution evidence for that container, accepting the same app-owned cached
environment evidence used by Resume; shared history fields cannot supply it.
Only `threadId` and `excludeTurns: true` are sent, preserving the server's native
fork defaults without transferring full conversation history through the GUI.
The successful result supplies the new ID and execution provenance before its
TUI opens. Requests for the same source are coalesced while pending. Unsupported
operations fail explicitly; mutations are never retried automatically. A lost
connection or fork timeout warns that the outcome may be unknown. A failed TUI
launch leaves the newly created conversation available in history.

### Decisions not agreed in advance

The user authorized checking/fixing activity views and adding the specified
menus. The following details were implementation choices for that scope:

| Choice/action | Reason, files and consequences |
| --- | --- |
| Use real state transitions and terminal focus for recent-use ordering; retain seconds-based timestamp storage. | `src/backend.*`, `src/sessionmodel.cpp`; avoids polling, snapshot churn and a cache migration. Changes into loaded states promote sessions; closure and duplicate events do not. Root group ordering remains fixed. |
| Keep the common grouping implementation and add regression coverage rather than an unproven replacement. | `tests/test_model.cpp`, `tests/test_window.cpp`; Waiting/Running group-loss remains unreproduced, with rendered views checked separately. |
| Fork through native RPC, with a fresh preflight, matching execution provenance and no automatic mutation retries. | `src/backend.*`, `src/asyncbackend.*`, `src/window.*`; requires server support for `thread/fork`. The original conversation is retained, a real new ID is opened, and native configuration/goal behavior is inherited. No CLI/default-server fallback or new myCodex integration command. |
| Close means close the selected GUI terminal; Expand All means the selected subtree. | `src/terminal.*`, `src/window.*`; closed conversations are not deleted or archived. Unopened Close and unavailable connection-dependent actions are disabled. Synthetic terminal entries cannot be forked before a real ID exists. |
| Use nonblocking native menus with stable identity captures and revalidate actions at invocation. | `src/window.*`; pushed updates can continue while the menu is open. No dependencies, permissions or persistent preferences added. |
| Package as 0.5.1 and commit the completed task locally under the standing instruction. | `CMakeLists.txt`, build script, AppStream and documentation; local `dist/Archcodex-0.5.1-x86_64.flatpak` and checksum. No push, tag or external publication. |

Build/test logs, screenshot fixtures and temporary runtime configuration remain
under `.build/explorer-0.5.1`; SDK/native/runtime trees, local OSTree repository
and isolated Flatpak installation are reused. The finalized package lives under
`.build/package-0.5.1`. Packaging uses the previously documented namespace-limited
SDK/export route, with unchanged runtime and permissions. Full Flatpak sandbox
execution and actual local/remote myCodex forks require host acceptance testing;
protocol fixtures do not substitute for those checks.

### Validation

All seven test suites pass: discovery and container Python tests, core protocol,
asynchronous runtime, model, window and real-Konsole terminal tests. New coverage
checks recency after pushed transitions and stale reads; grouped activity-view
ancestry; clicked-row menu routing and subtree expansion; native fork completion
ordering, duplicate suppression, server errors, matching/obsolete container
evidence and disconnect handling; and closing a noncurrent terminal. Model
invariants are exercised with QAbstractItemModelTester. The menu test initially
held a raw pointer after Qt had closed/deleted the popup; its lifetime tracking
was corrected to use QPointer before the passing run.

X11 screenshots of Waiting and Running with all three host categories were
visually reviewed. The staged application reports 0.5.1 and passes its real-PTY
Konsole smoke test against KDE Platform libraries. The bundle installed into the
project-local Flatpak installation; installed/staged executable bytes and the
bundle checksum match. Permissions match 0.5.0 exactly. The three cache-trigger
namespace warnings reflect the existing build-container restriction, not a
successful sandbox launch. AppStream still reports the existing missing-homepage
warning; formatting, whitespace, shell syntax, JSON and desktop-file checks pass.


## 0.6.0 shared myCodex transport and desktop TUIs

Implemented 2026-11-10 after explicit authorization of the pipe-only design and
approval of SSH keepalives at 15 seconds / three missed replies. The design is
now adopted in [local TUI connections](local-tui-connections.md) and
[container security](container-transport-security.md); [myCodex integration](mycodex.md)
describes the current behavior.

MyCodex terminals now run desktop Codex against private local relay sockets.
The original app-server, credentials, tools and workspace remain in the selected
container. Discovery, one observer per app-server and independently opened TUIs
share a single SSH command per remote host profile and a single Docker exec
attachment per container/runtime user. Local hosts omit SSH. The remote helpers
use only inherited pipes and outbound connections to existing sockets; they add
no remote listeners or mounts. Normal operation still follows Docker/inotify/
Codex events. Explicit refresh preserves healthy client streams and attachments.

A dead stream never falls back to the desktop server or another container sharing
history. Fresh discovery provides fresh private relay paths after a lost
attachment. Existing terminal output and Restart behavior remain available.
Disabling/removing a host closes its shared streams, retaining terminal output
and cached history; the host dialog now states this consequence.

### Decisions not agreed in advance

The shared-helper architecture, desktop TUI location, trusted-account boundary,
no remote exposure, event-driven operation and 15/3 keepalives were expressly
agreed. The following details were selected during implementation:

| Choice/action | Reason, affected files, consequences and status |
| --- | --- |
| Use isolated standard-library Python/asyncio and an application-private versioned byte codec. | `src/mux.py`, `src/transport.py`, `src/containers.py`, `src/discovery.qrc`; reuses the installed Python requirement without a new package or remote installation. Streams remain independent. Bounds are 16 KiB chunks, 64 KiB windows/frames, 1,024 streams per connection, 1 MiB data and 256 KiB control queues, 8 MiB inventories; timeouts are 10 seconds for handshake and 20 for stream setup. Implemented and fixture-tested. |
| Use one transient desktop systemd user service, parent-death signals, process groups and bounded cleanup. | `src/launch.*`, `src/containers.py`, `src/transport.py`; requires a running desktop user manager and its Docker/SSH-agent login environment. No persistent unit is installed. EOF gets 0.5 seconds, TERM one second, then KILL/reap; service stop timeout is five seconds. A narrowly scoped Python subprocess transport access closes full pipes before reaping, covered by a regression test. Unit syntax verified; actual systemd execution remains a host check. |
| Put mode-0600 desktop sockets in unique mode-0700 directories under `/tmp/codex-daemon-<uid>`; use peer/socket identity and generation checks. | `src/transport.py`, `src/types.*`, `src/backend.cpp`, `src/window.cpp`; preserves the inspected Codex 0.166.0 sandbox directory contract. Cleanup never removes Codex sockets or follows an instance symlink. Paths are ephemeral and never saved. New upstream masking behavior must be qualified. Ownership, no remote listening sockets, cleanup and stale-generation behavior tested with fixtures. |
| Select desktop Codex from host PATH and require version 0.166.0 or newer. | `src/types.cpp`, `src/transport.py`, `src/terminal.cpp`; explicit local relay, exact thread and remote cwd arguments; no remote CODEX_HOME or credentials copied. Missing/old clients retain a clear error. Existing server-side Fork/provenance rules remain. Launch selection/arguments are tested with a fake executable; real clipboard, image paste and server-default behavior require host acceptance. |
| Share one desktop discovery process and route observers through a small local stdio adapter; refresh without replacing healthy attachments. | `src/containerdiscovery.*`, `src/localbackend.cpp`, `src/transport.py`, `src/containers.py`, `src/window.cpp`; removes per-backend SSH/Docker chains while keeping the existing WebSocket bridge. Connection failures retry with 1–30 second backoff; host removal/disable disconnects affected terminals and retains output. No new UI preference or database migration. Forty-client, refresh, two-container and stalled-host tests pass. |
| Explicitly disable inherited SSH agent/X11/port forwarding, backgrounding and unrelated control masters. | `src/transport.py`; prevents host SSH configuration from adding exposures or changing ownership. Existing authentication/host-key checks and aliases remain; SSH server configuration is unchanged. Client keepalives do not guarantee an immediate remote-side partition cleanup deadline. Command flags tested; real SSH/jump-host behavior still requires host testing. |
| Package as 0.6.0, preserve existing dependencies/permissions, and commit the completed task locally under the standing instruction. | CMake, AppStream, build script, README and design docs; local bundle/checksum/BUILD-INFO under `dist/`. Native/KDE/PTY and process tests added under `tests/`. Existing SDK/runtime roots, local OSTree repository and isolated Flatpak installation reused; no regular host installation, push, tag or public release. All artifacts are local. |

### Validation and artifacts

All eight suites pass across the available environments: native discovery,
container, transport, core and runtime tests; KDE SDK model/window tests; and the
real Konsole PTY tests under X11 with packaged KDE Platform libraries. The 18
new Python transport cases include 40 independent streams, two container helpers
through one SSH invocation, full buffers, malformed/reused IDs, abrupt EOF,
failed endpoints, idle inventory unsubscribe, preserved refresh, container restart,
a stalled second host and frontend SIGKILL. The process fixture substitutes only
SSH/Docker entry and socket discovery; the actual packaged helper code runs over
real pipes and Unix sockets. It inspects remote-role descriptors for listeners.
Additional C++ tests exercise real WebSocket notifications through the new stdio
adapter, desktop executable/argument selection and offline systemd unit parsing.
The latter verifies syntax, not execution by a real user manager.

The stripped staged application reports 0.6.0 and passes the real-PTY smoke test;
its screenshot was inspected. The bundle imports/deploys into the project-local
Flatpak installation. Installed/staged executables, installed ref and checksum
are checked, and effective permissions match 0.5.1 exactly. Formatting, whitespace,
shell, JSON and desktop-entry validation pass. The existing AppStream homepage
warning remains.

Build/test logs, screenshots and isolated runtime configuration live in
`.build/shared-transport`, with staging under `.build/package-0.6.0`. The existing
`.build/sdk-root`, `.build/runtime-root`, `.build/repository` and `.build/flatpak`
were reused. A temporary D-Bus session supplied the missing system-bus address
only for the isolated install. Flatpak cache triggers still warn that namespaces
are unavailable; this is not a successful full Flatpak sandbox launch. The final
process/path audit found no running transport helpers or leftover Archcodex
instance socket directories.

This environment has no accessible Docker daemon, running systemd user manager
or permitted Flatpak namespaces. Real private/shared containers, rootless UID
mappings, SSH/jump hosts, remote partitions, systemd crash cleanup, native Codex
image paste/approvals and active model work surviving disconnect remain host
acceptance checks. No account credentials or live model turns were used.

### Host acceptance for 0.6.0

1. Install the bundle from the desktop KDE terminal, outside myCodex. Confirm
   `codex --version` is at least 0.166.0 and `systemctl --user is-system-running`
   reaches the desktop user manager. Start Archcodex normally.
2. Open local and remote myCodex conversations. Confirm desktop `codex --remote`
   clients and a single SSH transport per host; opening more terminals must not
   create more Docker exec helpers for an already attached container.
3. Test clipboard text/image paste, New in a remote-only directory, Resume and
   Fork, approvals and the expected remote settings. Check both private and
   shared environments and confirm WORKDIR/provenance restrictions remain.
4. Refresh discovery while terminals are open. Then close one terminal, restart
   a container, disconnect a remote host and reconnect. Only the affected streams
   should close; retained output and Restart must select the fresh endpoint.
5. While remote work is running, close/kill the GUI and helper service and inspect
   owned local/remote helpers and local socket paths. Repeat after a network
   partition; distinguish desktop 15/3 keepalives from remote SSH-server detection.
   Existing app-servers and accepted work must remain independent.


## 0.6.1 startup discovery and local TUI executable fixes

The reported local myCodex hosts stayed at “Discovering…” while remote hosts
connected. A deterministic QProcess test reproduced the fault: a newer complete
host configuration written during process startup was followed by the older
configuration captured by the started callback. That last stale message removed
hosts from the shared helper. Cached sessions then lacked live status. The user's
manual Connect / retry restored discovery (16 local containers, eight servers),
which is the recovery expected from resending the complete host list.

`JsonProcess` now keeps the latest pending configuration until the process has
started. No obsolete initial request follows it. This applies to full host lists
and local discovery path snapshots, without adding status polling or changing
running-process request behavior. Regression cases cover local-first and
remote-first registration.

The remaining connection error was independent: 0.6.0 used `shutil.which("codex")`
for myCodex TUIs, while ordinary local server connections already used a
discovered absolute executable. A GUI/Flatpak launch can lack the terminal's
PATH even when Codex is installed and local server connections work. Version
0.6.1 uses the optional local executable setting first, otherwise live local
server discovery, otherwise PATH. A restricted-PATH test proves that the selected
absolute executable is used for both Resume and New. Container paths are never
local candidates. The selected default local server takes precedence; otherwise
candidate paths are sorted for stable selection. Users with multiple versions
can choose explicitly. Automatic candidates are not saved as stale cache guesses.

“Local computer” means the machine running Archcodex. The version requirement is
for its local TUI, not for an executable on an SSH host outside the containers.
Errors now include the local hostname and distinguish lookup failure from an
incompatible selected version. No minimum-version change was made in this fix.
The server/workspace remain in the selected container, and every TUI launch
still supplies an explicit relay. Missing relays fail closed; no app-server is
started as a fallback for an inactive/disconnected entry.

### Decisions not agreed in advance

The bug report authorized correcting discovery and local TUI launch failures.
These details were selected during the repair:

| Choice/action | Reason, affected files, consequences and status |
| --- | --- |
| Coalesce pre-start discovery requests to the latest complete configuration. | `src/jsonprocess.*`; fixes the demonstrated ordering race without delays, session queries or another process. The regression failed with remote-only configuration before the fix; both startup orders now pass. |
| Reuse live local executable discovery, preferring the default server and then sorted paths, with PATH as the final fallback. | `src/types.*`, `src/window.*`, `src/terminal.*`, `src/transport.py`; avoids relying on GUI PATH and never uses container executable paths. Existing local server launches retain their own executable. No new dependency, shell startup or cache migration. Implemented and tested with restricted PATH and a real Konsole PTY. |
| Add an optional absolute-path field to the Application settings page. | `src/settingsdialog.*`, `src/window.*`; `application/localCodexExecutable` in `window.ini`, empty by default. Explicit selection overrides detection for subsequent myCodex client launches/restarts; current terminals are unchanged. Invalid relative/control-character paths cannot apply settings. Apply/Cancel/persistence tests pass; the enlarged settings dialog was visually reviewed. |
| Replace ambiguous “desktop host” launch diagnostics with local-computer/hostname wording and executable/version details. | `src/transport.py`, `src/containerdiscovery.cpp`, README and design docs; the existing 0.166.0 local-client minimum and remote requirements are unchanged. No credential inspection, copying or remote installation. |
| Package 0.6.1 and commit the completed repair under the standing instruction. | CMake, AppStream, build script and docs; local bundle/checksum/BUILD-INFO in `dist/`. Existing SDK/runtime/OSTree trees and project-local Flatpak installation reused. Logs, configuration and screenshots in `.build/fix-0.6.1`, staging in `.build/package-0.6.1`. No normal host installation, public release, tag or push. The intervening `89d1b0f` test-fixture changes are preserved. |

### Validation and remaining host check

All eight suites pass across native, KDE SDK and real X11/Konsole environments:
discovery, containers, transport, core, runtime, model, window and terminal.
Coverage includes startup configuration ordering, strict local candidate selection,
restricted PATH, argument boundaries, settings validation/persistence and a real
PTY that launches the chosen local executable with the exact relay/thread argv.
The PTY fixture does not invoke SSH or start an app-server. Existing 18 transport
cases and shared-history/lifetime tests also pass.

The stripped package reports 0.6.1, passes the real Konsole smoke test, installs
into the isolated project-local Flatpak installation, and matches staged binary
bytes and checksum. Permissions are byte-identical to 0.6.0. Formatting,
whitespace, shell/JSON and desktop-entry checks pass; AppStream retains its
pre-existing missing-homepage warning. Build namespace restrictions still prevent
full Flatpak execution; cache triggers warn about that during the isolated
installation. A temporary D-Bus session was used only for this test install.
No test relay directories remain. A live, non-fixture container helper visible
after the user's discovery retry was left untouched.

The startup defect and PATH-independent launch are reproduced and tested here;
actual local/remote myCodex TUI use, clipboard/image input and the complete Flatpak
host-command boundary still require testing on the user's desktop. Fully quit
the old application, install the 0.6.1 bundle, relaunch and confirm the version in
About. Connect / retry alone cannot replace 0.6.0's executable-resolution code.

## 0.6.2 independent Projects search and tab filters

[Issue #1](https://github.com/emsi/Archcodex/issues/1) is implemented. All retains
explicit Enter-to-submit, server-side conversation search, scoped requests,
pagination, visibility rules, error reporting and stale-reply rejection. Its
results can arrive while another tab is visible without replacing that tab's tree
or feedback. Tab navigation remains enabled.

Active, Running, Waiting and Opened filter cached titles, working-directory paths
and session IDs as the user types. Matching is case-insensitive literal text, not
regular expressions or conversation-body search. The model intersects text and
scope with the existing membership and visibility rules before rebuilding compact
folders and counts. Pushed session changes and terminal lifecycle signals update
matches through the same path. Query editing, clearing and tab switching do not
search the backend, fetch history, open terminals or introduce polling.

Window state stores each tab's query, captured scope and navigation separately,
including a snapshot of navigation before filtering. A selected scope remains
anchored when filtering removes its row; missing selection fails closed. Queries
remain transient for this application run. The UI distinguishes search from local
filtering in placeholders, accessible names, tooltips and result feedback. See
[the interaction specification](ui.md#6-search-and-connections).

### Decisions not agreed in advance

Addressing the issue authorized the behavior, regression coverage and UI-document
update. The standing instruction explicitly requires committing completed work.
The following implementation and delivery choices were made during this task:

| Choice/action | Reason, files, consequences and status |
| --- | --- |
| Search cached title, cwd and session ID using case-insensitive literal substrings. | `src/sessionmodel.*`, `src/window.cpp`; explicit, inexpensive semantics with no new index, dependency, conversation cache, regular-expression syntax or network calls. Implemented and covered by model/window tests. |
| Apply the local predicate in the source model before grouping; keep the global results model separate. | `src/sessionmodel.*`, `src/window.*`; preserves compact paths, counts, visibility exceptions and backend identity. An All search may finish in the background; stale tokens still reject superseded replies. Implemented and tested. |
| Retain per-tab queries and navigation in memory, anchor selected scopes, initially expand matches, and restore pre-filter navigation on clearing. | `src/window.*`, `docs/ui.md`; no settings migration or persistence of transient queries. Manual collapses survive refinements/events. To retarget a local scope, select All servers, choose a row, then select the scoped option. Missing selection never widens scope. Implemented and tested. |
| Enable Load more history only in All outside search results. | `src/window.cpp`; keeps explicit history loading in the history view and avoids unrelated history requests from local-filter tabs. Existing automatic connection snapshots remain unchanged. Implemented and tested through request-count assertions. |
| Extend protocol fixtures and model/window regressions, and verify with native, SDK and X11 tests. | `tests/fixture.h`, `tests/test_model.cpp`, `tests/test_window.cpp`; delayed search replies exercise background completion, stale results and disconnect errors. Test data, logs, screenshots and runtime launcher are under ignored `.build/issue-1*`. No real Codex conversations or credentials are used. |
| Use authenticated GitHub CLI read access after the connector returned 404; package a local 0.6.2 development build. | CMake, README, AppStream, build script and this document; existing SDK/runtime, staging and local OSTree repository reused. Bundle/checksum/build information are in ignored `dist/`; isolated installation uses `.build/flatpak`. No dependency or Flatpak permission changes, public release, tag, push, issue comment or closure. Commit is required by the user's standing instruction. |

### Validation

All eight suites pass across the native build, KDE SDK and X11/Konsole environment:
discovery, containers, transport, core, runtime, model, window and terminal. The
window suite passes all 16 cases under X11; terminal testing passes all 10 cases.
The new tests cover independent queries/scope, live membership, title changes,
opened/exited/closed terminals, scope boundaries, compressed grouping, navigation
restoration and zero additional protocol requests during local filtering.

The first direct-runtime X11 run could not find the fixture bridge because
launching via the runtime ELF loader makes Qt locate its application directory
beside that loader. `strace` confirmed ENOENT for that path. A temporary symlink
in the ignored runtime tree supplied the built bridge for the corrected run and
was removed afterward. This was a test-launcher issue; no application fallback
or packaging workaround was added. The filtered Waiting screenshot was reviewed.
Formatting, whitespace, shell syntax and desktop-entry checks pass. AppStream
retains the pre-existing missing-homepage warning.

The 0.6.2 bundle installs into the isolated project-local Flatpak installation;
installed binaries match the staged bytes and the bundle checksum passes. The
stripped application reports 0.6.2 and passes the direct-runtime Konsole smoke
test. Flatpak metadata/permissions are byte-identical to 0.6.1. Full sandbox launch
remains untestable here because this build environment prohibits user namespaces;
the isolated installation reports the corresponding cache-trigger warnings. The
normal desktop installation has not been changed. Fully quit the previous app,
install the new bundle and relaunch for desktop acceptance testing.

## 0.7.0 keyboard navigation

Implemented in three steps:

1. A shared command registry and native Konsole shortcut arbitration. Menus,
   the command palette and keyboard bindings use the same actions. The requested
   swap is applied: Ctrl+Shift+P opens the palette, Ctrl+Shift+F searches terminal
   scrollback, and Ctrl+Alt+P/F focuses Projects or its search/filter field.
   Alt+1–5 selects a Projects tab, Alt+0 returns to the current terminal, F6
   switches panes, and Enter/Menu/Shift+F10 support tree navigation and actions.
2. A Keyboard Shortcuts settings page using KDE's editor. Changes are drafts
   until Apply/OK, with validation, persistence and Restore Defaults. New, Fork,
   Restart, connection management and other existing actions are available in
   the palette even when they have no default binding.
3. Held-key terminal and project switchers. Ctrl+Tab and Ctrl+Shift+Tab choose
   from a frozen snapshot of activation recency. The current terminal stays
   visible until Ctrl is released. Escape or deactivation cancels. Ctrl+backtick
   uses the same mechanism for projects with open terminal views, selecting the
   last activated open terminal in each project. Retained exited views participate.

The [interaction specification](ui.md#keyboard-navigation-and-commands-070)
contains the defaults and detailed behavior. These commands are window-local,
including when the embedded terminal has focus; they are not desktop-wide grabs.
There are no new backend requests, helper processes or polling paths.

### Ordering verification

The earlier statement that background transitions necessarily reorder the
explorer was too broad. Controlled tests against the existing backend and Window
path prove that a strictly newer running-to-waiting transition **can** move a
session and its project ahead of the opened session. The comparator uses the
newer of activation and backend recency; parents use the newest descendant.
Duplicate statuses do not advance recency, timestamps have one-second precision,
and other newer descendants can keep a project ahead. Those conditions can leave
the visible order unchanged. The tests do not establish what happened in the
user's installed application. Backend timestamping and explorer sorting are
unchanged, as requested. Switcher recency follows actual terminal activation only.

### Decisions not agreed in advance

The three implementation steps, swapped defaults, configurable shortcuts,
mouse-free navigation and project cycling restricted to open terminals were
explicitly authorized. The standing instruction requires a commit. The following
implementation and delivery choices were made while completing that work:

| Choice/action | Reason, affected files/configuration, consequences and status |
| --- | --- |
| Centralize stable action IDs in `Commands`, use native QAction dispatch and Konsole's `overrideShortcut` hook. | `src/commands.*`, `src/window.*`, `src/terminal.*`; avoids forwarding synthetic key presses or modifying the terminal key table. Conflicting KPart bindings are removed while owned and restored on reassignment. Implemented and verified with a real PTY, including Print Screen versus the palette. |
| Use KDE's command palette and shortcut editor; explicitly link ConfigWidgets. | `CMakeLists.txt`, `src/window.cpp`, `src/settingsdialog.*`; ConfigWidgets is already in the selected KDE runtime. No additional Flatpak permission or runtime module. Settings opens at 900×600 to fit the editor, with an Archcodex category label. Implemented and visually checked under X11. |
| Keep shortcut drafts inert and store bindings in `shortcutsrc` beside existing application metadata. | `src/commands.*`, `src/settingsdialog.*`; existing `window.ini` preferences are preserved. Apply validates before replacing bindings and reports write failures; malformed saved bindings fall back to defaults with a status message. Restore Defaults affects only the draft until Apply. Tested draft cancellation, duplicates, persistence and defaults. |
| Restrict shortcuts to single combinations and reserve ordinary typing/navigation for widgets and the TUI. | `src/commands.cpp`, `docs/ui.md`; Ctrl/Alt/Meta or function keys are required, with Shift+Insert also allowed. Held switchers allow one binding per direction and one shared Ctrl/Alt/Meta hold modifier, optionally Shift. Duplicate and multi-step bindings are rejected. These restrictions are implemented and tested. |
| Scope Copy/Paste to the terminal and use native terminal actions for Copy/Paste/Find/Print. | `src/window.cpp`, `src/terminal.cpp`; text fields retain their standard editing shortcuts. Unavailable actions remain discoverable but disabled. Normal shortcuts use Qt contexts, while only held switching needs an event filter. Implemented; terminal search and ownership/rebinding are exercised against Konsole. |
| Capture contextual targets by identity when opening the palette and recheck availability before execution. | `src/window.*`; background selection/order changes cannot redirect New/Fork/Restart to another row. Commands use the existing connection and execution-context validation. Menus gain distinct mnemonics, and keyboard context menus use the selected row. Implemented; navigation and existing contextual-action regressions pass. |
| Implement the chooser as a child panel, with snapshots of existing activation history. | `src/terminal.*`; it changes neither terminal focus nor visit timestamps until commit. Project identity is backend plus normalized working-directory path, not a merged host path. Closing a candidate removes it; a project's next open terminal can replace its representative without moving the project. New views join the next gesture. Focus loss, Escape and closing the origin cancel. Implemented and tested. |
| Refresh chooser titles/status from existing pushed session and lifecycle updates. | `src/window.cpp`, `src/terminal.cpp`; status stays current without changing the frozen candidate order or adding queries. Error output remains accessible through retained terminal views. Implemented; lifecycle, disconnect and activity regressions pass. |
| Add protocol/window and real-terminal regressions, screenshots and isolated runtime checks. | `tests/test_window.cpp`, `tests/test_terminal.cpp`; fixtures use temporary directories and dummy clients, not real conversations or credentials. Logs, screenshots, an ELF-runtime launcher and builds remain in ignored `.build/keyboard*`. Owned stuck test processes were stopped while debugging; no live Codex helper was stopped. A temporary fixture-bridge symlink was removed after the X11 run. |
| Deliver a local 0.7.0 development Flatpak and commit the integrated work. | CMake, README, AppStream, build script and docs; reuses the existing pinned Konsole/KDE runtime and project-local package repository. Bundle/checksum/build information are in ignored `dist/`; isolated import updates only `.build/flatpak`. No push, tag, public release, issue comment or normal desktop installation. Full desktop acceptance remains necessary. |

### Validation

All eight suites pass across the native build, KDE SDK and X11/Konsole environment:
discovery, containers, transport, core, runtime, model, window and terminal. The
X11 window suite passes 21 cases; the terminal suite passes 13. Tests cover the
swapped defaults, tab/pane focus, palette navigation, keyboard context menus,
settings drafts and validation, frozen cycling, cancellation, rebindings,
candidate closure, project replacement and backend separation. A real PTY
receives ordinary input without app shortcut bytes. Original Konsole shortcuts
return after reassignment. The palette, settings and project chooser screenshots
were visually reviewed.

The offscreen platform cannot reactivate an owner window after a popup without a
window manager; the test explicitly reactivates it on that platform. The X11 test
uses normal automatic reactivation. No application focus workaround was added.
Formatting, whitespace, shell syntax, manifest JSON and desktop-entry validation
pass. AppStream retains its existing missing-homepage warning.

The stripped package reports 0.7.0 and passes the direct-runtime window/Konsole
smoke test. The bundle installs into the project-local Flatpak installation;
installed binary bytes match staging and the bundle checksum passes. Flatpak
metadata and permissions are byte-identical to 0.6.2. Full sandbox launch remains
unverified here: this environment prohibits user namespaces, and installation
cache triggers report that restriction. The normal desktop installation was not
modified. Fully quit the old application, install the bundle and relaunch for
desktop acceptance testing.

## 0.7.1 release of fa62908

The user requested a new GitHub release containing `fa62908a109fc6d5e5963c0d61fd84641df031a3`.
That commit pins Konsole 26.04.3 and adds a real-PTY scrollback regression.
Version 0.7.1 adds release metadata on top; it does not change the requested fix.
The bundled terminal uses legacy keyboard handling to keep Shift+Page Up/Down
and Shift+Home/End local when a client requests the enhanced keyboard protocol.
See [the release notes](releases/v0.7.1.md).

### Validation

The pinned Konsole archive checksum matched, both existing embedding patches
applied, and the component built in the KDE 6.11 SDK. Fresh staging contains
Konsole 26.04.3 libraries and plugin without stale 26.08.2 libraries; the plugin's
ELF dependency and the Konsole executable's version both confirm 26.04.3.
Application version and AppStream metadata are 0.7.1. Effective Flatpak
permissions are byte-identical to 0.7.0.

All eight suites pass across native, SDK and X11 environments: discovery,
containers, transport, core, runtime, model, window and terminal. There are
21 passing X11 window cases and 15 real-terminal cases, including both scrollback
variants and existing shortcut, switching and client-lifecycle coverage. The
stripped application passes the window/Konsole smoke test. Full Flatpak sandbox
launch remains unavailable because this environment rejects user namespaces;
direct runtime checks do not replace desktop acceptance. AppStream retains its
existing missing-homepage warning.

### Decisions not agreed in advance

Building and publishing a release containing fa62908 is explicitly authorized.
These supporting choices were made for that release:

- Use patch version 0.7.1 and update CMake, README, AppStream and the bundle script.
  The release tag points to a version-metadata commit containing fa62908, so
  source, executable, metadata and asset names agree. Historical tags/assets stay
  intact; the release catalog is updated in a separate provenance commit.
- Rebuild Konsole from the manifest-pinned archive with the existing patches and
  stage the package from fresh component installs, including the manifest's
  bundled license files. Reuse the existing KDE SDK/runtime and native build
  directories; no installed desktop application or host configuration is changed.
- Run the eight suites and verify packaged versions, library dependencies,
  metadata, checksums, isolated installation and smoke behavior. Logs, source
  archive, temporary runtime/configuration, staging and verification downloads
  remain under ignored `.build/release-0.7.1/`, `.build/package-0.7.1` and the SDK
  work directory; the bundle and local build record remain in ignored `dist/`.
  A temporary fixture-bridge symlink is removed after X11 testing.
- Follow the established GitHub draft/upload/download-verify/publish procedure
  using the CLI because the connector lacks release APIs. Publish the Flatpak
  and a per-release checksum file with reviewed notes and the explicit
  v0.7.0-to-v0.7.1 comparison. Mark 0.7.1 Latest as a normal GitHub release
  explicitly described as a development build. Keep the repository's current
  visibility (now public); no visibility or signing-policy change is made.
- Commit and push the release metadata, provenance and annotated version tag
  without force. No CI publisher, Flathub submission, unrelated source fix or
  rebuild of previous versions is included.

## 0.8.0 — remembered terminal workspace (#3)

Implemented workspace restoration with the user's explicit acceptance of the
stock-TUI `/resume`, `/fork`, `/new` and agent-navigation limitations. The launch
thread remains the saved target. Existing app-server rename events still update
its title. No Codex modification, protocol observer or additional relay process
was introduced. See [workspace behavior and future protocol observation](workspace.md).

`WorkspaceSnapshot` is separate from cached sessions. Its SQLite snapshot is
versioned, atomic, worker-written and coalesced. Stable view IDs, source evidence,
dispositions, selection and MRU survive normal shutdown and offline startup.
Unreadable/future records cannot be overwritten. Database schema 2 deliberately
prevents older releases from opening the upgraded database; downgrade requires
a pre-upgrade backup or separate data directory.

`OpeningCoordinator` handles manual opens, retries and automatic restores with
exact metadata reads, bounded concurrency, cancellation tokens and source
generations. Offline sources consume no slots. Same-container execution evidence
is required; shared history cannot authorize a different container. The terminal
UI restores placeholders immediately, launches incrementally without recording
visits or stealing focus, and retains disconnected/unresolved views for explicit
action. Shutdown freezes intent before client teardown.

### Validation and artifact

All nine suites pass: discovery, containers, transport, core, workspace, runtime,
model, window and terminal. Native core/protocol/Python tests passed; the workspace
suite additionally passed after the final cancellation-safety adjustment. Real
KDE runtime/X11 tests pass 22 window cases, 17 terminal cases and 28 model cases.
Coverage includes 80 queued views, source replacement, cancelled replies that
still occupy request slots, reconnects, failure/no-loop behavior, protected
storage, exact IDs beyond the first history page, persisted rename, selection,
MRU, focus, retained failures and offline restarts. The new placeholder and
Application settings screenshots were inspected.

A first full-window regression run exposed an existing test expecting a modal
preflight-error dialog. That test now verifies the intended nonmodal placeholder,
exact selected-server retry, stable view ID and absence of a terminal process.
Direct-runtime testing also needed the existing temporary bridge symlink beside
the ELF loader; that test-only symlink has been removed.

The stripped package reports **0.8.0** and passes an X11 window/Konsole smoke test
with fresh isolated data. Bundle installation into a separate project-local
Flatpak installation succeeded; installed application/bridge/AppStream bytes and
metadata match staging. Effective permissions are byte-identical to 0.7.1.
The first isolated installation attempt failed because this container lacks a
system D-Bus socket; as in prior builds, a temporary private D-Bus session supplied
that address solely for the install check. Cache triggers still report blocked
user namespaces. Full Flatpak sandbox execution and real desktop/SSH/container
restore acceptance remain unverified here. Existing AppStream validation still
reports the missing-homepage warning; desktop-entry, shell syntax, formatting
and whitespace checks pass.

Local test artifact: `dist/Archcodex-0.8.0-x86_64.flatpak`, 2,347,440 bytes.
SHA-256: `877f9112dfa0a21639711b07116f7eeeb43c09f18b7aeb3e0040864020f9d992`.
Flatpak commit: `3b8d7eae2d84a093d7dd91c135607ec7e8127ec20283601b0acae194b7940158`.
The sibling `.flatpak.sha256` and `.BUILD-INFO.txt` identify the local artifact.
No public release, tag, push, issue comment or issue closure is part of this task.

### Decisions not agreed in advance

Implementing #3 with limited in-TUI tracking, documenting a future proxy approach
without modifying Codex, and committing completed authorized changes are explicit
user instructions. The following supporting choices were made during this work:

| Choice/action | Reason, affected files and consequences | Status |
| --- | --- | --- |
| Separate `WorkspaceView`/`WorkspaceSnapshot` records and an event-driven `OpeningCoordinator`; keep one view per backend/thread pair. | `src/workspace.*`, `src/opening.*`, `CMakeLists.txt`; stable UUIDs separate intent from cached sessions without adding a plugin framework. Record version 1, max 512 views/4 MiB, strict navigation validation and SHA-256 logical source identity. A future observer can reassociate views; current in-TUI switches cannot. | Implemented and tested. |
| Database schema 2 and one monotonically revised workspace row, coalesced through the existing worker. | `src/store.*`, `src/asyncstore.*`; no new dependency. Old versions reject the upgraded database. Corrupt/future records and failed writes disable workspace saving for that run, preserving prior data. Individual failed operations no longer discard the rest of a queued cache batch. | Migration, atomicity, stale-write, corruption and flush tests pass. Downgrade procedure documented. |
| Four outstanding verification reads globally, two per host key, at most three remote; one widget launch per event iteration. | `src/opening.*`, `src/backend.*`, `src/asyncbackend.*`; explicit requests and saved selection get priority, cancelled reads still count until completion, offline sources do not block ready ones. Verification responses now carry tokens and exact metadata; inventory failure has an explicit signal so evidence waiting can finish. No new transport/process ownership. | 80-view, disconnect, replacement, cancellation and provenance tests pass. |
| Reuse placeholders and existing terminal navigation; add a nonmodal workspace detail window/status button and Connections action. | `src/terminal.*`, `src/window.*`, `src/types.*`; no visits/focus changes on automatic restore, no modal error storm. Exited clients remain disconnected and summaries replace persisted terminal output. Removing a remote myCodex host explicitly closes its views; disabling it keeps them. | Real KPart and X11 tests pass; screenshots reviewed. |
| Default startup restoration on; continue recording the current workspace when off. Unknown New IDs remain unresolved and their launcher is never replayed by restoration/reconnect. | `src/settingsdialog.*`, `src/window.*`, `src/terminal.*`; toggle applies next startup. A disabled startup begins empty, and saving that run replaces the prior workspace. GUI Fork keeps the confirmed returned ID. Rename remains event-driven. | Implemented; settings, offline restart and rename tests pass. |
| Keep exact-ID preflight and avoid archive scans or unarchive mutations. | `src/backend.cpp`, `docs/workspace.md`; known archive/removal events fail preflight. An offline archive not identified by Codex's metadata may instead be rejected by the TUI, with output retained and no retry loop. Reliable per-view proxy observation is documented, not implemented. | Protocol limitation explicit; no Codex patch. |
| Use 0.8.0 for a local development bundle, preserving published 0.7.1 assets and dependencies. | `CMakeLists.txt`, `data/`, `scripts/build-flatpak.sh`, `README.md`, `docs/workspace.md`, this log; README's published download still points to 0.7.1, while source build instructions use 0.8.0. Existing KDE 6.11/Konsole 26.04.3 and Flatpak permissions retained. | Bundle and checksum built/installed locally; publication and real-host acceptance remain separate. |
| Add isolated protocol/storage, window and terminal regressions; reuse extracted SDK/runtime with privileged chroot only for builds/install-to-staging. | `tests/test_workspace.cpp`, `tests/test_window.cpp`, `tests/test_terminal.cpp`; controlled fixtures, temporary clients, Xvfb, fresh data and a private install bus. No production conversation or credential access. Local Codex 0.166.0 probes used isolated homes and no model turns to test rename and TUI-switch behavior. | Tests and packaged smoke pass; owned clients/transports/Xvfb/probe processes are gone, temporary runtime bridge link removed. |
| Retain reproducible test/build evidence locally and commit this task on the existing branch. | Ignored `.build/issue-3/` contains official Codex source/probes, logs, screenshots, isolated data, repository and Flatpak install; `.build/package-0.8.0`, SDK staging and `dist/` contain build artifacts. Existing SDK/native build caches were reused. No normal desktop installation, global settings change, external message, push or publication. | Local artifacts retained; implementation committed under the standing instruction. |
