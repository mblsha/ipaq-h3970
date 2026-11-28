#!/usr/bin/env python3
"""Build the deterministic newc archives for the H3970 Doom kit."""
import argparse
import gzip
from io import BytesIO
from pathlib import Path
import re
import stat

ROOT = Path(__file__).resolve().parents[1]


def newc(entries):
    result = bytearray()

    def append_padded(data):
        result.extend(data)
        result.extend(b"\0" * (-len(result) % 4))

    for inode, (name, mode, contents) in enumerate([*entries, ("TRAILER!!!", 0, b"")], 1):
        filename = name.encode("ascii") + b"\0"
        fields = (inode, mode, 0, 0, 1, 0, len(contents), 0, 0, 0, 0, len(filename), 0)
        result.extend(("070701" + "".join(f"{n:08x}" for n in fields)).encode("ascii"))
        append_padded(filename)
        append_padded(contents)
    return bytes(result)


def file(name, source, perm):
    return name, stat.S_IFREG | perm, source.read_bytes()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("doom", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--warm", action="store_true")
    parser.add_argument("--kit-id", default="local")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9.+-]{1,64}", args.kit_id):
        parser.error("invalid kit identifier")
    linux = ROOT / "linux"
    inner = newc([
        ("doom", stat.S_IFDIR | 0o755, b""),
        ("doom/kit-id", stat.S_IFREG | 0o644, (args.kit_id + "\n").encode("ascii")),
        file("doom/init", linux / "doom-init.sh", 0o755),
        file("doom/usb-console", linux / "usb-console.sh", 0o755),
        file("doom/device-status", linux / "doom-status.sh", 0o755),
        file("doom/terminal", args.doom / "terminal", 0o755),
        file("doom/doom-fb", args.doom / "doom-fb", 0o755),
        file("doom/doomctl", args.doom / "doomctl", 0o755),
        file("doom/doom1.wad", args.doom / "doom1.wad", 0o644),
    ])
    if args.warm:
        payload = inner
    else:
        compressed = BytesIO()
        with gzip.GzipFile(filename="", mode="wb", fileobj=compressed, mtime=0) as stream:
            stream.write(inner)
        payload = newc([
            file("doom-load", linux / "doom-load", 0o755),
            ("doom.cpio.gz", stat.S_IFREG | 0o644, compressed.getvalue()),
            ("tmp", stat.S_IFDIR | 0o1777, b""),
        ])
    with args.output.open("xb") as output:
        output.write(payload)


if __name__ == "__main__":
    main()
