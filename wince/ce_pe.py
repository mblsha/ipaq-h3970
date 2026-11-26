#!/usr/bin/env python3
"""Link ARMv5 code and package a fixed-base Windows CE ARM PE executable."""

from pathlib import Path
import re
import struct
import subprocess
import tempfile

BASE = 0x10000
TEXT = 0x1000
IMPORTS = 0x4000
ALIGNMENT = 0x1000
FILE_ALIGNMENT = 0x200

# Microsoft Pocket PC SDK, Windows CE 3.0 ARM coredll.lib. The device's
# COREDLL exports may omit the names used by later desktop-side toolchains.
CORE_ORDINALS = {
    "CloseHandle": 553,
    "CreateFileW": 168,
    "FlushFileBuffers": 175,
    "GetDiskFreeSpaceExW": 184,
    "GetFileAttributesW": 166,
    "GetLastError": 516,
    "GetModuleFileNameW": 537,
    "MessageBoxW": 858,
    "MoveFileW": 163,
    "ReadFile": 170,
    "SystemIdleTimerReset": 837,
    "TerminateProcess": 544,
    "VirtualAlloc": 524,
    "VirtualCopy": 560,
    "VirtualFree": 525,
    "WriteFile": 171,
    "CreateWindowExW": 246,
    "DefWindowProcW": 264,
    "DispatchMessageW": 859,
    "GetModuleHandleW": 1177,
    "GetSystemMetrics": 885,
    "PeekMessageW": 864,
    "RegisterClassW": 95,
    "SetWindowTextW": 256,
    "TranslateMessage": 870,
    "UpdateWindow": 267,
}


def align(size, unit):
    return (size + unit - 1) & -unit


def import_section(names, ordinals):
    lookup = 40
    iat = lookup + 4 * (len(names) + 1)
    dll = iat + 4 * (len(names) + 1)
    data = bytearray(dll)
    data.extend(b"COREDLL.dll\0")
    symbols = []
    for index, name in enumerate(names):
        entry = 0x80000000 | ordinals[name]
        struct.pack_into("<I", data, lookup + 4 * index, entry)
        struct.pack_into("<I", data, iat + 4 * index, entry)
        symbols += ["--defsym", f"ce_{name}={BASE + IMPORTS + iat + 4 * index:#x}"]
    struct.pack_into("<5I", data, 0, IMPORTS + lookup, 0, 0, IMPORTS + dll, IMPORTS + iat)
    return data, iat, symbols


def pe_image(text, imports, entry, iat, count):
    if TEXT + len(text) > IMPORTS:
        raise ValueError("ARM code overlaps the import section")
    headers = bytearray(FILE_ALIGNMENT)
    headers[0:2] = b"MZ"
    struct.pack_into("<I", headers, 60, 0x80)
    headers[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", headers, 0x84, 0x1c0, 2, 0, 0, 0, 224, 0x10f)
    text_size = align(len(text), FILE_ALIGNMENT)
    imports_size = align(len(imports), FILE_ALIGNMENT)
    opt = 0x98
    struct.pack_into("<HBB9I", headers, opt, 0x10b, 6, 20, text_size, imports_size,
                     0, entry, TEXT, IMPORTS, BASE, ALIGNMENT, FILE_ALIGNMENT)
    struct.pack_into("<6H4I2H6I", headers, opt + 40, 4, 0, 0, 0, 3, 0, 0,
                     align(IMPORTS + len(imports), ALIGNMENT), FILE_ALIGNMENT, 0, 9, 0,
                     65536, 4096, 1048576, 4096, 0, 16)
    struct.pack_into("<II", headers, opt + 96 + 8, IMPORTS, 40)
    struct.pack_into("<II", headers, opt + 96 + 12 * 8, IMPORTS + iat, 4 * (count + 1))
    sections = opt + 224
    struct.pack_into("<8s6I2HI", headers, sections, b".text", len(text), TEXT,
                     text_size, FILE_ALIGNMENT, 0, 0, 0, 0, 0x60000020)
    struct.pack_into("<8s6I2HI", headers, sections + 40, b".idata", len(imports), IMPORTS,
                     imports_size, FILE_ALIGNMENT + text_size, 0, 0, 0, 0, 0xc0000040)
    return headers + text.ljust(text_size, b"\0") + imports.ljust(imports_size, b"\0")


def build(output, source, entry_symbol, extra_ordinals=None):
    names = re.findall(r"\(\*const ce_(\w+)\)", source.read_text())
    if not names or len(names) != len(set(names)):
        raise ValueError("Missing or duplicated Windows CE imports")
    imports, iat, symbols = import_section(names, CORE_ORDINALS | (extra_ordinals or {}))
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        obj, elf, flat = (work / name for name in ("dump.o", "dump.elf", "dump.bin"))
        linker = work / "dump.ld"
        linker.write_text(f"""ENTRY({entry_symbol})
SECTIONS {{
  . = {BASE + TEXT:#x};
  .text : {{ *(.text.start) *(.text*) *(.rodata*) }}
  .data : {{ *(.data*) }}
  .bss : {{ *(.bss*) *(COMMON) }}
  /DISCARD/ : {{ *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) *(.ARM.attributes*) }}
}}
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "unexpected writable program storage")
""")
        subprocess.run(["arm-none-eabi-gcc", "-c", "-Os", "-Wall", "-Wextra", "-Werror",
                        "-marm", "-march=armv4t", "-mthumb-interwork", "-mfloat-abi=soft", "-fshort-wchar",
                        "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
                        "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
                        "-o", obj, source], check=True)
        subprocess.run(["arm-none-eabi-ld", "-T", linker, "-o", elf, *symbols, obj], check=True)
        subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", elf, flat], check=True)
        entry_line = next(line for line in subprocess.check_output(
            ["arm-none-eabi-nm", elf], text=True).splitlines() if line.endswith(f" T {entry_symbol}"))
        entry = int(entry_line.split()[0], 16) - BASE
        output.write_bytes(pe_image(flat.read_bytes(), imports, entry, iat, len(names)))
