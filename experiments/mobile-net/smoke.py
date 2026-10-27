"""Three real ENet processes on loopback; this does not run MTA or GTA."""

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import secrets
import socket
import subprocess
import time


def records(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]


def capture(stdout, stderr):
    result = {"stdout": [], "raw_stdout": "", "stderr": "", "parse_errors": [], "read_errors": []}
    for field, path in (("raw_stdout", stdout), ("stderr", stderr)):
        try:
            result[field] = path.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            result["read_errors"].append(f"{path.name}: {error}")
    # A crashed child may leave an unfinished line; preserve evidence without parsing it strictly.
    for number, line in enumerate(result["raw_stdout"].splitlines(), 1):
        if line.strip():
            try:
                result["stdout"].append(json.loads(line))
            except json.JSONDecodeError as error:
                result["parse_errors"].append({"line": number, "error": str(error)})
    return result


def persist(path, evidence):
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    args = parser.parse_args()
    args.evidence.parent.mkdir(parents=True, exist_ok=True)
    processes, handles, paths = [], [], []
    evidence = {
        "scope": "ENet transport readiness only; no MTA/GTA world or Lua integration",
        "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "enet_commit": "2662c0de09e36f2a2030ccc2c528a3e4c9e8138a",
        "host": "127.0.0.1", "passed": False, "state": "running",
    }
    # Invalidate any previous success before a child can crash or the runner can be interrupted.
    persist(args.evidence, evidence)
    overall_deadline = time.monotonic() + 35

    def launch(name, *arguments):
        stdout = args.evidence.parent / f"{name}.stdout.jsonl"
        stderr = args.evidence.parent / f"{name}.stderr.log"
        out, err = stdout.open("w", encoding="utf-8"), stderr.open("w", encoding="utf-8")
        handles.extend([out, err])
        command = [str(args.probe.resolve()), *map(str, arguments)]
        flags = subprocess.CREATE_NO_WINDOW if hasattr(subprocess, "CREATE_NO_WINDOW") else 0
        process = subprocess.Popen(command, stdout=out, stderr=err, creationflags=flags)
        processes.append(process)
        paths.append((stdout, stderr))
        return process

    try:
        evidence["probe_sha256"] = hashlib.sha256(args.probe.read_bytes()).hexdigest()
        # The server confirms bind before clients start; a reservation race fails loudly.
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        evidence["port"] = port
        identities = [secrets.randbits(63) + 1, secrets.randbits(63) + 1]
        while identities[1] == identities[0]:
            identities[1] = secrets.randbits(63) + 1
        server = launch("server", "server", port)
        deadline = time.monotonic() + 5
        while not paths[0][0].read_text(encoding="utf-8").strip():
            if server.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError("server did not report ready")
            time.sleep(0.025)
        ready = records(paths[0][0])[0]
        if ready != {"event": "listening", "port": port}:
            raise RuntimeError("invalid server ready record")
        first = launch("client_a", "client", port, identities[0], 2)
        second = launch("client_b", "client", port, identities[1], 1)
        for process in (first, second, server):
            if process.wait(timeout=max(0.1, overall_deadline - time.monotonic())) != 0:
                raise RuntimeError(f"probe process {process.pid} failed")
        server_result = records(paths[0][0])[-1]
        expected = {"event": "server_complete", "connections": 3, "disconnected": 3,
                    "accepted": 6, "sessions": 2, "reconnected": 1}
        if server_result != expected:
            raise RuntimeError("unexpected server summary")
        for index, cycles in ((1, 2), (2, 1)):
            expected_client = {"event": "client_complete", "session": str(identities[index - 1]), "completed_cycles": cycles}
            if records(paths[index][0]) != [expected_client]:
                raise RuntimeError("unexpected client summary")
        evidence["passed"] = True
    except Exception as error:
        evidence["error"] = str(error)
    finally:
        cleanup_errors = []
        for process in processes:
            try:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired) as error:
                cleanup_errors.append(f"process {process.pid}: {error}")
        for handle in handles:
            try:
                handle.close()
            except OSError as error:
                cleanup_errors.append(str(error))
        evidence["processes"] = [
            {"pid": process.pid, "exit_code": process.returncode,
             **capture(*paths[index])}
            for index, process in enumerate(processes)
        ]
        if cleanup_errors:
            evidence["cleanup_errors"] = cleanup_errors
            evidence["passed"] = False
        evidence["state"] = "passed" if evidence["passed"] else "failed"
        persist(args.evidence, evidence)
        print(json.dumps(evidence))
    return 0 if evidence["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
