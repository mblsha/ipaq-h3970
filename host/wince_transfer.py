"""On-device progress and read-only NOR transfers over WinCE ActiveSync."""
from contextlib import closing, contextmanager, suppress
import hashlib
import os
from pathlib import Path
import struct
import tempfile
import time
from uuid import uuid4
import zlib

from wince_today import DOCUMENTS, delete_file

HELPERS = Path(__file__).resolve().parents[1] / ".build/h3970-wince-transfer"
STAGES = {"dump": 1, "upload": 2, "verify": 3, "complete": 4, "failed": 5}
PROGRESS_INTERVAL = 2
NOR_SIZE = 48 << 20
NOR_ALIGNMENT = 128 << 10
SLICE = 2 << 20
USB_READ_SIZE = 64 << 10
USB_WRITE_SIZE = 64 << 10
NOR_BLOCK = 16 << 10
BLOCKS_PER_RECORD = NOR_ALIGNMENT // NOR_BLOCK
RECORD_HEADER = struct.Struct("<I")
RECEIPT = struct.Struct("<4sIII")
ERROR_RECEIPT = struct.Struct("<4sII")
FOLDER = DOCUMENTS.rstrip("\\")


@contextmanager
def temporary_files(client, prefix, *extensions):
    stem = prefix + uuid4().hex[:6].upper()
    paths = {extension: DOCUMENTS + stem + "." + extension for extension in extensions}
    names = {name.upper() for name, _, _ in client.list(FOLDER)}
    if names.intersection(stem + "." + extension for extension in extensions):
        raise FileExistsError(f"WinCE transfer files for {stem} already exist")
    try:
        yield paths
    finally:
        for path in paths.values():
            with suppress(OSError, ValueError):
                delete_file(client, path)


def wait_file(client, paths, timeout):
    deadline = time.monotonic() + timeout
    suffixes = {path.rsplit("\\", 1)[-1].upper(): path for path in paths}
    while time.monotonic() < deadline:
        for name, _, _ in client.list(FOLDER):
            if name.upper() in suffixes:
                return suffixes[name.upper()]
        time.sleep(.25)
    raise TimeoutError(f"WinCE did not publish {', '.join(suffixes)}")


class Progress:
    def __init__(self, client, handle, stage, total):
        self.client = client
        self.handle = handle
        self.stage = stage
        self.done = 0
        self.counts = dict.fromkeys(STAGES, 0)
        self.total = total
        self.sent_stage = None
        self.last_send = 0

    def advance(self, stage, count):
        self.stage = stage
        self.counts[stage] += count
        self.done = self.counts[stage]
        self.publish()

    def publish(self, *, force=False):
        if (not force and self.stage == self.sent_stage
                and time.monotonic() - self.last_send < PROGRESS_INTERVAL):
            return
        data = struct.pack("<4sIII", b"USB1", STAGES[self.stage], self.done, self.total)
        self.client.write(self.handle, data)
        self.last_send, self.sent_stage = time.monotonic(), self.stage


@contextmanager
def device_progress(client, helper, stage, total):
    if not 0 < total <= 128 << 20:
        raise ValueError("Transfer progress requires a size between 1 byte and 128 MiB")
    with temporary_files(client, "TP", "EXE", "PRG", "ACK") as paths:
        client.push(helper, paths["EXE"])
        with client.file(paths["PRG"], 0x40000000, 1, share=1) as handle:
            progress = Progress(client, handle, stage, total)
            progress.publish(force=True)
            client.launch(paths["EXE"], None)
            wait_file(client, (paths["ACK"],), 10)
            delete_file(client, paths["ACK"])
            try:
                yield progress
            except BaseException:
                with suppress(OSError, ValueError):
                    progress.stage = "failed"
                    progress.publish(force=True)
                    wait_file(client, (paths["ACK"],), 5)
                raise
            else:
                progress.stage = "complete"
                progress.done = total
                progress.publish(force=True)
                wait_file(client, (paths["ACK"],), 5)


def decode_slice(chunks, offset, size, progress):
    data = bytearray()
    pending = bytearray()
    for chunk in chunks:
        pending.extend(chunk)
        while len(data) < size and len(pending) >= RECORD_HEADER.size:
            flags = RECORD_HEADER.unpack_from(pending)[0]
            if flags >= 1 << BLOCKS_PER_RECORD:
                raise ValueError(f"Invalid NOR block flags at {offset + len(data):#x}")
            length = RECORD_HEADER.size + NOR_ALIGNMENT - flags.bit_count() * (NOR_BLOCK - 4)
            if len(pending) < length:
                break
            cursor = RECORD_HEADER.size
            for block in range(BLOCKS_PER_RECORD):
                if flags & (1 << block):
                    data.extend(pending[cursor:cursor + 4] * (NOR_BLOCK // 4))
                    cursor += 4
                else:
                    data.extend(pending[cursor:cursor + NOR_BLOCK])
                    cursor += NOR_BLOCK
            del pending[:length]
            progress.advance("dump", NOR_ALIGNMENT)
        if len(data) == size and len(pending) > RECEIPT.size:
            raise ValueError(f"Oversized NOR slice at {offset:#x}")
    if len(data) != size or len(pending) != RECEIPT.size:
        raise ValueError(f"Short NOR slice at {offset:#x}")
    if RECEIPT.unpack(pending) != (b"ROM2", offset, size, zlib.crc32(data)):
        raise ValueError(f"WinCE NOR slice receipt or CRC mismatch at {offset:#x}")
    return data


def read_slice(client, paths, offset, size, progress):
    client.launch(paths["EXE"], f"/dump:{offset:08x}:{size:08x}")
    published = wait_file(client, (paths["ERR"], paths["BIN"]), 35)
    if published == paths["ERR"]:
        receipt = b"".join(client.read(published))
        if len(receipt) != ERROR_RECEIPT.size or receipt[:4] != b"ROM2":
            raise ValueError("Invalid WinCE NOR dump error receipt")
        _, status, error = ERROR_RECEIPT.unpack(receipt)
        raise OSError(f"WinCE NOR dump helper failed at step {status} (error {error})")

    with closing(client.read(published, chunk_size=USB_READ_SIZE)) as chunks:
        contents = decode_slice(chunks, offset, size, progress)
    delete_file(client, published)
    return contents


def dump_rom(client, destination, helpers, length=NOR_SIZE):
    if not 0 < length <= NOR_SIZE or length % NOR_ALIGNMENT:
        raise ValueError("ROM dump length must be a positive multiple of 128 KiB up to 48 MiB")
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(destination)
    digest = hashlib.sha256()
    with tempfile.TemporaryDirectory(prefix=".h3970-rom-", dir=destination.parent) as directory:
        partial = Path(directory) / "rom.bin"
        with temporary_files(client, "ND", "EXE", "PRT", "BIN", "ERR") as paths:
            client.push(helpers / "DUMPUSB.EXE", paths["EXE"])
            with device_progress(client, helpers / "TRANSFER.EXE", "dump", length) as progress:
                with partial.open("xb") as output:
                    for offset in range(0, length, SLICE):
                        size = min(SLICE, length - offset)
                        data = read_slice(client, paths, offset, size, progress)
                        output.write(data)
                        digest.update(data)
                os.link(partial, destination)
    return digest.hexdigest()
