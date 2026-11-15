# Large-search responsiveness

The opt-in `WindowTests::largeSearchPerformance` fixture exercises the real
Window, SessionModel, AsyncBackend and embedded Konsole PTY. It submits one All
search to three independently running fixture servers (2,400 matches each),
plus 24 disconnected profiles with long names. One fifth of the 7,200 matches
are hidden by the default visibility setting. Paths span several groups and
worktrees. Each page has 100 records; the delayed case adds 40 ms to each page
from one server only. Fixtures run on their own event loop.

A separate producer posts a timestamped GUI callback every 20 ms and a real
terminal-widget key sequence every 100 ms. A synthetic Python PTY child echoes
each sequence; Konsole's accessibility text exposes the received echo. The
reported round trip includes time waiting for the GUI, widget input, PTY I/O
and screen-buffer observation (sampled every 5 ms in the test). It does not
measure physical display scanout or a real Codex response. The final input is
drained after search completion. All matches and visible counts are asserted.
These sampling timers belong only to the benchmark.

Run an optimized build with the packaged Konsole plugin, a private D-Bus session
and a working PTY/X11 display:

```sh
ARCHCODEX_SEARCH_BENCHMARK="$PWD/search.jsonl" \
  dbus-run-session -- build/test-window largeSearchPerformance
```

Repeat each rapid/delayed pair three times without compilation running in the
background. Set `QT_PLUGIN_PATH`/`XDG_DATA_DIRS` for the staged packaged plugin
if it is not installed normally. The benchmark is skipped during ordinary test
runs. It uses temporary profiles, an isolated test home and synthetic messages;
it does not access a user's Codex conversations or credentials.

## Environment and budget

Recorded on an AMD Ryzen 9 3950X, x86_64 Linux, Qt 6.11.3 / KDE SDK 6.11,
GCC 15.2 optimized RelWithDebInfo build, the manifest's patched Konsole 26.04.3,
Xvfb 1600×1200×24 and a private D-Bus session. Window: 1400×950; requested
Projects pane: 360 pixels; default font: 9 pt. The SDK libraries run with the
host PTY through the SDK loader. Full Flatpak namespace execution and a real
KWin desktop are separate host acceptance checks.

The target for this fixture is GUI queue-delay p95 ≤50 ms and terminal round-trip
p95 ≤100 ms, with both maxima ≤200 ms. These are regression targets for this
controlled load, not hard real-time guarantees on arbitrary hardware. Report
per-run distributions and sample counts rather than claiming a universal FPS.

## Baseline

Application source: `7de03f61804882a6195d41b74c4800ceeb18191b` (0.11.0),
before either #23 or #24. Raw runs are in `search-baseline.jsonl`.

The 24 error messages consumed 637 pixels above a 68-pixel tree viewport.
Further measurements below distinguish the layout-only change, removal of the
duplicate expansion pass, and the final performance implementation.

## Layout fix (#23)

The feedback area is now 41 pixels high and the tree viewport 664 pixels.
The details dialog is non-modal, read-only plain text and scrollable; repeated
diagnostics are deduplicated per backend ID. It closes when leaving All, clearing
or resubmitting. Narrow-pane tests at 10 pt and 20 pt check viewport height,
keyboard access, focus and selection preservation, completeness and stale errors.

Median of three per-run p95 values (milliseconds); raw records retain maxima
and sample counts:

| Stage | Replies | GUI p95 | PTY echo p95 | Search duration |
|---|---|---:|---:|---:|
| Baseline | Rapid | 432.1 | 908.5 | 1457 |
| Baseline | One delayed source | 180.3 | 618.7 | 1975 |
| Layout only | Rapid | 363.0 | 848.2 | 1463 |
| Layout only | One delayed source | 133.3 | 486.9 | 1972 |

The layout fix does not meet the responsiveness budget; #24 remains necessary.
The complete Window suite passed after staging its required notification
configuration (the first invocation omitted that resource and its notification
test failed; that test passed on rerun). Real desktop acceptance remains separate.

## Performance fix (#24)

The duplicate expansion pass was removed and measured first at `4298f3f`.
It helped, but did not meet the budget. The subsequent changes are:

- AsyncBackend acknowledges a page only after the GUI consumes its final slice.
  Each backend retains at most one outstanding page, normally 100 records.
- A shared GUI queue delivers up to 32 records per turn, round-robin across
  ready backends. A one-shot 1 ms timer yields between slices and is armed only
  when work is queued. Slow servers do not hold other servers back. Error and
  completion messages follow their page's final slice.
