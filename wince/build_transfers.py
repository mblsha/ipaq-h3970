#!/usr/bin/env python3
"""Build the H3970 WinCE USB transfer window and read-only NOR helper."""
import argparse
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUTPUT = HERE.parent / ".build/h3970-wince-transfer"
from ce_pe import build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?", default=OUTPUT)
    output = parser.parse_args().output
    output.mkdir(parents=True, exist_ok=True)
    build(output / "TRANSFER.EXE", HERE / "transfer_ui.c", "transfer_start", {
        "BeginPaint": 260, "EndPaint": 261, "FillRect": 935, "CreateSolidBrush": 931,
        "DeleteObject": 912, "GetWindowLongW": 259, "SetWindowLongW": 258,
        "SetForegroundWindow": 702, "DestroyWindow": 265, "GetClientRect": 249,
        "InvalidateRect": 250,
        "SetFilePointer": 173, "GetTickCount": 535, "Sleep": 496,
    })
    build(output / "DUMPUSB.EXE", HERE / "dump_slice.c", "dump_slice_start",
          {"GetCommandLineW": 1231})


if __name__ == "__main__":
    main()
