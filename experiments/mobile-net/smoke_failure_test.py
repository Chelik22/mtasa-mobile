"""Regression: a crashing probe must replace stale success with durable failure evidence."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def require(condition, reason):
    if not condition:
        raise RuntimeError(reason)


def main():
    runner = Path(__file__).with_name("smoke.py")
    with tempfile.TemporaryDirectory(prefix="mobile-net-failure-") as directory:
        root = Path(directory)
        evidence = root / "evidence.json"
        evidence.write_text(json.dumps({"passed": True, "stale_marker": "previous-success"}), encoding="utf-8")
        # Python acts as the executable probe and resolves its server argument as this script.
        # This needs no shell, compiler, fake networking implementation or production test hook.
        (root / "server").write_text(
            'import json, pathlib, sys\n'
            'initial = json.loads(pathlib.Path("evidence.json").read_text(encoding="utf-8"))\n'
            'sys.stderr.write(json.dumps({"observed_running": initial.get("state") == "running" '
            'and initial.get("passed") is False, "stale_marker": initial.get("stale_marker")}) + "\\n")\n'
            'sys.stderr.flush()\n'
            'sys.stdout.write("not-json\\n{\\"event\\":")\n'
            'sys.stdout.flush()\n'
            'raise SystemExit(17)\n',
            encoding="utf-8",
        )
        result = subprocess.run(
            [sys.executable, str(runner), "--probe", sys.executable, "--evidence", str(evidence)],
            cwd=root, capture_output=True, text=True, timeout=15,
        )
        require(result.returncode != 0, "crashed probe incorrectly reported success")
        saved = json.loads(evidence.read_text(encoding="utf-8"))
        require(saved["passed"] is False and saved["state"] == "failed", "failed evidence was not persisted")
        require("stale_marker" not in saved and "error" in saved, "stale success survived")
        require(len(saved["processes"]) == 1, "crash should prevent client startup")
        child = saved["processes"][0]
        require(child["exit_code"] is not None and child["exit_code"] != 0, "crashing child exit was lost")
        require(child["raw_stdout"] == 'not-json\n{"event":', "raw partial stdout was lost")
        require(len(child["parse_errors"]) == 2, "malformed lines were not retained as parse errors")
        observed = json.loads(child["stderr"])
        require(observed == {"observed_running": True, "stale_marker": None}, "initial running evidence was missing")

        evidence.write_text(json.dumps({"passed": True, "stale_marker": "previous-success"}), encoding="utf-8")
        missing = subprocess.run(
            [sys.executable, str(runner), "--probe", str(root / "missing-probe"), "--evidence", str(evidence)],
            cwd=root, capture_output=True, text=True, timeout=15,
        )
        saved = json.loads(evidence.read_text(encoding="utf-8"))
        require(missing.returncode != 0 and saved["passed"] is False and saved["state"] == "failed", "startup failure evidence missing")
        require(saved["processes"] == [] and "stale_marker" not in saved, "startup failure retained stale state")
    print(json.dumps({"test": "smoke_failure_evidence", "passed": True, "cases": ["partial_stdout_crash", "missing_probe"]}))


if __name__ == "__main__":
    main()
