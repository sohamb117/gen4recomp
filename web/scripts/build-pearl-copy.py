#!/usr/bin/env python3
"""Rebuild Pearl from current translation inputs; all writes stay in build/."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
build = root / "build"
build.mkdir(exist_ok=True)
snapshot = Path(tempfile.mkdtemp(prefix="web-source-", dir=build))
(snapshot / "games").mkdir()
(snapshot / ".cache").mkdir()
for name in ("games/diamond", "games/platinum", "core", "tools"):
    source, target = root / name, snapshot / name
    if sys.platform == "darwin":
        subprocess.run(["cp", "-cRp", str(source), str(target)], check=True)
    else:
        shutil.copytree(source, target, symlinks=True)
(snapshot / ".cache/toolchains").symlink_to(root / ".cache/toolchains", target_is_directory=True)
# Compiler dependency files contain absolute paths. Redirect both targets and
# inputs into the snapshot before make reads them; never build original targets.
for dep in (snapshot / "games/diamond/build/pc-wasm").rglob("*.d"):
    text = dep.read_text()
    for name in ("games", "core", "tools", ".cache"):
        text = text.replace(str(root / name) + "/", str(snapshot / name) + "/")
    dep.write_text(text)
log = snapshot / "pearl-build.log"
print(f"Pearl snapshot: {snapshot}\nBuild log: {log}", flush=True)
with log.open("w") as output:
    result = subprocess.run(
        ["make", "-f", "pc/Makefile.wasm", f"-j{os.environ.get('NP_BUILD_JOBS', '8')}", "GAME_VERSION=PEARL"],
        cwd=snapshot / "games/diamond", stdout=output, stderr=subprocess.STDOUT,
    )
if result.returncode:
    print(log.read_text()[-8000:], file=sys.stderr)
    sys.exit(result.returncode)
source = snapshot / "games/diamond/build/pc-wasm/pokepearl.wasm"
output = build / "web-guest"
output.mkdir(exist_ok=True)
shutil.copyfile(source, output / "pearl.wasm.tmp")
(output / "pearl.wasm.tmp").replace(output / "pearl.wasm")
(output / "pearl-source.json").write_text(json.dumps({
    "snapshot": str(snapshot.relative_to(root)),
    "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
    "sourceCommit": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
}, indent=2) + "\n")
print(f"Pearl built: {output / 'pearl.wasm'}; original trees untouched.")
