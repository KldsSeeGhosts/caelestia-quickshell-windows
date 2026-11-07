"""Version 1 candidate/result producer. Python stdlib only, fail closed.

No raw command output, environment, source paths or JUnit system-out is retained.
Bootstrap actions support Python 3.9+; a passing final receipt requires 3.12.
Raw output is echoed only with an explicit opt-in for isolated hosted job logs.
The JSON format and trust boundary are documented in ../candidate-contract/README.md.
"""

import argparse
import copy
import datetime as dt
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

REPOS = {
    "backend": "KldsSeeGhosts/caelestia-quickshell-windows",
    "shell": "KldsSeeGhosts/caelestia-windows",
}
STAGES = ("sources", "producer", "prepare", "configure", "build", "portable")
TOOLS = (
    "runtimeMinimum", "vcToolsVersion", "vcCompilerFileVersion",
    "vcLinkerFileVersion", "vcRedistDirectoryVersion", "vcRedistVersion",
    "windowsSdkVersion", "clangVersion", "clangTarget", "rustVersion",
    "rustTarget", "rustCommit", "cargoVersion", "qtVersion", "cmakeVersion",
    "ninjaVersion", "cli11Version",
    "qtCompilerFileVersion", "qtLinkerFileVersion",
)
FLAGS = {
    "generator": "Ninja", "configuration": "Release", "compiler": "clang-cl",
    "testSelection": "^portable$", "qpa": "offscreen", "quickBackend": "software",
}
SHA = re.compile(r"[0-9a-f]{40}")
DIGEST = re.compile(r"[0-9a-f]{64}")
IDENTIFIER = re.compile(r"[A-Za-z0-9_.:-]{1,160}")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def read(path):
    # PowerShell 5.1 writes UTF-8 with BOM.
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def stages(kind):
    require(kind in ("standalone", "downstream"), "invalid candidate kind")
    return STAGES + (("integration",) if kind == "downstream" else ())


def new_receipt(kind, backend_sha, shell_sha):
    for sha in (backend_sha, shell_sha):
        require(SHA.fullmatch(sha or ""), "immutable full lowercase SHA required")
    return {
        "schemaVersion": 1,
        "kind": kind,
        "evidenceClass": "component-offscreen",
        "createdAt": now(),
        "finishedAt": None,
        "sources": {
            key: {"repository": repo, "requested": sha, "resolved": None,
                  "dirty": None, "override": False}
            for (key, repo), sha in zip(REPOS.items(), (backend_sha, shell_sha))
        },
        "toolchain": {"specSha256": None, "contractSha256": None, "expected": {}, "actual": {}},
        "producer": {},
        "flags": FLAGS.copy(),
        "stages": {
            name: {"outcome": "not_run", "exitCode": None, "startedAt": None,
                   "finishedAt": None, "durationSeconds": None, "diagnostics": {}}
            for name in stages(kind)
        },
        "tests": {},
        "omittedTiers": {},
        "workflowOutcome": None,
        "result": "incomplete",
    }


def git(root, *args):
    result = subprocess.run(["git", "-C", str(root), *args], capture_output=True)
    require(result.returncode == 0, "source inspection failed")
    return result.stdout.decode("utf-8", errors="replace").strip()


def inspect_source(root, repository, requested, exclude=None):
    # Worktree .git files are allowed. An accidental parent repo is not.
    require(Path(git(root, "rev-parse", "--show-toplevel")).resolve()
            == Path(root).resolve(), "source root mismatch")
    remote = git(root, "remote", "get-url", "origin").removesuffix(".git")
    require(remote == f"https://github.com/{repository}", "repository mismatch")
    args = ["status", "--porcelain", "--untracked-files=all", "--", "."]
    if exclude:
        args.append(f":(exclude){exclude}")
    return {"repository": repository, "requested": requested,
            "resolved": git(root, "rev-parse", "HEAD"),
            "dirty": bool(git(root, *args)), "override": False}


