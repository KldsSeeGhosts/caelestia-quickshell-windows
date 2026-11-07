"""Producer-only fixtures: synthetic receipts and commands, no shell consumer."""

import argparse
import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HELPER = Path(__file__).resolve().parents[1] / "scripts/candidate_contract.py"
spec = importlib.util.spec_from_file_location("candidate_contract", HELPER)
c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(c)


def passing(kind="standalone"):
    receipt = c.new_receipt(kind, "a" * 40, "b" * 40)
    for source in receipt["sources"].values():
        source.update(resolved=source["requested"], dirty=False)
    for name in c.stages(kind):
        c.start(receipt, name)
        c.finish(receipt, name, 0, 0.01)
    expected = {"contractId": "synthetic-v1", **dict.fromkeys(c.TOOLS, "1.0")}
    receipt["toolchain"] = {
        "specSha256": "e" * 64, "contractSha256": "c" * 64, "expected": expected,
        "actual": {**expected, "contractSha256": "c" * 64, "schemaVersion": 1},
    }
    receipt["producer"] = {"pythonVersion": "3.12.10"}
    counts = {"total": 2, "executed": 1, "failed": 0, "skipped": 1,
              "skips": [{"test": "synthetic", "reason": "synthetic-unavailable"}]}
    receipt["tests"] = {name: copy.deepcopy(counts) for name in
                        (("producer", "portable", "integration") if kind == "downstream"
                         else ("producer", "portable"))}
    receipt["omittedTiers"] = {
        name: {"count": 1, "reason": "separate-trusted-acceptance"}
        for name in ("native", "desktop", "gpu", "machine")}
    assert c.finalize(receipt, "success")
    return receipt


