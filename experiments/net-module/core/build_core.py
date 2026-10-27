"""Build only the experimental Windows x64 Core in its own worktree."""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

PIN = "128f7012508c9319d752fb9e8232b5f06adf0130"
NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
INPUT_TREES = ("Client", "Server", "Shared", "vendor", "utils", "premake5.lua")


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def save(path, record):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def available_mib():
    class Memory(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [(key, ctypes.c_ulonglong) for key in ("total", "available", "totalPage", "availablePage", "totalVirtual", "availableVirtual", "extended")]
    memory = Memory()
    memory.length = ctypes.sizeof(memory)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(memory)):
        raise ctypes.WinError()
    return memory.available // (1024 * 1024)


def inspect_project(project):
    xml = ET.parse(project).getroot()
    files = []
    for kind in ("ClCompile", "ClInclude", "ResourceCompile", "None"):
        for node in xml.findall(f".//m:{kind}", NS):
            include = node.get("Include")
            if not include:
                continue
            path = (project.parent / include).resolve()
            files.append({"kind": kind, "include": include, "path": str(path), "sha256": sha(path) if path.is_file() else None})
    references = [node.get("Include") for node in xml.findall(".//m:ProjectReference", NS)]
    links = []
    for group in xml.findall("m:ItemDefinitionGroup", NS):
        for node in group.findall("m:Link/m:AdditionalDependencies", NS):
            links.append({"condition": group.get("Condition"), "dependencies": node.text})
    return {"sha256": sha(project), "declaredInputs": files, "projectReferences": references, "linkDependencies": links}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--output", type=Path)
    parser.add_argument("--msbuild", type=Path, default=Path("C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe"))
    parser.add_argument("--inspect-only", action="store_true")
    args = parser.parse_args()
    source = args.source.resolve()
    output = (args.output or source / "experiments/net-module/core/build").resolve()
    output.mkdir(parents=True, exist_ok=True)
    result_path = output / "result.json"
    record = {"status": "running", "startedAtUtc": utc(), "runnerPid": os.getpid(), "source": str(source), "expectedCompileRef": PIN, "configuration": "Release", "platform": "x64", "target": "Server/Core", "maxWorkers": 1, "environmentOverrides": {"_CL_": "/MP1"}, "baselineArtifactsImported": False, "serverRuntime": "not_run", "steps": []}
    lock = output / "active.lock"
    acquired = False
    active_process = None
    try:
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        acquired = True
        with os.fdopen(descriptor, "w") as stream:
            json.dump({"runnerPid": os.getpid(), "startedAtUtc": utc()}, stream)
        save(result_path, record)
        if os.name != "nt":
            raise ValueError("This helper targets the installed Windows MSBuild toolchain")
        record["sourceHead"] = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        if subprocess.run(["git", "-C", str(source), "diff", "--quiet", PIN, "--", *INPUT_TREES]).returncode:
            raise ValueError("Compile source trees differ from the pinned opt-in loader revision")
        dirty = subprocess.check_output(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=all", "--", *INPUT_TREES], text=True).strip()
        if dirty:
            raise ValueError("Compile input trees must have no tracked/untracked modifications")
        premake = source / "utils/premake5.exe"
        for tool in (premake, args.msbuild):
            if not tool.is_file():
                raise ValueError(f"Installed build tool missing: {tool}")
        record["tools"] = {str(tool): sha(tool) for tool in (premake, args.msbuild)}
        environment = os.environ.copy()
        environment["_CL_"] = "/MP1"

        def step(name, command):
            nonlocal active_process
            stdout_path = output / f"{name}.{os.getpid()}.stdout.log"
            stderr_path = output / f"{name}.{os.getpid()}.stderr.log"
            entry = {"name": name, "command": list(map(str, command)), "startedAtUtc": utc(), "stdout": str(stdout_path), "stderr": str(stderr_path)}
            record["steps"].append(entry)
            with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
                kernel = ctypes.windll.kernel32
                previous = kernel.SetErrorMode(0x0001 | 0x0002 | 0x8000)
                try:
                    process = subprocess.Popen(command, cwd=source, env=environment, stdin=subprocess.DEVNULL, stdout=stdout, stderr=stderr, creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
                finally:
                    kernel.SetErrorMode(previous)
                entry["pid"] = process.pid
                active_process = process
                save(result_path, record)
                code = process.wait()
                active_process = None
            entry.update(exitCode=code, finishedAtUtc=utc())
            save(result_path, record)
            if code:
                raise RuntimeError(f"{name} failed with exit {code}; inspect its logs")

        record["freeMemoryMiBBeforeGenerate"] = available_mib()
        if record["freeMemoryMiBBeforeGenerate"] < 2048:
            record["status"] = "deferred_memory"
            return 3
        project = source / "Build/Core.vcxproj"
        marker = source / "Build/.experimental-core-project.json"
        cached = json.loads(marker.read_text()) if marker.is_file() else {}
        if not project.is_file() or cached.get("pin") != PIN or cached.get("projectSha256") != sha(project):
            step("generate", [str(premake), "vs2026"])
            cached = {"pin": PIN, "projectSha256": sha(project), "generationStep": record["steps"][-1]}
            save(marker, cached)
        record["generationStep"] = cached.get("generationStep")
        record["project"] = inspect_project(project)
        save(result_path, record)
        if any("deathmatch" in ref.lower() or "client" in ref.lower() for ref in record["project"]["projectReferences"]):
            record.update(status="dependency_review_required", reason="Core references an unexpected heavy gameplay target")
            return 3
        if args.inspect_only:
            record["status"] = "inspected"
            return 0
        record["freeMemoryMiBBeforeCompile"] = available_mib()
        if record["freeMemoryMiBBeforeCompile"] < 2048:
            record["status"] = "deferred_memory"
            return 3
        command = [str(args.msbuild), "Build/MTASA.sln", "/t:Server\\Core", "/p:Configuration=Release", "/p:Platform=x64", "/p:MultiProcessorCompilation=false", "/m:1", "/nr:false", "/v:minimal", "/nologo"]
        step("compile-core", command)
        dll = source / "Bin/server/x64/core.dll"
        if not dll.is_file():
            raise ValueError("Successful MSBuild did not produce the expected core.dll")
        record["artifact"] = {"path": str(dll), "bytes": dll.stat().st_size, "sha256": sha(dll)}
        # Preserve actual compiler/linker read dependencies, beyond project declarations.
        paths, tlogs = set(), []
        for log in (source / "Build").rglob("*.read.*.tlog"):
            if not any(part.lower() in ("core", "core.tlog") for part in log.parent.relative_to(source / "Build").parts):
                continue
            raw = log.read_bytes()
            text = raw.decode("utf-16") if raw.startswith((b"\xff\xfe", b"\xfe\xff")) else raw.decode("utf-8", errors="replace")
            tlogs.append({"path": str(log), "sha256": sha(log)})
            for line in text.splitlines():
                for candidate in line.lstrip("^").split("|"):
                    path = Path(candidate.strip())
                    if path.is_absolute() and path.is_file():
                        paths.add(path)
        actual = [{"path": str(path), "sha256": sha(path), "bytes": path.stat().st_size} for path in sorted(paths)]
        save(output / "actual-read-inputs.json", {"tlogs": tlogs, "files": actual})
        if not tlogs or not actual:
            raise ValueError("Core artifact exists but compiler/linker input trace is unavailable")
        record.update(status="passed", actualReadInputCount=len(actual), actualReadInputsSha256=sha(output / "actual-read-inputs.json"), tlogs=tlogs)
        return 0
    except FileExistsError as error:
        record.update(status="already_active" if not acquired else "failed", error=str(error))
        return 4 if not acquired else 1
    except Exception as error:
        record.update(status="failed", error=str(error))
        return 1
    finally:
        record["finishedAtUtc"] = utc()
        if acquired:
            if active_process and active_process.poll() is None:
                record.update(status="runner_interrupted_child_alive", activeChildPid=active_process.pid)
            save(result_path, record)
            save(output / f"result.{os.getpid()}.json", record)
            if not active_process or active_process.poll() is not None:
                lock.unlink()
        print(json.dumps({"status": record["status"], "result": str(result_path), "serverRuntime": "not_run"}))


if __name__ == "__main__":
    raise SystemExit(main())
