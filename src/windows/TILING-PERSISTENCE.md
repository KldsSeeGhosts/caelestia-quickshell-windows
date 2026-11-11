# Tiling layout persistence

The manager saves `tiling-layout.json` in `CAELESTIA_STATE_DIR`, or in
`QStandardPaths::GenericStateLocation/caelestia` when no profile override exists.
It uses compact version 1 JSON and `QSaveFile` atomic replacement, with no
direct-write fallback. It captures before clean stop restores floating
rectangles, before `aboutToQuit` unparks windows, and every two seconds while
enabled. Unchanged snapshots do not rewrite the file. A crash can lose the
changes since the last periodic snapshot.

The snapshot contains:

- `windows`, ordered by layout traversal, with hexadecimal string `hwnd`,
  `pid` and process creation `started` FILETIME, SHA-256 `app` and `title`
  hints, `monitor`, desktop GUID `desktop`, explicit `override`, `floating`
  and `autoFloat` state.
- `layouts`, with monitor key, desktop GUID, layout `kind` and `tree`.
  Dwindle saves its split tree, axes, pinned splits and ratios. Master saves
  entry order, weights and master factor. Scrolling saves column and row order,
  weights, widths, focused rows, active column and viewport offset.
- `options`, with the layout selection and layout-only split, master and
  scrolling options. These restore runtime choices after an update restart.
  An explicit option write by the new facade wins, even if it writes a
  default value. Opt-in, allowlists and exclusions are never restored.

IDs in trees are HWND tokens, never in-process object pointers. Monitor keys
are the screen registry's persistent interface-path hashes, not labels or
GDI display numbers. Session-only monitor identities are not saved. The file
contains no raw app ids, titles or monitor interface paths. Logs contain
neither snapshot contents nor state paths.

On start, exact HWND/PID/creation-time matches reserve their windows first.
A missing HWND can match by a unique app/title hint pair only within the same
PID and creation time. A handle now owned by a different process cannot use
the hint fallback. Missing creation times, ambiguous hints, vanished windows,
missing monitors and missing desktops do not match. A changed layout kind uses
ordinary adoption instead of restoring an incompatible tree.

Enabling explicitly rescans the tracker before planning adoption, including
when the normal startup scan still waits for the first shell frame.
The manager restores matched leaves before adopting unmatched windows through
the existing startup rules. Missing leaves collapse without changing surviving
leaf order. Current scope, privilege and floating rules still apply.
Explicit floating/tile overrides and automatic floats survive a stop/start.
Restoration uses the saved monitor for screen selectors while tracker facts
catch up. Desktop transfers use the existing virtual desktop provider. A
refused transfer falls back to ordinary adoption, never simulated input.
Stale entries disappear from the next snapshot. Reads are bounded to 4 MiB
and 4096 windows, and unknown format versions use ordinary adoption.

A QML generation replacement that retains the lease keeps the live trees.
If teardown stops the session, the process-lifetime manager retains its latest
snapshot in memory and restores it on the next enable. A new process has no
such cache and reads the same snapshot from the profile state directory.
Observe/off modes do not start persistence or adopt windows. Existing parked
window rescue behavior is unchanged.

Fixtures run only through CTest with the offscreen QPA plugin. Matching, tree
round trips and pruning are pure logic in `tiling_layout`; reload and fresh
manager/disk adoption use the existing fake desktop in `tiling_reload` and
`tiling_manager`. `tiling_compile_check` compiles the real native integration
without executing it.

Live acceptance belongs to the owner. Tile and reorder windows across two
monitors, change a split or column width, and leave an explicit float. Reload
the shell, then restart through an update. Check the same monitor, workspace,
tile order, split proportions and floating state after each operation.
