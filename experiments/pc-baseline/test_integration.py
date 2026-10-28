"""Exercise real integration-build refusal before any process can start."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import integration_build as integration
from build_pc import sdk_inputs


class IntegrationPreflight(unittest.TestCase):
    def rejected(self, change, expected):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sdk = root / "sdk"
            (sdk / "Include").mkdir(parents=True)
            (sdk / "Include/d3dx9.h").write_text("approved header")
            value = {"schema": integration.SCHEMA, "state": "ready_source_only", "dxsdk": str(sdk),
                     "sdkInputHashes": sdk_inputs(sdk)}
            change(value, sdk)
            plan = root / "plan.json"
            plan.write_text(json.dumps(value))
            # Deliberately no source/tool fields: preflight must refuse before
            # touching an actual Git/compiler process, not just before MSBuild.
            with patch.object(integration.subprocess, "Popen") as popen:
                self.assertEqual(integration.build(plan, root / "attempt"), 2)
                popen.assert_not_called()
            actual = json.loads((root / "attempt/build.run.json").read_text())
            self.assertEqual(actual["status"], "failed")
            self.assertEqual(actual["error"], expected)
            self.assertNotIn("msbuildPid", actual)
            self.assertEqual(actual["clientBuild"], "not_run")
            self.assertEqual((root / "attempt/build.exitcode").read_text(), "2")

    def test_added_shadowing_sdk_header(self):
        self.rejected(lambda plan, sdk: (sdk / "Include/dinput.h").write_text("unreviewed shadow"),
                      "Integration SDK file set or bytes changed")

    def test_modified_sdk_header(self):
        self.rejected(lambda plan, sdk: (sdk / "Include/d3dx9.h").write_text("modified content"),
                      "Integration SDK file set or bytes changed")

    def test_unreviewed_feature_does_not_self_approve(self):
        self.rejected(lambda plan, sdk: plan.update(approvedFeatureSha="unreviewed"),
                      "Integration approval or baseline provenance changed")


if __name__ == "__main__":
    unittest.main()
