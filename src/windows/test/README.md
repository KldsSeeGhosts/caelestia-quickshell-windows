# Windows backend fixtures

Qt Test fixtures for the Windows backend in `src/windows`. They are a standalone
CMake project that compiles the backend sources each fixture needs, so the
production build (`BUILD_TESTING=OFF`) is unchanged and no test binary is ever
installed.

## Runtime tiers

Every fixture has exactly one CTest label. Configuration fails if a fixture is
unclassified, classified twice, or a tier names a fixture that does not exist.

| Label | Needs | Where it runs |
| --- | --- | --- |
| `portable` | Fakes and offscreen Qt only | Required in CI (`.github/workflows/windows-tests.yml`) |
| `native` | Hidden or message-only windows, helper processes, a private desktop | Locally; candidate for a separate CI job |
| `desktop` | Visible native windows on the input desktop | By hand, with the shell stopped |
| `gpu` | Real D3D11 devices or RHI rendering | By hand, on a machine with a GPU |
| `machine` | Installed-app metadata on this machine | By hand |

All fixtures are built in CI, so every tier is at least compile-checked.

`DesktopLayer.externalWallpaperEnabled` defaults to `true` for compatibility.
Setting it to `false` places every Background panel above foreign wallpaper
children inside the desktop host, without changing the foreign windows or
placing the panels above desktop icons. `underlayActive` is false in this mode.
The private-desktop `desktop_underlay` fixture covers both monitor backgrounds,
restacking, toggling the policy and an absent renderer. `desktop_zorder` also
covers production event reconciliation and renderer replacement.

Desktop-wide reorder hooks include the shell process. A shell window's own
show/restack can move it above an unchanged visible wallpaper, including after
stop/play or a fresh start with an existing wallpaper window. Explorer's scoped
hooks and the wallpaper process hooks do not report that shell-originated
edge. The private-desktop `desktop_underlay` fixture uses separate host,
wallpaper and shell processes to reproduce it, with both hidden and still-visible
stop states. Repairs remain idempotent, and destroying the wallpaper window
while its process stays alive withdraws the acknowledgement.

Wallpaper Engine's real stop/play destroys its desktop windows on stop and, on
play, creates new hidden children directly inside the host and then shows them.
That raises no host reorder and no parent change, and the wallpaper process
hooks are already gone with the last foreign child. The desktop-wide hook also
covers show/hide of host children so that return is reclaimed;
`desktop_zorder`'s `stopPlayRecreatedInsideHostIsReclaimed` reproduces it.

`quickshell.windows.desktop.info` logs an `Underlay decision` line on state
changes and repairs. It reports policy, host availability, insert-after and
above-icons blockers, refusal, foreign/visible/background counts,
acknowledgement, actual remaining misorder and whether a repair ran. It includes
no titles, handles, paths or process/device identifiers.

For shell issue KldsSeeGhosts/caelestia-windows#62, the shell pin update must
also wire `Native.DesktopLayer.externalWallpaperEnabled` to
`!Wallpapers.sourceFresh || WallpaperEngine.enabled` in
`WindowsBackground.reconcileDesktop()`, and reconcile when that expression
changes. This preserves existing-profile behaviour and enabled integration.
Do not bind it to process liveness or `externalWallpaperActive`: those describe
ownership, not the user's integration choice, and depend on `underlayActive`.
The backend cannot infer the shell profile's setting. No shell pin is updated
by this backend change.

The portable tier includes `fullscreen_ownership`, `display_transaction`,
`clipboard_capture`, `capture_dimensions`, `erm625_native_desktop_facade`
and `network_wifi_identity`. These use synthetic operations and content, not
the user's windows, display configuration, clipboard, capture or WLAN.
The clipboard fixture receives an absolute disposable directory in the build
tree and refuses any personal-profile fallback. Test-only definitions are
scoped to each fixture target.

`lazyloader` runs real Qt incubation, including a required-property error after
component compilation succeeds, deferred incubator deletion and loading or
activation retries. Only shell-generation and render-timing integrations are
stubbed. `mpris_art_cache` checks encoded and decoded size limits without
opening a media session. `keyboard_hook` exercises consumed-Esc resets through
its fake input callbacks without installing hooks or injecting input.

