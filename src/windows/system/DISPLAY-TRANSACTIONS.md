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
subsequent state. The initial in-flight apply is busy but not yet pending
confirmation. Pending starts after successful apply; failed recovery also
publishes pending to expose the retained unconfirmed state.

The worker retains the original snapshot through apply, subsequent jobs,
confirmation, save and restore. The confirmation deadline uses a monotonic clock.
It begins after the first successful apply and is not reset by a second job.
The facade polls while busy or while a deadline exists, including the gap where
the deadline expired but the worker has not yet published rollback. Native
completion never requires a QML object or a functioning GUI event loop.

Replacing or destroying the owning facade requests rollback of a preview not
already accepted by Keep. A retiring generation cannot keep, mutate or release
its replacement's transaction. Cancelling an unconfirmed native apply is a
completion decision, not unsafe thread termination: when that call returns,
the worker restores the original snapshot once. Cancellation before dispatch
of the first confirming job
discards the unused snapshot without a restoration write. If an earlier
unconfirmed preview already changed the layout, cancelling before a second
native call still restores that original baseline.

Explicit Keep is ordered under the same transaction lock as lease changes and
deadline decisions:

1. Keep must be accepted before expiry, cancellation or lease loss. Acceptance
   suspends the active deadline but retains its original absolute value.
2. Once accepted, persistence runs even if the facade is then replaced/destroyed
   or shutdown begins. A successful save commits the confirmed layout, clears
   recovery state and is not silently undone by later expiry or lease loss.
3. A failed save reinstates the original deadline, not a new countdown. An
   expired deadline or ended lease immediately restores the original baseline.
   Otherwise the preview remains pending with the failure exposed and the
   remaining original confirmation window available for retry or revert.

Lease loss before Keep acceptance still rolls back the preview. Repeated keep,
timeout, cancel, teardown and shutdown requests do not duplicate a successful
save or recovery.

An error in a confirming layout apply may follow a partial native write, so it
also requests recovery. A failed non-confirming HDR/scale job reports its error
without cancelling a pending topology preview or changing its deadline. Lease
loss and timeout still restore that preview even during a non-confirming job.
A failed or unconfirmed restore retains the baseline, `pendingConfirmation = true`,
zero seconds left and a truthful error. Automatic rollback does not retry in a loop.
An extant or replacement facade may explicitly call `revertChanges()` to retry.
New mutations and keep are refused while failed recovery is unresolved, with
an actionable error directing the caller to `revertChanges()`. The underlying
native recovery failure and original snapshot are retained.

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
the process remains alive, resolves its retained decision when that call
returns: successful accepted Keep commits; failed save or an unconfirmed
preview restores. Shutdown cannot turn a confirmed successful save back into
an unconfirmed preview.

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
failed/mismatched recovery, explicit retry and bounded shutdown. Scheduling
barriers compiled only under `QS_DISPLAYS_TEST` exercise the deadline/worker gap
through the real facade timer and cancellation before dispatch. A save barrier
crosses success/failure, original expiry and both facade replacement orders,
including bounded shutdown during an accepted Keep. No additional test target
or registration dependency is needed for these cases.

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
