#!/usr/bin/env python3
"""Copy a compatible published Doom kit into the same-origin Pages assets."""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile

FILES = ("DOOMUSB.BIN", "DOOMUSB.EXE")
ASSET = "ipaq-h3970-doom.zip"


def main():
    output = Path(sys.argv[1]) / "kit"
    repository = os.environ["GH_REPO"]
    releases = json.loads(subprocess.check_output(["gh", "api", f"repos/{repository}/releases?per_page=30"]))
    published = sorted((item for item in releases if not item["draft"] and item["published_at"]),
                       key=lambda item: item["published_at"], reverse=True)
    for release in published:
        tag = release["tag_name"]
        assets = {asset["name"]: asset for asset in release["assets"] if asset["state"] == "uploaded"}
        if ASSET not in assets or not re.fullmatch(r"v[0-9][0-9A-Za-z.+-]*", tag):
            continue
        asset = assets[ASSET]
        if asset["size"] > 16 * 1024 * 1024 or not (asset["digest"] or "").startswith("sha256:"):
            raise SystemExit(f"Invalid GitHub release digest or size for {tag}")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / ASSET
            subprocess.run(["gh", "release", "download", tag, "-p", ASSET, "-O", str(path)], check=True)
            if "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest() != asset["digest"]:
                raise SystemExit(f"GitHub release ZIP digest mismatch for {tag}")
            with zipfile.ZipFile(path) as archive:
                manifest = json.loads(archive.read("build-manifest.json"))
                if manifest.get("browser_kit") != 1:
                    continue
                hashes = manifest["outputs_sha256"]
                copies = {}
                for name, limit in zip(FILES, (8 * 1024 * 1024, 64 * 1024), strict=True):
                    if not 0 < archive.getinfo(name).file_size <= limit:
                        raise SystemExit(f"Unexpected size for {tag} {name}")
                    data = archive.read(name)
                    if hashlib.sha256(data).hexdigest() != hashes[name]:
                        raise SystemExit(f"SHA-256 mismatch for {tag} {name}")
                    copies[name] = data
            output.mkdir(exist_ok=True)
            for name, data in copies.items():
                (output / name).write_bytes(data)
            (output / "kit.json").write_text(json.dumps({
                "tag": tag, "sha256": {name: hashes[name] for name in FILES},
            }) + "\n")
            print(f"Staged Doom kit {tag}")
            return
    print("No published release contains a browser-compatible kit")


if __name__ == "__main__":
    main()
