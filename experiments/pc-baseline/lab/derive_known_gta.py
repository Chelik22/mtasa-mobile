"""Derive full-image allowlist for exact pinned loader patches, offline only.

Mirrors GameExecutablePatcher.cpp a84e0c77 ApplyPatches (not its permissive
Restore masks). Reads the canonical private fixture as data, never starts it.
Optional new-directory .bin samples are private test data, never Git artifacts.
"""
import argparse
import array
import hashlib
import itertools
import json
from pathlib import Path
import struct

CANONICAL = "f63fa623d14e170d0ae9e7032186669cf3a177793be21c4824043f0279bd506a"
ENGINE = "a84e0c77c66f596b0c72def1c857253568208a2b"


def apply(original, debug, aero, gpu):
    image = bytearray(original)
    image[0x96] |= 0x20
    image[0xDF] |= 0x01
    library = b"mtasa_d.dll\0" if debug else b"mtasa.dll\0"
    image[0x4A0C8A:0x4A0C8A + len(library)] = library
    if aero:
        struct.pack_into("<I", image, 0x88, 0x437101CA)
    if gpu:
        directory = bytearray(144)
        base, values = 0x4A32B0, 0x4E3F00
        offset = 40
        struct.pack_into("<I", directory, 4, 0xFFFFFFFF)
        struct.pack_into("<I", directory, 12, base + offset)
        struct.pack_into("<III", directory, 16, 1, 2, 2)
        for text in (b"gta_sa.exe\0",):
            directory[offset:offset + len(text)] = text
            offset += len(text)
        names = []
        for text in (b"AmdPowerXpressRequestHighPerformance\0", b"NvOptimusEnablement\0"):
            names.append(base + offset)
            directory[offset:offset + len(text)] = text
            offset += len(text)
        struct.pack_into("<I", directory, 32, base + offset)
        struct.pack_into("<II", directory, offset, *names)
        offset += 8
        struct.pack_into("<I", directory, 36, base + offset)
        struct.pack_into("<HH", directory, offset, 0, 1)
        offset += 4
        struct.pack_into("<I", directory, 28, base + offset)
        struct.pack_into("<II", directory, offset, values, values + 4)
        offset += 8
        assert offset == 128
        image[0x4A1AB0:0x4A1AB0 + 144] = directory
        struct.pack_into("<II", image, 0x4E1B00, 1, 1)
        struct.pack_into("<II", image, 0xF8, base, offset)
    struct.pack_into("<I", image, 0xD8, 0)
    words = array.array("H", image)
    import sys
    if sys.byteorder != "little":
        words.byteswap()
    checksum = 0
    for word in words:
        checksum += word
        checksum = (checksum >> 16) + (checksum & 0xFFFF)
    checksum += checksum >> 16
    struct.pack_into("<I", image, 0xD8, checksum + len(image))
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--canonical", type=Path, required=True)
    parser.add_argument("--receipt", type=Path, required=True)
    parser.add_argument("--samples", type=Path)
    args = parser.parse_args()
    original = args.canonical.read_bytes()
    assert len(original) == 14383616 and hashlib.sha256(original).hexdigest() == CANONICAL
    if args.samples:
        args.samples.mkdir(exist_ok=False)
    variants = []
    for debug, aero, gpu in itertools.product((False, True), repeat=3):
        image = apply(original, debug, aero, gpu)
        name = f"{'debug' if debug else 'release'}-aero{int(aero)}-gpu{int(gpu)}.bin"
        item = dict(name=name, debug=debug, aero=aero, gpu=gpu, bytes=len(image), sha256=hashlib.sha256(image).hexdigest())
        if args.samples:
            path = args.samples / name
            with path.open("xb") as output:
                output.write(image)
        variants.append(item)
    record = dict(schema="mta-exact-loader-image-allowlist-v1", enginePin=ENGINE, source="Client/loader/GameExecutablePatcher.cpp",
                  canonicalSha256=CANONICAL, variants=variants, targetExecution="not_run", installedWrites=False)
    with args.receipt.open("x", encoding="utf-8") as output:
        json.dump(record, output, indent=2)
        output.write("\n")
    print(json.dumps(variants, indent=2))


if __name__ == "__main__":
    main()
