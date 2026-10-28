"""Prepare/inspect a pinned PC baseline. Building requires the explicit build command."""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import json
import os
from pathlib import Path
import shutil
import subprocess
import xml.etree.ElementTree as ET

from prepare_dependencies import HERE, save, sha, verify

PIN = "a84e0c77c66f596b0c72def1c857253568208a2b"
TREES = ("Client", "Server", "Shared", "vendor", "utils", "premake5.lua")
CLIENT_PROJECTS = ("Client Launcher", "Loader", "Loader Proxy", "Client Core", "Game SA", "Multiplayer SA", "GUI",
                   "Client Deathmatch", "Client Webbrowser", "CEFLauncher", "CEFLauncher DLL")
TARGETS = ["Client\\" + name for name in CLIENT_PROJECTS] + ["Shared\\XML"]
OUTPUTS = ("Multi Theft Auto.exe", "MTA/loader.dll", "MTA/mtasa.dll", "MTA/core.dll", "MTA/game_sa.dll",
           "MTA/multiplayer_sa.dll", "MTA/cgui.dll", "mods/deathmatch/client.dll", "MTA/cefweb.dll",
           "MTA/cef/CEFLauncher.exe", "MTA/cef/CEFLauncher_DLL.dll", "MTA/xmll.dll")
MSBUILD = Path("C:/Program Files/Microsoft Visual Studio/18/Community/MSBuild/Current/Bin/MSBuild.exe")
NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}


def utc():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def git(source, *arguments):
    return subprocess.check_output(["git", "-C", str(source), *arguments], text=True).strip()


def memory_mib():
    class Memory(ctypes.Structure):
        _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [(key, ctypes.c_ulonglong) for key in
                    ("total", "available", "totalPage", "availablePage", "totalVirtual", "availableVirtual", "extended")]
    value = Memory()
    value.length = ctypes.sizeof(value)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(value)):
        raise ctypes.WinError()
    return {"physicalFreeMiB": value.available // 1048576, "commitFreeMiB": value.availablePage // 1048576}


def single_root(folder):
    directories = [p for p in folder.iterdir() if p.is_dir()]
    if len(directories) != 1:
        raise ValueError("Expected a single upstream archive root: " + str(folder))
    return directories[0]


def copy_verified_tree(source, target, composed_subtree=None):
    files = {p.relative_to(source): p for p in source.rglob("*") if p.is_file()}
    if target.exists():
        unknown = {p.relative_to(target) for p in target.rglob("*") if p.is_file()
                   and not (composed_subtree and p.is_relative_to(target / composed_subtree))} - set(files)
        if unknown:
            raise ValueError("Unowned files in dependency destination: " + str(target))
    for relative, path in files.items():
        destination = target / relative
        if destination.exists():
            if sha(destination) != sha(path):
                raise ValueError("Refusing to replace changed dependency: " + str(destination))
        else:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)


def inspect(source, cache, dxsdk):
    if os.name != "nt":
        raise ValueError("Installed Windows toolchain is required")
    if subprocess.run(["git", "-C", str(source), "diff", "--quiet", PIN, "--", *TREES]).returncode:
        raise ValueError("Baseline compile trees differ from pinned upstream")
    manifest = json.loads((HERE / "dependencies.lock.json").read_text())
    receipt_path = cache / "dependencies-result.json"
    receipt = json.loads(receipt_path.read_text())
    if receipt["status"] != "passed" or receipt["lockSha256"] != sha(HERE / "dependencies.lock.json"):
        raise ValueError("Pinned dependency preparation did not pass")
    for item in manifest["artifacts"]:
        verify(cache / item["filename"], item)
        entry = next(row for row in receipt["artifacts"] if row["id"] == item["id"])
        if item["kind"] != "file":
            tree_path = Path(entry["treeManifest"])
            if sha(tree_path) != entry["treeSha256"]:
                raise ValueError("Dependency tree manifest changed")
            tree = json.loads(tree_path.read_text())
            actual = {str(p.relative_to(entry["extractedPath"])): {"sha256": sha(p), "bytes": p.stat().st_size}
                      for p in Path(entry["extractedPath"]).rglob("*") if p.is_file()}
            if actual != tree["files"]:
                raise ValueError("Cached extracted dependency changed: " + item["id"])
    mfc = list((cache / "mfc-headers-extracted").rglob("afxres.h"))
    if len(mfc) != 1:
        raise ValueError("Expected exact resource-only MFC header location")
    missing = [str(p) for p in (MSBUILD, source / "utils/premake5.exe", source / "utils/gen_language_list.exe",
                               dxsdk / "Include/d3dx9.h", dxsdk / "Lib/x86/d3dx9.lib", dxsdk / "Lib/x86/dxerr.lib") if not p.is_file()]
    return {"schema": "mta-pc-baseline-v1", "source": str(source), "sourceSha": git(source, "rev-parse", "HEAD"),
            "upstreamPin": PIN, "compileTreesMatchPin": True, "cache": str(cache), "dependencyReceipt": str(receipt_path),
            "dependencyReceiptSha256": sha(receipt_path), "mfcInclude": str(mfc[0].parent), "dxsdk": str(dxsdk),
            "targets": TARGETS, "configuration": "Release", "platform": "Win32", "maxWorkers": 1, "compilerOverride": "/MP1",
            "missing": missing, "state": "blocked" if missing else "inspected", "G1": "not_run", "clientBuild": "not_run",
            "gameExecution": "not_run", "memory": memory_mib()}


