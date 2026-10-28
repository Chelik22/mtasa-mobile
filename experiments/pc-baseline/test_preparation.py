"""Only filesystem/provenance failure checks; no MTA/client/compiler execution."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from prepare_dependencies import extract, verify
from build_pc import build, copy_verified_tree, sdk_inputs


class DependencyBoundaryTests(unittest.TestCase):
    def test_changed_cached_archive_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "archive.zip"
            path.write_bytes(b"original bytes")
            item = {"id": "fixture", "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            verify(path, item)
            path.write_bytes(b"changed bytes")
            with self.assertRaises(ValueError):
                verify(path, item)

    def test_zip_traversal_rejected_before_member_write(self):
        for member in ("../outside", "..\\outside", "C:/outside"):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                path = root / "fixture.zip"
                with zipfile.ZipFile(path, "w") as archive:
                    archive.writestr("ordinary.txt", "okay")
                    archive.writestr(member, "bad")
                with self.assertRaises(ValueError):
                    extract(path, root / "extracted", "zip")
                self.assertFalse((root / "outside").exists())
                self.assertFalse((root / "extracted/ordinary.txt").exists())

    def test_composed_dependency_preserved_and_unknown_file_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, target = root / "source", root / "target"
            source.mkdir()
            (source / "real.cpp").write_text("upstream")
            copy_verified_tree(source, target)
            nested = target / "thirdparty/rapidjson/include"
            nested.mkdir(parents=True)
            (nested / "real.h").write_text("separately verified dependency")
            copy_verified_tree(source, target, Path("thirdparty/rapidjson"))
            self.assertEqual((nested / "real.h").read_text(), "separately verified dependency")
            (target / "unowned.txt").write_text("preserve")
            with self.assertRaises(ValueError):
                copy_verified_tree(source, target, Path("thirdparty/rapidjson"))
            self.assertEqual((target / "unowned.txt").read_text(), "preserve")

    def test_blocked_plan_records_failure_without_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            plan = root / "blocked.json"
            plan.write_text(json.dumps({"schema": "mta-pc-baseline-v1", "state": "blocked"}))
            self.assertEqual(build(plan, root / "attempt"), 2)
            receipt = json.loads((root / "attempt/build.run.json").read_text())
            self.assertEqual(receipt["status"], "failed")
            self.assertEqual(receipt["clientBuild"], "not_run")
            self.assertNotIn("msbuildPid", receipt)
            self.assertEqual((root / "attempt/build.exitcode").read_text(), "2")

    def test_added_sdk_header_rejected_by_real_build_preflight(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / "sdk"
            (sdk / "Include").mkdir(parents=True)
            (sdk / "Include/d3dx9.h").write_text("original SDK header")
            frozen = sdk_inputs(sdk)
            (sdk / "Include/dinput.h").write_text("unexpected platform shadow")
            plan = root / "ready.json"
            # Deliberately no source/tool inputs: even a regression cannot launch a compiler.
            plan.write_text(json.dumps({"schema": "mta-pc-baseline-v1", "state": "ready", "dxsdk": str(sdk), "sdkInputHashes": frozen}))
            self.assertEqual(build(plan, root / "attempt"), 2)
            receipt = json.loads((root / "attempt/build.run.json").read_text())
            self.assertEqual(receipt["error"], "Prepared SDK file set or contents changed")
            self.assertEqual(receipt["clientBuild"], "not_run")
            self.assertNotIn("msbuildPid", receipt)


if __name__ == "__main__":
    unittest.main()
