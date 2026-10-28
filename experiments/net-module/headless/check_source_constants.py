"""Extract/check only the enum values needed by this narrow source-derived client."""

import argparse
from pathlib import Path
import re


def enum_value(path, enum_name, item):
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", path.read_text(encoding="utf-8"), flags=re.S)
    match = re.search(r"enum\s+" + enum_name + r"\s*\{(.*?)\}", text, flags=re.S)
    if not match:
        raise ValueError(f"enum not found: {path}:{enum_name}")
    value = -1
    for entry in match.group(1).split(","):
        entry = entry.strip()
        if not entry:
            continue
        parts = entry.split("=", 1)
        value = int(parts[1].strip(), 0) if len(parts) == 2 else value + 1
        if parts[0].strip() == item:
            return value
    raise ValueError(f"enum item not found: {path}:{item}")


def render(root):
    rpc = root / "Server/mods/deathmatch/logic/CRPCFunctions.h"
    element = root / "Server/mods/deathmatch/logic/CElement.h"
    game = root / "Client/sdk/game/CGame.h"
    values = {
        "rpc_ingame": enum_value(rpc, "eRPCFunctions", "PLAYER_INGAME_NOTICE"),
        "rpc_initial_data": enum_value(rpc, "eRPCFunctions", "INITIAL_DATA_STREAM"),
        "entity_vehicle": enum_value(element, "EElementType", "VEHICLE"),
        "game_version_us10": enum_value(game, "eGameVersion", "VERSION_US_10"),
    }
    lua = (root / "vendor/lua/src/lua.h").read_text(encoding="utf-8")
    for name in ("NIL", "BOOLEAN", "LIGHTUSERDATA", "NUMBER", "STRING", "TABLE", "USERDATA"):
        found = re.search(r"#define\s+LUA_T" + name + r"\s+(\d+)", lua)
        if not found:
            raise ValueError("Lua type macro not found: " + name)
        values["lua_" + name.lower()] = int(found.group(1))
    # Network-only marker is defined beside the actual server argument serializer.
    text = (root / "Server/mods/deathmatch/logic/lua/CLuaArgument.h").read_text(encoding="utf-8")
    values["lua_long_string"] = int(re.search(r"#define\s+LUA_TSTRING_LONG\s+(\d+)", text).group(1))
    declarations = "\n".join(f"    inline constexpr unsigned char {name} = {value};" for name, value in values.items())
    return "#pragma once\n// Generated from this checkout's authoritative MTA enums and Lua macros.\nnamespace headless::source\n{\n" + declarations + "\n}\n"


def main():
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    expected = render(here.parents[2])
    target = here / "SourceConstants.h"
    if args.write:
        target.write_text(expected, encoding="utf-8")
    else:
        pattern = r"inline\s+constexpr\s+unsigned\s+char\s+(\w+)\s*=\s*(\d+)\s*;"
        wanted = re.findall(pattern, expected)
        actual = re.findall(pattern, target.read_text(encoding="utf-8"))
        if len(actual) != len(wanted) or dict(actual) != dict(wanted):
            raise SystemExit("FAIL: SourceConstants.h differs from authoritative source enums")
    print("PASS source-derived enum values; no server/client runtime executed")


if __name__ == "__main__":
    main()
