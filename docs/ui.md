# Initial UI design

Status: rudimentary layout requested by the user; interaction details below are
the design baseline. The [initial implementation](implementation.md) now delivers
the two-pane layout and records which refinements remain open.
Last updated: 2026-11-09.

## 1. Starting layout

Start with a familiar IDE arrangement: a narrow project navigator on the left
and a large terminal workspace on the right. Use ordinary native widgets and a
resizable divider. Keep the first version simple.

The left navigator has horizontal tabs above its content: **All**, **Active**, **Running**, **Waiting**, then **Opened**. These tabs are views of the same
project/session data. Each view uses a predicate over the same session model.

Top-level rows separate the local computer (`username @ hostname`), local myCodex
(`myCodex @ hostname`) and each remote myCodex connection (`myCodex @ remote-host`).
The local computer comes first, local myCodex second, and remote groups follow
in name order. Server/container nodes within them keep backend routing explicit,
with short labels such as `Main` or the container name. Each expands into compact
folders and sessions ordered by recent use. No project creation is needed.
Group labels use discovery-provided host identity; configured SSH aliases are
the fallback until discovery reports the remote hostname. Terminal headers keep
the full server/container/host labels. Manual connection names are preserved.

The directory prefix common to visible sessions on a server is omitted and shown
in that server's details. This is calculated after applying the current view and
unsaved-entry filter, so hidden history cannot leave extra folder levels in Active,
Running or Waiting. Counts and aggregate indicators also describe only the visible
sessions. Folder chains without sessions or branching are
collapsed into one label; folders containing sessions stay explicit. With one
working directory, its basename remains as a folder under the server. For example,
with common root `/home/alice`:

```text
Projects                             Terminal workspace
[All] [Active] [Running] [Waiting] [Opened]
Search conversations…
Scope: All servers
v alice @ workstation
  v Main
    v git/team
      v alpha
          Refactor parser
      > beta
    > docs/notes
  > ~/.codex-work
> myCodex @ workstation
> myCodex @ remote-host
```

Names use configuration and user/host context, with a launch directory as a
fallback and numeric suffixes for otherwise identical automatic labels. PID,
full endpoint, executable, Codex home and cwd are details, not visible identity.
Manual connection names are preserved. Empty configured servers remain visible
in All, while filtered views show only matching branches.

The terminal owns the main workspace. There is no initial requirement for an
editor, file browser, dashboard, right-hand inspector, terminal splits or
customizable docking. These are not implied by the IDE resemblance.

## 2. Session views

The five views share one model, one connection per backend and one set of pushed
state updates. Filtering never opens/closes a terminal or resumes/cancels work.
All is the initial view; search temporarily covers all states and restores the
selected filter when cleared.

| View | Membership |
| --- | --- |
| All | Saved history and visible live sessions, including disconnected cached rows and open terminals. |
| Active | Loaded sessions and open terminal views, including retained client errors and other runtime states. |
| Running | Confirmed active turn with no waiting flags. |
| Waiting | Confirmed idle prompt or active turn waiting for input/approval. |
| Opened | Every open GUI terminal view, including retained exited/error output; visibility settings do not hide these views. |

Active includes server runtime presence and terminal views that the user has
opened. A retained exited/error client belongs to Active and Opened, even when the
server still advertises its thread as idle or running. A normal idle session is ready for a new prompt.
An approval is explicitly labelled as such in its status details. All confirmed
waiting states use green dots, including ordinary idle prompts, questions and
approvals. Running stays blue; errors stay red; saved history is neutral and
unknown/disconnected state has a hollow dot. A loaded server error stays in
Active without being misclassified as running or waiting. Unrecognized activity
flags also stay in Active and have an explicit other-activity label.

On disconnect, retain last-known Active membership with a hollow unknown dot.
Unknown rows never appear in Running or Waiting. At reconnect, the
complete loaded inventory reconciles cached rows outside the history page too;
partial/failed inventories do not falsely mark them unloaded. Pushed status and
closed events take priority over older snapshots. `thread/closed` moves a row to
saved history when no terminal is open; it does not delete it. No periodic status request is introduced.

Loaded does not prove that another terminal is attached: the server may retain
an unsubscribed idle thread for its unload grace period. This is the observable
runtime boundary, rather than a guess based on terminal output or process lists.

All and Active remember expanded groups; the first visit inherits the current
expansion state. Running, Waiting and Opened expand every branch by default, including
new branches arriving through events. Explicitly collapsed branches are remembered
separately for those views during the current GUI run and are not reopened by
updates or tab changes. Matching sessions retain selection through recompression
and search return. Switching views does not activate a terminal. The initial sidebar is wider to accommodate the
five labels; existing saved window geometry remains respected.

Rows update only when their values change, with changed roles sent together.
Changes into running, idle/waiting or other loaded states advance recency using
the event receipt time. Duplicate status notifications, inventory refreshes and
thread-close events do not promote old history. A concurrent older metadata read
cannot roll recency back. Sorting uses the later of this activity timestamp and
the last terminal focus time; host groups retain their fixed order.
Tooltips are escaped plain metadata
rendered beside the navigator, anchored to the hovered row, and remain a stable
snapshot while visible. This prevents metadata bursts from continually replacing
hover text. Fresh status is available when the tooltip is opened again.

