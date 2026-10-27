"""Package validation only. This does not execute MTA or Lua."""

from pathlib import Path
import xml.etree.ElementTree as ET


def main():
    root = Path(__file__).resolve().parent
    meta = ET.parse(root / "meta.xml").getroot()
    assert meta.tag == "meta"
    scripts = meta.findall("script")
    assert len(scripts) == 1, "Expected exactly one source-only server script"
    script = scripts[0]
    assert script.attrib == {"src": "server.lua", "type": "server"}
    assert (root / "server.lua").is_file()
    assert not (root / "client.lua").exists()
    print("PASS package: one source-only server.lua, no client.lua; B0/H1 NOT RUN")


if __name__ == "__main__":
    main()
