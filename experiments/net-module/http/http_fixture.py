"""Real local HTTP server + real native curl component. No MTA/game runtime."""
import base64
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

counts = {}
counts_lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def reply(self, code, data=b"", headers=()):
        self.send_response(code)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        for key, value in headers:
            self.send_header(key, value)
        self.end_headers()
        try:
            if self.command != "HEAD":
                self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        self.close_connection = True

    def handle_request(self):
        path = self.path.split("?", 1)[0]
        with counts_lock:
            counts[path] = counts.get(path, 0) + 1
            number = counts[path]
        length = int(self.headers.get("Content-Length", "0"))
        if length > 2 * 1024 * 1024:
            self.reply(413)
            return
        body = self.rfile.read(length) if length else b""
        if path == "/get":
            self.reply(200, b"hello\x00world", [("X-Fixture", "actual-server")])
        elif path == "/empty":
            self.reply(200)
        elif path == "/missing":
            self.reply(404, b"not-found")
        elif path == "/redirect":
            self.reply(302, b"ignored", [("Location", "/get")])
        elif path == "/loop":
            self.reply(302, headers=[("Location", "/loop")])
        elif path == "/header":
            self.reply(200, self.headers.get("X-Probe", "missing").encode())
        elif path == "/echo":
            self.reply(200, body)
        elif path == "/form":
            if "multipart/form-data" not in self.headers.get("Content-Type", ""):
                self.reply(400)
            else:
                self.reply(200, body)
        elif path == "/auth":
            expected = "Basic " + base64.b64encode(b"user:pass").decode()
            if self.headers.get("Authorization") != expected:
                self.reply(401, b"challenge", [("WWW-Authenticate", 'Basic realm="local-test"')])
            else:
                self.reply(200, b"authenticated")
        elif path == "/flaky" and number == 1 or path == "/flaky-post":
            self.close_connection = True
            self.connection.shutdown(socket.SHUT_RDWR)
        elif path == "/flaky":
            self.reply(200, b"retried")
        elif path == "/slow":
            time.sleep(2)
            self.reply(200, b"late")
        elif path == "/large":
            self.reply(200, b"a" * (128 * 1024))
        elif path == "/gzip":
            self.reply(200, gzip.compress(b"decoded-body" * 200), [("Content-Encoding", "gzip")])
        elif path == "/range":
            data = b"hello-range"
            value = self.headers.get("Range")
            if value and value.startswith("bytes="):
                offset = int(value[6:].split("-", 1)[0])
                self.reply(206, data[offset:], [("Content-Range", f"bytes {offset}-{len(data)-1}/{len(data)}")])
            else:
                self.reply(200, data)
        elif path == "/ignore-range":
            self.reply(200, b"hello-range")
        elif path in ("/file", "/validated"):
            self.reply(200, b"on-disk")
        else:
            self.reply(404)

    do_GET = handle_request
    do_HEAD = handle_request
    do_POST = handle_request
    do_PUT = handle_request


def main():
    if len(sys.argv) != 2:
        raise SystemExit("expected component executable")
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="mta-http-integration-") as temporary:
            result = subprocess.run([str(Path(sys.argv[1]).resolve()), f"http://127.0.0.1:{server.server_port}", temporary],
                                    stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=65,
                                    creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            print(result.stdout, end="")
            print(result.stderr, end="", file=sys.stderr)
            if result.returncode:
                return result.returncode
            if counts.get("/validated", 0) != 0 or counts.get("/flaky-post", 0) != 1:
                raise RuntimeError("Actual server request counts violate validation/retry contracts: " + str(counts))
            print("PASS real localhost HTTP fixture; MTA/game/client Lua/G4 NOT RUN")
            return 0
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


if __name__ == "__main__":
    raise SystemExit(main())
