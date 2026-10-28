"""Check resource packaging only; this deliberately makes no runtime claims."""

from pathlib import Path
import xml.etree.ElementTree as ET


def main():
    root = Path(__file__).resolve().parent
    meta = ET.parse(root / "meta.xml").getroot()
    assert meta.tag == "meta", "Expected meta root"
    scripts = meta.findall("script")
    assert len(scripts) == 2, "Expected server and client scripts"
    assert {script.get("type") for script in scripts} == {"server", "client"}
    for script in scripts:
        path = (root / script.attrib["src"]).resolve()
        assert path.is_relative_to(root), "Script path escapes resource"
        assert path.is_file(), f"Missing script: {path.name}"
        assert path.suffix == ".lua", "Expected Lua source"
    files = meta.findall("file")
    assert [item.get("src") for item in files] == ["http-witness.txt"]
    witness = root / "http-witness.txt"
    assert witness.read_bytes().rstrip(b"\r\n") == b"MTA mobile HTTP witness v1"
    print("PASS: source scripts and HTTP witness packaged; runtime NOT RUN")


if __name__ == "__main__":
    main()
