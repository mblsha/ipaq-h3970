#!/usr/bin/env python3
"""Pack a matched WinCE launcher and H3970 Linux RAM payload."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

from ce_pe import build

HERE = Path(__file__).resolve().parent
PAGE = 4096
START_MARKER = 0x109F7261
END_MARKER = 0x20A1C456


def align(n):
    return (n + PAGE - 1) & -PAGE


def run(*command):
    subprocess.run(command, check=True)


def patch_offset(dtb, marker):
    value = struct.pack(">I", marker)
    if dtb.count(value) != 1:
        raise ValueError(f"DTB needs one unique {marker:#010x} initrd marker")
    return dtb.index(value)


def pack(kernel_file, dtb_file, initrd_file, output):
    targets = [output / name for name in ("H3970.DTB", "DOOMUSB.BIN", "DOOMUSB.EXE")]
    if any(path.exists() or path.is_symlink() for path in targets):
        raise FileExistsError("Choose an output directory without a previous Doom kit")
    output.mkdir(parents=True, exist_ok=True)
    kernel = kernel_file.read_bytes()
    if kernel[0x24:0x28] != bytes.fromhex("18286f01"):
        raise ValueError("Expected an ARM Linux zImage")
    initrd = initrd_file.read_bytes()
    if not initrd.startswith(b"070701"):
        raise ValueError("Expected a newc outer initramfs")

    with tempfile.TemporaryDirectory(prefix=".pack-", dir=output) as directory:
        work = Path(directory)
        dtb_path = work / "H3970.DTB"
        dtb_path.write_bytes(dtb_file.read_bytes())
        run("fdtput", "-t", "s", dtb_path, "/chosen", "bootargs",
            "earlycon console=ttyS0,115200n8 clk_ignore_unused rdinit=/doom-load "
            "fw_devlink=off doom_audio=0 h3970_usb_cold=1")
        for key, marker in (("linux,initrd-start", START_MARKER), ("linux,initrd-end", END_MARKER)):
            run("fdtput", "-t", "x", dtb_path, "/chosen", key, f"{marker:x}")
        dtb = dtb_path.read_bytes()
        if not 40 <= len(dtb) <= 65536 or struct.unpack_from(">2I", dtb) != (0xD00DFEED, len(dtb)):
            raise ValueError("Expected a complete H3970 DTB no larger than 64 KiB")
        kernel_offset = align(len(dtb))
        # Reserve decompressor workspace after the zImage.
        initrd_offset = align(kernel_offset + len(kernel)) + (1 << 20)
        payload = (dtb.ljust(kernel_offset, b"\0") + kernel).ljust(initrd_offset, b"\0") + initrd
        # One page holds 16 bytes of metadata and 16-bit DRAM page indices.
        if align(len(payload)) // PAGE > (PAGE - 16) // 2:
            raise ValueError("Cold payload exceeds the WinCE handoff's page list")
        (work / "DOOMUSB.BIN").write_bytes(payload)

        obj, flat = work / "tramp.o", work / "tramp.bin"
        run("arm-none-eabi-as", "-march=armv5te", "-I", HERE, "--defsym", "BOOT_LINUX=1",
            HERE / "roundtrip.S", "-o", obj)
        run("arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", obj, flat)
        header = f"""#define HANDOFF_RETURN 1
#define HANDOFF_LINUX 1
#define HANDOFF_LCD 1
#define LINUX_BYTES {len(payload)}u
#define LINUX_KERNEL_OFFSET {kernel_offset}u
#define LINUX_CRC32 0x{zlib.crc32(payload):08x}u
#define LINUX_INITRD_OFFSET {initrd_offset}u
#define LINUX_INITRD_BYTES {len(initrd)}u
#define LINUX_INITRD_START_PROP {patch_offset(dtb, START_MARKER)}u
#define LINUX_INITRD_END_PROP {patch_offset(dtb, END_MARKER)}u
#define LINUX_PATH L"\\\\My Documents\\\\DOOMUSB.BIN"
"""
        (work / "handoff_blob.h").write_text(
            header + "static const unsigned char handoff_blob[] = {"
            + ",".join(f"0x{byte:02x}" for byte in flat.read_bytes()) + "};\n")
        for name in ("handoff.c", "linux_stage.h"):
            (work / name).write_bytes((HERE / name).read_bytes())
        build(work / "DOOMUSB.EXE", work / "handoff.c", "handoff_main", {
            "LockPages": 1161, "UnlockPages": 1162, "SetKMode": 630,
            "GetCommState": 113, "SetCommState": 118, "Sleep": 496,
        })
        for path in targets:
            (work / path.name).rename(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ("kernel", "dtb", "initrd", "output"):
        parser.add_argument(argument, type=Path)
    args = parser.parse_args()
    pack(args.kernel, args.dtb, args.initrd, args.output)


if __name__ == "__main__":
    main()