def resolve_sources(receipt, backend, shell):
    backend, shell = Path(backend).resolve(), Path(shell).resolve()
    require(backend != shell, "source roots must differ")
    exclusion = shell.relative_to(backend).as_posix() if shell.is_relative_to(backend) else None
    for key, root in (("backend", backend), ("shell", shell)):
        receipt["sources"][key] = inspect_source(
            root, REPOS[key], receipt["sources"][key]["requested"],
            exclusion if key == "backend" else None)
    spec_path = shell / "windows/toolchain.json"
    spec = read(spec_path)
    spec_text = spec_path.read_text(encoding="utf-8-sig")
    receipt["toolchain"]["specSha256"] = hashlib.sha256(spec_text.encode("utf-8")).hexdigest()
    # Match Get-CaelestiaToolchainHash exactly: LF text, ordered files, LF join.
    texts = [spec_text] + [
        (shell / "windows/toolchain" / name).read_text(encoding="utf-8-sig")
        for name in ("python-bootstrap.lock.txt", "python-tools.lock.txt")]
    receipt["toolchain"]["contractSha256"] = hashlib.sha256(
        "\n".join(texts).encode("utf-8")).hexdigest()
    receipt["toolchain"]["expected"] = {
        "contractId": spec["id"], **{key: spec[key] for key in TOOLS}
    }
    check_sources(receipt)


def check_sources(receipt):
    require(set(receipt["sources"]) == set(REPOS), "source set mismatch")
    for key, repository in REPOS.items():
        source = receipt["sources"][key]
        require(set(source) == {"repository", "requested", "resolved", "dirty", "override"},
                "source fields mismatch")
        require(source["repository"] == repository, "repository mismatch")
        require(isinstance(source["requested"], str)
                and SHA.fullmatch(source["requested"]), "invalid requested SHA")
        require(source["resolved"] == source["requested"], "resolved SHA mismatch")
        require(source["dirty"] is False, "source is dirty or unverified")
        require(source["override"] is False, "source override is not canonical")


def safe_name(value):
    return value if IDENTIFIER.fullmatch(value or "") else "redacted"


def junit(path):
    """Rebuild only safe structure. Never copy XML attributes/text wholesale."""
    root = ET.parse(path).getroot()
    require(root.tag in ("testsuite", "testsuites"), "invalid JUnit root")
    cases = list(root.iter("testcase"))
    require(bool(cases), "zero JUnit test cases")
    require(len(cases) <= 10000, "JUnit case bound exceeded")
    summary = {"total": len(cases), "executed": 0, "failed": 0, "skipped": 0,
               "skips": []}
    clean = ET.Element("testsuite")
    for case in cases:
        name = safe_name(case.get("name"))
        item = ET.SubElement(clean, "testcase", name=name)
        skipped = case.find("skipped") is not None or case.get("status") in ("notrun", "disabled")
        failed = case.find("failure") is not None or case.find("error") is not None
        require(not (skipped and failed), "contradictory JUnit outcome")
        if skipped:
            summary["skipped"] += 1
            summary["skips"].append({"test": name, "reason": "ctest-reported-skip"})
            ET.SubElement(item, "skipped", message="ctest-reported-skip")
        else:
            summary["executed"] += 1
        if failed:
            summary["failed"] += 1
            ET.SubElement(item, "failure", message="fixture-failed; raw output excluded")
        # Qt Test skips can live inside a successful CTest executable's output.
        for text in case.iter("system-out"):
            for line in (text.text or "").splitlines():
                if line.startswith("SKIP "):
                    reason = ("optional-qsb-not-configured" if "IIW_QSB" in line else
                              "optional-layer-rules-not-configured" if "IIW_LAYER_RULES" in line
                              else "qt-reported-skip; details-excluded")
                    summary["skips"].append({"test": name, "reason": reason})
    clean.set("tests", str(summary["total"]))
    clean.set("failures", str(summary["failed"]))
    clean.set("skipped", str(summary["skipped"]))
    if "tests" in root.attrib:
        require(int(root.get("tests")) == summary["total"], "JUnit count mismatch")
    if "failures" in root.attrib or "errors" in root.attrib:
        require(int(root.get("failures", 0)) + int(root.get("errors", 0))
                == summary["failed"], "JUnit failure count mismatch")
    if "skipped" in root.attrib:
        require(int(root.get("skipped")) == summary["skipped"], "JUnit skip count mismatch")
    return summary, ET.tostring(clean, encoding="unicode")


