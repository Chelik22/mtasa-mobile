"""Prepare or explicitly build reviewed integration in the existing PC cache.

The baseline helper keeps its strict upstream-only rule. This separate schema
approves exact reviewed engine deltas and never labels integration as baseline.
Prepare reads/verifies files only: no generation, compiler or target execution.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

import build_pc as baseline
from prepare_dependencies import save, sha, verify

BASELINE = "566558c415596c288cc15166361b45effe3ab0e1"
FEATURE = "bbe02075be1f9693d4a6ea1d173f8468b2855cb2"
BASELINE_PLAN_HASH = "4f41387a101cf52bd6377c771130585edee131459e0528cebd80a01f1d2f0db4"
CAPTURE_HASH = "13431a3e4300fa64a2184f97333ca5def28612cdabee7287fbe167826c1ab4c1"
SCHEMA = "mta-pc-reviewed-integration-v1"


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def physical_inventory(source):
    files = []
    for name in baseline.TREES:
        root = source / name
        for path in root.rglob("*") if root.is_dir() else (root,):
            if path.is_symlink() or path.is_junction():
                raise ValueError("Compile input reparse path: " + str(path))
            if path.is_file():
                files.append(path.relative_to(source).as_posix())
    return sorted(files)


def other_ignored_inputs(source, inventory):
    tracked = set(baseline.git(source, "ls-files", "--", *baseline.TREES).splitlines())
    # These exact materialized trees/font are separately verified against pinned
    # dependency manifests. Hash all remaining ignored compile inputs as well.
    managed = ("vendor/cef3/cef/", "vendor/discord-rpc/discord/")
    font = "Shared/data/MTA San Andreas/MTA/cgui/unifont.ttf"
    return {name: sha(source / name) for name in inventory
            if name not in tracked and not name.startswith(managed) and name != font}


def build_command(properties):
    return [str(baseline.MSBUILD), "Build/MTASA.sln", "/t:" + ";".join(baseline.TARGETS),
            "/p:Configuration=Release", "/p:Platform=Win32", "/p:MultiProcessorCompilation=false", "/m:1", "/nr:false",
            "/nologo", "/v:minimal", "/p:ForceImportBeforeCppTargets=" + properties]


def active_targets():
    # Read process names without spawning a diagnostic CLI or touching a target.
    from ctypes import wintypes
    class Entry(ctypes.Structure):
        _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD), ("pid", wintypes.DWORD),
                    ("heap", ctypes.c_size_t), ("module", wintypes.DWORD), ("threads", wintypes.DWORD),
                    ("parent", wintypes.DWORD), ("priority", wintypes.LONG), ("flags", wintypes.DWORD),
                    ("name", wintypes.WCHAR * 260)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Process32FirstW.argtypes = kernel.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Entry)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    found = []
    try:
        entry = Entry()
        entry.size = ctypes.sizeof(entry)
        valid = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while valid:
            name = entry.name.lower()
            if name.startswith(("emulator", "qemu-system-")) or name in ("msbuild.exe", "cl.exe", "ninja.exe"):
                found.append({"pid": entry.pid, "name": name})
            valid = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    return found


def source_delta(source, approved):
    if baseline.git(source, "diff", "--name-only", approved, "--", *baseline.TREES):
        raise ValueError("Compile trees differ from the reviewed feature")
    if baseline.git(source, "status", "--porcelain", "--untracked-files=no"):
        raise ValueError("Commit tracked source changes before preparation/build")
    names = baseline.git(source, "diff", "--name-only", BASELINE, "--", *baseline.TREES).splitlines()
    result = {}
    for name in names:
        path = source / name
        data = path.read_bytes()
        import hashlib
        result[name] = {"rawSha256": sha(path), "lfSha256": hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()}
    return result


def guard_sdk(plan):
    if baseline.sdk_inputs(Path(plan["dxsdk"])) != plan["sdkInputHashes"]:
        raise ValueError("Integration SDK file set or bytes changed")


def dependency_check(source, old):
    cache = Path(old["cache"])
    receipt_path = Path(old["dependencyReceipt"])
    if sha(receipt_path) != old["dependencyReceiptSha256"]:
        raise ValueError("Dependency preparation receipt changed")
    receipt = read(receipt_path)
    lock = source / "experiments/pc-baseline/dependencies.lock.json"
    if receipt["status"] != "passed" or receipt["lockSha256"] != sha(lock):
        raise ValueError("Pinned dependency preparation did not pass")
    for item in read(lock)["artifacts"]:
        verify(cache / item["filename"], item)
        entry = next(row for row in receipt["artifacts"] if row["id"] == item["id"])
        if item["kind"] != "file":
            manifest = Path(entry["treeManifest"])
            if sha(manifest) != entry["treeSha256"]:
                raise ValueError("Cached dependency tree manifest changed")
            actual = {str(p.relative_to(entry["extractedPath"])): {"sha256": sha(p), "bytes": p.stat().st_size}
                      for p in Path(entry["extractedPath"]).rglob("*") if p.is_file()}
            if actual != read(manifest)["files"]:
                raise ValueError("Cached dependency tree changed: " + item["id"])
    baseline.verify_materialized(source, cache)


def graph_check(source, old):
    graph = {}
    pending = [source / "Build" / (name + ".vcxproj") for name in (*baseline.CLIENT_PROJECTS, "XML")]
    while pending:
        path = pending.pop().resolve()
        if str(path) in graph:
            continue
        if not path.is_relative_to((source / "Build").resolve()):
            raise ValueError("Generated project escapes the existing cache")
        refs = [n.get("Include") for n in ET.parse(path).getroot().findall(".//m:ProjectReference", baseline.NS)]
        graph[str(path)] = {"path": str(path), "sha256": sha(path), "references": refs}
        pending.extend((path.parent / name).resolve() for name in refs)
    if graph != old["projects"] or sha(source / "Build/MTASA.sln") != old["solutionSha256"]:
        raise ValueError("Cached project graph differs from the proven baseline")
    if sha(old["resourceProps"]) != old["resourcePropsSha256"]:
        raise ValueError("Resource SDK properties changed")
    if sha(source / "Build/MTASA.sln.metaproj") != old["targetValidation"]["metaprojectSha256"]:
        raise ValueError("Cached solution traversal changed")
    return graph


def preservation_check(plan):
    capture = Path(plan["baselineCapture"])
    if sha(capture) != plan["baselineCaptureSha256"]:
        raise ValueError("Immutable baseline capture manifest changed")
    data = read(capture)
    for name, item in {**data["copies"], **data["extras"]}.items():
        path = capture.parent / "baseline-artifacts" / name
        if path.stat().st_size != item["bytes"] or sha(path) != item["sha256"]:
            raise ValueError("Immutable baseline artifact changed: " + name)
    if baseline.git(Path(plan["source"]), "rev-parse", "feat/pc-baseline-cli") != BASELINE:
        raise ValueError("Original baseline branch was moved")


def validate(plan):
    if plan["schema"] != SCHEMA or plan["state"] != "ready_source_only":
        raise ValueError("A ready integration plan is required")
    guard_sdk(plan)  # Exact set: added shadowing headers must fail before Popen.
    if plan["approvedFeatureSha"] != FEATURE or plan["baselinePlanSha256"] != BASELINE_PLAN_HASH or plan["baselineCaptureSha256"] != CAPTURE_HASH:
        raise ValueError("Integration approval or baseline provenance changed")
    source = Path(plan["source"])
    if baseline.git(source, "rev-parse", "HEAD") != plan["sourceSha"]:
        raise ValueError("Integration source HEAD changed")
    if source_delta(source, plan["approvedFeatureSha"]) != plan["compileTreeDelta"]:
        raise ValueError("Approved source delta changed")
    inventory = physical_inventory(source)
    if inventory != plan["compilePhysicalFileSet"] or other_ignored_inputs(source, inventory) != plan["otherIgnoredInputHashes"]:
        raise ValueError("Compile physical file set changed, including ignored inputs")
    if sha(plan["baselinePlan"]) != plan["baselinePlanSha256"]:
        raise ValueError("Original baseline plan changed")
    old = read(plan["baselinePlan"])
    if plan["command"] != build_command(old["resourceProps"]) or plan["environment"] != {"_CL_": "/MP1", "DXSDK_DIR": old["dxsdk"] + os.sep}:
        raise ValueError("Reviewed integration command or environment changed")
    graph_check(source, old)
    dependency_check(source, old)
    preservation_check(plan)
    if sha(baseline.MSBUILD) != plan["msbuildSha256"] or sha(__file__) != plan["helperSha256"]:
        raise ValueError("Reviewed build helper or MSBuild changed")
    for name, value in plan["nativePathOsSdkInputs"].items():
        if sha(name) != value:
            raise ValueError("Native-path Windows SDK input changed")
    return source


def prepare(args):
    if args.plan.exists():
        raise ValueError("Use a fresh integration plan path")
    old = read(args.baseline_plan)
    if sha(args.baseline_plan) != BASELINE_PLAN_HASH or sha(args.capture) != CAPTURE_HASH:
        raise ValueError("Expected the immutable proven baseline plan and capture")
    source = Path(old["source"])
    if old["schema"] != "mta-pc-baseline-v1" or old["sourceSha"] != BASELINE or old["state"] != "ready":
        raise ValueError("Expected the proven 566558 baseline preparation plan")
    approved = baseline.git(source, "rev-parse", FEATURE)
    command = build_command(old["resourceProps"])
    inventory = physical_inventory(source)
    plan = {"schema": SCHEMA, "state": "ready_source_only", "preparedUtc": baseline.utc(),
            "source": str(source), "sourceSha": baseline.git(source, "rev-parse", "HEAD"), "upstreamPin": baseline.PIN,
            "baselineSourceSha": BASELINE, "approvedFeatureSha": approved,
            "baselinePlan": str(args.baseline_plan), "baselinePlanSha256": sha(args.baseline_plan),
            "baselineCapture": str(args.capture), "baselineCaptureSha256": sha(args.capture),
            "compileTreeDelta": source_delta(source, approved), "compilePhysicalFileSet": inventory,
            "otherIgnoredInputHashes": other_ignored_inputs(source, inventory),
            "dxsdk": old["dxsdk"], "sdkInputHashes": baseline.sdk_inputs(Path(old["dxsdk"])),
            "projects": graph_check(source, old), "solutionSha256": old["solutionSha256"],
            "resourceProps": old["resourceProps"], "resourcePropsSha256": old["resourcePropsSha256"],
            "dependencyReceipt": old["dependencyReceipt"], "dependencyReceiptSha256": old["dependencyReceiptSha256"],
            "msbuildSha256": sha(baseline.MSBUILD), "helperSha256": sha(__file__), "command": command,
            "nativePathOsSdkInputs": {str(path): sha(path) for path in (
                Path("C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0/shared/bcrypt.h"),
                Path("C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x86/bcrypt.lib"))},
            "environment": {"_CL_": "/MP1", "DXSDK_DIR": old["dxsdk"] + os.sep},
            "priority": "Normal", "maxWorkers": 1, "minimumMemoryMiB": {"physical": 2048, "commit": 8192},
            "projectGeneration": "not_run; reuse exact existing 34-project graph", "objectCopies": False,
            "clientBuild": "not_run", "baselineVerification": "not_applicable", "gameExecution": "not_run", "G4": "not_run"}
    validate(plan)
    if plan["sdkInputHashes"] != old["sdkInputHashes"]:
        raise ValueError("Scoped SDK differs from the proven baseline")
    save(args.plan, plan)
    print(json.dumps({"state": plan["state"], "sourceSha": plan["sourceSha"], "planSha256": sha(args.plan),
                      "changedCompileFiles": len(plan["compileTreeDelta"]), "projects": len(plan["projects"]), "sdkFiles": len(plan["sdkInputHashes"])}))


def build(plan_path, run_root):
    # Separate explicit command: preparation never invokes this branch.
    run_root.mkdir(parents=True, exist_ok=False)
    record = {"schema": SCHEMA, "status": "running", "pid": os.getpid(), "startedUtc": baseline.utc(),
              "clientBuild": "not_run", "baselineVerification": "not_applicable", "gameExecution": "not_run", "G4": "not_run"}
    result = run_root / "build.run.json"
    code, lock, child = 2, None, None
    acquired = False
    try:
        save(result, record)
        plan = read(plan_path)
        source = validate(plan)
        lock = source / "Build/.pc-integration-active.lock"
        if (source / "Build/.pc-baseline-active.lock").exists():
            raise ValueError("Baseline worker lock exists; inspect its process before proceeding")
        with os.fdopen(os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY), "w") as stream:
            acquired = True
            json.dump({"runnerPid": os.getpid(), "runRoot": str(run_root)}, stream)
        memory = baseline.memory_mib()
        record["memory"] = memory
        record["activeTargetsAtPreflight"] = active_targets()
        if record["activeTargetsAtPreflight"]:
            record["status"] = "deferred_active_targets"
            code = 3
            return code
        if memory["physicalFreeMiB"] < 2048 or memory["commitFreeMiB"] < 8192:
            record["status"] = "deferred_memory"
            code = 3
            return code
        before = {name: sha(source / "Bin" / name) for name in baseline.OUTPUTS}
        record.update(source=str(source), sourceSha=plan["sourceSha"], targets=";".join(baseline.TARGETS), planSha256=sha(plan_path),
                      command=plan["command"], priority="Normal", maxWorkers=1, compilerOverride="/MP1", beforeArtifacts=before)
        environment = os.environ.copy()
        environment.update(plan["environment"])
        with (run_root / "stdout.log").open("xb") as out, (run_root / "stderr.log").open("xb") as err:
            child = subprocess.Popen(plan["command"], cwd=source, env=environment, stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                     creationflags=subprocess.CREATE_NO_WINDOW | subprocess.NORMAL_PRIORITY_CLASS)
            record.update(msbuildPid=child.pid, clientBuild="in_progress")
            save(lock, {"runnerPid": os.getpid(), "msbuildPid": child.pid, "runRoot": str(run_root)})
            save(result, record)
            code = child.wait()
        record["artifacts"] = {name: {"sha256": sha(source / "Bin" / name), "bytes": (source / "Bin" / name).stat().st_size}
                               for name in baseline.OUTPUTS}
        record["changedOutputs"] = [name for name, item in record["artifacts"].items() if before[name] != item["sha256"]]
        required = {"MTA/core.dll", "MTA/loader.dll", "MTA/mtasa.dll"}
        if code or not required.issubset(record["changedOutputs"]):
            raise ValueError("Integration build failed or required Core/loader/proxy outputs were reused")
        validate(plan)
        record.update(status="passed", clientBuild="passed")
        return 0
    except Exception as error:
        record.update(status="failed", error=str(error))
        code = code or 2
        return code
    finally:
        live = child is not None and child.poll() is None
        if live:
            record.update(status="runner_interrupted_child_alive", msbuildPid=child.pid)
        record.update(exitCode=code, finishedUtc=baseline.utc())
        save(result, record)
        if not live:
            (run_root / "build.exitcode").write_text(str(code), encoding="ascii")
        if acquired and not live:
            lock.unlink()
        print(json.dumps({"status": record["status"], "receipt": str(result), "gameExecution": "not_run"}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("prepare", "build"))
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--baseline-plan", type=Path)
    parser.add_argument("--capture", type=Path)
    parser.add_argument("--run-root", type=Path)
    args = parser.parse_args()
    if args.action == "prepare":
        if not args.baseline_plan or not args.capture:
            parser.error("prepare requires --baseline-plan and --capture")
        prepare(args)
        return 0
    if not args.run_root:
        parser.error("build requires --run-root")
    return build(args.plan, args.run_root)


if __name__ == "__main__":
    raise SystemExit(main())
