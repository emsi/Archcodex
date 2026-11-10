# Session notifications

**Settings → Notifications → Notify when Codex needs attention** is enabled by
default. Archcodex sends a desktop notification when it observes a session change
from Running to Waiting. Waiting includes idle prompts, approvals and blocking
questions. Ordinary discovery, history reads and reconnect snapshots do not alert.

The notification identifies the session and its server/container. Click its body
or **Open session** to focus an existing terminal or use the usual validated
session-opening path. Explorer filters do not prevent opening the target. Existing
exited terminal views retain their output; unavailable targets show a status-bar
message. Questions are answered in Codex. Their text and answer choices are never
included in desktop notifications.

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

- `CodexBackend` detects transitions before `BackendPump` combines metadata
  updates. Notification events travel separately from replaceable inventory.
- Each backend has at most 256 pending attention events and one acknowledged GUI
  batch (up to 32 events). At overload, the oldest pending events are discarded;
  one warning is emitted per overflow episode. Connection generations invalidate
  pending events, including while the GUI is busy or disconnect is requested.
- Live question tracking is bounded to 4,096 turns and 256 question IDs per turn.
  Completed/unloaded turns and disconnected generations release that state.
- The presenter retains at most 128 notifications, one per backend/session.
  A new occurrence replaces that session's older popup. Flatpak delivery has at
  most 128 pending calls with five-second reply deadlines; overload drops new
  deliveries. This is a resource bound, not a notification delivery guarantee.
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

The GUI explicitly links Qt DBus, KF6 Notifications and KF6 WindowSystem,
already available in the KDE runtime. Flatpak permissions are unchanged.

## Validation

Protocol tests cover transition deduplication, rapid updates, delayed metadata,
backpressure/reconnect invalidation and structured live question events. Portal
and native daemon fixtures exercise real D-Bus delivery, actions, escaping,
activation tokens, replacement, shutdown and bounds. Window tests cover routing
across backends and filters, reusing terminal views and stale targets.

`test-notifications` needs a private D-Bus session. CTest runs it automatically
when `dbus-run-session` is available; otherwise run the built executable under an
external `dbus-run-session`. Its test-only XCB dependency provides an opt-in EWMH
fixture on a private Xvfb (`ARCHCODEX_TEST_X11_WM=1`, `QT_QPA_PLATFORM=xcb`). Never
enable that fixture on an existing desktop: it owns root-window properties.

The X11 fixture checks requests for the existing desktop and window without a
move request. It is not a KWin acceptance test. Actual KDE/X11 notification clicks
from a sandboxed Flatpak, other desktop policies and XWayland remain host
acceptance checks; this build container cannot create Flatpak user namespaces.
