# Session notifications

**Settings → Notifications → Notify when Codex needs attention** is enabled by
default. Archcodex sends a desktop notification when it observes a session change
from Running to Waiting. Waiting includes idle prompts, approvals and blocking
questions. Ordinary discovery, history reads and reconnect snapshots do not alert.

Alerts require saved, non-ephemeral conversation metadata. A live transition may
arrive before that metadata: Archcodex holds it while the already scheduled
inventory read completes. It does not guess a working directory, substitute a
fork ancestor's ID, or generate an alert for an unresumable temporary thread.

**Settings → Backends → Show sessions outside the container's working directory**
controls myCodex notifications as well as Projects and search. By default, a
session's directory must be the container's WORKDIR or a descendant. Paths are
normalized in the server's path namespace; `/work/app-other` is not inside
`/work/app`. Unknown directories cannot qualify under this default. Enabling the
setting broadens scope but does not change the connection used to open a session.
The selected explorer tab and search text do not suppress otherwise eligible alerts.

The selected terminal stays quiet while the Archcodex window is visible, active
and not minimized. Other open terminals still notify, as does the selected
terminal when you switch to another application. Bringing a terminal into view
withdraws its outstanding popup. Suppressed occurrences are not replayed later.

The notification identifies the session and its server/container. Click its body
or **Open session** to focus an existing terminal or use the usual validated
session-opening path. Explorer tab filters and delayed GUI metadata do not prevent
opening the target. A pending view verifies the exact backend/session before launching;
repeated clicks reuse that view. Existing exited terminal views retain their output.
A disconnected, replaced or removed backend withdraws its outstanding notifications.
A failed session verification stays visible in the terminal view. Questions are answered in Codex.
Their text and answer choices are never included in desktop notifications.

Delivery alone does not activate the application. A click requests activation on
the application's existing X11 desktop, without moving its window. Restoring a
minimized window preserves maximized/full-screen state. The window manager can
refuse focus; Archcodex does not force it. XWayland and native Wayland activation
depend on the compositor. Native Wayland is not enabled in the current package.

## Question compatibility

| Server behavior | Notification support |
| --- | --- |
| Emits `thread/status/changed` from Running to Waiting | Supported, including blocking `request_user_input` and approval requests. |
| Broadcasts live `turn/started` and structured asynchronous `item/completed` events to the observer | Supported, independently of Running/Waiting state. Deduplicated by thread, turn and item ID. |
| Does not broadcast asynchronous question items to inventory observers | Asynchronous-question alerts are unavailable unless a Waiting transition occurs. This includes the Codex 0.166.0 behavior verified in issue #2. |
| Question belongs to a turn started before the observer connected | Not notified from replayed items. A subsequent observed Running → Waiting transition still notifies. |

Only an `agentMessage` item with `delivery: "async"` and structured `questions`
qualifies as an asynchronous question. Ordinary prose containing a question mark
does not. Repeated item events do not notify twice. Blocking questions use only
the status transition, so receiving a server input request cannot add a duplicate.
An asynchronous question notification means a question was observed, not that it
is still unanswered.

