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

`runtime_paths_test` compiles the production `src/core/paths.cpp` and runs
base-, instance-, shell-only failure cases plus a valid runtime in isolated
child processes. It uses regular-file blockers rather than changing ACLs. The
fixture checks for controlled diagnostics, no access violation, no relative
lock/log fallback, and no partial runtime links. It is deterministic coverage
of the path failure contract, not a Windows permission-denial launch test.

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