Right-click menus use the clicked row, independently of the selected terminal:

| Row | Actions |
| --- | --- |
| Session | Fork, New, Close |
| Directory | New, Expand All |
| Host group or server/container | Expand All |

New uses that backend and exact directory. Fork uses the selected backend's
native `thread/fork`, then opens its returned conversation ID; it does not infer
identity from a title or launch against a default server. The backend first
reads the source's metadata, and myCodex additionally requires execution evidence
for that container. Unsupported/failed forks report an error without a fallback.
Pending forks are not retried automatically after a timeout or disconnection.
Synthetic new-session terminals have no confirmed ID and cannot be forked.
Close releases only that row's open terminal, including retained exit output;
it is disabled for conversations without a terminal view. Expand All expands
the chosen subtree and records that expansion in the current view. Actions that
need a connection are disabled while disconnected and rechecked when invoked.

## 3. Existing-session discovery

Opening the application automatically discovers the current user's host-visible
Codex servers and connects to their existing sessions. The startup/reconnect
inventory includes loaded threads independently of paginated history. Project
rows arise from session working directories; they disappear when no sessions
remain. Cached sessions still supply groups while disconnected.

The **Discover** action explicitly repeats host socket discovery. Linux inotify
events discover new normal Codex sockets during operation. Neither mechanism
infers session status; that is supplied by app-server events. Discovery does not
start servers, resume conversations, launch terminals or create directories.

Identical working-directory paths on separate servers stay separate. Their root
supplies backend identity, while folder/session rows retain it for exact routing.

**View → Hide unsaved sessions** defaults to on and is persisted in `window.ini`.
Saved means a non-ephemeral session with a rollout path or paginated history in
the server store. A null path alone does not prove absence. Known missing entries
are hidden after a server metadata read; unrelated errors do not mark them missing.
Older cached entries without persistence metadata remain until fresh evidence
arrives. An open terminal overrides this visibility filter so errors remain
reachable. Closing its view removes that exception. Terminal presence and exit
messages are never persisted as live state across application restarts.
Opened always exposes those terminal views, including ones excluded by myCodex's
WORKDIR filter in the other tabs. It opens expanded by default. Closing a view
removes its Opened entry without altering the underlying Codex conversation.

Manual connection configuration remains available for custom endpoints and
restricted discovery environments. It is not required for discoverable local
servers. Previous display-name metadata is retained, but an old empty manual
project no longer creates a standalone row. See [local discovery](discovery.md)
for what host visibility and event coverage mean.

## 4. Project and session rows

Each server/folder row has an expand/collapse control, a name and an aggregate
status indicator. Each session row has its title and its own
status indicator. Full paths, backend details and status descriptions can be
shown in tooltips and accessible text instead of occupying several columns.
Use the existing MRU model for both project and session ordering. Expanding a
project or receiving output does not count as using a conversation. Preserve
selection and avoid moving rows under an ongoing keyboard or pointer action;
exact MRU update timing remains a design detail to settle.

Indicators distinguish running, waiting for approval/input, idle, failed and
unknown/disconnected states. Retained client errors stay in Active only. Do not communicate these differences through color
alone. Use accessible names and a supplementary symbol or text where necessary.
Choose actual colors from the desktop theme during visual refinement.

A collapsed project's indicator must still reveal that one of its sessions needs
input. Proposed attention priority is waiting, error, running, then idle; retain
connection uncertainty separately rather than hiding it behind that aggregate.
Detailed counts can appear in the tooltip. Final indicator appearance is open.

## 5. Opening a session and terminal behavior

Proposed activation behavior is Enter or double-click on a session row; a single
click selects it. Activating a session opens/focuses its Codex TUI in the main
workspace and passes the exact backend and thread identity to the terminal
service. Existing terminal views can be focused even while the backend is
disconnected. A new attachment first validates the session with its own server;
a failure displays that server error and does not fall back to another endpoint.
Selecting or expanding a folder does not launch a terminal.

Keep one visible terminal pane initially. Version 0.3.0 retains explicitly
opened terminal clients until closed, without a terminal tab strip. The
session-filter tabs are navigation views, not terminal tabs. Only explicitly opened
sessions may create terminal widgets, and changing a filter cannot detach or
cancel the current session.