Detecting and displaying notifications adds **zero Codex requests**. It does not query history,
poll sessions, subscribe by resuming them, or send answers/cancellations. Complete
asynchronous-question coverage on servers lacking observer events needs an
additional passive event source, such as an upstream observer notification API
or a future observer of the already-owned TUI transport. This implementation
does not claim that coverage. Clicking an alert uses the ordinary explicit
session-opening preflight. The protocol investigation is recorded in
[issue #2](https://github.com/emsi/Archcodex/issues/2).

## Implementation and resource bounds

- `Session::inWorkdir` is the common directory-boundary rule used by the explorer
  and notification presenter. The presenter's setting is independent of explorer
  selection/search state. Tightening the setting withdraws excluded popups.
- `CodexBackend` retains at most 256 occurrences awaiting metadata per connection.
  It reuses event-triggered reads, including an already queued refresh when a
  first turn materializes saved history. Ephemeral/unsaved results and failed
  reads discard pending alerts. Close/archive/delete/disconnect also invalidate
  pending occurrences; overload discards the oldest and warns once per episode.
- A started/resumed snapshot seeds unknown or reopened runtime state; it cannot
  overwrite a newer live status and rearm a duplicate Waiting transition.
- `CodexBackend` detects transitions before `BackendPump` combines metadata
  updates. Notification events travel separately from replaceable inventory.
- Each backend has at most 256 pending attention events and one acknowledged GUI
  batch (up to 32 events). At overload, the oldest pending events are discarded;
  one warning is emitted per overflow episode. Connection generations invalidate
  pending events, including while the GUI is busy or disconnect is requested.
- Live question tracking is bounded to 4,096 turns and 256 question IDs per turn.
  Completed/unloaded turns and disconnected generations release that state.
- The presenter retains at most 128 notifications, one per backend/session.
  A new occurrence updates that session's existing popup without withdrawing it.
  Different backends retain separate targets even when shared history gives them
  the same thread ID. Flatpak delivery has at most 128 pending calls with
  five-second reply deadlines; overload drops new deliveries. This is a resource
  bound, not a notification delivery guarantee.
  An older failed delivery cannot erase a newer update. The five-second deadline
  is for the D-Bus reply, not the popup lifetime. Banner duration is controlled by
  the desktop; Archcodex does not request urgent or non-dismissible notifications.
- Disabling notifications and orderly application shutdown withdraw outstanding
  popups. Native daemon expiry also releases application references. No extra
  process is launched. Notification actions are scoped to the running app;
  crash leftovers cannot reopen sessions in a new application instance.

Native installations use KNotifications and `archcodex.notifyrc`.
Flatpak uses the notification portal directly: the bundled KNotifications 6.31
[portal implementation discards action parameters](https://github.com/KDE/knotifications/blob/v6.31.0/src/notifybyportal.cpp#L210-L225).
The small Qt D-Bus adapter preserves the
[portal activation token](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Notification.html#org-freedesktop-portal-notification-actioninvoked).
It accepts signals from the portal service and routes opaque notification IDs
through an application-owned target map. Older portals without a token still
route clicks; successful focus depends on desktop policy. Portal notifications
are controlled by the app's checkbox and the desktop's portal permissions;
native per-event settings use KDE's notification configuration.

Native KNotifications 6.31 has a
[withdrawal race](https://github.com/KDE/knotifications/blob/v6.31.0/src/notifybypopup.cpp#L79-L101)
if a notification is closed
before the daemon's delivery reply arrives: its popup plugin has no daemon ID to
close yet. This upstream limitation is not patched here. The Flatpak uses the
direct portal path, where withdrawal uses the application-owned ID even while
delivery is pending. Host acceptance must still check the desktop's behavior.

The GUI explicitly links Qt DBus, KF6 Notifications and KF6 WindowSystem,
already available in the KDE runtime. Flatpak permissions are unchanged.

## Validation

Protocol tests cover transition deduplication, rapid updates, delayed metadata,
backpressure/reconnect invalidation and structured live question events. Portal
and native daemon fixtures exercise real D-Bus delivery, actions, escaping,
activation tokens, replacement, shutdown and bounds. Window tests cover routing
across backends and filters, reusing terminal views and stale targets.

Regressions additionally cover shared-history WORKDIR boundaries, tightening the
scope while popups exist, distinct actions for different containers, delayed
metadata and first-turn persistence, ephemeral paginated threads, stale started
snapshots, explicit opens without saved history, and foreground/background terminal
suppression. A transport fixture uses
different reply markers per container to detect a crossed route when another
container is discovered, for both local and simulated SSH connections. Passing
those fixtures does not establish the cause of a reported live-container mix-up.

`test-notifications` needs a private D-Bus session. CTest runs it automatically
when `dbus-run-session` is available; otherwise run the built executable under an
external `dbus-run-session`. Its test-only XCB dependency provides an opt-in EWMH
fixture on a private Xvfb (`ARCHCODEX_TEST_X11_WM=1`, `QT_QPA_PLATFORM=xcb`). Never
enable that fixture on an existing desktop: it owns root-window properties.

The X11 fixture checks requests for the existing desktop and window without a
move request. It is not a KWin acceptance test. Actual KDE/X11 notification clicks
from a sandboxed Flatpak, other desktop policies and XWayland remain host
acceptance checks; this build container cannot create Flatpak user namespaces.

## Diagnosing a container attribution problem

Start the candidate with opt-in identity tracing:

```sh
flatpak run --env=QT_LOGGING_RULES='archcodex.notifications.debug=true' io.github.emsi.Archcodex
```

The category records backend/container/endpoint bindings, conversation and fork
identifiers, metadata eligibility, popup targets and terminal reuse. It records
neither conversation text nor credentials. Identifiers and endpoint paths can
still reveal local names, so review a trace before sharing it. Logging is disabled
by default and adds no Codex requests. The actionable identity remains the exact
backend plus conversation ID. Shared history or matching WORKDIR alone is not
proof that a conversation can execute in a different container.
