"""CI-11: source-only CI reporting regressions. Synthetic gh responses; no network or game data."""
import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import progress_dashboard as pd


def workflow(wid=1, name="CI", state="active"):
    return {"id": wid, "name": name, "path": f".github/workflows/{wid}.yml", "state": state}


def run(conclusion="success", status="completed", wid=1, rid=100, date="2026-11-24T10:00:00Z"):
    return {"workflowDatabaseId": wid, "workflowName": "CI", "databaseId": rid, "attempt": 1,
            "conclusion": conclusion, "status": status, "headSha": "a" * 40, "event": "push",
            "createdAt": date, "url": f"https://github.com/o/r/actions/runs/{rid}"}


class CIReporting(unittest.TestCase):
    def collect(self, runs, workflows=None, branch=None):
        calls = []

        def fake_gh(args, timeout=60):
            calls.append(args)
            if args[:2] == ["run", "list"]:
                return runs
            if args[:2] == ["workflow", "list"]:
                return workflows
            return []

        col = pd.Collector()
        with mock.patch.object(pd, "gh_json", side_effect=fake_gh), \
                mock.patch.object(pd.shutil, "which", return_value="gh"):
            kwargs = {} if branch is None else {"ci_branch": branch}
            pd.collect_github(col, "o/r", None, **kwargs)
        return col, [c for c in col.components if c["panel"] == "ci"], calls

    def test_completed_results_are_distinct_and_actionable(self):
        cases = [("success", "verified", "passed"), ("failure", "failed", "failed"),
                 ("timed_out", "failed", "failed"), ("startup_failure", "failed", "failed"),
                 ("skipped", "unknown", "skipped"), ("cancelled", "unknown", "cancelled"),
                 ("neutral", "unknown", "neutral"), ("stale", "unknown", "unknown"),
                 ("", "unknown", "unknown"), ("future_result", "unknown", "unknown"),
                 ("action_required", "blocked", "authorization_pending")]
        for conclusion, status, ci_state in cases:
            with self.subTest(conclusion=conclusion):
                _, rows, _ = self.collect([run(conclusion)], [workflow()])
                self.assertEqual(len(rows), 1)
                self.assertEqual((rows[0]["status"], rows[0]["ci_state"]), (status, ci_state))
                self.assertTrue(rows[0]["action"])
                self.assertIn(rows[0]["action"], rows[0]["detail"])
                self.assertEqual(rows[0]["head_sha"], "a" * 40)
                self.assertEqual(rows[0]["url"], "https://github.com/o/r/actions/runs/100")

    def test_pending_states_do_not_reuse_a_completed_conclusion(self):
        for status in ("queued", "requested", "waiting", "pending", "in_progress"):
            with self.subTest(status=status):
                _, rows, _ = self.collect([run("success", status)], [workflow()])
                self.assertEqual((rows[0]["status"], rows[0]["ci_state"]), ("unknown", "pending"))
                self.assertIn(status, rows[0]["detail"])

    def test_authorization_status_is_blocked_without_counting_a_failure(self):
        _, rows, _ = self.collect([run("", "action_required")], [workflow()])
        self.assertEqual((rows[0]["status"], rows[0]["ci_state"]), ("blocked", "authorization_pending"))
        self.assertIn("review", rows[0]["action"].lower())

    def test_newest_run_replaces_old_pass_even_if_api_order_changes(self):
        old = run(rid=99, date="2026-11-23T10:00:00Z")
        new = run("action_required", rid=100)
        for runs in ([old, new], [new, old]):
            with self.subTest(runs=runs):
                _, rows, _ = self.collect(runs, [workflow()])
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0]["ci_state"], "authorization_pending")
                self.assertTrue(rows[0]["url"].endswith("/100"))

    def test_same_time_runs_have_a_deterministic_id_tie_break(self):
        _, rows, _ = self.collect([run(rid=99), run("cancelled", rid=100)], [workflow()])
        self.assertEqual(rows[0]["ci_state"], "cancelled")

    def test_workflow_rename_does_not_duplicate_the_run(self):
        _, rows, _ = self.collect([run()], [workflow(name="Renamed CI")])
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["name"], "Renamed CI")
        self.assertEqual(rows[0]["workflow_id"], 1)

    def test_unrecognized_status_does_not_reuse_success(self):
        _, rows, _ = self.collect([run("success", "future_status")], [workflow()])
        self.assertEqual((rows[0]["status"], rows[0]["ci_state"]), ("unknown", "unknown"))

    def test_active_workflow_without_run_has_unknown_trigger_reason(self):
        _, rows, _ = self.collect([], [workflow()])
        self.assertEqual((rows[0]["status"], rows[0]["ci_state"]), ("unknown", "not_observed"))
        self.assertIn("trigger", rows[0]["action"].lower())
        self.assertNotIn("skipped", rows[0]["ci_state"])

    def test_disabled_workflow_is_explicitly_not_configured(self):
        for state in ("disabled_manually", "disabled_inactivity", "disabled_fork", "deleted"):
            with self.subTest(state=state):
                _, rows, _ = self.collect([], [workflow(state=state)])
                self.assertEqual((rows[0]["status"], rows[0]["ci_state"]), ("missing", "not_configured"))
                self.assertIn(state, rows[0]["detail"])

    def test_disabled_configuration_does_not_hide_a_recorded_failure(self):
        _, rows, _ = self.collect([run("failure")], [workflow(state="disabled_manually")])
        self.assertEqual(rows[0]["ci_state"], "failed")
        self.assertEqual(rows[0]["configuration_state"], "disabled_manually")
        self.assertIn("disabled_manually", rows[0]["detail"])

    def test_empty_inventory_is_not_a_green_baseline(self):
        _, rows, _ = self.collect([], [])
        self.assertEqual((rows[0]["status"], rows[0]["ci_state"]), ("missing", "not_configured"))

    def test_unknown_configuration_state_is_never_assumed_disabled(self):
        _, rows, _ = self.collect([], [workflow(state="future_state")])
        self.assertEqual((rows[0]["status"], rows[0]["ci_state"]), ("unknown", "unknown"))

    def test_inventory_error_preserves_known_run_and_records_source_error(self):
        col, rows, _ = self.collect([run("failure")], None)
        self.assertEqual(rows[0]["ci_state"], "failed")
        self.assertEqual(next(s for s in col.sources if s["id"] == "github_ci_workflows")["status"], "invalid")

    def test_run_query_error_does_not_claim_skipped_or_missing_configuration(self):
        col, rows, _ = self.collect(None, [workflow()])
        self.assertEqual(rows[0]["ci_state"], "source_unavailable")
        self.assertEqual(next(s for s in col.sources if s["id"] == "github_ci")["status"], "invalid")

    def test_both_queries_failing_still_explain_the_missing_evidence(self):
        _, rows, _ = self.collect(None, None)
        self.assertEqual(rows[0]["ci_state"], "source_unavailable")

    def test_names_are_not_workflow_identity(self):
        _, rows, _ = self.collect([run(wid=1), run("failure", wid=2)], [workflow(1), workflow(2)])
        self.assertEqual(len({r["id"] for r in rows}), 2)
        self.assertEqual({r["ci_state"] for r in rows}, {"passed", "failed"})

    def test_selected_branch_and_limits_are_explicit(self):
        col, _, calls = self.collect([run()], [workflow()], "integration/ci-recovery")
        query = next(c for c in calls if c[:2] == ["run", "list"])
        self.assertEqual(query[query.index("--branch") + 1], "integration/ci-recovery")
        self.assertIn("--all", query)
        inventory = next(c for c in calls if c[:2] == ["workflow", "list"])
        self.assertIn("--all", inventory)
        self.assertEqual(col.panels["ci"]["title"], "CI on integration/ci-recovery")
        source = next(s for s in col.sources if s["id"] == "github_ci")
        self.assertIn("40", source["note"])
        # CI-10 and CI-15 own test inventories/runners. This path only queries metadata.
        self.assertTrue(all(c[:2] in (["pr", "list"], ["run", "list"], ["workflow", "list"], ["issue", "list"])
                            for c in calls))

    def test_missing_gh_explains_how_to_get_evidence(self):
        col = pd.Collector()
        with mock.patch.object(pd.shutil, "which", return_value=None):
            pd.collect_github(col, "o/r", None)
        self.assertIn("install", next(s for s in col.sources if s["id"] == "github")["note"].lower())

    def test_cli_branch_requires_github(self):
        with tempfile.TemporaryDirectory() as out:
            result = subprocess.run([sys.executable, "-I", str(ROOT / "tools/progress_dashboard.py"),
                                     "--out", out, "--ci-branch", "integration/ci-recovery"], capture_output=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"--ci-branch requires --github", result.stderr)

    def test_report_generation_preserves_its_exit_code_and_failure_evidence(self):
        def fake_gh(args, timeout=60):
            if args[:2] == ["run", "list"]:
                return [run("failure")]
            if args[:2] == ["workflow", "list"]:
                return [workflow()]
            return []

        with tempfile.TemporaryDirectory() as out, contextlib.ExitStack() as stack:
            for name in ("collect_unit", "collect_ctest", "collect_gpu", "collect_contract", "collect_imports", "collect_corpus"):
                stack.enter_context(mock.patch.object(pd, name))
            stack.enter_context(mock.patch.object(pd, "git_info", return_value={"available": False}))
            stack.enter_context(mock.patch.object(pd, "gh_json", side_effect=fake_gh))
            stack.enter_context(mock.patch.object(pd.shutil, "which", return_value="gh"))
            with contextlib.redirect_stdout(io.StringIO()):
                result = pd.main(["--out", out, "--github", "--ci-branch", "integration/ci-recovery"])
            self.assertEqual(result, 0)  # The reporting tool is not a CI gate.
            snapshot = json.loads(next(Path(out, "snapshots").glob("*.json")).read_text(encoding="utf-8"))
            row = next(c for c in snapshot["components"] if c["panel"] == "ci")
            self.assertEqual((row["status"], row["ci_state"]), ("failed", "failed"))
            self.assertIn(row["action"], Path(out, "progress.html").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