def diagnostics(output):
    return {
        "outputBytesExcluded": len(output),
        "compilerCodes": sorted(set(re.findall(
            rb"\b(?:C[0-9]{4}|LNK[0-9]{4})\b", output)))[:20],
        "categories": [name for name, pattern in (
            ("compile-error", rb"(?:fatal )?error:"),
            ("undeclared-identifier", rb"undeclared identifier"),
            ("missing-include", rb"(?:file not found|cannot open include file)"),
            ("link-error", rb"(?:unresolved external|undefined symbol|linker command failed)"),
            ("test-assertion", rb"(?:FAIL!|Assertion.*failed)"),
            ("tool-prerequisite", rb"(?:Missing supported|Missing Windows SDK|version mismatch)"),
        ) if re.search(pattern, output, flags=re.IGNORECASE)],
    }


def start(receipt, stage):
    require(stage in stages(receipt["kind"]), "unknown stage")
    require(receipt["stages"][stage]["outcome"] == "not_run", "stage already attempted")
    receipt["stages"][stage].update(outcome="running", startedAt=now())
    receipt["finishedAt"] = None
    receipt["result"] = "incomplete"


def finish(receipt, stage, code, elapsed, diagnostic=None):
    receipt["stages"][stage].update(
        outcome="success" if code == 0 else "failure", exitCode=code,
        finishedAt=now(), durationSeconds=round(elapsed, 6), diagnostics=diagnostic or {})


