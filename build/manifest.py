#!/usr/bin/env python3
"""Record source pins, exact input hashes, and kit checksums."""
import json
from pathlib import Path
import platform
import subprocess
import zlib
from sys import argv

from fetch import digest, PINS

root = Path(__file__).resolve().parents[1]
kernel, iwad, out, doom, initramfs = map(Path, argv[1:])
outputs = [out / name for name in ("DOOMUSB.EXE", "DOOMUSB.BIN", "H3970.DTB")]
pins = {
    "sources": PINS,
    "packager": {
        "python": platform.python_version(),
        "zlib": zlib.ZLIB_RUNTIME_VERSION,
        "gcc": subprocess.check_output(["arm-none-eabi-gcc", "--version"], text=True).splitlines()[0],
        "as": subprocess.check_output(["arm-none-eabi-as", "--version"], text=True).splitlines()[0],
    },
    "patch_sha256": {path.name: digest(path) for path in sorted((root / "linux/patches").glob("*.patch"))},
    "built_inputs_sha256": {
        "iwad": digest(iwad),
        "doom-fb": digest(doom),
        "ZIMAGE.BIN": digest(kernel / "ZIMAGE.BIN"),
        "H3970.DTB": digest(kernel / "H3970.DTB"),
        "doom-outer.cpio": digest(initramfs),
    },
    "outputs_sha256": {path.name: digest(path) for path in outputs},
}
for name in ("kernel.config", "busybox.config", "toolchain.txt"):
    if (kernel / name).is_file():
        pins[name] = (kernel / name).read_text()
(out / "build-manifest.json").write_text(json.dumps(pins, indent=2, sort_keys=True) + "\n")
(out / "SHA256SUMS").write_text("".join(f"{digest(path)}  {path.name}\n" for path in outputs))
