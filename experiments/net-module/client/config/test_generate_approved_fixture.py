"""Filesystem/CLI tests only: no native compile, network, SDK or game execution."""

import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("generate_approved_fixture.py")
SPEC = importlib.util.spec_from_file_location("approved_generator_test_target", SCRIPT)
GENERATOR = importlib.util.module_from_spec(SPEC)
_previous = sys.dont_write_bytecode
try:
    sys.dont_write_bytecode = True
    SPEC.loader.exec_module(GENERATOR)
finally:
    sys.dont_write_bytecode = _previous


def manifest():
    return {"schema": GENERATOR.SCHEMA, "runtime": "not_run", "entries": [{
        "destination_relative": "fixture/asset.bin", "url": "http://127.0.0.1:23805/fixture/asset.bin",
        "raw_sha256": "a" * 64, "max_bytes": 17, "format": "binary_asset"}]}


def encoded(data):
    return json.dumps(data, ensure_ascii=True).encode("utf-8")


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.base = SCRIPT.parent / "build/tests"
        self.base.mkdir(parents=True, exist_ok=True)
        self.assertTrue(self.base.resolve().is_relative_to(SCRIPT.parent.resolve()))
        self.root = Path(tempfile.mkdtemp(dir=self.base))
        self.addCleanup(self.clean_fixture)

    def clean_fixture(self):
        if not self.root.resolve().is_relative_to(self.base.resolve()):
            raise AssertionError("Refusing recursive cleanup outside test fixture root")
        shutil.rmtree(self.root)

    def test_actual_producer_schema_is_accepted(self):
        resource = SCRIPT.resolve().parents[4] / "experiments/mobile-world-fixture"
        data = GENERATOR._producer.generate(resource, "http://127.0.0.1:23805", "mobile-world-fixture", "mobile-world-fixture")
        result = GENERATOR.render(GENERATOR.parse_manifest(encoded(data)))
        self.assertIn(b"experimental_client::approved_fixture", result)
        self.assertIn(b"experimental_http::FixtureFormat::BinaryAsset", result)

    def test_utf8_and_quote_bytes_are_escaped_not_cpp(self):
        data = manifest()
        data["entries"][0]["destination_relative"] = "fixture/машина'7.bin"
        data["entries"][0]["url"] = "http://localhost:23805/fixture/%22%29%3Bquoted%27.bin"
        result = GENERATOR.render(GENERATOR.parse_manifest(encoded(data))).decode("ascii")
        literals = re.findall(r'"((?:\\[0-7]{3})+)"', result)
        decoded = [bytes(int(value[index + 1:index + 4], 8) for index in range(0, len(value), 4)).decode("utf-8") for value in literals]
        self.assertEqual(decoded, [data["entries"][0]["destination_relative"], data["entries"][0]["url"], "a" * 64])
        self.assertNotIn("quoted", result)
        self.assertNotIn("машина", result)

    def test_both_formats_and_zero_size_boundaries(self):
        data = manifest()
        data["entries"][0]["max_bytes"] = 0
        data["entries"].append({**data["entries"][0], "destination_relative": "fixture/client.lua", "format": "lua_source_utf8", "max_bytes": 1024 * 1024})
        result = GENERATOR.render(GENERATOR.parse_manifest(encoded(data)))
        self.assertIn(b"FixtureFormat::LuaSourceUtf8", result)
        self.assertIn(b"std::size_t{0}", result)
        self.assertIn(b"std::size_t{1048576}", result)

    def test_semantic_order_and_hash_case_are_deterministic(self):
        left = manifest()
        left["entries"].append({**left["entries"][0], "destination_relative": "fixture/z.bin"})
        right = copy.deepcopy(left)
        right["entries"].reverse()
        right["entries"][0]["raw_sha256"] = "A" * 64
        self.assertEqual(GENERATOR.render(GENERATOR.parse_manifest(encoded(left))),
                         GENERATOR.render(GENERATOR.parse_manifest(json.dumps(right, sort_keys=True, indent=2).encode())))

    def test_exact_input_and_entry_count_upper_bounds(self):
        data = manifest()
        data["entries"] = [{**data["entries"][0], "destination_relative": f"fixture/{index}.bin"} for index in range(128)]
        payload = encoded(data)
        payload += b" " * (GENERATOR.MAX_INPUT_BYTES - len(payload))
        self.assertEqual(len(GENERATOR.parse_manifest(payload)), 128)
        with self.assertRaisesRegex(ValueError, "1MiB"):
            GENERATOR.parse_manifest(payload + b" ")

    def test_duplicate_json_keys_and_nonfinite_numbers_rejected(self):
        payload = encoded(manifest())
        for bad in (payload.replace(b'"max_bytes": 17', b'"max_bytes": 17, "max_bytes": 18'),
                    payload.replace(b'"max_bytes": 17', b'"max_bytes": NaN'),
                    payload.replace(b'"max_bytes": 17', b'"max_bytes": Infinity')):
            with self.subTest(payload=bad), self.assertRaises(ValueError):
                GENERATOR.parse_manifest(bad)

    def test_invalid_utf8_and_unicode_surrogate_rejected(self):
        for bad in (b"\xff", encoded(manifest()).replace(b"fixture/asset.bin", b"fixture/\\ud800.bin", 1)):
            with self.assertRaises(ValueError):
                GENERATOR.parse_manifest(bad)

    def test_duplicate_windows_case_paths_rejected(self):
        for alternate in ("FIXTURE/ASSET.BIN", "fixture/asset.bin"):
            data = manifest(); data["entries"].append({**data["entries"][0], "destination_relative": alternate})
            with self.assertRaisesRegex(ValueError, "duplicate"):
                GENERATOR.parse_manifest(encoded(data))

    def test_cli_deterministic_atomic_output_and_invalid_preserves_old_header(self):
        source, output = self.root / "manifest.json", self.root / "generated.h"
        source.write_bytes(encoded(manifest()))
        command = [sys.executable, str(SCRIPT), "--manifest", str(source), "--output", str(output)]
        first = subprocess.run(command, capture_output=True, timeout=30)
        self.assertEqual(first.returncode, 0, first.stderr)
        before = output.read_bytes()
        second = subprocess.run(command, capture_output=True, timeout=30)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(hashlib.sha256(output.read_bytes()).digest(), hashlib.sha256(before).digest())
        source.write_bytes(b" " * (GENERATOR.MAX_INPUT_BYTES + 1))
        failed = subprocess.run(command, capture_output=True, timeout=30)
        self.assertNotEqual(failed.returncode, 0)
        self.assertEqual(output.read_bytes(), before)