def materialize(source, cache):
    copy_verified_tree(single_root(cache / "cef-extracted"), source / "vendor/cef3/cef")
    copy_verified_tree(single_root(cache / "discord-extracted"), source / "vendor/discord-rpc/discord", Path("thirdparty/rapidjson"))
    copy_verified_tree(single_root(cache / "rapidjson-extracted"), source / "vendor/discord-rpc/discord/thirdparty/rapidjson")
    font = source / "Shared/data/MTA San Andreas/MTA/cgui/unifont.ttf"
    cached_font = cache / "unifont-16.0.04.ttf"
    if font.exists() and sha(font) != sha(cached_font):
        raise ValueError("Refusing to replace existing font")
    if not font.exists():
        font.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(cached_font, font)


def verify_materialized(source, cache):
    components = ((single_root(cache / "cef-extracted"), source / "vendor/cef3/cef", None),
                  (single_root(cache / "discord-extracted"), source / "vendor/discord-rpc/discord", Path("thirdparty/rapidjson")),
                  (single_root(cache / "rapidjson-extracted"), source / "vendor/discord-rpc/discord/thirdparty/rapidjson", None))
    for original, destination, composed in components:
        expected = {str(p.relative_to(original)): sha(p) for p in original.rglob("*") if p.is_file()}
        actual = {str(p.relative_to(destination)): sha(p) for p in destination.rglob("*") if p.is_file()
                  and not (composed and p.is_relative_to(destination / composed))}
        if expected != actual:
            raise ValueError("Materialized source dependency changed: " + str(destination))
    if sha(source / "Shared/data/MTA San Andreas/MTA/cgui/unifont.ttf") != sha(cache / "unifont-16.0.04.ttf"):
        raise ValueError("Materialized font changed")


