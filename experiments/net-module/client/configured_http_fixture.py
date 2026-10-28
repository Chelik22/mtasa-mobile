"""Real configured CNet DLL + localhost asset server; no Core/game/Lua mock."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path, PurePosixPath
import struct
import subprocess
import sys
import tempfile
import threading
from urllib.parse import urlsplit


def i386(path):
    with path.open("rb") as stream:
        if stream.read(2) != b"MZ":
            raise ValueError("Expected own built PE artifact")
        stream.seek(0x3C)
        offset = struct.unpack("<I", stream.read(4))[0]
        stream.seek(offset)
        if stream.read(4) != b"PE\0\0" or struct.unpack("<H", stream.read(2))[0] != 0x14C:
            raise ValueError("Configured test requires actual I386 EXE and DLL")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("exe", "dll", "manifest", "asset"):
        parser.add_argument("--"+option, type=Path, required=True)
    args = parser.parse_args()
    document = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
    if document["schema"] != "mta-fixture-approved-content-v1" or document["runtime"] != "not_run" or len(document["entries"]) != 1:
        raise ValueError("This narrow integration scenario needs one trusted fixture entry")
    entry = document["entries"][0]
    relative = PurePosixPath(entry["destination_relative"])
    if relative.is_absolute() or any(part in ("", ".", "..") for part in entry["destination_relative"].split("/")) or "\\" in str(relative):
        raise ValueError("Invalid test destination")
    data = args.asset.read_bytes()
    if not data or len(data) > 1024*1024 or type(entry["max_bytes"]) is not int or len(data) > entry["max_bytes"] or \
            hashlib.sha256(data).hexdigest() != entry["raw_sha256"].lower():
        raise ValueError("Actual source asset bytes do not match trusted raw manifest")
    url = urlsplit(entry["url"])
    if url.scheme != "http" or url.hostname != "127.0.0.1" or not url.port or url.username or url.password or url.query or url.fragment:
        raise ValueError("This actual test binds only the trusted literal IPv4 loopback HTTP URL")
    for path in (args.exe, args.dll):
        i386(path)
    count = 0
    count_lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            nonlocal count
            if self.path != url.path:
                self.send_error(404)
                return
            with count_lock:
                count += 1
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass

    # Fail closed on occupied ports; never stop/reuse another server or silently
    # change the URL already compiled into the actual DLL's trusted manifest.
    server = ThreadingHTTPServer(("127.0.0.1", url.port), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="mta-configured-cnet-http-") as directory:
            cache = Path(directory)/"empty-cache"
            cache.mkdir()
            if any(cache.iterdir()):
                raise RuntimeError("Initial test cache is not empty")
            environment = os.environ.copy()
            environment["MTA_EXPERIMENTAL_CLIENT_CACHE_ROOT"] = str(cache.resolve())
            environment["MTA_EXPERIMENTAL_CLIENT_IDENTITY"] = "0123456789ABCDEF0123456789ABCDEF"
            command = [str(args.exe.resolve()), str(args.dll.resolve()), entry["url"], str(relative), str(args.asset.resolve())]
            result = subprocess.run(command, stdin=subprocess.DEVNULL, env=environment, capture_output=True, text=True, timeout=40,
                                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode:
                return result.returncode
            if count != 2 or "PASS configured real CNet DLL HTTP/file/snapshot/Shutdown/Release I38632" not in result.stdout:
                raise RuntimeError(f"Missing actual native witness or unexpected request count: {count}")
            target = cache/Path(relative)
            if target.read_bytes() != data or list(cache.rglob("*.part")):
                raise RuntimeError("Published bytes mismatch or owned temporary leaked")
            print(f"PASS actual configured DLL + localhost HTTP requests={count}; rawSHA256={hashlib.sha256(data).hexdigest()}; Core/Lua/G4 NOT RUN")
            return 0
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