- Internal request revisions invalidate queued slices on replacement, clear,
  disconnect and backend removal, independently of caller-supplied search tokens.
- Existing folders receive rows directly, with affected ancestor aggregates
  updated once per batch. Hidden additions do not regroup the tree. New paths,
  moved/removed visible rows and visibility changes retain the existing compact
  grouping algorithm. Regrouping scans only that backend's indexed records and
  leaves unchanged session metadata alone.
- Result pages preserve browsing selection, manual collapse and scroll position;
  the displayed conversation is selected once when it first becomes available.

The 32-record slice and 1 ms yield were selected from the measured workload.
Folder topology can still need a whole-backend regroup when paths change;
there is no hard real-time guarantee for arbitrary project counts or hardware.
Qt models remain on the GUI thread. The existing 10,000-result per-backend limit
and its coverage diagnostic remain in force.

| Stage | Replies | GUI p95 | PTY echo p95 | Search duration |
|---|---|---:|---:|---:|
| One expansion pass | Rapid | 319.5 | 805.0 | 1424 |
| One expansion pass | One delayed source | 146.2 | 514.5 | 1983 |
| Paced + incremental | Rapid | 9.0 | 56.4 | 1924 |
| Paced + incremental | One delayed source | 7.6 | 23.5 | 2749 |

Across the six final runs, the largest GUI queue delay was 20.1 ms and the
largest observed PTY round trip 57.6 ms. Every run delivered 7,200 matches,
5,760 visible rows, and every offered terminal input echo. The tradeoff is
roughly 0.5–0.8 seconds longer total search duration in this fixture while
input and painting can proceed. All three responsiveness targets are met.

Regression coverage includes explicit page acknowledgements, wrong/stale
acknowledgements, the result cap, a stalled source, complete sliced delivery,
and clear/replacement/disconnect/removal from inside a result callback. Model
tests compare incremental grouping and aggregates with a fresh model and check
index stability, removal and reset. Window tests exercise live search browsing,
scroll position, manual collapse, terminal focus and existing per-tab semantics.
The performance implementation passed 35 Window cases (two opt-in benchmark
cases skipped in that ordinary run), 29 model cases and the eight focused
search-runtime cases, plus six explicit benchmark runs. Counts include QtTest
initialization/cleanup. Candidate-wide checks and desktop verification belong
in the RC testing receipt.

## Candidate review follow-up

Final review after building 0.11.1-rc.1 added two stronger local-filter
regressions: adding a match to an existing folder left the summary count stale,
and the viewed conversation could re-enter that folder's filter without regaining
its missing selection. Both were reproduced. The Window now observes the model's
top-level aggregate changes for those updates, retaining browsing selection when
one exists. This uses the model's existing dataChanged signal and does not restore
the expensive search regrouping/expansion passes. The completed rc.1 identity is
retained; 0.11.1-rc.2 supersedes it for desktop testing.

## Diverse working directories and native input (rc.3)

Desktop testing of rc.2 still found perceptible typing delay during result delivery.
The stronger fixture at `7058831` keeps the 7,200 matches, 24 disconnected sources
and hidden-record ratio, but adds clustered, distinct, deeply nested and single-folder
workloads, plus a delayed distinct source and an idle control. Distinct/nested cases
have 2,400 different working directories per server before visibility filtering.
The viewed conversation is among the results. Searches repeat for at least ten
seconds, completing the final search; measurements include replacing the preceding
result set. The original clustered fixture introduces directories in only four of
96 delivery slices per server.

The opt-in `searchWorkloads` QtTest cases require X11 and runtime `libXtst`. They
inject keys through XTest into the focused terminal and run a normal `QEventLoop`
during measurement instead of QtTest's wait/sleep loop. Input offers still originate
from an independent thread. The synthetic child redraws its acknowledgement text;
all acknowledgements remain visible so a delayed observer cannot lose earlier keys
to scrollback. A paint observer records echoes present before a terminal paint and
timestamps a queued callback after that paint returns. This includes callback
scheduling delay: it is a conservative paint-completion observation, not physical
screen scanout or a measurement of the real Codex TUI. The idle control uses the
same child and input path. The final harness also records synchronous submission
cost separately; the latency distributions continue to include it.

Run with the same staged SDK/packaged Konsole environment described above:

```sh
ARCHCODEX_SEARCH_BENCHMARK="$PWD/search-workloads.jsonl" \
  dbus-run-session -- build/test-window searchWorkloads
```