def prepare(source, cache, dxsdk, plan):
    value = inspect(source, cache, dxsdk)
    if value["missing"]:
        return value
    materialize(source, cache)
    environment = os.environ.copy()
    environment["DXSDK_DIR"] = str(dxsdk) + os.sep
    # Call generation directly: the upstream .bat also downloads and creates shell shortcuts.
    log = plan.with_suffix(".premake.stdout.log")
    error_log = plan.with_suffix(".premake.stderr.log")
    with log.open("wb") as out, error_log.open("wb") as err:
        process = subprocess.Popen([str(source / "utils/premake5.exe"), "vs2026"], cwd=source, env=environment,
                                   stdin=subprocess.DEVNULL, stdout=out, stderr=err, creationflags=subprocess.CREATE_NO_WINDOW)
        value["projectGeneration"] = {"pid": process.pid, "stdout": str(log), "stderr": str(error_log), "exitCode": process.wait()}
    if value["projectGeneration"]["exitCode"]:
        raise ValueError("Premake generation failed; inspect logs")
    # Solution target names retain spaces on this MSBuild version. Inventory the
    # real generated traversal rather than assuming a sanitized name mapping.
    environment["MSBUILDEMITSOLUTION"] = "1"
    command = [str(MSBUILD), "Build/MTASA.sln", "/t:ValidateSolutionConfiguration", "/p:Configuration=Release",
               "/p:Platform=Win32", "/m:1", "/nr:false", "/nologo", "/v:minimal"]
    with plan.with_suffix(".targets.stdout.log").open("wb") as out, plan.with_suffix(".targets.stderr.log").open("wb") as err:
        process = subprocess.Popen(command, cwd=source, env=environment, stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                   creationflags=subprocess.CREATE_NO_WINDOW)
        value["targetValidation"] = {"pid": process.pid, "command": command, "exitCode": process.wait(),
                                     "scope": "ValidateSolutionConfiguration only; no compiler/project build targets executed"}
    if value["targetValidation"]["exitCode"]:
        raise ValueError("Solution configuration validation failed")
    meta_path = source / "Build/MTASA.sln.metaproj"
    actual_targets = {node.get("Name") for node in ET.parse(meta_path).getroot().findall("m:Target", NS)}
    if not set(TARGETS).issubset(actual_targets):
        raise ValueError("Expected solution build targets are absent")
    value["targetValidation"].update(allPresent=True, metaprojectSha256=sha(meta_path))
    properties = ET.Element("Project", xmlns=NS["m"])
    group = ET.SubElement(properties, "ItemDefinitionGroup")
    resources = ET.SubElement(group, "ResourceCompile")
    ET.SubElement(resources, "AdditionalIncludeDirectories").text = value["mfcInclude"] + ";%(AdditionalIncludeDirectories)"
    props_path = plan.with_suffix(".resource-sdk.props")
    ET.ElementTree(properties).write(props_path, encoding="utf-8", xml_declaration=True)
    projects = {}
    pending = [source / "Build" / (name + ".vcxproj") for name in (*CLIENT_PROJECTS, "XML")]
    while pending:
        path = pending.pop().resolve()
        if str(path) in projects:
            continue
        if not path.is_relative_to((source / "Build").resolve()):
            raise ValueError("Project reference escapes generated build directory")
        project = ET.parse(path).getroot()
        references = [node.get("Include") for node in project.findall(".//m:ProjectReference", NS)]
        projects[str(path)] = {"path": str(path), "sha256": sha(path), "references": references}
        pending.extend((path.parent / reference).resolve() for reference in references)
    value.update(state="ready", projects=projects, resourceProps=str(props_path), resourcePropsSha256=sha(props_path),
                 solutionSha256=sha(source / "Build/MTASA.sln"),
                 sdkInputHashes={str(path): sha(path) for part in ("Include", "Lib/x86") for path in (dxsdk / part).rglob("*") if path.is_file()})
    return value


