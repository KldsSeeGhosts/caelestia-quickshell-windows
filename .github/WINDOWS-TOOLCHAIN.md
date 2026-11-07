# Windows fixture toolchain

The producer workflow consumes the shell repository's canonical public
`windows/toolchain.json`, Python locks and shared PowerShell helpers at
`fcacc6ea53683cdbb8c1bd65c8b4fe90e9ac6664`. There is no second compiler,
Rust or runtime policy in this repository. Updating that contract source is
a deliberate reviewed toolchain upgrade, not a floating branch checkout.

That exact commit must be anonymously fetchable in
`KldsSeeGhosts/caelestia-windows` before this workflow can pass. A missing source
commit is a publication prerequisite, not permission to add private credentials
or select a different toolchain. Hosted acceptance must be recorded after
publication; a prepared workflow is not evidence that its job ran.

The `windows-2022` label alone does not pin installed software. Before compiling,
the shared helper selects VC tools 14.44.35207 and Windows SDK 10.0.26100.0
explicitly, binds the x64 linker, and asserts actual clang-cl, Rust/target, Qt,
CMake, Ninja and CLI11 versions. Preparation verifies committed public download
hashes, including the Python tools, Qt archives and Rust release manifest.
Serviced compiler/linker versions and the official Rust commit are exact
requirements. The VS redist marker must select CRT DLLs whose actual versions
match the qualified `vcRedistVersion` and do not exceed the runtime floor.
An extracted tooling cache is trusted only through the existing trusted-main
writer boundary; archive checksums do not reauthenticate every cached file.
Qt's checked supplier SBOM build-tool versions are not a declared minimum CRT
requirement or a clean-VM qualification.

Public receipts contain only version/hash fields, backend and contract-source
revisions, and the separate VS installation version. They exclude raw paths,
environment reports and build logs. The portable fixture selection and native
test project are unchanged. Fixture passes do not establish a package/runtime,
interactive Windows desktop or hardware qualification.

Capture source can move within the shell repository without introducing another
toolchain policy: the specification and helpers do not select its source path.

The [candidate/result v1 contract](candidate-contract/README.md) records requested
and resolved source identities, source dirt, canonical specification/lock digests,
actual tools, stage outcomes/timings and nonempty test counts. Its producer-only
fixtures run before preparing native tools. The existing stable fixture job
always attempts a small sanitized receipt/JUnit upload, including on failure.
Hard runner cancellation may prevent finalization or upload and is not a pass.
Build parallelism is measured by `.github/scripts/Get-BuildJobs.ps1`: one job
per logical processor, bounded by floor(total physical RAM / 3 GiB), clamped to
1–16 (the fixture gate's accepted range). Measurement failure stops the job.
Only compilation is parallelized, not test execution. Main push and manual
runs have unique concurrency groups; only PR runs share a per-PR group and
cancel earlier runs. The job label remains `Build all fixtures, run portable tier`.
Opt-in command echo streams live without adding raw output to receipts.
The producer's `merge` command supports two unfinalized same-run downstream
receipts from parallel fixture/integration jobs, then normal finalization and
validation using the unchanged v1 schema. This does not split the standalone
backend workflow; artifact provenance must still be checked by its consumer.
The shell checkout remains toolchain-only in standalone results, not evidence of
an end-to-end tested shell/backend pair. See that contract for clean commands,
privacy limits and root-owned intentional compile/test failure acceptance.

## Optional fixture object cache

The standalone workflow trials sccache 0.15.0, not the rejected ccache
configuration. `.github/scripts/Prepare-FixtureSccache.ps1` pins the Windows x64
release archive and its SHA-256
`b0b257a164bf438b2dea134ca7ded41c100f59a64b3bf275a202f1e8102ab217`.
Extraction follows checksum verification. The cache is local disk only, bounded
to 2 GiB on hosted runners. No remote cache service or credentials are used.

The gate accepts optional `-CompilerLauncher <absolute executable>` or
`QS_FIXTURE_COMPILER_LAUNCHER`, setting both CMake compiler launchers at configure
time. Existing shell calls remain uncached and unchanged. The shell fetches this
gate from its pinned producer revision, so adopting it requires a separate shell
pin bump and explicit opt-in. `candidate_contract.py` is unchanged.

Object-cache keys include a schema version, the sccache release (its preparation
script) and the canonical toolchain and Python locks. Fixture CMake files and the
gate are excluded on purpose: sccache keys every object by compiler identity,
arguments and preprocessed input, so graph edits cannot serve a stale object and
need not discard the whole warm cache. Only a `push`
to trusted `refs/heads/main` may save the shared cache. PRs and manual dispatches
only restore it. During measurement, a temporary writer saved to the PR's own
GitHub cache ref and restore prefix; main and other PRs cannot read that ref.
That writer and prefix are removed from the final workflow.
Statistics are printed after each build and uploaded as `sccache-stats.json`.
Build failures remain failures; caching does not replace any fixture or receipt.

This workflow has no docs-only skip: documentation-only PRs still run the stable
`Build all fixtures, run portable tier` job and all portable fixtures.