def validate(receipt, *, kind=None, backend_sha=None, shell_sha=None,
             contract_sha256=None, spec_sha256=None):
    """Validate a PASS claim, not merely JSON shape. Any uncertainty is non-pass."""
    require(isinstance(receipt, dict) and set(receipt) == {
        "schemaVersion", "kind", "evidenceClass", "createdAt", "finishedAt",
        "sources", "toolchain", "producer", "flags", "stages", "tests",
        "omittedTiers", "workflowOutcome", "result"}, "receipt fields mismatch")
    require(receipt["schemaVersion"] == 1 and type(receipt["schemaVersion"]) is int,
            "unsupported version")
    require(receipt["evidenceClass"] == "component-offscreen", "invalid evidence class")
    require(receipt["flags"] == FLAGS, "canonical flags mismatch")
    check_sources(receipt)
    if kind is not None:
        require(receipt["kind"] == kind, "unexpected candidate kind")
    for key, expected_sha in (("backend", backend_sha), ("shell", shell_sha)):
        if expected_sha is not None:
            require(isinstance(expected_sha, str) and SHA.fullmatch(expected_sha),
                    "invalid expected source SHA")
            require(receipt["sources"][key]["requested"] == expected_sha
                    and receipt["sources"][key]["resolved"] == expected_sha,
                    "unexpected source SHA")
    require(receipt["workflowOutcome"] == "success", "workflow did not succeed")
    require(receipt["result"] == "pass" and receipt["finishedAt"], "result is not complete pass")
    created, finished = (dt.datetime.fromisoformat(receipt[key])
                         for key in ("createdAt", "finishedAt"))
    require(created.utcoffset() == dt.timedelta(0)
            and finished.utcoffset() == dt.timedelta(0) and finished >= created,
            "invalid receipt timestamps")
    require(set(receipt["stages"]) == set(stages(receipt["kind"])), "required stage set mismatch")
    for stage in receipt["stages"].values():
        require(isinstance(stage, dict) and set(stage) == {
            "outcome", "exitCode", "startedAt", "finishedAt", "durationSeconds", "diagnostics"},
            "stage fields mismatch")
        require(stage["outcome"] == "success" and type(stage["exitCode"]) is int
                and stage["exitCode"] == 0, "required stage did not succeed")
        duration = stage["durationSeconds"]
        require(type(duration) in (int, float) and math.isfinite(duration) and duration >= 0,
                "invalid stage duration")
        begin, end = (dt.datetime.fromisoformat(stage[key])
                      for key in ("startedAt", "finishedAt"))
        require(begin.utcoffset() == dt.timedelta(0) and end.utcoffset() == dt.timedelta(0)
                and finished >= end >= begin >= created,
                "invalid stage timestamps")
    toolchain = receipt["toolchain"]
    require(isinstance(toolchain, dict) and set(toolchain) == {
        "specSha256", "contractSha256", "expected", "actual"}, "toolchain fields mismatch")
    require(isinstance(toolchain["specSha256"], str)
            and DIGEST.fullmatch(toolchain["specSha256"]), "invalid spec digest")
    expected, actual = toolchain["expected"], toolchain["actual"]
    require(isinstance(expected, dict) and isinstance(actual, dict),
            "invalid tool identity maps")
    require(set(expected) == {"contractId", *TOOLS}, "missing expected tool identities")
    require(isinstance(toolchain["contractSha256"], str)
            and DIGEST.fullmatch(toolchain["contractSha256"]), "invalid combined contract digest")
    for key, expected_digest in (("contractSha256", contract_sha256),
                                 ("specSha256", spec_sha256)):
        if expected_digest is not None:
            require(isinstance(expected_digest, str) and DIGEST.fullmatch(expected_digest),
                    "invalid expected toolchain digest")
            require(toolchain[key] == expected_digest, "unexpected toolchain digest")
    require(actual.get("contractSha256") == toolchain["contractSha256"],
            "toolchain digest mismatch")
    require(type(actual.get("schemaVersion")) is int and actual["schemaVersion"] == 1,
            "toolchain receipt version mismatch")
    require(set(actual) <= {"schemaVersion", "contractSha256", "vsInstallationVersion",
                            "contractId", *TOOLS}, "unknown actual tool field")
    for key, value in expected.items():
        require(isinstance(value, str) and value and IDENTIFIER.fullmatch(value),
                "invalid tool identity")
        require(actual.get(key) == value, "actual tool identity mismatch")
    require(re.fullmatch(r"3\.12\.\d+", receipt["producer"]["pythonVersion"]),
            "producer Python identity mismatch")
    require(set(receipt["producer"]) == {"pythonVersion"}, "producer fields mismatch")
    required_tests = {"producer", "portable"} | (
        {"integration"} if receipt["kind"] == "downstream" else set())
    require(set(receipt["tests"]) == required_tests, "required test results missing")
    for counts in receipt["tests"].values():
        require(isinstance(counts, dict) and set(counts) == {
            "total", "executed", "failed", "skipped", "skips"}, "test count fields mismatch")
        for key in ("total", "executed", "failed", "skipped"):
            require(type(counts[key]) is int and counts[key] >= 0, "invalid test count")
        require(counts["executed"] > 0 and counts["failed"] == 0, "zero or failed tests")
        require(counts["total"] == counts["executed"] + counts["skipped"],
                "test count mismatch")
        require(isinstance(counts["skips"], list)
                and len(counts["skips"]) >= counts["skipped"], "missing skip reasons")
        for skip in counts["skips"]:
            require(set(skip) == {"test", "reason"} and skip["test"] and skip["reason"],
                    "empty or invalid skip reason")
    require(set(receipt["omittedTiers"]) == {"native", "desktop", "gpu", "machine"},
            "omitted tiers missing")
    for tier in receipt["omittedTiers"].values():
        require(set(tier) == {"count", "reason"} and type(tier["count"]) is int
                and tier["count"] >= 0 and tier["reason"],
                "invalid omitted tier")


