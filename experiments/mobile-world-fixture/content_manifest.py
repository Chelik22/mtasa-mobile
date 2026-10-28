"""Describe approved fixture downloads; this does not download or execute them."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
from urllib.parse import quote, urlsplit
import xml.etree.ElementTree as ET


def relative_path(value):
    path = PurePosixPath(value)
    if (not value or any(ord(char) < 32 or char in '\\:<>"|?*' for char in value) or path.is_absolute()
            or any(part in ("", ".", "..") or part.endswith((".", " ")) for part in value.split("/"))):
        raise ValueError("Expected a nonempty relative resource path without traversal")
    return path


def generate(root, http_base, resource_name, destination_prefix):
    root = root.resolve(strict=True)
    origin = urlsplit(http_base)
    if origin.scheme not in ("http", "https") or not origin.hostname or origin.username or origin.password or origin.query or origin.fragment:
        raise ValueError("Expected an HTTP base URL without credentials/query/fragment")
    relative_path(resource_name)
    if "/" in resource_name:
        raise ValueError("Expected one resource name")
    prefix = relative_path(destination_prefix)
    entries, seen = [], set()
    for node in ET.parse(root / "meta.xml").getroot().findall("file"):
        relative = relative_path(node.attrib["src"])
        path = (root / str(relative)).resolve(strict=True)
        if not path.is_relative_to(root) or not path.is_file():
            raise ValueError("Downloadable file escapes resource or is not a regular file")
        destination = str(prefix / relative)
        if destination.casefold() in seen:
            raise ValueError("Duplicate destination on the Windows client")
        seen.add(destination.casefold())
        payload = path.read_bytes()
        if len(payload) > 64 * 1024 * 1024:
            raise ValueError("Fixture artifact size outside supported bounds")
        entries.append({"destination_relative": destination,
                        "url": http_base.rstrip("/") + "/" + quote(resource_name, safe="") + "/" + quote(str(relative), safe="/"),
                        "raw_sha256": hashlib.sha256(payload).hexdigest(),
                        "max_bytes": len(payload), "format": "binary_asset"})
    if not entries:
        raise ValueError("No explicit HTTP file assets in resource metadata")
    return {"schema": "mta-fixture-approved-content-v1", "runtime": "not_run", "entries": entries}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource-root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--http-base", required=True)
    parser.add_argument("--resource-name", default="mobile-world-fixture")
    parser.add_argument("--destination-prefix", default="mobile-world-fixture")
    args = parser.parse_args()
    print(json.dumps(generate(args.resource_root, args.http_base, args.resource_name, args.destination_prefix), indent=2))


if __name__ == "__main__":
    main()
