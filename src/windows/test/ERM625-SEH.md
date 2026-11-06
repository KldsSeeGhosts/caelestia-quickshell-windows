# ERM625: opt-in native desktop SEH recovery

## Exposure and policy

This is the build-gated native desktop provider, selected only when both
`CAELESTIA_WINDOWS_HYPR=1` and `CAELESTIA_WINDOWS_HYPR_DISPATCH=1`. It is not a
default-provider crash claim. The existing 26100/26200/26300 build gate remains
unchanged. No interface layouts, DLL restart limits or input fallbacks change.

Catch structured exceptions at each native COM call boundary, rather than
around a whole accessor operation. With `/EHsc`, catching outside the operation
could skip local COM-owner destructors. A fault returns the operation's existing
failure sentinel, records `E_UNEXPECTED`, reports
`unavailable/provider-disconnected`, clears the snapshot/current desktop, and
drops all provider-owned interfaces. Detach each owned interface before
attempting `Release`, so a broken release cannot leave a reusable stale pointer.
A failed release is best-effort cleanup, not proof the remote object freed itself.

Local COM outputs and temporary lists are explicitly released through the same
SEH guard on success and failure, before an operation or acquisition can report
success. `GetViewForHwnd` transfers ownership only on success; a failure or SEH
must not cause adoption or release of its potentially half-written output.

The facade's post-call availability check unloads it. As defense in depth,
the native `accessorFailed` branch also invalidates the provider after any
escaping SEH, including local cleanup, even if the facade was already unloaded.
That branch records `E_UNEXPECTED` and enters bounded recovery instead of
claiming immediate dispatch retry. Its existing
queued state notification emits the capability change and terminates pending
switch/creation requests as denied. Recovery never replays those commands.
Ordinary HRESULT handling, including an ordinary `E_UNEXPECTED`, is unchanged.

Use the existing recovery policy: first retry after 250 ms, exponential delay
capped at 4 s, automatic attempts limited to a 30 s window. Repeated faults
during acquisition retain that window and backoff. A dispatch cannot bypass an
active timer. After exhaustion, explicit dispatch or Explorer-restart
invalidation may start another bounded window. Every recovery reacquires COM
interfaces through the factory path; it does not re-enable the dropped
interfaces. A real COM server can return the same identity after reacquisition,
so this is not an assertion that every successful acquisition has a new server.
An ABI fault is not evidence that probing another private layout is safe.

## Original working-tree component receipt

The following is the preserved original receipt. Those tests ran before commit
`c0286d14817f12e9c3b95f04a552da11bcc02d17`, without capturing HEAD or dirty state.
They are working-tree evidence, not exact-SHA proof. The review-repair validation
must be run after a clean commit and retain source/artifact hashes separately.

Validated with Windows 11 x64 build 26300, Qt 6.11.2, clang-cl 20.1.8,
MSVC tools 14.44.35207 (compiler file version 19.44.35229.0),
Windows SDK 10.0.26100.0, Release `/EHsc`, Ninja, two build jobs.
The source parent is backend `762a109e9006ce89bbbd46d20e4f66ca8f560370`,
retaining ancestry from `e6039b2b9cbb35b97473bda4dcc3f12789ae7f0c`.

Fixture evidence:

- `native_desktop_recovery`: 24 passed, no failures or skips, including the
  initialization/cleanup rows. Injected access-violation SEH covers enumeration,
  desktop ID reads, switch, partial creation output, window membership reads,
  partial acquisition and a broken provider release. Checks ownership counts,
  snapshot clearing, blocked stale dispatch and a fresh healthy acquisition.
  Retains HRESULT disconnect, unsupported build, synthetic Explorer return,
  backoff, deadline exhaustion and explicit dispatch retry cases.
- `erm625_native_desktop_facade`: 5 passed, no failures or skips, including
  initialization/cleanup. Exercises production `VirtualDesktops::call`,
  switch/create requests and queued capability/result events with injected COM
  fixtures. Verifies terminal denial with the original request identity and no
  resurrection after recovery.
- `virtual_desktops`: 66 passed, no failures or skips, including
  initialization/cleanup. Existing accepted-versus-observed and fallback
  regression coverage remains intact.
