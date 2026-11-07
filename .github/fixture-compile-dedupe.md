# Fixture compile deduplication

Measured locally with the canonical clang-cl 20.1.8 / Qt 6.11.2 / Release /
Ninja toolchain, against `origin/main` at `46fc1d2`.

* C++ compile edges in `build.ninja`: 291 before, 224 after (67 fewer, 23.0%).
  This includes generated moc/resource translation units, not just source files.
* `redact.cpp`: 21 compilations before, one after.
* All other repeated production `.cpp` inputs are now compiled once too.
  Each helper has its own static archive, including its moc where needed.
  Unrelated implementation objects cannot be pulled across fixture fake boundaries
  just because they were bundled into one large archive.
* Only `network_wifi_stubs.cpp` remains compiled twice. Its
  `QS_WIFI_NATIVE_FIXTURE` branches deliberately provide different fake versus
  real Wi-Fi boundaries. Combining those would change coverage or duplicate
  definitions. All fixture-local stubs remain local to their executables.
* `native_desktop_accessor`, `virtual_desktops`, `desktop_host` share the
  `QS_TEST` variant only. All current consumers use that variant.
  `tiling` shares the `QS_TILING_FAKE_DESKTOP` variant across its three fixtures.
  None of these archives is used by production. Other target-only seam macros
  do not alter the allowlisted helpers.

`ctest --show-only=json-v1` was captured before and after configuring the same
source/build paths. The entire `tests` array is equal after removing only
`backtrace` (source line numbers moved). Names, executable commands, arguments,
properties, labels, environments, timeouts and counts are unchanged:
57 total, 44 portable, 6 native, 3 desktop, 3 GPU, 1 machine.
Raw inventory files remain local validation evidence, not public receipts.

The full fixture graph built locally. All 44 portable tests passed in 48.17 s.
`native_desktop_recovery` took 45.59 s at baseline and 14.84 s after the change.
These are workstation measurements, not hosted-runner build-time claims.
The producer's 52 Python tests passed.

The recovery fixture advances the existing elapsed-time callback to exactly
30,000 ms while retaining the real retry timer and COM factory. It asserts the
timer is exhausted, no more acquisitions occur, and an Explorer-return signal
starts a new 250 ms recovery window and reacquires successfully. The original
31 s sleep is gone. The setter and timer observation are `QS_TEST`-only and reset
with the existing fake factory; release behavior is unchanged.

To reproduce graph counts, configure both revisions with the same canonical
toolchain and count `^build .*: CXX_COMPILER` edges in `build.ninja`. To compare
inventory, capture `ctest --test-dir <build> --show-only=json-v1` for each revision,
remove each test's `backtrace` field, and compare the `tests` arrays. Reuse the
same build path with built executables present so CTest can resolve commands.
