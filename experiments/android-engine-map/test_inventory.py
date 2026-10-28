"""Synthetic validation regressions, not engine or ABI tests."""

import copy
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest

from verify_inventory import verify


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.library, self.dump = root / "synthetic.elf", root / "synthetic.txt"
        self.header = bytearray(64)
        self.header[:7] = b"\x7fELF\x02\x01\x01"
        struct.pack_into("<HH", self.header, 16, 3, 183)
        self.library.write_bytes(self.header)
        self.dump.write_bytes(b"1: 00001000 4 FUNC GLOBAL DEFAULT 15 candidate\n")
        self.contract = {
            "library": {"sha256": hashlib.sha256(self.header).hexdigest()},
            "symbol_dump": {"sha256": hashlib.sha256(self.dump.read_bytes()).hexdigest()},
            "capabilities": [{"state": "candidate_only", "callable": False,
                              "symbols": [{"mangled": "candidate", "dump_line": 1}]}],
        }

    def test_candidate_only_matches(self):
        verify(self.contract, self.library, self.dump)

    def test_changed_binary_rejected(self):
        self.library.write_bytes(self.header + b"changed")
        with self.assertRaisesRegex(ValueError, "library SHA256"):
            verify(self.contract, self.library, self.dump)

    def test_changed_dump_rejected(self):
        self.dump.write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "symbol dump SHA256"):
            verify(self.contract, self.library, self.dump)

    def test_wrong_machine_rejected_even_with_matching_digest(self):
        struct.pack_into("<H", self.header, 18, 62)
        self.library.write_bytes(self.header)
        self.contract["library"]["sha256"] = hashlib.sha256(self.header).hexdigest()
        with self.assertRaisesRegex(ValueError, "AArch64"):
            verify(self.contract, self.library, self.dump)

    def test_callable_claim_rejected(self):
        self.contract["capabilities"][0]["callable"] = True
        with self.assertRaisesRegex(ValueError, "cannot authorize"):
            verify(self.contract, self.library, self.dump)

    def test_wrong_export_rejected(self):
        bad = copy.deepcopy(self.contract)
        bad["capabilities"][0]["symbols"][0]["mangled"] = "other"
        with self.assertRaisesRegex(ValueError, "export mismatch"):
            verify(bad, self.library, self.dump)


if __name__ == "__main__":
    unittest.main()
