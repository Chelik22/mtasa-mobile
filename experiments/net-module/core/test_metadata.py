"""Pure artifact-metadata tests; no compiler, DLL or runtime is executed."""
import unittest
from build_core import artifact_is_fresh


class ArtifactMetadataTests(unittest.TestCase):
    def test_noop_is_not_new_build(self):
        old = {"mtimeNs": 100, "sha256": "same"}
        self.assertFalse(artifact_is_fresh(old, dict(old), 150))

    def test_same_hash_can_be_freshly_reproduced(self):
        self.assertTrue(artifact_is_fresh({"mtimeNs": 100, "sha256": "same"}, {"mtimeNs": 200, "sha256": "same"}, 150))

    def test_artifact_predating_attempt_is_rejected(self):
        self.assertFalse(artifact_is_fresh(None, {"mtimeNs": 100}, 150))

    def test_missing_output_is_rejected(self):
        self.assertFalse(artifact_is_fresh({"mtimeNs": 100}, None, 150))

    def test_changed_output_still_must_follow_attempt(self):
        self.assertFalse(artifact_is_fresh({"mtimeNs": 50}, {"mtimeNs": 100}, 150))


if __name__ == "__main__":
    unittest.main()
