# Display transaction ownership

The existing `Displays` QML API remains unchanged. No display settings UI or
new caller is enabled by this change. Layout preview mutations remain an
activation gate pending real dual-monitor acceptance.

## Ownership and outcomes

One process-lifetime `DisplayTransaction` serializes asynchronous work. A facade
owns a generation lease, not the rollback state. Its first confirming request
captures the original topology before accepting a native write. Snapshot
failure still returns `false` immediately. Accepted work is not completed work:
`busy`, `pendingConfirmation`, `lastError` and the existing signals report
subsequent state.

The worker retains the original snapshot through apply, subsequent jobs,
confirmation, save and restore. The confirmation deadline uses a monotonic clock.
It begins after the first successful apply and is neither suspended nor reset
by a second job or failed save. Native completion never requires a QML object or
a functioning GUI event loop.

Replacing or destroying the owning facade requests rollback. A retiring
generation cannot keep, mutate or release its replacement's transaction.
Cancellation during a native operation is a completion decision, not unsafe
thread termination: when that call returns, the worker restores the original
snapshot once. Successful keep clears state only after persistence succeeds
and only if its lease and deadline are still valid. Repeated keep, timeout,
cancel, teardown and shutdown requests do not duplicate a successful recovery.

An apply error may follow a partial native write, so it also requests recovery.
A failed save retains the baseline and original deadline. A failed or
unconfirmed restore retains the baseline, `pendingConfirmation = true`, zero
seconds left and a truthful error. Automatic rollback does not retry in a loop.
An extant or replacement facade may explicitly call `revertChanges()` to retry.
New mutations and keep are refused while failed recovery is unresolved.

## Disappeared monitors

Recovery submits the exact baseline without `SDC_ALLOW_CHANGES`. It does not
fall back to `SDC_USE_DATABASE_CURRENT`, which might restore a different layout.
After the write, a fresh active-topology query must agree on source and target
identity, geometry, rotation, scaling and timing. Array order and equivalent
rational refresh rates do not matter. A missing monitor, refused write, failed
query or mismatched observation leaves recovery unconfirmed. A monitor returning
can permit an explicit retry with the retained original baseline. This does not
promise physical output verification or crash recovery.

## Bounded clean shutdown

`aboutToQuit` revokes mutation authority and requests recovery. It waits at most
250 ms for the worker and logs an incomplete-recovery warning on timeout or
failure. Facade destruction itself does not wait or perform native writes on
the GUI thread. The worker owns any in-flight call after this budget and, if
the process remains alive, restores when that call returns.

There is no durable recovery journal, subprocess guardian or unsafe cancellation
of a Windows API call. A process that exits before late recovery completes,
an arbitrary kill, driver hang or crash is not guaranteed to restore its layout.
The bounded wait is not evidence that every clean shutdown completed recovery.

## Focused fixture registration

The fixture compiles the real facade and transaction with injected operations,
gated worker threads and synthetic two-display layouts. It creates no GUI or
native service window and queries or changes no real display. It covers initial
and second apply teardown, both replacement orders, original deadline/baseline,
successful and failed keep, timeout, cancellation, partial apply failure,
failed/mismatched recovery, explicit retry and bounded shutdown.

The shared test-registration owner should add:

```cmake
qs_add_test_executable(display_transaction_test
    display_transaction_test.cpp
    display_transaction_stubs.cpp
    ${QS_WINDOWS_SRC}/system/displays.cpp
    ${QS_WINDOWS_SRC}/system/displays.hpp
)
target_compile_definitions(display_transaction_test PRIVATE
    QS_DISPLAYS_TEST NOMINMAX WIN32_LEAN_AND_MEAN
)
target_link_libraries(display_transaction_test PRIVATE
    Qt6::Gui Qt6::Qml Qt6::Test user32
)
add_test(NAME display_transaction COMMAND display_transaction_test)
```

Also add `display_transaction` to `QS_TESTS_PORTABLE`, update any expected
registration count through its owner, and retain the normal manifest helper.
The header-only helper needs no production CMake source addition. Do not set
`QS_DISPLAYS_TEST` on production targets. A separate production object compile
without that definition checks the native route and Qt MOC declarations.

Synthetic fixtures and object compilation do not qualify a packaged backend,
the exact downstream shell pair, Windows/driver behavior or physical monitors.
Disposable real dual-monitor acceptance must still record topology before and
after delayed apply, facade reload, confirmation timeout, keep, failed recovery
and shutdown. That acceptance remains not run; it requires a separately
authorized native desktop session.