def finalize(receipt, workflow_outcome):
    receipt["workflowOutcome"] = workflow_outcome
    receipt["finishedAt"] = now()
    receipt["result"] = "pass"
    try:
        validate(receipt)
    except (ValueError, KeyError, TypeError):
        receipt["result"] = "cancelled" if workflow_outcome == "cancelled" else "non-pass"
        return False
    return True


def merge(fixture, integration):
    """Join unfinalized same-run downstream evidence, never a partial PASS."""
    shared = {"sources", "producer", "prepare"}
    fixture_only = {"configure", "build", "portable"}
    identity = ("schemaVersion", "kind", "evidenceClass", "sources",
                "toolchain", "flags", "producer")
    require(all(fixture[key] == integration[key] for key in identity),
            "partial receipt identity mismatch")
    require(fixture["kind"] == "downstream", "merge requires downstream receipts")
    template = new_receipt("downstream", "a" * 40, "b" * 40)
    empty = template["stages"]
    require(type(fixture["schemaVersion"]) is int and fixture["schemaVersion"] == 1,
            "unsupported version")
    for receipt, owned, suites in (
            (fixture, shared | fixture_only, {"producer", "portable"}),
            (integration, shared | {"integration"}, {"producer", "integration"})):
        require(set(receipt) == set(template), "partial receipt fields mismatch")
        check_sources(receipt)
        require(receipt["finishedAt"] is None and receipt["workflowOutcome"] is None
                and receipt["result"] == "incomplete", "input already finalized")
        require(set(receipt["stages"]) == set(empty), "partial stage set mismatch")
        require(set(receipt["tests"]) == suites, "partial test set mismatch")
        for name, stage in receipt["stages"].items():
            require(set(stage) == set(empty[name]), "partial stage fields mismatch")
            if name in owned:
                require(stage["outcome"] == "success" and type(stage["exitCode"]) is int
                        and stage["exitCode"] == 0, "owned stage did not succeed")
                require(dt.datetime.fromisoformat(stage["startedAt"])
                        >= dt.datetime.fromisoformat(receipt["createdAt"]),
                        "stage predates its receipt")
            else:
                require(stage == empty[name], "non-owned stage attempted")
    merged = copy.deepcopy(fixture)
    merged["createdAt"] = min(
        (fixture["createdAt"], integration["createdAt"]), key=dt.datetime.fromisoformat)
    merged["stages"]["integration"] = copy.deepcopy(integration["stages"]["integration"])
    merged["tests"]["integration"] = copy.deepcopy(integration["tests"]["integration"])
    # Actual tool identities and producer Python may still be empty: the normal
    # finalizer records them after its source recheck. Merge is not a PASS claim.
    return merged


