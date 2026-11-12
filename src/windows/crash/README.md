# Windows crash diagnostics and privacy

## A2 investigation

This investigation uses backend main `85e385741dac4b50fe45d33d9273b28d0bd724e8`,
which includes backend PR 45, and the shell main checked out on 2026-11-11.
The citations below describe the code before the retention change, except where
the new retention helper is named.

1. `CRASH_HANDLER` defaults to ON in `CMakeLists.txt:69`. On Windows it adds
   `quickshell-windows-crash`, a static library containing `handler.cpp`, and
   links `dbghelp`, the Windows SDK import library for the inbox `dbghelp.dll`.
   It adds no executable, shipped DLL, Qt module or third-party dependency
   (`src/windows/CMakeLists.txt:144`, `src/windows/crash/CMakeLists.txt:1`).
   The `cpptrace` dependency and Qt Widgets crash reporter under `src/crash`
   are non-Windows only (`src/CMakeLists.txt:83`,
   `src/crash/CMakeLists.txt:8,60`). The existing Windows executables remain
   `qs.exe` and `qsw.exe` (`src/CMakeLists.txt:3`).

2. With `QS_DISABLE_CRASH_RELAUNCH` present, regardless of its value, the handler
   skips its only `CreateProcessW` call. No reporter window, dialog, browser,
   upload or network request appears in this Windows path
   (`src/windows/crash/relaunch_policy.hpp:7`,
   `src/windows/crash/handler.cpp:103,118,183,269`).
   `src/crash/interface.cpp:117` opens a folder or report URL only in the
   non-Windows reporter. It is not part of this Windows build.

   Once ready, the Windows handler attempts to create
   `<instance-id>-<launch-time-in-milliseconds>.dmp` in
   `QStandardPaths::CacheLocation/crashes/<instance-id>`. With application name
   `quickshell`, the normal Windows location is
   `%LOCALAPPDATA%\quickshell\cache\crashes\<instance-id>`. This is inside the
   Windows user profile, **not** the separate Caelestia installation profile.
   The QML CacheDir pragma and shell XDG overrides do not control this path
   (`src/core/paths.cpp:78`, `src/launch/main.cpp:182`,
   `src/windows/crash/handler.cpp:300`).

   The dump type is `MiniDumpWithDataSegs | MiniDumpWithThreadInfo`, numeric
   `0x1001`, not `MiniDumpWithFullMemory`. It contains the normal minidump
   thread contexts/stacks and module information, module data sections and
   additional thread information. It includes process memory. Its size depends
   on the process; there is no byte cap or reliable fixed size. The writer's
   return value is not checked, so a failed write can leave an empty or partial
   dump (`src/windows/crash/handler.cpp:145,166`).

   Unless this is a stack overflow, the handler also attempts to copy the entire
   encoded detailed log to `log.qslog.log` and write `report.txt` in that
   directory. The 64 KiB buffer is a copy buffer, not a file-size limit.
   The report contains version, build flags/configuration, GPU and OS data,
   environment variables beginning with QS_, QT_, QML_, QML2_ or QSG_, the
   crash reason/exception code and redacted config/dump path fields
   (`src/windows/crash/handler.cpp:59,89,187`,
   `src/core/debuginfo.cpp:37,79,91,231`).
   These files have no byte cap. Files use the directory's inherited access
   permissions; the handler does not encrypt them.

   Existing log scrubbing remains unchanged. The logger scrubs messages before
   encoding them (`src/core/logging.cpp:252`). The report's combined debug
   information does not pass through that logger, so its environment values
   can contain personal paths or user-supplied secrets. Dumps cannot be
   redacted. Neither a scrubbed log nor a report is proof that a dump is safe
   to share. Keep these files local unless the user explicitly chooses to
   disclose them.

   Cleanup resets tracked DWM borders/corners to Windows defaults and returns
   parked tiling windows to recorded positions. It does not launch a cleanup
   process (`src/windows/window_decorations.cpp:89`,
   `src/windows/tiling.cpp:2377`). Returning parked windows can visibly move
   existing windows; it does not create a diagnostic UI.
   The handler then terminates the process with exit code 1. A second
   concurrent/reentrant crash waits 30 seconds and terminates
   (`src/windows/crash/handler.cpp:134,194`).

3. Previously no nonempty crash dump/report/log directory had a retention
   limit. Runtime pruning keeps five dead instances per shell after a
   five-minute grace period, but removes only *empty* crash directories
   (`src/launch/launch.cpp:380`, `src/core/paths.cpp:536,571`).
   Ordinary backend logs have no byte rotation in `src/core/logging.cpp`.
   The shell launcher rotates stdout/stderr to the previous session on Start,
   independently of these crash copies
   (`windows/scripts/Start-Caelestia.ps1:373` in the shell repository).

