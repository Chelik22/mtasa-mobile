"""One-shot, fresh-directory CLI build; no game/ADB/GUI operations."""
import argparse
import ctypes
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import xml.etree.ElementTree as ET


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def sha(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


class Memory(ctypes.Structure):
    _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [
        (name, ctypes.c_ulonglong) for name in
        ("totalPhys", "availPhys", "totalPage", "availPage", "totalVirtual", "availVirtual", "availExtended")]


def resources():
    value = Memory()
    value.length = ctypes.sizeof(value)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(value)):
        raise OSError("GlobalMemoryStatusEx failed")
    return {"free_physical_mib": value.availPhys // 1048576, "free_commit_mib": value.availPage // 1048576}


def symbols(output):
    result = []
    for line in output.splitlines():
        columns = line.split()
        if len(columns) >= 8 and re.fullmatch(r"\d+:", columns[0]):
            result.append({"type": columns[3], "bind": columns[4], "visibility": columns[5],
                           "index": columns[6], "name": columns[7].split("@")[0]})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", required=True, type=Path)
    parser.add_argument("--expected-source", required=True)
    parser.add_argument("--ndk", type=Path, default=Path("C:/Android/ndk/28.2.13676358"))
    parser.add_argument("--ninja", type=Path, default=Path("C:/Android/cmake/3.22.1/bin/ninja.exe"))
    args = parser.parse_args()
    if os.name != "nt" or not re.fullmatch(r"[0-9a-f]{40}", args.expected_source):
        parser.error("requires Windows and an exact source commit SHA")
    source = Path(__file__).resolve().parent
    root = source.parents[1]
    run = args.run_root
    if not run.is_absolute() or run.exists() or root == run.resolve() or root in run.resolve().parents:
        parser.error("run-root must be fresh, absolute, and outside the checkout")
    for ancestor in run.parents:
        if ancestor.exists() and (ancestor.stat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT):
            parser.error("run-root ancestors must not be reparse points")
    run.mkdir(parents=True, exist_ok=False)
    receipt = {"schema": "mta-position-build-v1", "source_sha": args.expected_source, "started_utc": now(),
               "state": "preflight", "driver_pid": os.getpid(), "workers": 1, "memory_floor_mib": 1536,
               "resource_policy": "root-authorized tiny TU-only host/ARM64 link slice, no VM/full client build",
               "commit_floor_mib": 8192, "stages": [], "android_runtime": "not_run", "game_runtime": "not_run",
               "frame_lifetime": "not_run", "apk_modified": False}

    def write():
        tmp = run / "receipt.tmp"
        tmp.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        tmp.replace(run / "receipt.json")

    env = os.environ.copy()
    env.update(CMAKE_BUILD_PARALLEL_LEVEL="1", MSBUILDDISABLENODEREUSE="1")
    env.pop("CL", None)
    env.pop("_CL_", None)
    env["CL"] = "/MP1"

    def guard():
        if subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip() != args.expected_source:
            raise RuntimeError("source HEAD changed")
        if subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True).strip():
            raise RuntimeError("source checkout dirty")
        for path, digest in receipt["input_sha256"].items():
            if sha(Path(path)) != digest:
                raise RuntimeError("source/tool input changed: " + path)

    def stage(name, argv):
        guard()
        memory = resources()
        command = "Get-CimInstance Win32_Process | Where-Object { $_.Name -match '^(emulator|qemu-system.*|MSBuild|cl|clang|clang\\+\\+|ld.lld)\\.exe$' } | Select-Object ProcessId,Name | ConvertTo-Json -Compress"
        active = subprocess.check_output(["powershell", "-NoProfile", "-Command", command], text=True).strip()
        item = {"name": name, "argv": list(map(str, argv)), "started_utc": now(), "memory": memory,
                "competing_processes": json.loads(active) if active else [], "state": "preflight"}
        receipt["stages"].append(item)
        if memory["free_physical_mib"] < 1536 or memory["free_commit_mib"] < 8192 or active:
            item["state"] = "not_run"
            receipt["state"] = "deferred_resources"
            write()
            raise RuntimeError("resource preflight deferred before " + name)
        with (run / (name + ".stdout.log")).open("wb") as out, (run / (name + ".stderr.log")).open("wb") as err:
            process = subprocess.Popen(item["argv"], cwd=root, env=env, stdout=out, stderr=err,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            item.update(pid=process.pid, state="running")
            receipt["state"] = "running"
            write()
            print(json.dumps({"stage": name, "pid": process.pid, "receipt": str(run / "receipt.json")}), flush=True)
            item["exit_code"] = process.wait()
        item.update(ended_utc=now(), state="passed" if item["exit_code"] == 0 else "failed")
        item["logs_sha256"] = {suffix: sha(run / (name + suffix)) for suffix in (".stdout.log", ".stderr.log")}
        write()
        if item["exit_code"]:
            raise RuntimeError(name + " exited " + str(item["exit_code"]))

    try:
        cmake = Path(shutil.which("cmake") or "")
        ctest = cmake.with_name("ctest.exe")
        llvm = args.ndk / "toolchains/llvm/prebuilt/windows-x86_64/bin"
        readelf = llvm / "llvm-readelf.exe"
        sysroot = llvm.parent / "sysroot/usr/lib/aarch64-linux-android/28"
        stubs = [sysroot / name for name in ("libc.so", "libdl.so", "libm.so")]
        tools = [cmake, ctest, args.ninja, args.ndk / "source.properties", args.ndk / "build/cmake/android.toolchain.cmake",
                 readelf, llvm / "clang.exe", llvm / "ld.lld.exe", *stubs]
        files = [p for p in source.rglob("*") if p.is_file()] + [root / "Shared/sdk/sha2.h", root / "Shared/sdk/sha2.hpp"]
        receipt["input_sha256"] = {str(path.resolve()): sha(path) for path in sorted(set(files + tools))}
        write()
        stage("host-configure", [cmake, "-S", source, "-B", run / "host", "-G", "Visual Studio 18 2026", "-A", "x64"])
        stage("host-build", [cmake, "--build", run / "host", "--config", "Release", "--parallel", "1", "--", "/m:1", "/nr:false", "/p:MultiProcessorCompilation=false"])
        stage("host-tests", [ctest, "--test-dir", run / "host", "-C", "Release", "--output-on-failure", "--output-junit", run / "host-tests.xml"])
        tests = ET.parse(run / "host-tests.xml").getroot().findall(".//testcase")
        if len(tests) != 1 or tests[0].get("name") != "pinned_position_candidate_boundaries" or any(t.find("failure") is not None or t.find("skipped") is not None or t.find("error") is not None for t in tests):
            raise RuntimeError("required native host test did not pass")
        stage("arm64-configure", [cmake, "-S", source, "-B", run / "arm64", "-G", "Ninja",
              "-DCMAKE_MAKE_PROGRAM=" + str(args.ninja), "-DCMAKE_TOOLCHAIN_FILE=" + str(args.ndk / "build/cmake/android.toolchain.cmake"),
              "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-28", "-DANDROID_STL=c++_static", "-DBUILD_TESTING=OFF", "-DCMAKE_BUILD_TYPE=Release"])
        stage("arm64-link", [cmake, "--build", run / "arm64", "--target", "android_position_link_probe", "--parallel", "1"])
        artifact = run / "arm64/libandroid_position_link_probe.so"
        blob = artifact.read_bytes()
        if blob[:7] != b"\x7fELF\x02\x01\x01" or struct.unpack_from("<HH", blob, 16) != (3, 183):
            raise RuntimeError("expected final ELF64 little-endian AArch64 DYN")
        stage("elf-inspect", [readelf, "-h", "-d", "--dyn-syms", "--wide", artifact])
        output = (run / "elf-inspect.stdout.log").read_text(encoding="utf-8")
        needed = set(re.findall(r"\(NEEDED\).*?\[([^\]]+)\]", output))
        if not needed or not needed <= {"libc.so", "libdl.so", "libm.so"}:
            raise RuntimeError("unexpected ELF dependency: " + repr(needed))
        dynamic = symbols(output)
        exports = {s["name"] for s in dynamic if s["index"] != "UND" and s["bind"] in ("GLOBAL", "WEAK")}
        if exports != {"MtaPositionCandidateBuildAbi"}:
            raise RuntimeError("unexpected exported symbols: " + repr(exports))
        imports = {s["name"] for s in dynamic if s["index"] == "UND" and s["bind"] == "GLOBAL"}
        providers = set()
        for stub in stubs:
            stage("stub-" + stub.stem, [readelf, "--dyn-syms", "--wide", stub])
            providers.update(s["name"] for s in symbols((run / ("stub-" + stub.stem + ".stdout.log")).read_text(encoding="utf-8")) if s["index"] != "UND")
        if (imports - providers or not {"process_vm_readv", "dl_iterate_phdr"} <= imports
                or not imports.intersection({"pread", "pread64"}) or not imports.intersection({"fstat", "fstat64"})):
            raise RuntimeError("unexpected/unresolved imports or backend removed from final link: " + repr(imports - providers))
        receipt["elf"] = {"needed": sorted(needed), "exports": sorted(exports), "global_imports": sorted(imports), "resolved_against_api28_stubs": True}
        guard()
        artifacts = run / "artifacts"
        artifacts.mkdir()
        receipt["artifacts"] = {}
        for path in (artifact, run / "host/Release/position_candidate_tests.exe"):
            destination = artifacts / path.name
            shutil.copy2(path, destination)
            destination.chmod(stat.S_IREAD)
            receipt["artifacts"][path.name] = {"path": str(destination), "bytes": destination.stat().st_size, "sha256": sha(destination)}
        receipt.update(state="passed", ended_utc=now())
        write()
        print(json.dumps({"state": "passed", "receipt": str(run / "receipt.json"), "game_runtime": "not_run"}), flush=True)
        return 0
    except Exception as error:
        if receipt["state"] != "deferred_resources":
            receipt["state"] = "failed"
        receipt.update(error=repr(error), ended_utc=now())
        write()
        print(json.dumps({"state": receipt["state"], "error": repr(error), "receipt": str(run / "receipt.json")}), flush=True)
        return 3 if receipt["state"] == "deferred_resources" else 1


if __name__ == "__main__":
    sys.exit(main())
