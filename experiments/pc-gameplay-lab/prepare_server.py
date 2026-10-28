"""Create a fresh gameplay server using verified H1 binaries, never its DB/logs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import xml.etree.ElementTree as ET

NATIVE_FILES = (
    "MTA Server64.exe", "x64/core.dll", "x64/deathmatch.dll", "x64/xmll.dll",
    "x64/lua5.1.dll", "x64/pcre2.dll", "x64/pthread.dll", "x64/net_experimental.dll",
)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def prepare(plan_path, destination, net_module=None, net_module_sha256=None):
    plan = json.loads(plan_path.read_text(encoding="utf-8-sig"))
    if plan["schema"] != "mta-mobile-h1-scenario-v1" or plan["state"] != "ready":
        raise ValueError("Expected the existing verified H1 preparation plan")
    here = Path(__file__).resolve().parent
    fixture = here.parent / "mobile-world-fixture"
    inputs = {}
    for name in NATIVE_FILES:
        item = plan["files"][name]
        # Build outputs may be rebuilt later. The passed H1 runtime retains its
        # own verified copies; never depend on a mutable build-cache DLL here.
        origin = Path(plan["runtime"]) / name
        if digest(origin) != item["sha256"]:
            raise ValueError("H1 native input changed: " + name)
        inputs[name] = origin
    if bool(net_module) != bool(net_module_sha256):
        raise ValueError("A new net module requires its explicit verified SHA256")
    if net_module:
        if digest(net_module) != net_module_sha256:
            raise ValueError("New server net module SHA256 mismatch")
        inputs["x64/net_experimental.dll"] = net_module
    inputs.update({
        "mods/deathmatch/mtaserver.conf": here / "server-config.xml",
        "mods/deathmatch/acl.xml": here / "server-acl.xml",
        "mods/deathmatch/resources/mobilelab-spawn/meta.xml": here / "bootstrap/meta.xml",
        "mods/deathmatch/resources/mobilelab-spawn/server.lua": here / "bootstrap/server.lua",
    })
    for name in ("meta.xml", "server.lua", "client.lua", "http-witness.txt"):
        inputs["mods/deathmatch/resources/mobile-world-fixture/" + name] = fixture / name
    for path in inputs.values():
        if path.is_symlink() or not path.is_file():
            raise ValueError("Expected a regular explicit input: " + str(path))
    for name in ("server-config.xml", "server-acl.xml", "bootstrap/meta.xml"):
        ET.parse(here / name)
    if ET.parse(here / "server-config.xml").getroot().findtext("serverip") != "127.0.0.1":
        raise ValueError("Gameplay lab must bind loopback")
    # A fresh directory prevents silently inheriting any old account DB/cache.
    destination.mkdir(parents=True, exist_ok=False)
    receipt = {"schema": "mta-gameplay-server-preparation-v1", "runtime": "not_run",
               "source_h1_plan_sha256": digest(plan_path), "root": str(destination), "files": {}}
    for name, origin in inputs.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        expected = digest(origin)
        shutil.copyfile(origin, target)
        if digest(target) != expected:
            raise ValueError("Copied input hash mismatch: " + name)
        receipt["files"][name] = {"origin": str(origin), "sha256": expected, "bytes": target.stat().st_size}
    for name in ("logs", "databases/global", "databases/system", "resource-cache"):
        (destination / "mods/deathmatch" / name).mkdir(parents=True, exist_ok=True)
    (destination / "preparation.json").write_text(json.dumps(receipt, indent=2), encoding="utf-8")
    return receipt


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--h1-plan", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--net-module", type=Path)
    parser.add_argument("--net-module-sha256")
    args = parser.parse_args()
    print(json.dumps(prepare(args.h1_plan, args.destination, args.net_module, args.net_module_sha256), indent=2))