class ContractTests(unittest.TestCase):
    def setUp(self):
        # Caller controls TEMP on local runs; no source checkout is created.
        self.temporary = tempfile.TemporaryDirectory(prefix="contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def rejected(self, receipt):
        with self.assertRaises((ValueError, KeyError, TypeError)):
            c.validate(receipt)

    def test_standalone_success(self):
        c.validate(passing())

    def test_downstream_success(self):
        c.validate(passing("downstream"))

    def partials(self):
        fixture = passing("downstream")
        integration = copy.deepcopy(fixture)
        empty = c.new_receipt("downstream", "a" * 40, "b" * 40)
        for receipt, absent in ((fixture, ("integration",)),
                                (integration, ("configure", "build", "portable"))):
            receipt.update(finishedAt=None, workflowOutcome=None, result="incomplete")
            for name in absent:
                receipt["stages"][name] = copy.deepcopy(empty["stages"][name])
        del fixture["tests"]["integration"]
        del integration["tests"]["portable"]
        integration["omittedTiers"] = {}
        return fixture, integration

    def test_merge_then_finalize_and_validate_with_overlapping_stages(self):
        fixture, integration = self.partials()
        integration["createdAt"] = "2000-01-01T00:00:00+00:00"
        integration["stages"]["prepare"]["diagnostics"] = {"outputBytesExcluded": 99}
        original = copy.deepcopy((fixture, integration))
        merged = c.merge(fixture, integration)
        self.assertEqual((fixture, integration), original)
        self.assertEqual(merged["createdAt"], integration["createdAt"])
        self.assertEqual(merged["stages"]["prepare"], fixture["stages"]["prepare"])
        self.assertEqual(merged["omittedTiers"], fixture["omittedTiers"])
        self.assertEqual(merged["tests"]["portable"], fixture["tests"]["portable"])
        self.assertEqual(merged["tests"]["integration"], integration["tests"]["integration"])
        self.assertEqual(merged["result"], "incomplete")
        self.assertTrue(c.finalize(merged, "success"))
        c.validate(merged, kind="downstream")

    def test_merge_refuses_mismatched_identity(self):
        for path, value in (
                (("kind",), "standalone"), (("schemaVersion",), 2),
                (("sources", "backend", "requested"), "d" * 40),
                (("sources", "shell", "resolved"), "d" * 40),
                (("toolchain", "specSha256"), "d" * 64),
                (("toolchain", "contractSha256"), "d" * 64),
                (("toolchain", "expected", "clangVersion"), "drift"),
                (("toolchain", "actual", "clangVersion"), "drift"),
                (("flags", "qpa"), "windows"),
                (("producer", "pythonVersion"), "3.12.11")):
            fixture, integration = self.partials()
            target = integration
            for key in path[:-1]:
                target = target[key]
            target[path[-1]] = value
            with self.subTest(path=path), self.assertRaises(ValueError):
                c.merge(fixture, integration)

    def test_merge_refuses_failed_owned_or_attempted_nonowned_stages(self):
        for index, owned, absent in (
                (0, ("sources", "producer", "prepare", "configure", "build", "portable"),
                 ("integration",)),
                (1, ("sources", "producer", "prepare", "integration"),
                 ("configure", "build", "portable"))):
            for name in owned + absent:
                for field, value in (("outcome", "failure"), ("exitCode", 1),
                                     ("exitCode", True)):
                    pair = self.partials()
                    pair[index]["stages"][name][field] = value
                    with self.subTest(index=index, name=name, field=field), self.assertRaises(ValueError):
                        c.merge(*pair)

    def test_merge_refuses_finalized_inputs(self):
        for index in (0, 1):
            for field, value in (("finishedAt", c.now()), ("result", "non-pass"),
                                 ("workflowOutcome", "success")):
                pair = self.partials()
                pair[index][field] = value
                with self.subTest(index=index, field=field), self.assertRaises(ValueError):
                    c.merge(*pair)

    def test_merge_does_not_rescue_invalid_evidence_at_finalization(self):
        fixture, integration = self.partials()
        fixture["omittedTiers"] = {}
        self.assertFalse(c.finalize(c.merge(fixture, integration), "success"))
        for suite, index in (("portable", 0), ("integration", 1), ("producer", 0)):
            pair = self.partials()
            pair[index]["tests"][suite]["executed"] = 0
            self.assertFalse(c.finalize(c.merge(*pair), "success"))

    def test_merge_cli_fresh_output_and_finalize(self):
        fixture, integration = self.partials()
        actual = copy.deepcopy(fixture["toolchain"]["actual"])
        # These fields are only populated by normal finalization, not run.
        for receipt in (fixture, integration):
            receipt["toolchain"]["actual"] = {}
            receipt["producer"] = {}
        left, right, output = (self.root / name for name in ("fixture.json", "integration.json", "merged.json"))
        c.write(left, fixture)
        c.write(right, integration)
        command = [sys.executable, "-B", str(HELPER), "--receipt", str(output),
                   "merge", "--fixture", str(left), "--integration", str(right)]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
        original = output.read_bytes()
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
        self.assertEqual(output.read_bytes(), original)
        merged = c.read(output)
        toolchain = self.root / "actual.json"
        c.write(toolchain, actual)
        argv = ["candidate-contract", "--receipt", str(output), "finalize",
                "--workflow-outcome", "success", "--toolchain", str(toolchain),
                "--backend", "synthetic-backend", "--shell", "synthetic-shell"]
        with patch.object(sys, "argv", argv), patch.object(c, "resolve_sources"), patch.object(
                c.sys, "version_info", (3, 12, 10)):
            self.assertEqual(c.main(), 0)
        c.validate(c.read(output))

    def test_consumer_expected_pair_kind_and_digests(self):
        c.validate(passing("downstream"), kind="downstream", backend_sha="a" * 40,
                   shell_sha="b" * 40, contract_sha256="c" * 64, spec_sha256="e" * 64)
        for kwargs in ({"kind": "downstream"}, {"backend_sha": "d" * 40},
                       {"shell_sha": "d" * 40}, {"backend_sha": "main"},
                       {"contract_sha256": "d" * 64}, {"spec_sha256": "d" * 64},
                       {"contract_sha256": "invalid"}):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                c.validate(passing(), **kwargs)

    def test_cli_consumer_expectations(self):
        path = self.root / "receipt.json"
        c.write(path, passing("downstream"))
        command = [sys.executable, "-B", str(HELPER), "--receipt", str(path), "validate",
                   "--kind", "downstream", "--backend-sha", "a" * 40,
                   "--shell-sha", "b" * 40, "--contract-sha256", "c" * 64,
                   "--spec-sha256", "e" * 64]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
        command[command.index("downstream")] = "standalone"
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)

    def test_finalize_writes_nonpass_for_invalid_or_missing_toolchain(self):
        for content in ("{invalid", "[]", "null", None):
            with self.subTest(content=content):
                path = self.root / "receipt.json"
                toolchain = self.root / "toolchain.json"
                c.write(path, passing())
                if content is None:
                    toolchain.unlink(missing_ok=True)
                else:
                    toolchain.write_text(content, encoding="utf-8")
                argv = ["candidate-contract", "--receipt", str(path), "finalize",
                        "--workflow-outcome", "success", "--toolchain", str(toolchain),
                        "--backend", "synthetic-backend", "--shell", "synthetic-shell"]
                with patch.object(sys, "argv", argv), patch.object(c, "resolve_sources"):
                    self.assertEqual(c.main(), 1)
                result = c.read(path)
                self.assertEqual(result["workflowOutcome"], "success")
                self.assertEqual(result["result"], "non-pass")
                self.assertIsNotNone(result["finishedAt"])
                self.assertEqual(result["toolchain"]["actual"], {})
                self.assertEqual(result["stages"]["prepare"]["diagnostics"]["finalToolchain"],
                                 "unavailable-or-invalid")

    def test_final_source_type_error_preserves_original_diagnostics_and_writes(self):
        path, toolchain = self.root / "receipt.json", self.root / "toolchain.json"
        receipt = passing()
        receipt["stages"]["sources"]["diagnostics"] = {"reason": "initial-source-result"}
        c.write(path, receipt)
        c.write(toolchain, receipt["toolchain"]["actual"])
        argv = ["candidate-contract", "--receipt", str(path), "finalize",
                "--workflow-outcome", "failure", "--toolchain", str(toolchain),
                "--backend", "synthetic-backend", "--shell", "synthetic-shell"]
        with patch.object(sys, "argv", argv), patch.object(c, "resolve_sources", side_effect=TypeError):
            self.assertEqual(c.main(), 1)
        result = c.read(path)
        self.assertEqual(result["workflowOutcome"], "failure")
        self.assertEqual(result["result"], "non-pass")
        self.assertEqual(result["stages"]["sources"]["diagnostics"],
                         {"reason": "initial-source-result", "finalVerification": "failed"})

    def test_inventory_failure_retains_reason_and_cannot_reuse_old_counts(self):
        path = self.root / "receipt.json"
        c.write(path, passing())
        argv = ["candidate-contract", "--receipt", str(path), "inventory",
                "--ctest", "synthetic-ctest", "--build", "synthetic-build"]
        with patch.object(sys, "argv", argv), patch.object(
                c.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, b"private")):
            self.assertEqual(c.main(), 1)
        result = c.read(path)
        self.assertEqual(result["omittedTiers"], {})
        self.assertEqual(result["stages"]["configure"]["diagnostics"]["inventory"],
                         "unavailable-or-invalid")
        self.assertFalse(c.finalize(result, "success"))

    def test_initial_and_running_receipts_are_not_passes(self):
        receipt = c.new_receipt("standalone", "a" * 40, "b" * 40)
        self.assertFalse(c.finalize(receipt, "success"))
        c.start(receipt, "prepare")
        self.rejected(receipt)

    def test_requested_shas_must_be_immutable(self):
        for value in ("main", "abc123", "A" * 40, "a" * 39, "", None, "a" * 41):
            with self.subTest(value=value), self.assertRaises(ValueError):
                c.new_receipt("standalone", value, "b" * 40)

    def test_invalid_versions_and_kinds(self):
        for key, value in (("schemaVersion", 2), ("schemaVersion", True),
                           ("kind", "desktop"), ("evidenceClass", "hardware")):
            receipt = passing()
            receipt[key] = value
            self.rejected(receipt)

    def test_malformed_and_unknown_fields_rejected(self):
        for key, value in (("createdAt", "invalid"), ("toolchain", [])):
            receipt = passing()
            receipt[key] = value
            self.rejected(receipt)
        receipt = passing()
        receipt["unfilteredOutput"] = "synthetic-private-data"
        self.rejected(receipt)
        receipt = passing()
        receipt["toolchain"]["actual"] = []
        self.rejected(receipt)

    def test_repository_identities_are_fixed(self):
        for name in c.REPOS:
            receipt = passing()
            receipt["sources"][name]["repository"] = "attacker/repository"
            self.rejected(receipt)

    def test_requested_and_resolved_are_separate_and_equal(self):
        for name in c.REPOS:
            for key, value in (("requested", "main"), ("resolved", None),
                               ("resolved", "d" * 40)):
                receipt = passing()
                receipt["sources"][name][key] = value
                self.rejected(receipt)

    def test_dirty_unknown_and_overridden_sources_are_non_pass(self):
        for name in c.REPOS:
            for key, value in (("dirty", True), ("dirty", None), ("dirty", 0),
                               ("override", True), ("override", None)):
                receipt = passing()
                receipt["sources"][name][key] = value
                self.rejected(receipt)

    def test_missing_source_is_rejected(self):
        receipt = passing()
        del receipt["sources"]["shell"]
        self.rejected(receipt)

    def test_every_required_stage_must_succeed(self):
        for name in c.stages("downstream"):
            for outcome in ("failure", "cancelled", "not_run", "running", "skipped"):
                receipt = passing("downstream")
                receipt["stages"][name]["outcome"] = outcome
                self.assertFalse(c.finalize(receipt, "success"))

    def test_missing_or_extra_stage_rejected(self):
        for name in c.STAGES:
            receipt = passing()
            del receipt["stages"][name]
            self.rejected(receipt)
        receipt = passing()
        receipt["stages"]["invented"] = receipt["stages"]["build"]
        self.rejected(receipt)

    def test_nonzero_or_missing_exit_codes_rejected(self):
        for value in (None, 1, -1, True, "0"):
            receipt = passing()
            receipt["stages"]["build"]["exitCode"] = value
            self.rejected(receipt)

    def test_failed_cancelled_skipped_workflow_is_not_pass(self):
        for outcome in ("failure", "cancelled", "skipped", None):
            receipt = passing()
            self.assertFalse(c.finalize(receipt, outcome))
            self.assertNotEqual(receipt["result"], "pass")

    def test_incomplete_and_bad_timing_rejected(self):
        for key, value in (("durationSeconds", None), ("durationSeconds", -1),
                           ("durationSeconds", float("nan")),
                           ("durationSeconds", float("inf")),
                           ("startedAt", "not-a-timestamp"),
                           ("finishedAt", "2000-01-01T00:00:00+00:00"),
                           ("startedAt", "2026-01-01T00:00:00")):
            receipt = passing()
            receipt["stages"]["portable"][key] = value
            self.rejected(receipt)

    def test_zero_all_skipped_and_failed_tests_rejected(self):
        for suite in ("producer", "portable"):
            for counts in (
                {"total": 0, "executed": 0, "failed": 0, "skipped": 0, "skips": []},
                {"total": 1, "executed": 0, "failed": 0, "skipped": 1,
                 "skips": [{"test": "synthetic", "reason": "skip"}]},
                {"total": 1, "executed": 1, "failed": 1, "skipped": 0, "skips": []},
            ):
                receipt = passing()
                receipt["tests"][suite] = counts
                self.rejected(receipt)

    def test_missing_tests_and_counts_rejected(self):
        for suite in ("producer", "portable", "integration"):
            receipt = passing("downstream")
            del receipt["tests"][suite]
            self.rejected(receipt)
        for key, value in (("total", 5), ("executed", True), ("failed", -1),
                           ("skipped", "1"), ("skips", [])):
            receipt = passing()
            receipt["tests"]["portable"][key] = value
            self.rejected(receipt)

    def test_skip_reasons_required(self):
        receipt = passing()
        receipt["tests"]["portable"]["skips"][0]["reason"] = ""
        self.rejected(receipt)
        receipt = passing()
        receipt["omittedTiers"]["gpu"]["reason"] = ""
        self.rejected(receipt)

    def test_toolchain_digest_mismatch_rejected(self):
        for value in (None, "main", "d" * 64):
            receipt = passing()
            receipt["toolchain"]["actual"]["contractSha256"] = value
            self.rejected(receipt)

    def test_each_actual_tool_identity_is_required_and_exact(self):
        for key in ("contractId", *c.TOOLS):
            for missing in (True, False):
                receipt = passing()
                if missing:
                    del receipt["toolchain"]["actual"][key]
                else:
                    receipt["toolchain"]["actual"][key] = "drift"
                self.rejected(receipt)

    def test_missing_expected_tool_identity_rejected(self):
        receipt = passing()
        del receipt["toolchain"]["expected"]["clangVersion"]
        self.rejected(receipt)

    def test_wrong_python_or_flags_rejected(self):
        receipt = passing()
        receipt["producer"]["pythonVersion"] = "3.13.0"
        self.rejected(receipt)
        receipt = passing()
        receipt["flags"]["qpa"] = "windows"
        self.rejected(receipt)

    def test_repeated_stage_rejected(self):
        with self.assertRaises(ValueError):
            c.start(passing(), "build")

    def test_junit_retains_counts_and_excludes_private_content(self):
        path = self.root / "raw.xml"
        path.write_text(
            '<testsuite tests="3"><testcase name="fixture"><system-out>'
            'SKIP   : Synthetic::optional() IIW_QSB is not configured\n'
            'private-title private-clipboard C:\\Users\\synthetic\\private'
            '</system-out></testcase><testcase name="failed">'
            '<failure message="private-network">private-title</failure>'
            '</testcase><testcase name="skipped"><skipped message="private"/>'
            '</testcase></testsuite>', encoding="utf-8")
        counts, clean = c.junit(path)
        self.assertEqual((counts["total"], counts["executed"], counts["failed"],
                          counts["skipped"]), (3, 2, 1, 1))
        self.assertEqual(counts["skips"][0]["reason"], "optional-qsb-not-configured")
        self.assertNotIn("private", clean)
        self.assertNotIn("Users", json.dumps(counts))

    def test_empty_malformed_and_contradictory_junit_rejected(self):
        for xml in ("<testsuite/>", "<bad/>",
                    "<testsuite tests='2'><testcase name='one'/></testsuite>",
                    "<testsuite failures='1'><testcase name='one'/></testsuite>",
                    "<testsuite><testcase name='t'>"
                    "<skipped/><failure/></testcase></testsuite>"):
            path = self.root / "raw.xml"
            path.write_text(xml, encoding="utf-8")
            with self.assertRaises(ValueError):
                c.junit(path)

    def test_diagnostics_only_retain_allowlisted_codes(self):
        info = c.diagnostics(
            b"private-title C:\\Users\\synthetic error C1234 LNK4321 error: undeclared identifier")
        self.assertEqual(info["compilerCodes"], [b"C1234", b"LNK4321"])
        self.assertEqual(info["categories"], ["compile-error", "undeclared-identifier"])
        self.assertNotIn("private", str(info))

    def test_schema_top_level_and_stage_shape_match_producer(self):
        schema = c.read(Path(__file__).with_name("v1.schema.json"))
        receipt = passing("downstream")
        self.assertEqual(set(schema["required"]), set(receipt))
        self.assertEqual(set(schema["properties"]["stages"]["properties"]),
                         set(receipt["stages"]))
        self.assertEqual(set(schema["$defs"]["stage"]["required"]),
                         set(receipt["stages"]["build"]))

    def run_fixture(self, code, junit=None, unittest_mode=False, echo=False):
        receipt = c.new_receipt("standalone", "a" * 40, "b" * 40)
        args = argparse.Namespace(
            receipt=str(self.root / "result.json"), stage="portable", junit=junit,
            unittest=unittest_mode, echo=echo, command=[sys.executable, "-B", "-c", code])
        result = c.run_command(args, receipt)
        return result, c.read(args.receipt)

    def test_raw_output_echo_is_opt_in_and_never_enters_receipt(self):
        for echo in (False, True):
            output = io.TextIOWrapper(io.BytesIO(), encoding="cp1252", errors="strict")
            with contextlib.redirect_stdout(output):
                code, receipt = self.run_fixture("print('synthetic-private-output')", echo=echo)
            self.assertEqual(code, 0)
            self.assertEqual(b"synthetic-private-output" in output.buffer.getvalue(), echo)
            self.assertNotIn("synthetic-private-output", json.dumps(receipt))
            if not echo:
                self.assertEqual(output.buffer.getvalue(), b"")

    def test_echo_streams_before_child_exit_and_preserves_diagnostics(self):
        marker = self.root / "echo-observed"

        class ObservedOutput(io.BytesIO):
            def write(stream, data):
                result = super().write(data)
                if b"first-line" in data:
                    marker.touch()
                return result

        output = io.TextIOWrapper(ObservedOutput(), encoding="utf-8")
        # The child cannot emit its second line or succeed until the parent
        # echoes its first. Buffered-at-exit implementations fail after 5s.
        code = (
            "import pathlib,time,sys;print('first-line',flush=True);"
            f"marker=pathlib.Path({str(marker)!r});"
            "deadline=time.monotonic()+5\n"
            "while not marker.exists() and time.monotonic()<deadline: time.sleep(.01)\n"
            "sys.stdout.buffer.write(b'error: C1234\\xff');sys.stdout.flush();"
            "sys.exit(0 if marker.exists() else 8)"
        )
        with contextlib.redirect_stdout(output):
            result, receipt = self.run_fixture(code, echo=True)
        self.assertEqual(result, 0)
        info = receipt["stages"]["portable"]["diagnostics"]
        self.assertEqual(info["outputBytesExcluded"],
                         len(b"first-line\nerror: C1234\xff") +
                         (1 if sys.platform == "win32" else 0))
        self.assertEqual(info["compilerCodes"], ["C1234"])
        self.assertIn("compile-error", info["categories"])

    def test_echo_and_silent_capture_retain_identical_diagnostics_on_failure(self):
        results = []
        for echo in (False, True):
            output = io.TextIOWrapper(io.BytesIO(), encoding="utf-8")
            with contextlib.redirect_stdout(output):
                result, receipt = self.run_fixture(
                    "import sys;sys.stdout.buffer.write(b'error: C1234\\xff');sys.exit(7)",
                    echo=echo)
            self.assertEqual(result, 7)
            results.append(receipt["stages"]["portable"]["diagnostics"])
        self.assertEqual(*results)

    def test_echo_preserves_non_utf8_output_on_legacy_stdout(self):
        output = io.TextIOWrapper(io.BytesIO(), encoding="cp1252", errors="strict")
        with contextlib.redirect_stdout(output):
            code, receipt = self.run_fixture(
                "import sys;sys.stdout.buffer.write(bytes([255, 129, 10]))", echo=True)
        self.assertEqual(code, 0)
        self.assertEqual(output.buffer.getvalue(), bytes([255, 129, 10]))
        self.assertEqual(receipt["stages"]["portable"]["exitCode"], 0)

    def test_actual_process_failure_preserves_exit_code(self):
        code, receipt = self.run_fixture("print('private-title');raise SystemExit(7)")
        self.assertEqual(code, 7)
        self.assertEqual(receipt["stages"]["portable"]["exitCode"], 7)
        self.assertNotIn("private-title", json.dumps(receipt))
        self.assertFalse(c.finalize(receipt, "success"))

    def test_command_launch_failure_remains_durable_with_or_without_echo(self):
        for echo in (False, True):
            receipt = c.new_receipt("standalone", "a" * 40, "b" * 40)
            args = argparse.Namespace(
                receipt=str(self.root / "launch.json"), stage="build", junit=None,
                unittest=False, echo=echo, command=[str(self.root / "absent-command")])
            output = io.TextIOWrapper(io.BytesIO(), encoding="utf-8")
            with contextlib.redirect_stdout(output):
                self.assertEqual(c.run_command(args, receipt), 1)
            stage = c.read(args.receipt)["stages"]["build"]
            self.assertEqual(stage["outcome"], "failure")
            self.assertEqual(stage["diagnostics"]["reason"], "command-launch-or-test-report-invalid")
            if not echo:
                self.assertEqual(output.buffer.getvalue(), b"")

    def test_missing_junit_is_failure_even_after_exit_zero(self):
        code, receipt = self.run_fixture("pass", str(self.root / "absent.xml"))
        self.assertEqual(code, 1)
        self.assertEqual(receipt["stages"]["portable"]["outcome"], "failure")

    def test_stale_junit_cannot_rescue_failed_execution(self):
        path = self.root / "stale.xml"
        path.write_text('<testsuite><testcase name="old"/></testsuite>', encoding="utf-8")
        code, receipt = self.run_fixture("raise SystemExit(5)", str(path))
        self.assertEqual(code, 5)
        self.assertNotIn("portable", receipt["tests"])
        self.assertFalse(path.exists())

    def test_zero_unittest_execution_is_failure(self):
        code, receipt = self.run_fixture(
            "print('Ran 0 tests in 0.000s')", unittest_mode=True)
        self.assertEqual(code, 1)
        self.assertEqual(receipt["tests"]["portable"]["executed"], 0)

    def test_unittest_preserves_failure_counts(self):
        code, receipt = self.run_fixture(
            "print('Ran 4 tests in 0.001s');"
            "print('FAILED (failures=2, errors=1, skipped=1)');raise SystemExit(1)",
            unittest_mode=True)
        self.assertEqual(code, 1)
        self.assertEqual(receipt["tests"]["portable"]["failed"], 3)
        self.assertEqual(receipt["tests"]["portable"]["executed"], 3)

    def test_source_inspector_records_dirty_and_resolved_not_requested(self):
        root = self.root.resolve()
        with patch.object(c, "git", side_effect=(
                str(root), "https://github.com/" + c.REPOS["backend"] + ".git",
                "d" * 40, " M synthetic.txt")):
            source = c.inspect_source(root, c.REPOS["backend"], "a" * 40)
        self.assertTrue(source["dirty"])
        self.assertEqual(source["requested"], "a" * 40)
        self.assertEqual(source["resolved"], "d" * 40)

    def test_spec_and_combined_hashes_match_canonical_lf_contract(self):
        shell = self.root / "shell"
        locks = shell / "windows/toolchain"
        locks.mkdir(parents=True)
        spec_text = json.dumps({"id": "synthetic-v1", **dict.fromkeys(c.TOOLS, "1.0")}) + "\n"
        (shell / "windows/toolchain.json").write_bytes(spec_text.replace("\n", "\r\n").encode())
        for name in ("python-bootstrap.lock.txt", "python-tools.lock.txt"):
            (locks / name).write_bytes(b"synthetic-lock\r\n")
        receipt = c.new_receipt("standalone", "a" * 40, "b" * 40)
        identities = copy.deepcopy(passing()["sources"])
        with patch.object(c, "inspect_source", side_effect=identities.values()):
            c.resolve_sources(receipt, self.root / "backend", shell)
        self.assertEqual(receipt["toolchain"]["specSha256"],
                         hashlib.sha256(spec_text.encode()).hexdigest())
        combined = "\n".join((spec_text, "synthetic-lock\n", "synthetic-lock\n"))
        self.assertEqual(receipt["toolchain"]["contractSha256"],
                         hashlib.sha256(combined.encode()).hexdigest())
        self.assertNotEqual(receipt["toolchain"]["specSha256"],
                            receipt["toolchain"]["contractSha256"])

    def test_wrong_origin_and_parent_source_root_rejected(self):
        for responses in (
            (str(self.root), "https://github.com/attacker/fork.git"),
            (str(self.root.parent),),
        ):
            with patch.object(c, "git", side_effect=responses), self.assertRaises(ValueError):
                c.inspect_source(self.root, c.REPOS["backend"], "a" * 40)

    def test_cli_initial_receipt_is_durable_and_nonpass(self):
        path = self.root / "receipt.json"
        result = subprocess.run(
            [sys.executable, "-B", str(HELPER), "--receipt", str(path), "init",
             "--backend-sha", "a" * 40, "--shell-sha", "b" * 40], capture_output=True)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(c.read(path)["result"], "incomplete")
        result = subprocess.run(
            [sys.executable, "-B", str(HELPER), "--receipt", str(path), "validate"],
            capture_output=True)
        self.assertEqual(result.returncode, 1)
        self.assertNotIn(str(self.root).encode(), result.stderr)


if __name__ == "__main__":
    unittest.main()
