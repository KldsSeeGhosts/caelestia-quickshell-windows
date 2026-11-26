# T3 progress

- Created `fix/coalesce-panel-geometry-updates-r5` from `e66bb44f71c35ac1cc34342c7a7630bbd917988b`.
- Traced the close path: `beginClose()` flushes pending geometry before starting animation; a geometry probe can reenter visibility control.
- Confirmed the immediate hide branches in `setVisibleDirect(false)` and `hideForScreenLoss()` do not invalidate `mCloseRequestSerial`, and the post-flush guard does not check backing-window visibility.
- Added `immediateHideDuringCloseFlushCancelsAnimatedClose` and built `panel_hidden_reembed_test` successfully.
- Pre-fix evidence: running the new case exits 1 because `panel.closeStartsForTesting()` is 1 instead of 0.
- Next: invalidate armed close requests on immediate hides and add the backing-window visibility guard.
