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
redaction. `ClipboardTestAccess` uses a private, non-QML constructor with
explicit read, write and listener providers, a disposable directory and a worker
pool. It enables history through the production setter with a fake listener.
It bypasses personal cache-path resolution and stale-cache pruning. Stubs abort
if the production service-window or cache-pruning path is accidentally entered.
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

## Registration

`clipboard_capture` and `clipboard_writer` use the manifest helper and the
`portable` tier in `CMakeLists.txt`. Both receive an explicit disposable
`clipboard-fixture-data` directory in the build tree and run offscreen.

The fixture refuses to run without an existing absolute
`QS_CLIPBOARD_FIXTURE_ROOT`. It never falls back to a personal profile/cache.
Use the existing Qt runtime on PATH and `QT_QPA_PLATFORM=offscreen`:

```powershell
cmake --build <build> --target clipboard_capture_test clipboard_writer_test --parallel 2
ctest --test-dir <build> -R '^clipboard_(capture|writer)$' --no-tests=error --output-on-failure
```

## Write-only API and opt-in observation

`Quickshell.Windows.ClipboardWriter` is a QML singleton with no history storage,
read provider, listener, or cache path. `copyImageFile(path)` and `copyText(text)`
return positive integer operation IDs, not success flags.
`writeFinished(id, ok, error)` delivers one result per operation on the GUI thread,
after the method returns. `error` is empty on success, otherwise `read-failed`,
`decode-failed`, `allocation-failed`, `clipboard-busy`, or `set-failed`.
The last code also covers failure to empty the clipboard. Files are read before
returning, so callers may delete their hand-over file immediately. Image decode
runs on the worker pool. Writes retry open five times with 10 ms between attempts.
Destroying the singleton cancels pending results and frees untransferred data.

The legacy `Clipboard.copyImageFile` still returns a bool indicating file-read
acceptance, not write success. `Clipboard.copyText` still returns void. Both use
the same write implementation as `ClipboardWriter`, as does copying a history
entry. Only successful writes update history's own-write suppression.

`Clipboard.historyEnabled` defaults to false. Disabled construction does not
resolve a cache path, prune stale caches, install a handler, register a listener,
or read clipboard data. Enabling retains the `shell.qml` filename guard.
Disabling removes the handler, releases the shared native listener registration
when the last observer stops, cancels capture generations and wipes entries and
the instance cache directory. A cancelled worker cannot publish or retain its
late result. The privacy-marker checks above are unchanged.

`clipboard_writer` exercises these paths with fake registration and Win32 write
boundaries. Production dormant constructors also run with fatal native-window
and prune stubs. The fixture checks asynchronous success and every error code,
distinct overlapping IDs, GUI-thread delivery, fifth-attempt success, bounded
open failure, legacy compatibility and failed-write suppression. Fake data
ownership detects double frees, transfer mistakes and leaks when destruction
cancels a queued retry or a decoded result. No test opens the real clipboard.

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
