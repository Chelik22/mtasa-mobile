"""Drive only the owned helper: native game image is never executed."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="mta-native-paths-") as temporary:
    root = Path(temporary) / "lab"
    for name in ("app", "gta", "data", "common", "documents", "local-appdata", "temp", "settings", "logs"):
        (root / name).mkdir(parents=True)
    env = os.environ.copy()
    env["MTA_EXPERIMENTAL_LAB_ROOT"] = str(root)
    result = subprocess.run([sys.argv[1], "paths"], env=env, capture_output=True, text=True,
                            timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)
    if result.returncode or "PASS" not in result.stdout:
        raise SystemExit(result.returncode or 1)
    if list(root.rglob("registry.*")):
        raise RuntimeError("native directory helper unexpectedly wrote settings")