`daemon_handshake` is a native-tier, console-only fixture. It spawns only a copy
of itself through the production daemon launcher, checks both daemon flags,
verifies that the consumed pipe is no longer inheritable and acknowledges
readiness. It never launches the shell or creates GUI windows.

`runtime_paths_test` compiles the production `src/core/paths.cpp` and runs
base-, instance-, shell-only failure cases plus a valid runtime in isolated
child processes. It uses regular-file and directory blockers rather than changing
ACLs. It also covers required PID/path link and lock failures, including rollback
of a shell link when PID publication fails. The fixture checks independent
directory states, explicit failure results, controlled diagnostics, no access
violation and no relative lock/log fallback. It is deterministic coverage of the
path failure contract, not a Windows permission-denial or full-launch test.

`runtime_paths_launch_test.ps1` separately exercises a built **real** `qs.exe`
with a synthetic windowless QML config on offscreen/software Qt. It disables
native control providers, never installs or starts the live shell, and checks:

- An existing regular-file `QS_RUNTIME_DIR` blocker.
- A real ACL denial of child-directory creation inside a newly owned temporary
  runtime directory. A .NET creation probe must independently confirm denial.
- Independent instance-only and shell-only blockers, plus PID/path bookkeeping
  blockers.
- Controlled nonzero startup refusal before QML readiness or IPC, and no relative
  artifacts or runtime lock/logs on failure.
- A valid control that loads QML, accepts a real IPC connection and exits zero.

The script restores the fixture's original ACL in `finally` and removes synthetic
profiles. Only per-case logs and `receipt.json` remain. Its evidence is actual
isolated launch behavior, not installed-shell or physical-desktop qualification.
Regular-file blockers are not permission-denial evidence.

```powershell
./src/windows/test/runtime_paths_launch_test.ps1 `
  -BackendExe <backend-build>/qs.exe -QtPrefix <Qt>/6.11.2/msvc2022_64 `
  -EvidenceRoot <new-absolute-evidence-directory>
```

Required runtime bookkeeping failure aborts launch before filesystem logging,
QML/plugin initialization or IPC startup. This does not alter data/state/cache
path policy. File-backed runtime links alone are not live-instance readiness:
discovery also requires a held instance lock.

The lock indicates process liveness, not IPC readiness: it is held during Qt
and plugin startup, so a client may briefly see a live instance whose IPC
endpoint is not listening yet. A later bookkeeping refusal can leave unheld
runtime links or an empty lock file; those are not a live/ready instance.
Existing early-log temporary files are outside this fixture's runtime-log
assertion and are not claimed to be eliminated by this repair.

The launch harness records only a named-pipe connection, not a command
round trip. Its receipt and per-case logs scrub fixture/profile/tool paths
and omit security descriptors. On failure, process output is retained even
after a timeout; the failed synthetic profile is kept locally with restored
ACLs. Do not upload that profile directory or older unsanitized receipts.

## Building

From a Visual Studio x64 developer shell with Qt 6.11, clang-cl, CMake and
Ninja on `PATH`:

```powershell
cmake -S src/windows/test -B build-fixtures -G Ninja `
  -DCMAKE_PREFIX_PATH=<Qt>\6.11.2\msvc2022_64 -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Release
cmake --build build-fixtures
```

## Running

```powershell
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QUICK_BACKEND = 'software'
ctest --test-dir build-fixtures -L '^portable$' --no-tests=error --output-on-failure
```

Run another tier by changing the label, for example `-L '^native$'`. Run the
`desktop` tier only with the shell stopped: those fixtures create visible
windows and embed into the desktop layer the shell uses. Fixtures that set
their own `QT_QPA_PLATFORM` (such as `panel_hidden_reembed`) override the
offscreen default.

## Optional checks

Two `panel_animation` cases skip unless configured:

- `IIW_QSB=<path to qsb.exe>` rebuilds `shaders/panel_effect.frag.qsb` and
  compares the deserialized shader with the committed one.
- `IIW_LAYER_RULES=<path to ii-windows defaults/windows/layerrules.json>`
  parses ii-windows' shipped layer rules.

## Test data

All names, paths, monitor labels and addresses in these fixtures are synthetic.
Keep it that way: do not add real device names, account or profile paths, logs
or captures from a real machine.