def run_command(args, receipt):
    start(receipt, args.stage)
    write(args.receipt, receipt)  # Durable running marker before launching work.
    began = time.monotonic()
    code, output, reason = 1, b"", "command-exit-nonzero"
    try:
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        require(bool(command), "command required")
        if args.junit:
            # Never reuse an old successful JUnit if this execution fails early.
            Path(args.junit).unlink(missing_ok=True)
            (Path(args.receipt).parent / f"{args.stage}.xml").unlink(missing_ok=True)
        if getattr(args, "echo", False):
            # Opt-in job diagnostics are not copied into retained artifacts.
            # Never enable this for commands that can read personal data/secrets.
            output = bytearray()
            with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as child:
                for line in iter(child.stdout.readline, b""):
                    output.extend(line)
                    sys.stdout.buffer.write(line)
                    sys.stdout.flush()
                code = child.wait()
        else:
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            code, output = result.returncode, result.stdout
        if args.unittest:
            match = re.search(rb"(?m)^Ran (\d+) tests? in ", output)
            require(match is not None, "unittest count missing")
            total = int(match[1])
            skip = re.search(rb"skipped=(\d+)", output)
            skipped = int(skip[1]) if skip else 0
            failed = sum(int(value) for value in re.findall(
                rb"\b(?:failures|errors)=(\d+)", output))
            receipt["tests"][args.stage] = {
                "total": total, "executed": total - skipped, "skipped": skipped,
                "failed": failed,
                "skips": [{"test": "producer", "reason": "unittest-reported-skip"}
                          for _ in range(skipped)],
            }
            if failed:
                code = code or 1
                reason = "unittest-reported-failures"
            if total <= skipped:
                code = code or 1
                reason = "zero-tests-executed"
        if args.junit:
            counts, xml = junit(args.junit)
            receipt["tests"][args.stage] = counts
            (Path(args.receipt).parent / f"{args.stage}.xml").write_text(xml, encoding="utf-8")
            if not counts["executed"] or counts["failed"]:
                code = code or 1
    except (OSError, ValueError, ET.ParseError):
        code = code or 1
        reason = "command-launch-or-test-report-invalid"
    finally:
        if args.junit:
            Path(args.junit).unlink(missing_ok=True)
        info = diagnostics(output)
        info["compilerCodes"] = [value.decode("ascii") for value in info["compilerCodes"]]
        if code:
            info["reason"] = reason
        finish(receipt, args.stage, code, time.monotonic() - began, info)
        write(args.receipt, receipt)
    print(f"{args.stage}: {'success' if code == 0 else 'failure'}; exit={code}; raw output excluded")
    return code if 0 <= code <= 255 else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipt", required=True)
    sub = parser.add_subparsers(dest="action", required=True)
    init = sub.add_parser("init")
    init.add_argument("--kind", choices=("standalone", "downstream"), default="standalone")
    init.add_argument("--backend-sha", required=True)
    init.add_argument("--shell-sha", required=True)
    join = sub.add_parser("merge")
    join.add_argument("--fixture", required=True)
    join.add_argument("--integration", required=True)
    sources = sub.add_parser("sources")
    sources.add_argument("--backend", required=True)
    sources.add_argument("--shell", required=True)
    run = sub.add_parser("run")
    run.add_argument("--stage", required=True)
    run.add_argument("--junit")
    run.add_argument("--unittest", action="store_true")
    run.add_argument("--echo", action="store_true")
    run.add_argument("command", nargs=argparse.REMAINDER)
    inventory = sub.add_parser("inventory")
    inventory.add_argument("--ctest", required=True)
    inventory.add_argument("--build", required=True)
    end = sub.add_parser("finalize")
    end.add_argument("--workflow-outcome", required=True,
                     choices=("success", "failure", "cancelled", "skipped"))
    end.add_argument("--toolchain")
    end.add_argument("--backend", required=True)
    end.add_argument("--shell", required=True)
    expected = sub.add_parser("validate")
    expected.add_argument("--kind", choices=("standalone", "downstream"))
    expected.add_argument("--backend-sha")
    expected.add_argument("--shell-sha")
    expected.add_argument("--contract-sha256")
    expected.add_argument("--spec-sha256")
    args = parser.parse_args()
    try:
        if args.action == "merge":
            require(not Path(args.receipt).exists(), "receipt already exists")
            receipt = merge(read(args.fixture), read(args.integration))
            path = Path(args.receipt)
            path.parent.mkdir(parents=True, exist_ok=True)
            # Exclusive creation also refuses races, aliases and input overwrites.
            with path.open("x", encoding="utf-8") as output:
                output.write(json.dumps(receipt, indent=2) + "\n")
            return 0
        if args.action == "init":
            require(not Path(args.receipt).exists(), "receipt already exists")
            write(args.receipt, new_receipt(args.kind, args.backend_sha, args.shell_sha))
            return 0
        receipt = read(args.receipt)
        if args.action == "run":
            return run_command(args, receipt)
        if args.action == "sources":
            start(receipt, "sources")
            write(args.receipt, receipt)
            began = time.monotonic()
            try:
                resolve_sources(receipt, args.backend, args.shell)
            except (ValueError, KeyError, TypeError, OSError):
                finish(receipt, "sources", 1, time.monotonic() - began,
                       {"reason": "source-or-spec-verification-failed"})
                write(args.receipt, receipt)
                raise ValueError("source-or-spec-verification-failed") from None
            finish(receipt, "sources", 0, time.monotonic() - began)
        elif args.action == "inventory":
            try:
                result = subprocess.run([args.ctest, "--test-dir", args.build, "--show-only=json-v1"],
                                        capture_output=True)
                require(result.returncode == 0, "CTest inventory failed")
                tests = json.loads(result.stdout)["tests"]
                require(isinstance(tests, list) and tests, "CTest inventory is empty")
                for label in ("native", "desktop", "gpu", "machine"):
                    count = sum(label in prop["value"] for test in tests
                                for prop in test["properties"] if prop["name"] == "LABELS")
                    receipt["omittedTiers"][label] = {
                        "count": count, "reason": "outside-portable-gate; trusted-separate-acceptance"}
            except (ValueError, KeyError, TypeError, OSError):
                receipt["omittedTiers"] = {}
                receipt["stages"]["configure"]["diagnostics"]["inventory"] = "unavailable-or-invalid"
                write(args.receipt, receipt)
                raise ValueError("inventory-unavailable-or-invalid") from None
        elif args.action == "finalize":
            # Recheck after execution as well: generated files must stay out of
            # the source roots and a mid-run edit must not become a clean pass.
            try:
                resolve_sources(receipt, args.backend, args.shell)
            except (ValueError, KeyError, TypeError, OSError):
                receipt["stages"]["sources"]["outcome"] = "failure"
                receipt["stages"]["sources"]["exitCode"] = 1
                # Preserve the original stage diagnostics and timing; this is a
                # separate final recheck, not a replacement initial execution.
                receipt["stages"]["sources"]["diagnostics"]["finalVerification"] = "failed"
            receipt["toolchain"]["actual"] = {}
            try:
                actual = read(args.toolchain) if args.toolchain else {}
                require(isinstance(actual, dict), "invalid toolchain object")
                # Allowlist only public version/hash fields, never paths or arbitrary data.
                receipt["toolchain"]["actual"] = {
                    key: actual[key] for key in ("schemaVersion", "contractId", "contractSha256",
                                                 "vsInstallationVersion", *TOOLS) if key in actual
                    and (type(actual[key]) is int or
                         isinstance(actual[key], str) and IDENTIFIER.fullmatch(actual[key]))
                }
            except (ValueError, KeyError, TypeError, OSError):
                receipt["stages"]["prepare"]["diagnostics"]["finalToolchain"] = "unavailable-or-invalid"
            receipt["producer"] = {"pythonVersion": ".".join(map(str, sys.version_info[:3]))}
            passed = finalize(receipt, args.workflow_outcome)
            write(args.receipt, receipt)
            print("candidate result: " + receipt["result"])
            return 0 if passed else 1
        elif args.action == "validate":
            validate(receipt, kind=args.kind, backend_sha=args.backend_sha,
                     shell_sha=args.shell_sha, contract_sha256=args.contract_sha256,
                     spec_sha256=args.spec_sha256)
            print("valid complete pass")
            return 0
        write(args.receipt, receipt)
        return 0
    except (ValueError, KeyError, TypeError, OSError, ET.ParseError):
        # Exception text may contain paths or user content. Never print it.
        print("candidate contract rejected; inspect sanitized receipt stage states", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
