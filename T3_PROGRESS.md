# T3 progress

- Created `fix/coalesce-panel-geometry-updates-r5` from `e66bb44f71c35ac1cc34342c7a7630bbd917988b`.
- Traced the close path: `beginClose()` flushes pending geometry before starting animation; a geometry probe can reenter visibility control.
- Confirmed the immediate hide branches in `setVisibleDirect(false)` and `hideForScreenLoss()` did not invalidate `mCloseRequestSerial`, and the post-flush guard did not check backing-window visibility.
- Added `immediateHideDuringCloseFlushCancelsAnimatedClose` and built `panel_hidden_reembed_test` successfully.
- Pre-fix evidence: running the new case exited 1 because `panel.closeStartsForTesting()` was 1 instead of 0.
- Implemented close invalidation in both immediate-hide paths and added the hidden-backing-window guard after the geometry flush.
- Post-fix evidence: the new regression case exits 0 with 3 passed and 0 failed.
- Next: build the required targets and run the required CTest fixtures.