- The native provider translation unit also compiles without `QS_TEST`.
- Mutation proof: temporarily omitting the exception invalidation/drop/recovery
  transition makes `structuredExceptionInvalidates:switch` fail because the
  provider remains `Available` instead of `Disconnected`. The mutation is
  removed from the final source.

These are 3 component fixtures, 95 Qt Test pass rows (89 excluding lifecycle
rows), zero skips. They use generated stack-owned COM fixtures only, never a
real COM activation, Explorer mutation, keyboard input or desktop mutation.
The facade fixture uses the public test constructor and a test-only white-box
native-selection flag; it keeps refresh on the injected model. It separately
checks the real native capability projection without registry refresh.

## Review repair and exact-commit receipt protocol

The follow-up replaces the private/public macro with a bounded `QS_TEST` friend
hook. It restores both environment variables and clears injected faults before
cleanup assertions can abort. Synthetic view objects implement the IUnknown
prefix, and a test-only window validator permits generated handles without
creating a native window.

Additional cases exercise `GetCurrentDesktop`, `GetViewForHwnd`,
`CanViewMoveDesktops`, `MoveViewToDesktop`, local array/active-desktop/create/view
releases, snapshot replacement and temporary-list cleanup. Failed HRESULT view
acquisition leaves the borrowed output untouched. Outer-release SEH is injected
through production facade calls, including terminal pending switch/create
failure and recovery; the outer failure path also covers an unloaded facade.

Commit the repaired source first. From that clean commit, configure, clean-build
the three fixtures plus the production provider/facade objects without
`QS_TEST`, and run the affected CTest expression below. The exact-commit receipt
must capture full HEAD/tree, empty dirty state before and after, per-file source
SHA-256 manifests, a `git archive HEAD` source artifact hash, executable/log
hashes, commands, counts and skips. The source manifests must match before and
after validation. Preserve the original and preliminary logs with distinct
names; they must not be presented as the exact committed result.

## Registration handoff

The existing recovery target needs no registration change. The isolated
validation wrapper registered the additional facade fixture outside the source
tree. The integration owner should add this target to the shared test build,
and add `erm625_native_desktop_facade` to its portable tier list:

```cmake
qs_add_test_executable(erm625_native_desktop_facade_test
    erm625_native_desktop_facade_test.cpp
    ${QS_WINDOWS_SRC}/native_desktop_accessor.cpp
    ${QS_WINDOWS_SRC}/virtual_desktops.cpp
    ${QS_WINDOWS_SRC}/services/message_window.cpp
    ${QS_WINDOWS_SRC}/../core/redact.cpp
    ${QS_WINDOWS_SRC}/accessor_state.hpp
)
target_compile_definitions(erm625_native_desktop_facade_test PRIVATE
    QS_TEST NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(erm625_native_desktop_facade_test PRIVATE ${QS_WINDOWS_SRC})
target_link_libraries(erm625_native_desktop_facade_test PRIVATE Qt6::Gui Qt6::Qml Qt6::Test ole32)
add_test(NAME erm625_native_desktop_facade COMMAND erm625_native_desktop_facade_test)
```

Build the three named targets with `cmake --build <build> --parallel 2`, then:

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
ctest --test-dir <build> `
    -R '^(native_desktop_recovery|virtual_desktops|erm625_native_desktop_facade)$' `
    --no-tests=error --output-on-failure -V
```

## Evidence limits and remaining acceptance

Source inspection establishes the former stale-provider control flow.
Injected Windows component execution establishes the modeled invalidation,
ownership, facade events and recovery contracts. Mutation execution establishes
that the new regression detects the missing transition. None is a real
Explorer restart, native COM ABI qualification, installed-shell pass, hardware
pass or CI result.

Still required from the designated desktop operator: qualify the exact
integrated shell/backend candidate on supported Windows builds with explicit
native opt-in, verify real healthy switching/creation and Explorer-loss/return
capability reporting, and confirm DLL compatibility remains intact where used.
No deliberate real-provider crash is needed. This receipt does not close
ERM625 or substitute for independent review and downstream registration.
