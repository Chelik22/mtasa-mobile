"""Exercise actual native file storage across processes; never launch MTA/GTA."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

EXE = Path(sys.argv.pop(1)).resolve()
ENVIRONMENT = "MTA_EXPERIMENTAL_LAB_ROOT"
CHILDREN = ("app", "gta", "data", "common", "documents", "local-appdata", "temp", "settings", "logs")


class LabContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mta-lab-paths-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "lab"
        for child in CHILDREN:
            (self.root / child).mkdir(parents=True)
        self.environment = os.environ.copy()
        self.environment[ENVIRONMENT] = str(self.root)

    def invoke(self, mode, environment=None):
        result = subprocess.run([str(EXE), mode], env=environment or self.environment,
                                capture_output=True, text=True, timeout=30,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)

    def test_absent_and_invalid_roots_do_not_create_settings(self):
        absent = self.environment.copy()
        absent.pop(ENVIRONMENT)
        self.invoke("disabled", absent)
        incomplete = Path(self.temporary.name) / "incomplete"
        incomplete.mkdir()
        for value in ("", "relative-path", str(self.root / "missing"), str(incomplete), str(self.root / ".." / "lab")):
            environment = self.environment.copy()
            environment[ENVIRONMENT] = value
            self.invoke("reject-root", environment)
        self.assertFalse(list(self.root.rglob("registry.*")))
        self.assertFalse(list(incomplete.iterdir()))

    def test_root_is_frozen_and_child_namespace_is_closed(self):
        self.invoke("freeze")
        self.assertFalse(list(self.root.rglob("registry.*")))

    def test_actual_persistence_case_delete_and_bound_preserves_prior_file(self):
        self.invoke("write")
        self.invoke("read")
        path = self.root / "settings/registry.bin"
        before = hashlib.sha256(path.read_bytes()).digest()
        self.invoke("oversize")
        self.assertEqual(hashlib.sha256(path.read_bytes()).digest(), before)
        self.invoke("read")
        self.assertFalse(list((self.root / "settings").glob("*.tmp")))

    def test_corrupt_store_rejected_without_rewrite(self):
        self.invoke("write")
        path = self.root / "settings/registry.bin"
        original = path.read_bytes()
        for payload in (b"", b"bad-magic!!!", original[:15], original + b"trailing", b"MTALAB01" + b"\xff" * 4):
            path.write_bytes(payload)
            self.invoke("corrupt")
            self.assertEqual(path.read_bytes(), payload)

    def test_two_real_writers_do_not_lose_updates(self):
        self.invoke("write")
        processes = []
        try:
            for name in ("writer-a", "writer-b"):
                processes.append(subprocess.Popen([str(EXE), "writer", name], env=self.environment,
                                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                                 creationflags=subprocess.CREATE_NO_WINDOW))
            for process in processes:
                stdout, stderr = process.communicate(timeout=30)
                self.assertEqual(process.returncode, 0, (stdout + stderr).decode(errors="replace"))
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        self.invoke("concurrent-result")
        self.assertFalse(list((self.root / "settings").glob("*.tmp")))


if __name__ == "__main__":
    unittest.main()
