# Candidate/result contract v1

The backend owns this producer contract. The shell can consume it without copying
the compiler policy or waiting for a shell-specific producer implementation.
`../scripts/candidate_contract.py` is the normative stdlib validator.
`v1.schema.json` describes the wire shape; JSON Schema alone is **not** a pass
validator. Unknown versions, malformed documents and validator exceptions fail
closed. A validator exit of zero means a complete pass, not just valid JSON.

## Immutable identity and kinds

Both source records are required, including for a standalone run:

| Field | Meaning |
| --- | --- |
| `repository` | Exactly `KldsSeeGhosts/caelestia-quickshell-windows` for backend, `KldsSeeGhosts/caelestia-windows` for shell |
| `requested` | Explicit immutable 40-character lowercase Git SHA, never a branch, tag, short SHA or PR number |
| `resolved` | Independently inspected checkout `HEAD`, never copied from the request |
| `dirty` | Actual tracked/untracked dirt; `null` means not inspected, not clean |
| `override` | Whether a noncanonical source override was used; only literal `false` can pass |

`kind: standalone` means the shell SHA supplies only the canonical toolchain,
not a tested application pair. On PRs the backend requested SHA is `github.sha`,
the exact GitHub synthetic merge commit, **not** the PR head. `kind: downstream`
requires an additional `integration` stage and nonempty `integration` test
results for the exact requested pair. Only the downstream shell consumer can claim that
application integration was executed. Neither kind qualifies packages, CRT
installation on a clean VM, interactive Windows 11 behavior or physical hardware.

Sources are inspected before setup and again during finalization. Origins must
be the exact public HTTPS repository URLs (optional `.git` suffix). Worktree
`.git` files are supported. A parent repository accidentally selected as a source
root is rejected. When the shell toolchain checkout is nested inside the backend,
only that exact infrastructure directory is excluded from the backend dirt scan;
its own entire checkout is independently checked. Other generated/untracked
source files are dirt. Ignored build outputs are not source inputs.

## Receipt fields and pass semantics

`schemaVersion` is integer `1`. `evidenceClass` is `component-offscreen`.
`createdAt`/`finishedAt` and each stage's `startedAt`/`finishedAt` are UTC ISO
timestamps. `durationSeconds` is nonnegative finite monotonic elapsed time.

Required command stages are `sources`, `producer`, `prepare`, `configure`,
`build`, `portable`, plus `integration` for downstream. Each records `outcome`,
`exitCode`, timestamps, elapsed seconds and bounded `diagnostics`. Outcomes are
`not_run`, `running`, `success`, `failure`. Finalization never changes an
interrupted `running` stage into success. Cancellation is represented by
`workflowOutcome: cancelled` and `result: cancelled`, or by a retained incomplete
receipt if no finalizer could run. Only all-success required stages with integer
zero exit codes can pass. Setup/cache/action failures also propagate through
`workflowOutcome`; they cannot be rescued by a successful artifact upload.

`toolchain` records:

- `specSha256`: SHA-256 of UTF-8 `windows/toolchain.json` with CRLF normalized to LF.
- `contractSha256`: the canonical helper's digest of that normalized spec followed
  by `python-bootstrap.lock.txt` and `python-tools.lock.txt`, joined by LF in that
  order. This is intentionally distinct from the spec-only digest.
- `expected`: contract ID and version/target/servicing identities read from the
  exact shell's spec.
- `actual`: allowlisted results of `Get-CaelestiaActualToolchain`, including its
  combined contract digest, compiler, linker, CRT, SDK, clang target, Rust
  commit/target, Cargo, Qt/supplier compiler/linker, CMake, Ninja and CLI11.

Actual identities must match expected identities and the combined digest.
`producer.pythonVersion` records the actual Python 3.12 patch version.
The producer does not substitute a runner banner for selected compiler identity.
`flags` fixes Ninja, Release, clang-cl, portable selection and offscreen/software
Qt. Build workers are selected by `.github/scripts/Get-BuildJobs.ps1`: one per
logical processor, bounded by floor(physical RAM / 3 GiB), clamped to 1–16.
Failed processor/memory measurement fails closed rather than guessing.
Tests are not parallelized; packaging is not performed.

