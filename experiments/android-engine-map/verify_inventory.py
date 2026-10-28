"""Bind a static candidate map to exact local artifacts; never load target code."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def verify(contract, library, dump):
    if digest(library) != contract["library"]["sha256"]:
        raise ValueError("library SHA256 mismatch")
    if digest(dump) != contract["symbol_dump"]["sha256"]:
        raise ValueError("symbol dump SHA256 mismatch")
    with library.open("rb") as stream:
        header = stream.read(64)
    if len(header) != 64 or header[:7] != b"\x7fELF\x02\x01\x01":
        raise ValueError("expected ELF64 little endian version 1")
    if struct.unpack_from("<HH", header, 16) != (3, 183):
        raise ValueError("expected ET_DYN AArch64")
    lines = dump.read_text(encoding="utf-8-sig").splitlines()
    for capability in contract["capabilities"]:
        if capability["state"] != "candidate_only" or capability["callable"] is not False:
            raise ValueError("static inventory cannot authorize engine calls")
        for item in capability["symbols"]:
            line = lines[item["dump_line"] - 1]
            pattern = r"\bFUNC\s+(?:GLOBAL|WEAK)\s+DEFAULT\s+(?!UND\b)\d+\s+" + re.escape(item["mangled"]) + r"\s*$"
            if not re.search(pattern, line):
                raise ValueError("defined function export mismatch: " + item["mangled"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--readelf", type=Path, required=True)
    parser.add_argument("--contract", type=Path, default=Path(__file__).with_name("capabilities.json"))
    args = parser.parse_args()
    try:
        contract = json.loads(args.contract.read_text(encoding="utf-8"))
        verify(contract, args.library, args.readelf)
    except (OSError, ValueError, KeyError, IndexError, TypeError) as error:
        parser.exit(1, "static inventory rejected: " + str(error) + "\n")
    print(json.dumps({"static_inventory": "matched", "runtime": "not_run", "callable": False}))


if __name__ == "__main__":
    main()
