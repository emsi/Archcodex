# Initial UI design

Status: rudimentary layout requested by the user; interaction details below are
the design baseline. The [initial implementation](implementation.md) now delivers
the two-pane layout and records which refinements remain open.
Last updated: 2026-11-09.

## 1. Starting layout

Start with a familiar IDE arrangement: a narrow project navigator on the left
and a large terminal workspace on the right. Use ordinary native widgets and a
resizable divider. Keep the first version simple.

The left navigator has horizontal tabs above its content: **All**, **Active**, **Running**, then **Waiting for input**. These tabs are views of the same
project/session data. Each view uses a predicate over the same session model.

Projects are the top-level rows. Each project expands to reveal its sessions,
ordered by recent use. Projects are groups derived from existing sessions; there
is no project-creation prerequisite or standalone empty-project workflow.
The interface must not require navigating a separate host tree before reaching
a project; show a backend label where needed to distinguish locations.

This wireframe expresses arrangement, not final styling or exact dimensions:

```text
+----------------------------- Archcodex -------------------------------+
| Session   View   Connections   Help                                   |
+-------------------------+---------------------------------------------+
| Projects     [Discover] | Refactor parser                             |
| All Active Running Input| Archcodex / laptop                          |
|-------------------------+---------------------------------------------+
| Search conversations... |                                             |
| Scope: All projects  v  |                                             |
|-------------------------|                                             |
| v Archcodex  [laptop] o |              Embedded Codex TUI             |
|     Refactor parser   o |                                             |
|     Packaging notes   o |                                             |
| > Backend API [build] o |                                             |
| v Notes       [local] o |                                             |
|     Search behavior   o |                                             |
|                         |                                             |
|                         |                                             |
+-------------------------+---------------------------------------------+
| Connections: 2 connected, 1 disconnected                              |
+-----------------------------------------------------------------------+
```

The terminal owns the main workspace. There is no initial requirement for an
editor, file browser, dashboard, right-hand inspector, terminal splits or
customizable docking. These are not implied by the IDE resemblance.

## 2. Session views

The four views share one model, one connection per backend and one set of pushed
state updates. Filtering never opens/closes a terminal or resumes/cancels work.
All is the initial view; search temporarily covers all states and restores the
selected filter when cleared.

| View | Membership |
| --- | --- |
| All | Loaded sessions and saved history, including disconnected cached rows. |
| Active | Loaded sessions: running, ordinary idle prompts, explicit input/approval requests, errors and other runtime states. |
| Running | Confirmed active turn with no waiting flags. |
| Waiting for input | Confirmed idle prompt or active turn waiting for input/approval. |

Active describes runtime presence, not whether a turn is executing or whether
Archcodex has opened a terminal. A normal idle session is ready for a new prompt.
An approval is explicitly labelled as such. Ordinary idle prompts use a quiet
neutral dot; explicit questions and approvals use amber, so waiting prompts do
not make every live project look urgent. A loaded server error stays in
Active without being misclassified as running or waiting. Unrecognized activity
flags also stay in Active and have an explicit other-activity label.

On disconnect, retain last-known Active membership with a hollow unknown dot.
Unknown rows never appear in Running or Waiting for input. At reconnect, the
complete loaded inventory reconciles cached rows outside the history page too;
partial/failed inventories do not falsely mark them unloaded. Pushed status and
closed events take priority over older snapshots. `thread/closed` moves a row to
saved history; it does not delete it. No periodic status request is introduced.

Loaded does not prove that another terminal is attached: the server may retain
an unsubscribed idle thread for its unload grace period. This is the observable
runtime boundary, rather than a guess based on terminal output or process lists.

Each view remembers expanded groups. The first visit inherits the current
expansion state, and a selected matching session stays selected. Switching views
does not activate a terminal. The initial sidebar is wider to accommodate the
four labels; existing saved window geometry remains respected.

Rows update only when their values change, with changed roles sent together.
Status-only events do not change recency. Tooltips are escaped plain metadata
rendered beside the navigator, anchored to the hovered row, and remain a stable
snapshot while visible. This prevents metadata bursts from continually replacing
hover text. Fresh status is available when the tooltip is opened again.

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

Sessions from servers established to share the host filesystem environment are
grouped by exact working-directory path. Identical path spellings on unrelated
or container environments remain separate. Thread routing still retains the
backend and thread ID, even when their project row is shared.

Manual connection configuration remains available for custom endpoints and
restricted discovery environments. It is not required for discoverable local
servers. Previous display-name metadata is retained, but an old empty manual
project no longer creates a standalone row. See [local discovery](discovery.md)
for what host visibility and event coverage mean.

## 4. Project and session rows

Each project row has an expand/collapse control, a name, a compact backend label
and an aggregate status indicator. Each session row has its title and its own
status indicator. Full paths, backend details and status descriptions can be
shown in tooltips and accessible text instead of occupying several columns.
If an explicitly grouped project has locations on several backends, put the
backend label on its session rows as well.

Use the existing MRU model for both project and session ordering. Expanding a
project or receiving output does not count as using a conversation. Preserve
selection and avoid moving rows under an ongoing keyboard or pointer action;
exact MRU update timing remains a design detail to settle.

Indicators distinguish running, waiting for approval/input, idle, failed and
unknown/disconnected states. Do not communicate these differences through color
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
service. Selecting or expanding a project does not launch a terminal.

Keep one visible terminal pane initially. Version 0.2.1 retains explicitly
opened terminal clients until closed, without a terminal tab strip. The
session-filter tabs are navigation views, not terminal tabs. Only explicitly opened
sessions may create terminal widgets, and changing a filter cannot detach or
cancel the current session.

The workspace header identifies the conversation, project and backend even when
the session is hidden by a navigation filter. Before any session is opened, show
a "Select an existing session" empty state, automatic discovery status and the
Discover/custom-connection actions.
Client exit retains the terminal buffer and displays an inline exit/crash message.
An explicit Restart client action creates a fresh terminal for that launch, and
Close terminal releases the view. Opening other sessions does not discard it.
The terminal's lifetime is separate from the server conversation's lifetime.
For an existing conversation, restart uses the same thread ID; for a new-session
launcher it repeats the new-session action. No automatic restart loop is used.
A small packaged Konsole extension exposes hold-open and process-exit events;
Archcodex rejects a component lacking those capabilities with a clear error.

Opening a session transfers keyboard focus deliberately to the terminal. Tree
navigation and view switching must remain available by keyboard without stealing
normal Codex TUI input. Exact shortcuts will follow native KDE conventions when
the interaction design is finalized.

## 6. Search and connections

Retain the existing requirement to search titles and conversation text. The
proposed initial placement is one search field in the left pane with an explicit
scope: all projects or a selected project. Results identify their project,
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

## 7. First UI validation checklist

- The left pane offers All, Active, Running and Waiting for input; all preserve project/session
  hierarchy and distinguish same-named projects on different backends.
- Existing sessions appear without project setup. Servers in the same host
  working directory share a group while retaining their separate routing.
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
