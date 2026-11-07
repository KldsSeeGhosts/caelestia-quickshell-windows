# Bounded fixture sccache trial

## Local cacheability evidence

The rejected ccache experiment was not repeated. sccache 0.15.0 was downloaded
from its public Windows x64 release, verified against the committed SHA-256
and the release's checksum asset, then exercised with actual commands from
the fixture `build.ninja`. No dependency options were ignored or rewritten.
The commands retain CMake 4.4's `-clang:-MD`, `-clang:-MT`, `-clang:-MF` flags.

Three real compile commands (`redact.cpp`, `screen_identity.cpp`, `util.cpp`)
were replayed twice, on the selected canonical clang-cl toolchain:
6 requests, 3 misses, 3 hits, 0 non-cacheable calls/compilations,
0 unsupported compiler calls and 0 errors.

A separate full configure/build with the launcher produced:

| Workstation measurement | Configure + build | Hits | Misses | Non-cacheable calls |
| --- | ---: | ---: | ---: | ---: |
| Cold local disk | 83.33 s | 0 | 291 | 0 |
| Clean rebuild, warm local disk | 21.72 s | 291 | 0 | 0 |

Both full builds succeeded with 12 workers. These are not GitHub-hosted timings.
The warm rebuild used Ninja's clean target, not an incremental no-op build.
The local cache occupied about 9 MiB; both runs reported zero cache errors,
write errors, compilation failures and unsupported compiler calls.
All 44 portable fixtures passed in 78.53 s on the unmodified test graph.

The gate was also configured with an explicit launcher and then without one,
in the same build directory. Both configurations succeeded. Both CMake
compiler-launcher entries were set on opt-in and cleared on opt-out.
52 producer Python tests and touched PowerShell parser checks passed.
The unchanged validator Git blob has SHA-256
`e7e890428a49812208866349a00482cf0cddbca6ebbaf71cb7a06a5c3284b5a0`.

## Hosted acceptance

Measured on the independent, un-deduplicated graph at
`980a5eb49acc4d3e71ec462c106de8b7b7d0976b`, using four hosted workers.
Cold run: [42408812180, attempt 1](https://github.com/KldsSeeGhosts/caelestia-quickshell-windows/actions/runs/42408812180/attempts/1).
The required stable job passed.

| Hosted measurement | Build every fixture step | Compile stage | Hits | Misses | Non-cacheable calls |
| --- | ---: | ---: | ---: | ---: | ---: |
| Cold local disk | 366 s | 353.375 s | 0 | 291 | 0 |
| Warm local disk | Pending | Pending | Pending | Pending | Pending |

Cold configure: 11.312 s. Portable: 78.532 s, all 44 passed. Cache size:
9,785,938 bytes. Unsupported compiler calls, non-cacheable compilations,
compilation failures, cache errors and write errors: all zero.
The workflow was rerun with `gh run rerun 42408812180` for warm evidence.

Do not merge until warm hosted job/step timings and statistics are recorded here.
The temporary PR-ref cache writer and its restore prefix have been removed from
the final workflow. The shared writer remains restricted to trusted main pushes.
The full compile-deduplication change is intentionally not included in this PR.
Measurements must distinguish the two changes.
