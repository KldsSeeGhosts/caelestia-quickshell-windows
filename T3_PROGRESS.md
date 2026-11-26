# T3 progress

- Created `fix/coalesce-panel-geometry-updates-r5` from `e66bb44f71c35ac1cc34342c7a7630bbd917988b`.
- Traced the close path: `beginClose()` flushes pending geometry before starting animation; a geometry probe can reenter visibility control.
- Confirmed the immediate hide branches in `setVisibleDirect(false)` and `hideForScreenLoss()` do not invalidate `mCloseRequestSerial`, and the post-flush guard does not check backing-window visibility.
- Next: add the failing regression case, then invalidate armed close requests on immediate hides.
