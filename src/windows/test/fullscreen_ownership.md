# Optional fullscreen ownership

This changes the optional native/custom-dispatch fullscreen path only. The
normal shell facade still refuses fullscreen dispatch, and no default binding
is added.

## Ownership and release

`fullscreen_ownership::Owner` keeps the first complete `WINDOWPLACEMENT`, the
caption/resize bits it actually removed, and the frame's PID, thread and process
creation time. A window property containing the nonmovable owner's token is
also required before every native write. Window destruction removes that
property, so even same-process/thread HWND reuse cannot inherit the snapshot.
The original snapshot is captured again after acquiring that marker.
Missing process identity, denied property access and an existing owner fail
closed. The native adapter checks again between writes; Win32 does not offer
an atomic validate-and-mutate transaction against another process.

Explicit exit, tracking removal (before callbacks/deferred deletion), tracked
object destruction, tracker destruction and `aboutToQuit` release ownership.
The quit hook is necessary because the singleton is process-lifetime. Removed
children awaiting deferred deletion are included in normal shutdown. Teardown
disables reentry. Restore failure is warned about, retains ownership for a later
release attempt while the object lives, and is never reported as a restored
window. There is no persisted recovery after a crash or forced termination.

Release merges back only removed frame bits. Other style/exstyle changes are
not replayed from the old snapshot. Original normal/maximized placement and
show state are restored, except a subsequent hide/minimize is preserved instead
of showing the application. Hidden/minimized entry is refused. Existing
application fullscreen is refused using both tracker observation and native
monitor bounds. Changes to the owned frame bits or fullscreen placement
relinquish ownership without restoring an obsolete snapshot. An application
transition with exactly identical placement and style is not distinguishable
from the owned episode; this is not an application-fullscreen protocol.

Normal/restore placement uses `SW_SHOWNOACTIVATE`, and minimized placement uses
`SW_SHOWMINNOACTIVE`. No `ShowWindow`, explicit focus/foreground request,
z-order promotion, synthetic input or queued asynchronous placement is added.
Windows has no non-activating maximize show command: restoration of a saved
maximized window retains `SW_SHOWMAXIMIZED` and may activate it. This remains an
explicit desktop acceptance gate, not a no-focus claim.
`WPF_ASYNCWINDOWPLACEMENT` is retained in the original snapshot but cleared on
native delivery so no queued write outlives the window marker.
These cross-process writes are synchronous. A target that hangs after the
initial responsiveness check can block the shell thread, including shutdown;
there is no bounded-shutdown guarantee for this optional path.

## Deterministic fixture and registration

`fullscreen_ownership_test.cpp` executes the production owner through injected
operations. It never creates an HWND, instantiates the native adapter, observes
user windows or sends native window operations. `QCoreApplication` is sufficient.
It covers complete placement fields, normal/maximized explicit release and
destruction/restart, hidden/minimized refusal and release, same-process HWND
reuse, PID/creation-time/thread changes, application fullscreen/takeover,
unrelated style changes, retargeting, repeated release, partial failure/retry
and identity loss between writes. It also covers application fullscreen and
hide/minimize reactions inside synchronous style callbacks.

The integration owner must register the following in the existing test project
and add `fullscreen_ownership` to its portable label list. No production source
registration is needed because the helper is header-only.

```cmake
qs_add_test_executable(fullscreen_ownership_test fullscreen_ownership_test.cpp)
target_compile_definitions(fullscreen_ownership_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(fullscreen_ownership_test PRIVATE Qt6::Core Qt6::Test user32)
add_test(NAME fullscreen_ownership COMMAND fullscreen_ownership_test)
set_tests_properties(fullscreen_ownership PROPERTIES LABELS portable TIMEOUT 60)
```

Run `ctest --test-dir <build> -R '^fullscreen_ownership$' --no-tests=error
--output-on-failure -V`. Compile `window_tracker.cpp` and its MOC output against
the production Qt dependencies too. Neither compiling these lifecycle hooks
nor running the fake establishes real tracker/shutdown desktop behavior.

## Windows desktop acceptance, explicitly later

Use an identified assembled shell/backend pair and disposable test applications,
with desktop authority. For both normal and maximized windows, record caption,
resize frame, full placement, normal rectangle and show state before entry and
after explicit exit, tracking removal and clean shell exit/restart. Check hidden
and minimized removal without resurrection, closed/recycled handles,
application-owned fullscreen and intervening unrelated style/exstyle changes.
Check focus/z-order and observed native outcomes rather than posted requests.
Do not use arbitrary crash/restart as proof of clean-shutdown recovery.

These interactive cases, the real Qt tracker lifecycle, full backend link,
downstream pair, installed candidate and public CI are not established by the
bounded fixture/object-compile lane.
