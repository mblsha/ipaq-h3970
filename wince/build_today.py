#!/usr/bin/env python3
"""Build the WinCE RAM helpers for returning an H3970 to Today."""

import argparse
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_OUTPUT = HERE.parent / ".build/h3970-wince-today"
from ce_pe import build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?", default=DEFAULT_OUTPUT)
    output = parser.parse_args().output
    output.mkdir(parents=True, exist_ok=True)
    helpers = (
        ("RT83WN", "window", "window_main",
         {"GetForegroundWindow": 701, "GetClassNameW": 283, "GetWindowTextW": 257,
          "FindWindowW": 286}),
        ("RT83GO", "setup_ram", "setup_main",
         {"GetForegroundWindow": 701, "GetClassNameW": 283, "PostMessageW": 865,
          "RegOpenKeyExW": 461, "RegSetValueExW": 464, "RegCloseKey": 455, "Sleep": 496}),
        ("RT83TP", "dismiss_splash", "dismiss_main",
         {"GetForegroundWindow": 701, "GetClassNameW": 283, "GetClientRect": 249,
          "ScreenToClient": 255, "PostMessageW": 865, "Sleep": 496}),
        ("RT83TD", "show_today", "today_main",
         {"GetForegroundWindow": 701, "FindWindowW": 286, "PostMessageW": 865}),
        ("RT83SC", "screen", "screen_main",
         {"GetDC": 262, "ReleaseDC": 263, "CreateCompatibleDC": 910, "DeleteDC": 911,
          "CreateDIBSection": 90, "SelectObject": 921, "DeleteObject": 912, "BitBlt": 903}),
    )
    for name, source, entry, imports in helpers:
        build(output / (name + ".EXE"), HERE / "today" / (source + ".c"), entry, imports)


if __name__ == "__main__":
    main()
