# Unread sessions

Unread replaces the fourth Projects tab, Waiting. The short name covers both
updates not yet visited and unresolved input or approval requests. It is a view
of the same sessions, with the same host hierarchy, directory compression,
visibility settings, search scope and recent-use ordering.

| Situation | Behavior |
| --- | --- |
| An observed running session becomes idle or blocked | Mark unread unless its terminal is already being viewed. |
| An observed live asynchronous question arrives | Mark unread using the existing question-event support. |
| An ordinary idle session is discovered or reconnected | Do not invent an unread update. |
| A snapshot reports an unresolved blocking request | Include it in Unread, even without an unread marker. |
| The user views the session | Clear its unread marker. Keep an unresolved request until its server status clears it. |
| The server starts another turn, unloads the thread, or disconnects | Preserve an unvisited update. |
| A popup expires, is dismissed, or cannot be delivered | Leave unread state unchanged. |
| A session or configuration is explicitly removed | Remove its unread record. Missing automatic connections with pending attention are retained during Refresh. |

An amber marker accompanies the existing execution-status dot and propagates to
folders, configurations and hosts. It identifies unread or unresolved work;
bold titles distinguish an unvisited update. Reading an unresolved request removes
the bold text, while its amber marker remains. Parent counts count sessions, not
events, and reflect their visible descendants. The Unread tab count covers eligible
sessions even when its current text filter or selected scope hides some of them.

The session context menu contains exactly one of **Mark as unread** and **Mark as
read**, based on its current unread flag. Marking read cannot answer or dismiss a
server request. The action targets the clicked session, including when another
terminal is selected. Marking the displayed session unread lasts until another
visit; metadata changes alone do not undo that choice.

**Next unread session** is available in the View menu and command palette, with
an optional shortcut in Settings. It includes unresolved requests, opens through
the normal verification path, and cycles in server-activity order so merely
visiting a still-blocked session cannot reorder the cycle. Existing tab and
Ctrl+Tab ordering stay unchanged. The fourth tab retains its saved shortcut binding.

A visit requires a real client displayed in an active, visible, non-minimized
Archcodex window. Browsing rows, expanding parents, opening placeholders, failed
verification, and Ctrl+Tab previews do not count as visits. The newly read row
temporarily remains in Unread with a `Read` suffix until another terminal is
selected, so the tree does not immediately disappear under the user's selection.
It no longer contributes to attention counts. No terminal or tab is switched
automatically by an incoming update.

The unread flag is saved independently of replaceable server metadata in the
existing asynchronous SQLite store. Records use the exact backend/thread pair and
the existing execution-source identity; identical IDs in other containers or hosts
do not acknowledge each other. The table is additive and keeps database version 2:
older versions can open the cache but do not display or clear unread markers.
Returning after using an older version can therefore show markers that were read
there. A changed execution-source identity invalidates its old markers.

Live occurrences receive an in-process sequence at detection, before metadata
classification or GUI batching. A visit acknowledges occurrences detected before
that visit; late delivery cannot make them unread again. Pending delivery combines
multiple occurrences for the same thread, retaining its newest occurrence, rather
than evicting another session's update during a burst. There is no event journal
or second runtime state machine. Read state and known server status remain separate.

Limits remain those of the current connection and terminal integration. Archcodex
cannot reconstruct work completed while disconnected. An asynchronous question
cannot remain classified as unresolved after reading unless the server also
reports a blocking status. In-TUI `/resume`, `/fork`, `/new` or agent navigation can
change the displayed thread without changing Archcodex's binding. Starting a
client also does not prove that the TUI successfully rendered a resumed thread;
known placeholders/errors are excluded, but a failure reported only inside the
TUI can occur after acknowledgement. These require observing the client protocol,
as described in the existing workspace design; unread tracking does not parse
terminal text or infer those transitions.

## Implementation choices and validation

The user approved persistent observed markers, keeping unresolved requests after
visiting, the Unread label, and one state-dependent context-menu action. The
following implementation choices make that behavior concrete:

| Choice | Files and consequences | Status |
| --- | --- | --- |
| Store only unread rows in an additive table; delete a row when marked read. | `src/store.*`, `src/asyncstore.*`, `src/types.h`: existing metadata writes remain independent; no new runtime dependency. Database version stays 2, with the downgrade limitation described above. | Implemented and tested. |
| Reuse the existing source hash with a stricter wrapper for workspace restoration. | `src/workspace.*`, `src/window.*`: renames and transient relay changes preserve markers; endpoint/environment changes invalidate them. Learning a previously unknown Codex home also changes the source identity, conservatively invalidating a manual marker made before identification. | Implemented and tested for source replacement. |
| Stamp occurrences before delayed metadata delivery and combine pending occurrences by thread. | `src/types.*`, `src/backend.*`, `src/asyncbackend.cpp`, `src/window.*`: a late event cannot undo a later visit. A backlog can produce one popup update for several occurrences on the same thread. Existing disconnect-generation invalidation remains in force. | Implemented; delay, burst and reconnect tests pass. |
| Use amber `#f0b232` beside the status dot, bold unread titles, and numeric parent/tab counts. | `src/sessionmodel.*`, `src/window.*`: unresolved requests keep their marker after reading; the tab's total includes eligible sessions hidden by its query/scope. Original status colors and MRU sorting remain separate. | Implemented; model and rendered-window checks pass. |
| Retain the fourth tab's internal `view_waiting` command ID and leave the new navigation command unassigned. | `src/window.cpp`: existing custom Alt+4 bindings survive; users can assign Next unread session in Settings. Its cycle uses activity rather than visit order. | Implemented; existing shortcut/navigation tests pass. |
| Retain a just-read row only while it is being viewed in Unread. | `src/sessionmodel.*`, `src/window.*`: the temporary `Read` suffix prevents immediate removal under the selection without counting the row as pending work. | Implemented and tested. |
| Validate in the retained KDE SDK and with private X11/D-Bus services and real Konsole PTYs. | Tests use Qt 6.11.3 and the bundled patched Konsole 26.04.3. A temporary harness extracts `patchelf` without installing it, copies test binaries and libraries, isolates HOME/XDG directories, and removes its artifacts afterward. No application dependency or Flatpak permission changes are required. | Completed; actual installed Flatpak/KDE desktop behavior remains a host verification step. |

Validation on 2026-11-23: core **20 passed**, runtime **46 passed**, model **38
passed**, workspace **17 passed**, notifications **15 passed**, window **63
passed**. Optional real-Codex, systemd, window-manager activation and performance
cases were skipped where their prerequisites were not enabled. The rapid
large-search case was then run explicitly: **7,200 delivered / 5,760 visible**,
maximum GUI scheduling delay **5.120 ms**, terminal echo **13.069 ms**, terminal
paint **27.494 ms**, and zero unobserved echoes. These are one isolated fixture
run, not measurements of the user's desktop or a comparative performance claim.
The synthetic-client window suite also emitted process-teardown warnings; its
reported process and temporary-directory leftovers were checked afterward and
were absent.

The feature is implemented on `feature/session-attention`. Source and
documentation are committed locally; this task does not change `VERSION`, create
a release tag or Flatpak candidate, or publish to GitHub. Test-only copies in the
existing SDK are retained for incremental builds; the temporary harness is removed.
