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