`tests` contains `producer` and `portable`, plus `integration` for downstream.
Each has integer `total`, `executed`, `failed`, `skipped`, and `skips` entries
with a test identifier and reason. CTest counts executables, not individual
Qt assertions. Whole-executable skips reduce `executed`; Qt's internal `SKIP`
lines are separate entries in `skips` and do not change CTest executable counts.
Zero executed tests, all-skipped results, missing JUnit, malformed/count-mismatched
JUnit and any failure are non-pass, even if the command exits zero.
`omittedTiers` records configured native/desktop/gpu/machine counts and explicit
separate-acceptance reasons. Those are not executed tests.

`workflowOutcome` is the job status observed by the finalizer, before uploading.
`result` starts `incomplete` and becomes `pass`, `non-pass`, or `cancelled`.
Stage receipts are written atomically before starting commands and after they
exit. A setup failure can therefore retain the request even without tools.
Checkout failure before the helper is available, hard runner cancellation,
termination during finalization and upload/service failure can prevent a complete
artifact. No complete artifact is promised for those cases.

Consumers must also require the actual upstream job/run conclusion to be success,
compare both requested/resolved identities against their independently selected
pair, recompute both toolchain digests from their trusted shell checkout, check
artifact/run provenance and execute a trusted version of the validator. Use the
validator's `--kind`, `--backend-sha`, `--shell-sha`, `--contract-sha256` and
`--spec-sha256` expectations rather than relying on a receipt's self-consistency.
A JSON receipt is not a signature or proof that arbitrary PR code
honestly executed. Never trust a PR-supplied validator as an authorization boundary.

## Interface and clean commands

From a clean backend checkout, using Python 3.12:

```powershell
python -B -m unittest discover -s .github/candidate-contract -p "test_*.py"
python -B .github/scripts/candidate_contract.py --receipt <results>/candidate-result.json init `
  --kind standalone --backend-sha <full-backend-sha> --shell-sha <full-shell-sha>
python -B .github/scripts/candidate_contract.py --receipt <results>/candidate-result.json sources `
  --backend . --shell <clean-exact-shell-checkout>
python -B .github/scripts/candidate_contract.py --receipt <results>/candidate-result.json run `
  --stage producer --unittest -- python -B -m unittest discover -s .github/candidate-contract -p "test_*.py"
```

The existing workflow prepares tools through the pinned shell
`Prepare-Toolchain.ps1`. `Invoke-WindowsFixtureGate.ps1` runs phases `Configure`,
`Build` and `Portable`, taking `-ContractRoot`, `-ToolRoot`, `-BuildRoot`,
`-Results` and optional `-Jobs` (1–16, default 2 for direct callers). CI selects
hardware-aware parallelism and passes `-Jobs $env:BUILD_JOBS`.
Each phase runs through the helper's `run`
command with its corresponding stage. `Portable` additionally passes
`--junit <results>/portable.raw.xml` to the recorder. `inventory --ctest <ctest>
--build <build-root>` records omitted tiers from configured CTest metadata.
No release package or second copy of the native gate is added.

```powershell
python -B .github/scripts/candidate_contract.py --receipt <results>/candidate-result.json finalize `
  --workflow-outcome success --toolchain <results>/actual-toolchain.json `
  --backend . --shell <clean-exact-shell-checkout>