The workspace header identifies the conversation, project and backend even when
the session is hidden by a navigation filter. Before any session is opened, show
a "Select an existing session" empty state and the Discover/custom-connection
actions. Application-wide discovery status belongs in the status bar.
Client exit retains the terminal buffer and displays an inline exit/crash message.
An explicit Restart client action revalidates an existing conversation against
its selected server and creates a terminal using the current connection profile.
The previous terminal and output remain until validation and component creation
succeed. A live client cannot be replaced through Restart client, and
Close terminal releases the view. Opening other sessions does not discard it.
The terminal's lifetime is separate from the server conversation's lifetime.
Ctrl+Tab switches through recently used open terminals, and Ctrl+Shift+Tab
reverses direction. Holding Ctrl freezes the list so repeated Tab presses cycle
through it; releasing Ctrl makes the selected terminal most recent. Exited views
remain in the list until closed. Ordinary keyboard input stays with the TUI,
and these shortcuts are not intercepted inside separate dialogs. Switching
focuses an existing terminal; it does not reconnect, resume or query a server.
A new-session launcher has no conversation ID until Codex creates it. It is
shown as **New session terminal** until explicitly closed; its independently
discovered conversation can also appear. Automatic correlation is not guessed
from titles, cwd or output. For an existing conversation, restart uses the same thread ID; for a new-session
launcher it repeats the new-session action. No automatic restart loop is used.
Status dots retain their colours when selected, focused or disabled; selection
highlighting must not invent a different session state. Closing a failed terminal
clears its client error and reveals the server state; this is not a reconnect.
A small packaged Konsole extension exposes hold-open and process-exit events;
Archcodex rejects a component lacking those capabilities with a clear error.

### Settings

**Settings → Configure Archcodex…** opens a modal with a category list and pages.
Application contains the saved-session visibility setting, synchronized with the
existing View menu shortcut. Backends contains myCodex's shared-history visibility
option: WORKDIR and its subdirectories only by default, with an explicit option
to show sessions outside it. This applies before tree compression and aggregation
in history/activity tabs and search results. Opened always lists open terminal
views. Hidden sessions are not discarded.
Apply saves without closing; OK saves and closes; Cancel discards unapplied edits.
The settings use the existing application-specific `window.ini` file. Additional
backend or terminal pages can be added when they contain actual settings; no empty
pages, plugin ABI or generalized settings schema are introduced.

Opening a session transfers keyboard focus deliberately to the terminal. Tree
navigation and view switching must remain available by keyboard without stealing
normal Codex TUI input. Exact shortcuts will follow native KDE conventions when
the interaction design is finalized.

## 6. Search and connections

Retain the existing requirement to search titles and conversation text. The
proposed initial placement is one search field in the left pane with an explicit
scope: all servers, or the selected server/folder. Folder scope includes
descendants; a selected session scopes by its working directory on that server. Results identify their project,
session and backend and open the corresponding conversation.

Searching conversation history is distinct from filtering the Active view. A
global search should include idle history; its scope must be visible. A simple
results view can temporarily replace the tree while keeping session-filter state
available when search is cleared. Offline or incomplete coverage is shown as
such. Final result presentation and keyboard interaction remain open.

Backend setup is reached through **Connections**, outside the project hierarchy.
Use a small connection summary in the status bar rather than an initial
connection dashboard. Connection status and session status remain different
concepts. A disconnected backend keeps its saved projects visible in All.

### Discovery feedback

The explorer contains its controls and tree, plus search-result feedback only
while a search is active. Routine tab explanations stay in tab tooltips. Successful
discovery does not insert a server count or explanatory banner above the tree.
Connection readiness remains in the application status bar: “Connections ready”
when all profiles are connected, with counts only when partially connected.

The status bar exposes a compact, keyboard-accessible **Discovery limited** or
**Discovery failed** action when applicable. It also shows transient checking
state and an actionable no-local-servers state. A successful nonempty discovery
clears that action. **Connections → Discovery details…** always remains available.
No discovery event opens a window, steals focus, starts a toast timer or interrupts
terminal input automatically.

Clicking the action opens one native, nonmodal details window. It shows the time
of the last check, what failed, its effect and a useful next step. Each issue has
collapsed **Technical details** with exact paths, failed operations and OS errors.
Controls provide **Retry discovery**, **Add connection…**, **Copy report** and
**Close**; Escape closes the window. Retry is disabled while a requested check is
pending. Identical reports preserve expanded details and text selection. Updated
reports keep expanded sections by issue code; successful checks clear old issues.

Unidentified processes are distinguished from confirmed Codex processes. A denied
executable link does not prove that a Codex session is missing. If all expected
servers are listed, the user need not fix unrelated inaccessible processes.
Reports include bounded examples and explicitly state when further entries were
omitted; they are held in memory and copied only on the user's action.

## 7. First UI validation checklist

- The left pane offers All, Active, Running, Waiting and Opened; all preserve project/session
  hierarchy and distinguish same-named projects on different backends.
- Existing sessions appear without project setup. Separate servers within host groups preserve
  routing even for identical working directories and thread IDs.
- MRU ordering and pagination keep large histories usable without moving the
  selected row unpredictably.
- Pushed events update row indicators and Active membership without recurring
  status requests. Reconnection handles unknown state explicitly.
- Collapse, filter changes and a session becoming idle cannot close the selected
  terminal or cancel a turn.
- Keyboard navigation, theme contrast, accessible status names and terminal
  focus work with the simple two-pane layout.

These are UI acceptance criteria. See [implementation and testing](implementation.md)
for current evidence, dependencies, packaging permissions and outstanding host
checks.
