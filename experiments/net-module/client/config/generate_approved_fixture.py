"""Compile a trusted local approved-content manifest into typed C++ entries.

Only Python stdlib and the repository's content_manifest.relative_path are used.
No downloaded data, environment cache root or runtime JSON parser is involved.
"""

import argparse
import importlib.util
import ipaddress
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from urllib.parse import unquote_to_bytes, urlsplit


MAX_INPUT_BYTES = 1024 * 1024
SCHEMA = "mta-fixture-approved-content-v1"
ROOT_FIELDS = {"schema", "runtime", "entries"}
ENTRY_FIELDS = {"destination_relative", "url", "raw_sha256", "max_bytes", "format"}
FORMATS = {"binary_asset": ("BinaryAsset", 256 * 1024 * 1024),
           "lua_source_utf8": ("LuaSourceUtf8", 1024 * 1024)}
PRODUCER = Path(__file__).resolve().parents[4] / "experiments/mobile-world-fixture/content_manifest.py"
_spec = importlib.util.spec_from_file_location("approved_content_producer", PRODUCER)
_producer = importlib.util.module_from_spec(_spec)
_previous_bytecode = sys.dont_write_bytecode
try:
    sys.dont_write_bytecode = True
    _spec.loader.exec_module(_producer)
finally:
    sys.dont_write_bytecode = _previous_bytecode


def exact_fields(value, expected, context):
    if type(value) is not dict or set(value) != expected:
        raise ValueError(f"{context} must contain exactly {sorted(expected)}")


def utf8_string(value, maximum, context):
    if type(value) is not str:
        raise ValueError(f"{context} must be a string")
    try:
        encoded = value.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise ValueError(f"{context} is not valid UTF-8") from error
    if not encoded or len(encoded) > maximum or any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise ValueError(f"{context} has invalid length/control characters")
    return value


def destination(value):
    value = utf8_string(value, 4096, "destination_relative")
    _producer.relative_path(value)
    for segment in value.split("/"):
        base = segment.split(".", 1)[0].rstrip(" ").upper()
        if base in {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"} or re.fullmatch(r"(?:COM|LPT)[1-9¹²³]", base):
            raise ValueError("destination_relative uses a Windows device alias")
    return value


def approved_url(value):
    value = utf8_string(value, 8192, "url")
    if (not value.startswith(("http://", "https://")) or any(ord(char) >= 127 or char.isspace() for char in value)
            or any(char in value for char in '\\<>"^`{}|?#') or re.search(r"%(?![0-9A-Fa-f]{2})", value)):
        raise ValueError("url must be an ASCII HTTP(S) URL without credentials/query/fragment/unsafe characters")
    try:
        parts = urlsplit(value)
        host, port = parts.hostname, parts.port
        if not host or parts.username is not None or parts.password is not None or not parts.path.startswith("/"):
            raise ValueError("url must have a host and absolute resource path without credentials")
        if port is not None and not 1 <= port <= 65535 or parts.netloc.endswith(":"):
            raise ValueError("url has invalid port")
        if ":" in host:
            if not re.fullmatch(r"\[[0-9A-Fa-f:.]+\](?::[0-9]+)?", parts.netloc):
                raise ValueError("malformed IPv6 authority")
            if "%" in host:
                raise ValueError("scoped IPv6 URL is not supported")
            ipaddress.IPv6Address(host)
        elif re.fullmatch(r"[0-9.]+", host):
            ipaddress.IPv4Address(host)
        elif len(host) > 253 or not all(re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label) for label in host.split(".")):
            raise ValueError("url hostname is not canonical ASCII DNS/IP")
        decoded = unquote_to_bytes(parts.path).decode("utf-8", errors="strict")
        if (any(ord(char) < 32 or ord(char) == 127 for char in decoded) or "\\" in decoded
                or any(segment in (".", "..") for segment in decoded.split("/")) or re.search(r"%(?:2f|5c)", parts.path, re.IGNORECASE)):
            raise ValueError("url path has traversal/control/encoded separators")
    except (UnicodeError, ValueError) as error:
        raise ValueError(f"Invalid approved url: {error}") from error
    return value


def reject_duplicates(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON field: {key}")
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError(f"Non-finite JSON number is forbidden: {value}")


def parse_manifest(payload):
    if len(payload) > MAX_INPUT_BYTES:
        raise ValueError("Manifest exceeds 1MiB")
    try:
        data = json.loads(payload.decode("utf-8", errors="strict"), object_pairs_hook=reject_duplicates, parse_constant=reject_constant)
    except (UnicodeError, RecursionError) as error:
        raise ValueError("Manifest must be bounded UTF-8 JSON") from error
    exact_fields(data, ROOT_FIELDS, "manifest")
    if data["schema"] != SCHEMA or type(data["schema"]) is not str or data["runtime"] != "not_run" or type(data["runtime"]) is not str:
        raise ValueError("Unsupported schema/runtime marker")
    if type(data["entries"]) is not list or not 1 <= len(data["entries"]) <= 128:
        raise ValueError("Manifest requires 1..128 entries")
    entries, folded, upper = [], set(), set()
    for entry in data["entries"]:
        exact_fields(entry, ENTRY_FIELDS, "entry")
        path = destination(entry["destination_relative"])
        if path.casefold() in folded or path.upper() in upper:
            raise ValueError("Case-insensitive duplicate destination")
        folded.add(path.casefold()); upper.add(path.upper())
        url = approved_url(entry["url"])
        digest = entry["raw_sha256"]
        if type(digest) is not str or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
            raise ValueError("raw_sha256 must be exactly 64 hexadecimal characters")
        format_name = entry["format"]
        if type(format_name) is not str or format_name not in FORMATS:
            raise ValueError("Unsupported fixture format")
        enum_name, maximum = FORMATS[format_name]
        size = entry["max_bytes"]
        if type(size) is not int or not 0 <= size <= maximum:
            raise ValueError("max_bytes must be an integer within the format bound")
        entries.append((path, url, digest.lower(), size, enum_name))
    return sorted(entries, key=lambda entry: entry[0].encode("utf-8"))


def cpp_utf8(value):
    # Fixed three-digit octal escapes cannot absorb a following hex digit or
    # terminate the C++ literal; compiler source/execution charset is irrelevant.
    return '"' + "".join(f"\\{byte:03o}" for byte in value.encode("utf-8")) + '"'


def render(entries):
    lines = ["// Generated from a trusted local manifest; do not edit.", "#pragma once", "#include <cstddef>",
             "#include <string>", "#include <vector>", '#include "FixtureContentPolicy.h"', "",
             "namespace experimental_client::approved_fixture", "{", "inline std::vector<experimental_http::FixtureEntry> Entries()", "{", "    return {"]
    for path, url, digest, size, enum_name in entries:
        lines.append(f"        {{{cpp_utf8(path)}, {cpp_utf8(url)}, {cpp_utf8(digest)}, std::size_t{{{size}}}, experimental_http::FixtureFormat::{enum_name}}},")
    return "\n".join(lines + ["    };", "}", "}", ""]).encode("ascii")


def generate(manifest, output):
    if manifest.resolve() == output.resolve():
        raise ValueError("Manifest and generated header must have different paths")
    with manifest.open("rb") as source:
        payload = source.read(MAX_INPUT_BYTES + 1)
    content = render(parse_manifest(payload))
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=output.name + ".", delete=False) as file:
            temporary = Path(file.name); file.write(content)
        os.replace(temporary, output)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return content


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args.manifest, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"approved fixture generation failed: {error}\n")


if __name__ == "__main__":
    main()
