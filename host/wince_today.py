"""Save an H3970's calibration and finish stock WinCE setup over RAPI."""

from contextlib import contextmanager
import os
from pathlib import Path
import re
import struct
import tempfile
import time

from activesync_rapi import RemoteError, optional_string, string, words

HELPERS = Path(__file__).resolve().parents[1] / ".build/h3970-wince-today"
BUILD = {"major": 3, "minor": 0, "build": 11171}
DOCUMENTS = "\\My Documents\\"
TOUCH_KEY = r"HARDWARE\DEVICEMAP\TOUCH"
CALIBRATION_FILE = "RT83CA.DAT"
DESKTOP = "DesktopExplorerWindow"
SCREEN_HEADER = struct.Struct("<2sI4xIIiiHH")


def read_optional(reply):
    if not reply.integer():
        return None
    size, present = reply.integer(), reply.integer()
    if present != 1:
        raise ValueError("Missing RAPI response value")
    return reply.take(size)


def validate_calibration(data):
    if not 24 <= len(data) < 192 or len(data) % 2 or data[-2:] != b"\0\0":
        raise ValueError("Invalid WinCE calibration size or terminator")
    value = data[:-2].decode("utf-16le")
    if not re.fullmatch(r"\s*-?[0-9]+(?:[,\s]+-?[0-9]+){9}\s*", value):
        raise ValueError("Expected five raw touchscreen coordinate pairs")
    return data


def read_calibration(client):
    reply = client.call(0x1E, words(0x80000002) + string(TOUCH_KEY))
    _, status = reply.integer(), reply.integer()
    if status:
        raise RemoteError("CeRegOpenKeyEx", status)
    key = reply.integer()
    try:
        capacity = 256
        args = words(key) + optional_string("CalibrationData") + words(
            0, 1, 4, 0, 1, capacity, 0, 1, 4, 1, capacity)
        reply = client.call(0x26, args)
        _, status = reply.integer(), reply.integer()
        if status:
            raise RemoteError("CeRegQueryValueEx", status)
        kind, data, length = (read_optional(reply) for _ in range(3))
        if kind != words(1) or data is None or length != words(len(data)):
            raise ValueError("Unexpected WinCE calibration value type")
        value = data.decode("utf-16le").rstrip("\0")
        return validate_calibration((value + "\0").encode("utf-16le"))
    finally:
        client.call(0x21, words(key))


def save_calibration(client, destination):
    data = read_calibration(client)
    fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as output:
        output.write(data)
    print(f"Saved the five-point WinCE calibration to {destination}", flush=True)


def delete_file(client, path):
    try:
        client.call(0x1C, optional_string(path)).result(f"CeDeleteFile({path})")
    except RemoteError as error:
        if error.code != 2:
            raise


@contextmanager
def launch_helper(client, directory, stem, *, report=None, data=None):
    executable = stem + ".EXE"
    uploads = [directory / executable]
    if data is not None:
        uploads.append(data)
    names = {path.name for path in uploads}
    if report:
        names.add(report)
    occupied = {name.upper() for name, _, _ in client.list(DOCUMENTS.rstrip("\\"))} & names
    if occupied:
        raise FileExistsError(f"WinCE helper files already exist: {', '.join(sorted(occupied))}")

    cleanup = []
    try:
        for local in uploads:
            target = DOCUMENTS + local.name
            client.push(local, target)
            cleanup.append(target)
        if report:
            cleanup.append(DOCUMENTS + report)
        client.launch(DOCUMENTS + executable, None)
        yield
    finally:
        for target in reversed(cleanup):
            delete_file(client, target)


def read_report(client, name, size):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            data = b"".join(client.read(DOCUMENTS + name))
        except RemoteError as error:
            if error.code not in (2, 32):
                raise
        else:
            if len(data) == size:
                return data
            if len(data) > size:
                raise ValueError(f"Invalid WinCE report size for {name}")
        time.sleep(.3)
    raise TimeoutError(f"WinCE did not produce a complete {name}")


def helper_report(client, directory, stem, size, *, suffix=".BIN", data=None):
    report = stem + suffix
    with launch_helper(client, directory, stem, report=report, data=data):
        return read_report(client, report, size)


def foreground(client, directory):
    report = helper_report(client, directory, "RT83WN", 516)
    name = report[:256].decode("utf-16le").split("\0", 1)[0]
    today = struct.unpack_from("<I", report, 512)[0]
    if name == DESKTOP and today != 1:
        raise ValueError("WinCE shell did not identify the desktop as foreground")
    return name


def restore_setup(client, directory, path, calibration):
    with tempfile.TemporaryDirectory(prefix="h3970-cal-") as scratch:
        value = Path(scratch) / CALIBRATION_FILE
        value.write_bytes(calibration)
        report = helper_report(client, directory, "RT83GO", 268, data=value)
    expected, status, closed = struct.unpack_from("<iii", report)
    if (expected, status, closed) != (1, 0, 1):
        raise ValueError(f"Welcome restore did not finish (window={expected}, status={status}, close={closed})")
    if read_calibration(client) != calibration:
        raise ValueError(f"WinCE calibration read-back differed from {path}")


def capture_today(client, directory):
    bitmap = helper_report(client, directory, "RT83SC", 230454, suffix=".BMP")
    if SCREEN_HEADER.unpack_from(bitmap) != (b"BM", len(bitmap), 54, 40, 240, 320, 1, 24):
        raise ValueError("WinCE returned an invalid screenshot")
    # Ignore the Start bar and input panel, which also draw on a blank desktop.
    first = 54 + 32 * 720
    last = 54 + 290 * 720
    body = bitmap[first:last]
    if body == body[:3] * (len(body) // 3):
        raise ValueError("WinCE desktop is active but the Today panel is blank")
    return bitmap


def return_today(client, directory, path, screenshot):
    calibration = validate_calibration(path.read_bytes())
    for stem in ("RT83WN", "RT83GO", "RT83TP", "RT83TD", "RT83SC"):
        if not (directory / (stem + ".EXE")).is_file():
            raise FileNotFoundError(f"Build the WinCE Today helpers in {directory} first")
    if screenshot is not None and (screenshot.exists() or screenshot.is_symlink()):
        raise FileExistsError(screenshot)

    window = foreground(client, directory)
    if window == "Welcome":
        restore_setup(client, directory, path, calibration)
        window = foreground(client, directory)
    if window == "SHOWBMP":
        flags, = struct.unpack("<I", helper_report(client, directory, "RT83TP", 4))
        if flags != 7:
            raise ValueError(f"WinCE splash dismiss failed (flags={flags:#x})")
        window = foreground(client, directory)
    if window != DESKTOP:
        raise ValueError(f"Unexpected WinCE foreground window: {window or '<none>'}")

    with launch_helper(client, directory, "RT83TD"):
        time.sleep(3)
    if foreground(client, directory) != DESKTOP:
        raise ValueError("WinCE Today is no longer the foreground window")
    bitmap = capture_today(client, directory)
    if screenshot is not None:
        with screenshot.open("xb") as output:
            output.write(bitmap)
    print("WinCE Today panel verified", flush=True)