NEGATIVE_ENTRIES = {
    "quote_path": ("destination_relative", 'fixture/a"); throw 1; //.bin'),
    "traversal": ("destination_relative", "fixture/../outside"),
    "absolute": ("destination_relative", "/outside"),
    "drive": ("destination_relative", "C:/outside"),
    "backslash": ("destination_relative", "fixture\\outside"),
    "empty_segment": ("destination_relative", "fixture//outside"),
    "trailing_dot": ("destination_relative", "fixture/file."),
    "trailing_space": ("destination_relative", "fixture/file "),
    "device_alias": ("destination_relative", "fixture/CON.txt"),
    "device_alias_space": ("destination_relative", "fixture/CON .txt"),
    "device_alias_console": ("destination_relative", "fixture/CONOUT$"),
    "device_alias_superscript": ("destination_relative", "fixture/COM¹.bin"),
    "path_type": ("destination_relative", 7),
    "path_oversize": ("destination_relative", "a" * 4097),
    "path_control": ("destination_relative", "fixture/\x7f.bin"),
    "url_quote": ("url", 'http://localhost/a");throw(1);//'),
    "url_traversal": ("url", "http://localhost/safe/../outside"),
    "url_encoded_traversal": ("url", "http://localhost/%2e%2e/outside"),
    "url_encoded_separator": ("url", "http://localhost/a%2foutside"),
    "url_encoded_nul": ("url", "http://localhost/a%00.bin"),
    "url_credentials": ("url", "http://user:password@localhost/a"),
    "url_query": ("url", "http://localhost/a?"),
    "url_fragment": ("url", "http://localhost/a#"),
    "url_scheme": ("url", "file:///tmp/a"),
    "url_port": ("url", "http://localhost:65536/a"),
    "url_bad_percent": ("url", "http://localhost/%GG"),
    "url_bad_dns": ("url", "http://bad_host/a"),
    "url_bad_ip": ("url", "http://127.999.0.1/a"),
    "url_bad_ipv6_authority": ("url", "http://[::1]injected/a"),
    "url_type": ("url", ["http://localhost/a"]),
    "url_oversize": ("url", "http://localhost/" + "a" * 8192),
    "hash_type": ("raw_sha256", 7),
    "hash_short": ("raw_sha256", "a" * 63),
    "hash_nonhex": ("raw_sha256", "g" * 64),
    "size_bool": ("max_bytes", True),
    "size_float": ("max_bytes", 1.0),
    "size_negative": ("max_bytes", -1),
    "size_overflow": ("max_bytes", 256 * 1024 * 1024 + 1),
    "format_unknown": ("format", "cpp_code"),
    "format_type": ("format", {}),
}


def rejection_test(field, value):
    def test(self):
        data = manifest(); data["entries"][0][field] = value
        with self.assertRaises(ValueError):
            GENERATOR.parse_manifest(encoded(data))
    return test


for _name, (_field, _value) in NEGATIVE_ENTRIES.items():
    setattr(GeneratorTests, "test_reject_" + _name, rejection_test(_field, _value))


class SchemaTests(unittest.TestCase):
    def test_strict_root_entry_fields_and_types(self):
        cases = [[], {**manifest(), "extra": True}, {key: value for key, value in manifest().items() if key != "runtime"},
                 {**manifest(), "runtime": "passed"}, {**manifest(), "schema": "other"}, {**manifest(), "entries": []},
                 {**manifest(), "entries": {}}, {**manifest(), "entries": manifest()["entries"] * 129},
                 {**manifest(), "entries": [{**manifest()["entries"][0], "extra": "injected"}]},
                 {**manifest(), "entries": [{key: value for key, value in manifest()["entries"][0].items() if key != "format"}]}]
        for data in cases:
            with self.subTest(data=data), self.assertRaises(ValueError):
                GENERATOR.parse_manifest(encoded(data))


if __name__ == "__main__":
    unittest.main()