python -B .github/scripts/candidate_contract.py --receipt <results>/candidate-result.json validate
```

`finalize` returns nonzero on non-pass but writes the receipt first. Its two
source arguments are required to detect mid-run dirt. `run` returns the actual
command exit code (or a nonzero validation/launch failure). Raw command output is
suppressed by default. The isolated hosted workflow opts into `run --echo` for
line-by-line live job diagnostics, preserving original output bytes for sanitized
diagnostic extraction; retained JSON/JUnit still contain no raw output. Never
enable echo for commands with access to personal data or secrets. Bootstrap
receipt/source actions support Python 3.9+ before setup-python; passing final
receipts require the recorded Python 3.12 identity.
Results/build roots must be outside source checkouts. Set
`PYTHONDONTWRITEBYTECODE=1` or use `-B` to avoid Python source dirt.
Every fresh attempt requires a new result directory; `init` refuses overwrite.
The producer fixtures use synthetic metadata and disposable files only, not a
shell consumer, native desktop, network, clipboard or installed app.

### Parallel downstream receipts

Two parallel jobs from the same downstream run may record partial receipts.
The fixture job owns `configure`, `build`, `portable`, `tests.portable` and the
`inventory` output. The integration job owns `integration` and
`tests.integration`. Both must successfully execute `sources`, `producer` and
`prepare`, with integer zero exit codes. Every non-owned stage must remain
untouched (`not_run` with empty timing/diagnostics), and neither input may be
finalized. The actual toolchain and producer Python fields may still be empty,
as in normal unfinalized receipts, but must match between inputs.

```powershell
python -B .github/scripts/candidate_contract.py --receipt <results>/merged.json merge `
  --fixture <fixture-results>/candidate-result.json `
  --integration <integration-results>/candidate-result.json
```

The output path must be fresh. Kind (downstream only), schema, source identities,
toolchain digests/expected/actual identities, flags and producer info must match.
The merge keeps the fixture's shared stage values and inventory, copies the
integration stage/test counts, and uses the earliest `createdAt`. Stage timing
may overlap; no inter-stage ordering is
required. The output is still incomplete: run the normal `finalize` command
above, with verified source roots, actual toolchain and aggregate workflow
outcome, then `validate`. Finalization records actual toolchain and Python
identities and applies all existing PASS checks, including counts and inventory.
A failed job cannot be rescued by merge.
There are no schema changes. Historical and sequential validation is unchanged.
The caller must verify both jobs' same-run artifact provenance and conclusions;
v1 receipts do not contain a run ID and cannot establish that provenance alone.

## Retention, security and acceptance

The existing stable check remains **Build all fixtures, run portable tier** in
**Windows fixtures**. It builds every fixture once and runs only portable tests.
PR runs share a concurrency group per PR with cancellation enabled. Push and
manual runs use unique run IDs, so newer main runs do not queue behind older
runs in the workflow concurrency group; only PR runs cancel earlier runs.
The job retains `candidate-result.json` and sanitized `portable.xml` on both
success and failure for seven days, when the runner can execute finalization
and upload. It uploads no build tree, raw logs, original JUnit, user profile or
unfiltered tool receipt. JUnit is rebuilt from safe names, counts and generic
failure/skip categories. Failure diagnostics retain exit codes, compiler error
codes, fixed compile/link/assertion/prerequisite categories and excluded-output
byte counts, never arbitrary output text. Qt optional
shader/layer-rule skips get explicit configured-prerequisite reasons.
Detailed raw output is not retained in artifacts. Hosted job logs opt into it
for compiler/CTest diagnosis on the isolated, read-only runner.

Permissions stay read-only; checkout persists no credentials. PRs cannot save
the trusted-main tooling cache, obtain release secrets or access a desktop.
Canonical toolchain preparation and identity assertions remain mandatory.

Local producer controls cover valid standalone/downstream receipts, malformed
identities, dirt/overrides, failed/cancelled/incomplete stages, drift, empty and
failed tests, stale/malformed JUnit, parallel receipt merging, live opt-in echo
and privacy filtering. These are contract
fixtures, not native acceptance.

Root-owned hosted acceptance still requires:

1. Run an unchanged clean public candidate and retain the run/check URL,
   receipt, actual test counts/skips and both exact SHAs.
2. On a disposable review branch, introduce a deliberate compile error in a
   fixture. The unchanged stable check must fail, with a failed build stage,
   non-pass receipt and retained sanitized diagnostics. Do not merge the control.
3. Independently introduce a deliberate portable assertion failure. Require a
   nonzero CTest exit, failed testcase/JUnit, non-pass receipt and failed check.
4. Cancel a running disposable job. Any available receipt must be incomplete or
   cancelled/non-pass. If the runner cannot upload, record the missing artifact
   and cancelled job, not a fictional completed bundle.

Those hosted controls, independent review and branch-protection configuration
are not supplied by local synthetic fixtures. End-to-end pair execution remains
the shell integration gate. Clean-VM runtime and trusted desktop/hardware acceptance remain separate.