`search-distribution-before.jsonl` records the unchanged rc.2 application with the
stronger harness, one run per case. In the distinct-project case, p95 GUI delay was
125.7 ms, terminal echo 447.5 ms and paint observation 852.6 ms. Nested projects
also reproduced substantial delays. The result count was therefore insufficient
as the earlier benchmark's principal scale dimension.

The follow-up implementation keeps the existing Qt model and compact grouping:

- An ordered index of visible working directories finds the adjacent branch when
  a new directory arrives. Only that branch is split or reparented. A common-root
  change retains existing session objects. General regrouping remains the path for
  removals, moved sessions and changes to visibility rules; randomized insertion
  orders and batches are compared with that independent reference algorithm.
- New rows carry their metadata before insertion, same-parent session additions
  are inserted together, and new folders have their initial recency before entering
  the sort order. Recursive proxy filtering is unnecessary because the source
  model already determines membership and ancestor counts.
- Appending history adds counts and maxima to affected ancestors once per batch.
  Changes to existing rows and removals retain full aggregate calculation so a
  decreasing maximum is handled correctly.
- Branch moves preserve navigation only within the moved branch. Restoring saved
  keys uses indexed lookups, and newly visible branches receive default expansion
  locally. Query replacement uses a model reset and avoids preserving navigation
  for rows being discarded.
- Delivery estimates synchronous work per record and targets 4 ms of application
  work, within 1–32 records per slice. Separately, elapsed time until the next turn
  includes deferred Qt work and determines a 1–8 ms one-shot yield. Charging fixed
  layout cost per record would shrink batches towards one row and repeat layout
  unnecessarily. One outstanding page per backend and generation cancellation
  remain the bounds on queued work.

The 4 ms target is adaptive, not preemption: a Qt layout, branch move, explicit
filter rebuild or large model reset can still exceed it. Widget/model operations
remain on the GUI thread. Validation must report maxima and query replacement
costs as well as p95; the desktop's actual Codex workload remains a separate
acceptance check. The new synthetic comparison targets p95 GUI delay ≤10 ms,
terminal echo ≤30 ms, and paint observation ≤50 ms; maxima ≤125 ms including
repeated query replacement. These are test-workload targets, not production
hard-real-time guarantees.

### Measured result

Application changes: `3066190` (0.11.1-rc.3). Final runs are recorded in
`search-distribution-after.jsonl`: three runs per case, 1,932 native input echoes
and matching paint observations, 75 completed searches with all 7,200 records
per search. Every case's final visible count was 5,760. All per-run p95 values
and maxima met the stronger fixture targets above.

Before values are the single stronger-baseline run for each workload; after
values are the median of three per-run p95s, in milliseconds:

| Workload | GUI before → after | PTY echo before → after | Paint observation before → after |
| --- | ---: | ---: | ---: |
| Idle control | 0.03 → 0.03 | 5.9 → 6.0 | 13.0 → 12.9 |
| Clustered folders | 7.3 → 3.3 | 19.0 → 12.7 | 41.7 → 25.0 |
| Distinct projects | 125.7 → 5.3 | 447.5 → 24.6 | 852.6 → 42.9 |
| Nested projects | 81.4 → 5.9 | 266.9 → 25.6 | 541.2 → 45.0 |
| One folder per server | 5.0 → 3.1 | 23.8 → 13.3 | 46.1 → 24.9 |
| Distinct, one delayed server | 132.7 → 5.8 | 437.9 → 25.2 | 834.8 → 40.1 |

Average search duration within each run, taking the median of those averages,
was 2.5 seconds for distinct projects (baseline 20.3 seconds), 3.8 seconds for
nested projects (15.6 seconds), and 3.0 seconds with the delayed server
(21.0 seconds). The maximum across all final runs was 59.5 ms for GUI delay,
72.3 ms for echo, and 91.4 ms for paint observation. Synchronous submission,
including replacement of the previous full tree, reached 64.3 ms. These occasional
replacement costs remain visible in the distributions; the adaptive target is
not a guarantee that every GUI operation fits four milliseconds.

The final model suite passed 31 cases, including randomized path insertion and
batched comparisons against full regrouping, populated/batched row insertion,
visibility, aggregates and index behavior. The Window suite passed 35 ordinary
cases; eight opt-in benchmark rows are skipped in that suite, with the six native
workload rows executed separately three times above. Existing search cancellation,
page acknowledgement and stale-generation tests also pass. Candidate-wide runtime,
package and desktop boundaries are recorded in the versioned testing receipt.
