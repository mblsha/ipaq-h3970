#!/usr/bin/env python3
"""Fetch the source pins and Zig into a disposable build cache."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
from tempfile import TemporaryDirectory
from urllib.request import urlopen

PINS = json.loads(Path(__file__).with_name("sources.json").read_text())


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(destination, url, expected):
    if destination.exists():
        if digest(destination) != expected:
            raise ValueError(f"SHA-256 differs from source lock for {destination}")
        return
    with TemporaryDirectory(dir=destination.parent) as temp:
        staged = Path(temp) / destination.name
        print(f"Fetching {url}", flush=True)
        with urlopen(url, timeout=60) as source, staged.open("wb") as output:
            shutil.copyfileobj(source, output)
        if digest(staged) != expected:
            raise ValueError(f"SHA-256 differs from source lock for {url}")
        staged.rename(destination)


def kernel(destination, checkout):
    pin = PINS["linux"]
    if destination.exists():
        with tarfile.open(destination, "r:gz") as archive:
            if archive.pax_headers.get("comment") != pin["base"]:
                raise ValueError(f"Wrong Git revision in {destination}")
        return
    with TemporaryDirectory(dir=destination.parent) as tmp:
        directory = Path(tmp)
        if checkout is None:
            checkout = directory / "linux"
            subprocess.run(["git", "init", "-q", checkout], check=True)
            subprocess.run(["git", "-C", checkout, "fetch", "--no-tags", "--depth=1",
                            pin["url"], pin["base"]], check=True)
        commit = subprocess.check_output(["git", "-C", checkout, "rev-parse",
                                          pin["base"] + "^{commit}"], text=True).strip()
        if commit != pin["base"]:
            raise ValueError("Local checkout does not contain the pinned public Linux commit")
        staged = directory / destination.name
        with staged.open("wb") as raw, gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0, compresslevel=1) as packed:
            archive = subprocess.Popen(["git", "-C", checkout, "archive", "--format=tar", commit],
                                       stdout=subprocess.PIPE)
            with archive.stdout:
                shutil.copyfileobj(archive.stdout, packed)
            if archive.wait():
                raise RuntimeError("git archive failed")
        staged.rename(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--kernel-checkout", type=Path, help="reuse the pinned commit in a local Linux checkout")
    parser.add_argument("--zig-platform", help="fetch this Zig host platform instead of the current platform")
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    machine = {"amd64": "x86_64", "arm64": "aarch64"}.get(platform.machine().lower(), platform.machine().lower())
    system = {"Darwin": "macos", "Linux": "linux"}.get(platform.system(), platform.system())
    target = args.zig_platform or machine + "-" + system
    if target not in PINS["zig"]["sha256"]:
        parser.error("unsupported Zig platform")
    for key, name in (("busybox", "busybox-1.37.0.tar.bz2"), ("doomgeneric", "doomgeneric.tar.gz")):
        pin = PINS[key]
        download(args.directory / name, pin["url"], pin["sha256"])
    version = PINS["zig"]["version"]
    name = f"zig-{target}-{version}.tar.xz"
    download(args.directory / name, f"https://ziglang.org/download/{version}/{name}",
             PINS["zig"]["sha256"][target])
    kernel(args.directory / "linux-base.tar.gz", args.kernel_checkout)


if __name__ == "__main__":
    main()
