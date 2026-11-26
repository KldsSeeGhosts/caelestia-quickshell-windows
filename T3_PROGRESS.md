# Panel geometry coalescing progress

- Branch `fix/coalesce-panel-geometry-updates` created from `origin/main` (`18890c654a5f4894edd1c4223522420d3f61b5c3`).
- Inspection complete: live width/height and bindable margin/anchor/exclusion property handlers run `updateDimensions()` synchronously; initial window connection, native initialization, first visibility placement, screen migration, and `WM_DISPLAYCHANGE` need synchronous placement.
- Regression fixture added in `src/windows/test/panel_hidden_reembed_test.cpp` with test-only geometry counters. Baseline evidence: `panel_hidden_reembed_test.exe initialGeometryIsPlacedSynchronously sameTurnGeometryPropertiesCoalesce` passes initial placement and fails coalescing as expected (`actual dimension updates 7`, `updatesBefore 4`).
- Implementation step: live width/height and margin/anchor/exclusion property updates now use `scheduleUpdateDimensions()` once a native HWND exists; pre-native sizing stays synchronous. `followFocusedScreen()` no longer runs a duplicate placement after `updateScreen()`.
- Focused evidence after the change: `panel_hidden_reembed_test.exe initialGeometryIsPlacedSynchronously sameTurnGeometryPropertiesCoalesce` reports 4 passed, 0 failed.
- Next: run the broader panel/window-stack fixtures and review the final diff.