4. Healthy startup reads the relaunch environment flag, installs a forwarding
   Qt message handler that catches QtFatalMsg, requests a 64 KiB stack guarantee
   for the calling thread, and installs the unhandled-exception, CRT invalid
   parameter, C++ terminate and SIGABRT handlers. It obtains executable/config
   paths, calculates the dump name and creates the instance's empty crash
   directory. It does not create a dump/report/log copy, start a timer,
   worker process or network request. No measured startup-cost claim is made
   (`src/launch/launch.cpp:172`, `src/windows/crash/handler.cpp:269,300`).
   The existing QS_DEBUG_CRASH_TEST hook deliberately crashes only when set;
   this investigation did not use it. An exception before dump-path readiness
   cannot produce these diagnostics (`src/windows/crash/handler.cpp:142,231`).

5. Shell `git log origin/main -S"CRASH_HANDLER=OFF" --
   windows/scripts/Build-Prototype.ps1` traces OFF to `7b2a1841`,
   "Initial public Caelestia for Windows source". No reason is recorded in
   that change. `windows/NATIVE-BUILD.md:133` documents that it is disabled,
   but does not explain why. There is no basis to attribute the choice to
   dependency, privacy or crash-loop concerns.

6. The follow-up shell PR must change
   `windows/scripts/Build-Prototype.ps1:436` and
   `windows/NATIVE-BUILD.md:133`. CI invokes that builder through
   `windows/ci/Invoke-PairIntegration.ps1:210`; there is no separate production
   CRASH_HANDLER option in the workflow template. No workflow gate needs to
   change. Keep `.github/workflows/windows-build.yml` byte-identical to
   `windows/ci/windows-build.yml.template`.

   No payload filename changes are implied by this static library. Check
   `windows/scripts/Install-Caelestia.ps1:150,168,287`, which stages the
   executable/scripts and runs windeployqt, and
   `windows/scripts/New-CaelestiaRelease.ps1:289`, which copies the payload.
   Do not bundle the OS dbghelp.dll. Existing payload-list assertions in
   `windows/tests/nexus_pipeline/test_nexus_pipeline.py:645` and
   `windows/tests/pair_contract/test_pair_contract.py:242,915` should retain
   their file lists unless staging evidence demonstrates a real change.

   Update the backend gitlink, `windows/sources.lock.json`,
   `windows/tests/provider-support.json` and regenerate
   `windows/release/component-inventory.json` using
   `python -I -B windows/release/dependencies.py inventory`, then run
   `inventory --check`. No new component is needed for an inbox Windows DLL.
   `windows/release/dependencies.py:92,130` already accounts for Qt payloads
   and the SDK; its SBOM consumes actual release ZIP files.
   Revalidate `windows/tests/TestDependencies.py`.

   Document the local cache location, memory contents, inability to redact
   dumps and startup retention in `windows/docs/SUPPORT-BUNDLE.md` and
   `windows/docs/USER-GUIDE.md`. Correct the Windows applicability of the
   upstream-reporter URL row in `windows/docs/CODE-SIGNING-POLICY.md:245`.
   `windows/scripts/Caelestia-Privacy.ps1:1` sanitizes manifest records,
   not dumps; do not apply it to binary memory.
   `windows/scripts/Export-CaelestiaSupport.ps1:18` delegates to a fixed-field
   allowlist in `windows/scripts/Caelestia-Support.ps1`, which never opens
   profiles or logs. Keep dumps, reports and raw logs excluded. No new consent
   switch or dump collection is necessary. Extend
   `windows/tests/support-bundle/support-bundle.test.js` and run
   `windows/scripts/Test-SupportBundle.ps1` to protect that exclusion.
   Do not change DesktopTakeover.cs in this slice.

## Retention added here

At handler startup, `retention.hpp` keeps the newest three recognized crash sets,
ordered by dump modification time, or supporting-file modification time for a
set without a dump. A set contains one instance-named dump and its report/log.
The pruning call runs before the current empty crash directory is created.
It never runs in the exception handler. A crash can temporarily leave a fourth
set until the next handler-enabled start. This is a count cap, not a byte cap.
If the whole handler is disabled, no pruning runs.

Pruning deletes only known files from direct instance directories. It skips
symlinks, junctions, unexpected contents and directories with multiple dumps.
It does not recursively delete directories. If a dump is locked or deletion
is denied, it preserves its report/log and retries at a later start. Remaining
support-only sets are eligible at later starts. Unknown content and failed
deletions can exceed the cap.

The portable `crash_retention` fixture covers newest-three selection, associated
files, idempotence, dump-only/report-only sets, unknown/empty directories, a
missing root, locked dumps and symbolic-directory exclusion. The symlink case
skips when the current account lacks permission, without changing security
settings. No test crashes or starts qs.exe.

## Integration and live acceptance

This backend change does not enable the product build or bump the shell pin.
Merge/pin this backend first, then submit the separate
`fix/coexist-a2-enable-crash-handler` shell PR with the privacy and export
regressions above.

On an explicitly authorized disposable desktop, confirm an exception produces
a readable dump with matching symbols, cleanup restores borders/corners and
parked windows, no diagnostic UI or extra relaunch process appears with
QS_DISABLE_CRASH_RELAUNCH, and the supervisor alone decides whether to restart.
Confirm the effective cache path, inherited permissions, next-start pruning,
actual file sizes and support-export exclusion. No live installed shell or
desktop was changed to perform this investigation.
