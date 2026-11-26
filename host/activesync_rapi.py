"""Pocket PC 2002 RAPI framing, filesystem operations, and process launch."""

from contextlib import contextmanager, suppress
import hashlib
import os
import stat
import struct
import tempfile
import uuid

CHUNK = 8192
MAX_REPLY = 4 * 1024 * 1024


def words(*values):
    return struct.pack("<" + "I" * len(values), *values)


def utf16(value):
    if "\0" in value:
        raise ValueError("A RAPI string cannot contain a null character")
    return (value + "\0").encode("utf-16le")


def string(value):
    data = utf16(value)
    return words(1, len(data) // 2) + data


def optional_string(value):
    if value is None:
        return words(0)
    data = utf16(value)
    return words(1, len(data), 1) + data


def remote_path(value):
    value = value.replace("/", "\\")
    if not value.startswith("\\") or value.startswith("\\\\") or "\0" in value or ":" in value:
        raise ValueError("Use an absolute device path such as /My Documents/DOOMUSB.BIN")
    return value


def read_exact(socket, count):
    data = bytearray()
    while len(data) < count:
        chunk = socket.recv(count - len(data))
        if not chunk:
            raise ConnectionError("WinCE closed the connection")
        data.extend(chunk)
    return bytes(data)


class Reply:
    def __init__(self, data):
        self.data = data
        self.position = 0

    def take(self, size):
        end = self.position + size
        if end > len(self.data):
            raise ValueError("Truncated RAPI response")
        value = self.data[self.position:end]
        self.position = end
        return value

    def integer(self):
        return int.from_bytes(self.take(4), "little")

    def result(self, name, failure=0):
        error, result = self.integer(), self.integer()
        if result == failure:
            raise RemoteError(name, error)
        return result


class RemoteError(OSError):
    def __init__(self, operation, code):
        super().__init__(f"{operation} failed (WinCE error {code})")
        self.code = code


class Rapi:
    def __init__(self, socket):
        self.socket = socket

    def call(self, command, args=b""):
        self.socket.sendall(words(4 + len(args), command) + args)
        length = int.from_bytes(read_exact(self.socket, 4), "little")
        if not 4 <= length <= MAX_REPLY:
            raise ValueError(f"Invalid RAPI response length {length}")
        reply = Reply(read_exact(self.socket, length))
        envelope = reply.integer()
        if envelope == 1:
            hresult = reply.integer()
            if hresult & 0x80000000:
                raise OSError(f"RAPI command {command:#04x} failed (HRESULT {hresult:#010x})")
        elif envelope != 0:
            raise ValueError(f"Unsupported RAPI response envelope {envelope}")
        return reply

    def version(self):
        reply = self.call(0x3B)
        reply.result("CeGetVersionEx")
        version = Reply(reply.take(reply.integer()))
        version.integer()  # dwOSVersionInfoSize
        return {"major": version.integer(), "minor": version.integer(), "build": version.integer()}

    def list(self, path):
        query = path.rstrip("\\") + "\\*"
        reply = self.call(0x09, string(query) + words(0x00A1))  # name, attributes, size
        count = reply.integer()
        if count > (len(reply.data) - reply.position) // 12:
            raise ValueError("Invalid RAPI directory entry count")
        for _ in range(count):
            name_units = reply.integer()
            attributes = reply.integer()
            size = reply.integer()
            name = reply.take(name_units * 2).decode("utf-16le").rstrip("\0")
            yield name, bool(attributes & 0x10), size

    @contextmanager
    def file(self, path, access, mode, *, share=0):
        params = words(access, share, mode, 0x80, 0) + string(path)
        handle = self.call(0x05, params).result(f"CeCreateFile({path})", 0xFFFFFFFF)
        try:
            yield handle
        except BaseException:
            with suppress(OSError, ValueError):
                self.call(0x08, words(handle)).result("CeCloseHandle")
            raise
        else:
            self.call(0x08, words(handle)).result("CeCloseHandle")

    def read(self, path, *, chunk_size=CHUNK):
        with self.file(path, 0x80000000, 3) as handle:  # GENERIC_READ, OPEN_EXISTING
            while True:
                reply = self.call(0x06, words(handle, 1, chunk_size, 0, 0))
                reply.result("CeReadFile")
                count = reply.integer()
                if count > chunk_size:
                    raise ValueError("WinCE returned more data than requested")
                if count == 0:
                    return
                yield reply.take(count)

    def write(self, handle, data):
        reply = self.call(0x07, words(handle, 1, len(data)) + data + words(0))
        reply.result("CeWriteFile")
        if reply.integer() != len(data):
            raise OSError("WinCE reported a short write")

    def push(self, source, path, *, progress=None, write_size=CHUNK, verify_size=CHUNK):
        folder, _, name = path.rpartition("\\")
        if not name:
            raise ValueError("A remote file name is required")
        temp_path = folder + "\\AS" + uuid.uuid4().hex[:6].upper() + ".TMP"
        cleanup = False
        try:
            with source.open("rb") as stream:
                if not stat.S_ISREG(os.fstat(stream.fileno()).st_mode):
                    raise ValueError("The upload source must be a regular file")
                original = hashlib.sha256()
                with self.file(temp_path, 0x40000000, 1) as handle:  # GENERIC_WRITE, CREATE_NEW
                    cleanup = True
                    while chunk := stream.read(write_size):
                        self.write(handle, chunk)
                        original.update(chunk)
                        if progress:
                            progress("upload", len(chunk))
            copied = hashlib.sha256()
            if progress:
                progress("verify", 0)
            for chunk in self.read(temp_path, chunk_size=verify_size):
                copied.update(chunk)
                if progress:
                    progress("verify", len(chunk))
            if original.digest() != copied.digest():
                raise OSError("Remote readback SHA-256 differs from the upload")
            args = optional_string(temp_path) + optional_string(path)
            self.call(0x1A, args).result(f"CeMoveFile({path})")
            cleanup = False
            return copied.hexdigest()
        finally:
            if cleanup:
                with suppress(OSError, ValueError):
                    self.call(0x1C, optional_string(temp_path)).result("CeDeleteFile (staging file)")

    def pull(self, path, destination):
        if destination.exists() or destination.is_symlink():
            raise FileExistsError(destination)
        with tempfile.TemporaryDirectory(prefix=".activesync-", dir=destination.parent) as directory:
            temp_path = os.path.join(directory, "download")
            digest = hashlib.sha256()
            with open(temp_path, "xb") as stream:
                for chunk in self.read(path):
                    stream.write(chunk)
                    digest.update(chunk)
            os.link(temp_path, destination)
        return digest.hexdigest()

    def launch(self, path, command_line):
        args = optional_string(path) + optional_string(command_line) + words(*([0] * 7), 1, 16, 0)
        reply = self.call(0x19, args)
        reply.result(f"CeCreateProcess({path})")
        if (reply.integer(), reply.integer(), reply.integer()) != (1, 16, 1):
            raise ValueError("WinCE returned malformed process information")
        process, thread, _, _ = struct.unpack("<4I", reply.take(16))
        for handle in (thread, process):
            if handle:
                self.call(0x08, words(handle)).result("CeCloseHandle")
