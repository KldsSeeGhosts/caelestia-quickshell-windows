# Inherited local-history clipboard capture

This fixture covers `system/clipboard.cpp`, the optional inherited `Clipboard`
singleton in `Quickshell.Windows`. It is not the shell's
`Caelestia.Windows.WindowsClipboard` provider and does not enable either provider.
It is not evidence of a failure in the active picker or capture host.

## Contracts

All three privacy decisions use the same successfully opened snapshot as the
content. Presence of `ExcludeClipboardContentFromMonitorProcessing` or
`Clipboard Viewer Ignore` rejects the capture. The supported first-byte
`CanIncludeInClipboardHistory` convention is unchanged: zero excludes, nonzero
permits. An advertised history marker that cannot be read/locked, has no bytes,
or a privacy format that cannot be registered fails closed. Rejection always
closes the snapshot before returning, without decoding or spilling content.

Each capture carries a shared generation identity. Clear cancels the identity,
replaces it, discards pre-clear `captureAgain` work and records the sequence at
clear. A previous generation cannot publish, including a result already queued
on the object thread. Workers check cancellation before reading, between retries,
before materialization and after writing. Completion removes only the result's
own files on rejection. A file already written by a pending result is removed
when that result is delivered; clear does not synchronously wait for workers.

A notification for the sequence observed at clear cannot restore it. A distinct
identified change after clear can schedule one capture in the new generation,
including while an old worker is finishing. An unknown sequence is not evidence
of a change. If clear itself has unknown identity, the first known identity
establishes a baseline and a subsequent distinct identity is required.
Existing permitted text/image behavior, text duplicate filtering, copy
suppression and bounded read retry are retained. Failed text spill still falls
back to memory, but removes any incomplete file; failed image output is rejected.
This is invalidation, not secure memory erasure or cancellation of copy commands.

## Deterministic boundary

The fixture compiles the real `clipboard.cpp`, its meta-object, and real
redaction. `ClipboardTestAccess` uses a private, non-QML constructor with an
explicit provider, disposable directory and worker pool. It bypasses cache-path
resolution, stale-cache pruning and listener registration. Stubs abort if the
production service-window or cache-pruning path is accidentally entered.
No real clipboard or Windows history API is called.

The fake replaces ordinary content at the exact open operation. Availability
outside an owned snapshot is counted as a test failure. Independent marker cases
cover short text, text one character over the spill threshold, and a 48x32
generated DIB. Permitted controls check the actual text bytes and PNG pixels.
The image path uses Windows GDI to convert this synthetic DIB, not screen pixels.

Semaphores hold a worker at snapshot close or a retry. Waiting for the private
pool without pumping the event loop holds a real completed result in the Qt
queue. This covers clear both before materialization and after file creation,
including when the sequence at clear differs from the queued result. Tests also
cover queued-again work, rapid copy/clear/copy, unknown sequences, repeated
clear, destruction/reload with separate cache ownership, read and output errors,
bounded retries, stale own-write results, duplicates and suppression.
No race is scheduled with sleeps, clipboard secrets or huge images.
Each semaphore/pool wait is bounded at five seconds.

## Target registration request

The focused sources are supplied without changing shared test registration.
The test-entry owner should register this target using the existing manifest
helper, classify `clipboard_capture` in exactly one runtime tier (recommended
`portable`, synthetic content only), and create its disposable output directory
in the build tree:

```cmake
qs_add_test_executable(clipboard_capture_test
    clipboard_capture_test.cpp
    clipboard_capture_test_stubs.cpp
    ${QS_WINDOWS_SRC}/system/clipboard.cpp
    ${QS_WINDOWS_SRC}/system/clipboard.hpp
    ${QS_WINDOWS_SRC}/../core/redact.cpp
)
target_compile_definitions(clipboard_capture_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(clipboard_capture_test PRIVATE
    Qt6::Gui Qt6::Qml Qt6::Test user32 gdi32
)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clipboard-fixture-data")
add_test(NAME clipboard_capture COMMAND clipboard_capture_test
    -o "${CMAKE_CURRENT_BINARY_DIR}/clipboard-qt.xml,xml" -o -,txt
)
set_tests_properties(clipboard_capture PROPERTIES ENVIRONMENT
    "QS_CLIPBOARD_FIXTURE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/clipboard-fixture-data"
)
```

The fixture refuses to run without an existing absolute
`QS_CLIPBOARD_FIXTURE_ROOT`. It never falls back to a personal profile/cache.
Use a dedicated disposable directory, the existing Qt runtime on PATH and
`QT_FORCE_STDERR_LOGGING=1`. Once registered:

```powershell
cmake --build <build> --target clipboard_capture_test --parallel 2
ctest --test-dir <build> -R '^clipboard_capture$' --no-tests=error --output-on-failure
```

## Evidence limits and remaining native acceptance

Windows component execution with the fake provider establishes these production
policy/lifecycle paths, real Qt queueing/file ownership, and synthetic GDI/PNG
conversion. It does not establish native clipboard registration, producer
ownership, delayed rendering, Win32 handle/lock failure behavior, OS listener
delivery, live Windows history behavior, or current-overlay behavior.

Before native acceptance, use an explicitly authorized disposable Windows
session and generated producer content. Repeat each independent marker and
permitted control at a synchronized open boundary, native open contention and
failure/retry, large text/image clear/completion, new copy after clear, and
reload/destruction. Check history and spill storage after completion. Do not run
those scenarios against a personal clipboard or infer acceptance from this
fixture, a production build, or offscreen smoke.