def build(plan_path, run_root):
    # Caller must authorize this separate command after reviewing the preparation receipt.
    if run_root.exists():
        raise ValueError("Use a new attempt directory; do not overwrite prior build evidence")
    run_root.mkdir(parents=True)
    value = {"status": "running", "pid": os.getpid(), "startedUtc": utc(), "clientBuild": "not_run", "G1": "not_run", "gameExecution": "not_run"}
    result = run_root / "build.run.json"
    exit_path = run_root / "build.exitcode"
    code = 2
    lock = None
    acquired = False
    active_process = None
    try:
        save(result, value)
        plan = json.loads(plan_path.read_text())
        if plan["schema"] != "mta-pc-baseline-v1" or plan["state"] != "ready":
            raise ValueError("A ready preparation manifest is required")
        source, cache, dxsdk = Path(plan["source"]), Path(plan["cache"]), Path(plan["dxsdk"])
        lock = source / "Build/.pc-baseline-active.lock"
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
        acquired = True
        with os.fdopen(descriptor, "w") as stream:
            json.dump({"runnerPid": os.getpid(), "runRoot": str(run_root)}, stream)
        current = inspect(source, cache, dxsdk)
        if current["missing"] or current["sourceSha"] != plan["sourceSha"]:
            raise ValueError("Prepared baseline inputs changed")
        verify_materialized(source, cache)
        if git(source, "status", "--porcelain", "--untracked-files=no"):
            raise ValueError("Commit tracked changes before compiling")
        for item in plan["projects"].values():
            if sha(item["path"]) != item["sha256"]:
                raise ValueError("Generated project changed")
        if sha(source / "Build/MTASA.sln") != plan["solutionSha256"]:
            raise ValueError("Generated solution changed")
        for path, expected in plan["sdkInputHashes"].items():
            if sha(path) != expected:
                raise ValueError("SDK input changed")
        if sha(plan["resourceProps"]) != plan["resourcePropsSha256"]:
            raise ValueError("Resource SDK properties changed")
        memory = memory_mib()
        value["memory"] = memory
        if memory["physicalFreeMiB"] < 2048 or memory["commitFreeMiB"] < 8192:
            value["status"] = "deferred_memory"
            code = 3
            return 3
        arguments = [str(MSBUILD), "Build/MTASA.sln", "/t:" + ";".join(TARGETS), "/p:Configuration=Release", "/p:Platform=Win32",
                     "/p:MultiProcessorCompilation=false", "/m:1", "/nr:false", "/nologo", "/v:minimal",
                     "/p:ForceImportBeforeCppTargets=" + plan["resourceProps"]]
        value.update(source=str(source), sourceSha=plan["sourceSha"], upstreamPin=PIN, targets=";".join(TARGETS),
                     configuration="Release", platform="Win32", trackedDirty="", maxWorkers=1, compilerOverride="/MP1", command=arguments,
                     planSha256=sha(plan_path), beforeArtifacts={name: sha(source / "Bin" / name) for name in OUTPUTS if (source / "Bin" / name).is_file()})
        environment = os.environ.copy()
        environment.update(_CL_="/MP1", DXSDK_DIR=str(dxsdk) + os.sep)
        with (run_root / "stdout.log").open("wb") as out, (run_root / "stderr.log").open("wb") as err:
            kernel = ctypes.windll.kernel32
            previous = kernel.SetErrorMode(0x0001 | 0x0002 | 0x8000)
            try:
                process = subprocess.Popen(arguments, cwd=source, env=environment, stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                           creationflags=subprocess.CREATE_NO_WINDOW | subprocess.BELOW_NORMAL_PRIORITY_CLASS)
            finally:
                kernel.SetErrorMode(previous)
            active_process = process
            value.update(msbuildPid=process.pid, clientBuild="in_progress")
            save(lock, {"runnerPid": os.getpid(), "msbuildPid": process.pid, "runRoot": str(run_root)})
            save(result, value)
            code = process.wait()
        value["artifacts"] = {name: {"sha256": sha(source / "Bin" / name), "bytes": (source / "Bin" / name).stat().st_size}
                              for name in OUTPUTS if (source / "Bin" / name).is_file()}
        missing = set(OUTPUTS) - set(value["artifacts"])
        if code or missing:
            raise ValueError("Client build failed or outputs missing: " + str(sorted(missing)))
        if all(value["beforeArtifacts"].get(name) == item["sha256"] for name, item in value["artifacts"].items()):
            raise ValueError("No changed client outputs; existing cache is not a fresh build proof")
        value.update(status="passed", clientBuild="passed")
        return 0
    except Exception as error:
        value.update(status="failed", error=str(error))
        code = code or 2
        return code
    finally:
        if active_process is not None and active_process.poll() is None:
            value.update(status="runner_interrupted_child_alive", msbuildPid=active_process.pid)
        value.update(exitCode=code, finishedUtc=utc())
        save(result, value)
        exit_path.write_text(str(code), encoding="ascii")
        if acquired and (active_process is None or active_process.poll() is not None):
            lock.unlink()
        print(json.dumps({"status": value["status"], "clientBuild": value["clientBuild"], "G1": "not_run", "evidence": str(result)}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("inspect", "materialize", "prepare", "build"))
    parser.add_argument("--source", type=Path, default=HERE.parents[1])
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--dxsdk", type=Path)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--run-root", type=Path)
    args = parser.parse_args()
    if args.action == "build":
        if not args.run_root:
            parser.error("build requires --run-root")
        return build(args.plan.resolve(), args.run_root.resolve())
    if not args.cache or not args.dxsdk:
        parser.error("inspect/prepare require --cache and --dxsdk")
    args.plan.parent.mkdir(parents=True, exist_ok=True)
    value = {"state": "preparing", "pid": os.getpid(), "startedUtc": utc(), "G1": "not_run", "clientBuild": "not_run", "gameExecution": "not_run"}
    save(args.plan, value)
    try:
        function = prepare if args.action == "prepare" else inspect
        parameters = (args.source.resolve(), args.cache.resolve(), args.dxsdk.resolve())
        value = function(*parameters, args.plan.resolve()) if args.action == "prepare" else function(*parameters)
        if args.action == "materialize":
            materialize(parameters[0], parameters[1])
            value["dependenciesMaterialized"] = True
    except Exception as error:
        value.update(state="blocked", error=str(error))
    finally:
        save(args.plan, value)
    print(json.dumps(value))
    return 0 if value["state"] in ("inspected", "ready") else 2


if __name__ == "__main__":
    raise SystemExit(main())
