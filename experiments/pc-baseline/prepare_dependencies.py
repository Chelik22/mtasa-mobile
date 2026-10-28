"""Cache pinned build dependencies without installers, global settings or target execution."""
from __future__ import annotations

import argparse
import base64
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import tarfile
import urllib.request
import zipfile

HERE = Path(__file__).resolve().parent
MAX_ARCHIVE = 1024 * 1024 * 1024
MAX_EXTRACTED = 2 * MAX_ARCHIVE


def sha(path, algorithm="sha256"):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, algorithm).hexdigest()


def save(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def verify(path, item):
    if path.stat().st_size > MAX_ARCHIVE:
        raise ValueError("Archive exceeds byte limit")
    expected = item.get("sha256")
    if not expected and not item.get("sha512Base64"):
        raise ValueError("An authoritative pinned dependency digest is required")
    if expected and sha(path) != expected:
        raise ValueError("SHA256 mismatch: " + item["id"])
    expected512 = item.get("sha512Base64")
    if expected512 and bytes.fromhex(sha(path, "sha512")) != base64.b64decode(expected512, validate=True):
        raise ValueError("NuGet authoritative SHA512 mismatch")


def safe_member(name, destination):
    # Both separator forms must be rejected on Windows, even for ZIPs made on Unix.
    name = name.replace("\\", "/")
    if ":" in name:
        raise ValueError("Archive member contains a drive/stream name")
    target = (destination / name).resolve()
    if not target.is_relative_to(destination.resolve()):
        raise ValueError("Archive member escapes destination")
    return target


def extract(path, destination, kind):
    if destination.exists():
        raise ValueError("Extraction destination already exists")
    destination.mkdir(parents=True)
    if kind == "zip":
        with zipfile.ZipFile(path) as archive:
            members = archive.infolist()
            if sum(item.file_size for item in members) > MAX_EXTRACTED:
                raise ValueError("Extracted size exceeds limit")
            for item in members:
                safe_member(item.filename, destination)
                if (item.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError("ZIP symlinks are unsupported")
            archive.extractall(destination)
    else:
        with tarfile.open(path, "r:bz2") as archive:
            members = archive.getmembers()
            if sum(item.size for item in members) > MAX_EXTRACTED:
                raise ValueError("Extracted size exceeds limit")
            for item in members:
                safe_member(item.name, destination)
                if not (item.isfile() or item.isdir()):
                    raise ValueError("Only ordinary files/directories may be extracted")
            archive.extractall(destination, members=members, filter="data")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--download", action="store_true", help="Fetch missing pinned artifacts; never runs an installer")
    args = parser.parse_args()
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    result = {"status": "running", "pid": os.getpid(), "startedUtc": dt.datetime.now(dt.timezone.utc).isoformat(),
              "lockSha256": sha(HERE / "dependencies.lock.json"), "targetExecution": "not_run", "artifacts": []}
    result_path = cache / "dependencies-result.json"
    lock = cache / "active.lock"
    acquired = False
    try:
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        acquired = True
        with os.fdopen(descriptor, "w") as stream:
            json.dump({"pid": os.getpid()}, stream)
        save(result_path, result)
        if shutil.disk_usage(cache).free < 8 * MAX_ARCHIVE:
            raise ValueError("Require at least 8 GiB free for dependency cache")
        items = json.loads((HERE / "dependencies.lock.json").read_text())["artifacts"]
        for item in items:
            path = cache / item["filename"]
            entry = {"id": item["id"], "status": "checking", "path": str(path), "url": item["url"]}
            result["artifacts"].append(entry)
            save(result_path, result)
            if not path.is_file():
                if not args.download:
                    raise ValueError("Missing cached artifact: " + str(path))
                temporary = path.with_suffix(path.suffix + ".part")
                with urllib.request.urlopen(item["url"], timeout=45) as response, temporary.open("wb") as stream:
                    if not response.geturl().startswith("https://"):
                        raise ValueError("Dependency redirect must remain HTTPS")
                    entry["resolvedUrl"] = response.geturl()
                    length = int(response.headers.get("Content-Length", "0"))
                    if length > MAX_ARCHIVE:
                        raise ValueError("Download exceeds byte limit")
                    total = 0
                    while chunk := response.read(1024 * 1024):
                        total += len(chunk)
                        if total > MAX_ARCHIVE:
                            raise ValueError("Download exceeds byte limit")
                        stream.write(chunk)
                verify(temporary, item)
                temporary.replace(path)
            verify(path, item)
            entry.update(sha256=sha(path), bytes=path.stat().st_size)
            if item["kind"] != "file":
                destination = cache / (item["id"] + "-extracted")
                tree_path = cache / (item["id"] + "-tree.json")
                if destination.exists() and not tree_path.is_file():
                    raise ValueError("Unfinished/unowned extraction requires inspection: " + str(destination))
                if not destination.exists():
                    extract(path, destination, item["kind"])
                # A marker binds extraction to the verified archive; file hashes below
                # record the actual tree rather than asserting a folder name is provenance.
                files = {str(p.relative_to(destination)): {"sha256": sha(p), "bytes": p.stat().st_size}
                         for p in sorted(destination.rglob("*")) if p.is_file()}
                if not files:
                    raise ValueError("Extracted dependency is empty")
                if tree_path.is_file():
                    previous = json.loads(tree_path.read_text())
                    if previous["archiveSha256"] != entry["sha256"] or previous["files"] != files:
                        raise ValueError("Extracted dependency changed: " + item["id"])
                else:
                    save(tree_path, {"archiveSha256": entry["sha256"], "files": files})
                entry.update(extractedPath=str(destination), treeManifest=str(tree_path), treeSha256=sha(tree_path), fileCount=len(files))
            entry["status"] = "passed"
            save(result_path, result)
            print(item["id"] + ": verified", flush=True)
        result["status"] = "passed"
        return 0
    except Exception as error:
        result.update(status="failed", error=str(error))
        return 2
    finally:
        if acquired:
            result["finishedUtc"] = dt.datetime.now(dt.timezone.utc).isoformat()
            save(result_path, result)
            lock.unlink()
        print(json.dumps({"status": result["status"], "evidence": str(result_path), "targetExecution": "not_run"}), flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
