import {hex, join, Reply, sha256, utf16, words} from "./bytes.mjs";

const CHUNK = 64 * 1024;
const MAX_REPLY = 4 * 1024 * 1024;
export const DOCUMENTS = "\\My Documents";

function string(value) {
    const bytes = utf16(value);
    return join(words(1, bytes.length / 2), bytes);
}

function optional(value) {
    if (value === null) return words(0);
    const bytes = utf16(value);
    return join(words(1, bytes.length, 1), bytes);
}

export function temporaryStem(prefix) {
    return prefix + hex(crypto.getRandomValues(new Uint8Array(3))).toUpperCase();
}

export function isCalibration(value) {
    return typeof value === "string" && value.length >= 11 && value.length < 95 &&
        /^\s*-?[0-9]+(?:[,\s]+-?[0-9]+){9}\s*$/.test(value);
}

export class WinCeError extends Error {
    constructor(operation, code) {
        super(operation + " failed (WinCE error " + code + ")");
        this.code = code;
    }
}

function result(reply, operation, failure = 0) {
    const error = reply.integer(), value = reply.integer();
    if (value === failure) throw new WinCeError(operation, error);
    return value;
}

export class Rapi {
    constructor(transport) {
        this.transport = transport;
    }

    async call(command, args = new Uint8Array(), timeout) {
        let reply;
        try {
            await this.transport.send(join(words(4 + args.length, command), args));
            const size = new Reply(await this.transport.receive(4, timeout)).integer();
            if (size < 4 || size > MAX_REPLY) throw new Error("Invalid RAPI reply size");
            reply = new Reply(await this.transport.receive(size, timeout));
        } catch (error) {
            this.transport.fail(error);
            throw error;
        }
        const envelope = reply.integer();
        if (envelope === 1) {
            const hresult = reply.integer();
            if (hresult & 0x80000000) throw new Error("RAPI error 0x" + hresult.toString(16));
        } else if (envelope !== 0) {
            throw new Error("Invalid RAPI reply envelope");
        }
        return reply;
    }

    async version() {
        const reply = await this.call(0x3b);
        result(reply, "CeGetVersionEx");
        const version = new Reply(reply.take(reply.integer()));
        version.integer();
        return {major: version.integer(), minor: version.integer(), build: version.integer()};
    }

    async readCalibration() {
        function registryResult(reply, name) {
            reply.integer();
            const status = reply.integer();
            if (status) throw new WinCeError(name, status);
        }
        function optionalValue(reply) {
            if (!reply.integer()) return null;
            const size = reply.integer();
            if (reply.integer() !== 1) throw new Error("Unexpected WinCE registry reply");
            return reply.take(size);
        }
        const open = await this.call(0x1e, join(words(0x80000002), string("HARDWARE\\DEVICEMAP\\TOUCH")));
        registryResult(open, "CeRegOpenKeyEx");
        const key = open.integer();
        try {
            const capacity = 256;
            const query = join(words(key), optional("CalibrationData"),
                words(0, 1, 4, 0, 1, capacity, 0, 1, 4, 1, capacity));
            const reply = await this.call(0x26, query);
            registryResult(reply, "CeRegQueryValueEx");
            const type = optionalValue(reply), data = optionalValue(reply), size = optionalValue(reply);
            if (!type || !data || !size || type.length !== 4 || size.length !== 4 ||
                new Reply(type).integer() !== 1 || new Reply(size).integer() !== data.length ||
                data.length % 2) {
                throw new Error("Unexpected WinCE calibration value");
            }
            const value = new TextDecoder("utf-16le", {fatal: true}).decode(data).replace(/\0+$/, "");
            if (!isCalibration(value)) {
                throw new Error("WinCE did not report five valid touch calibration points");
            }
            return value;
        } finally {
            await this.call(0x21, words(key));
        }
    }

    async list(timeout) {
        const reply = await this.call(0x09, join(string(DOCUMENTS + "\\*"), words(0x00a1)), timeout);
        const count = reply.integer();
        if (count > Math.floor(reply.remaining / 12)) throw new Error("Invalid WinCE directory listing");
        const names = new Map();
        const decoder = new TextDecoder("utf-16le");
        for (let i = 0; i < count; i++) {
            const units = reply.integer(), attributes = reply.integer(), size = reply.integer();
            const name = decoder.decode(reply.take(units * 2)).replace(/\0+$/, "").toUpperCase();
            names.set(DOCUMENTS + "\\" + name, {directory: Boolean(attributes & 0x10), size});
        }
        return names;
    }

    async open(path, access, mode, share = 0) {
        const reply = await this.call(0x05, join(words(access, share, mode, 0x80, 0), string(path)));
        return result(reply, "Opening " + path, 0xffffffff);
    }

    async close(handle) {
        result(await this.call(0x08, words(handle)), "CeCloseHandle");
    }

    async remove(path) {
        try {
            result(await this.call(0x1c, optional(path)), "Removing " + path);
        } catch (error) {
            if (error.code !== 2) throw error;
        }
    }

    async *read(path) {
        const handle = await this.open(path, 0x80000000, 3);
        try {
            for (;;) {
                const reply = await this.call(0x06, words(handle, 1, CHUNK, 0, 0));
                result(reply, "Reading " + path);
                const n = reply.integer();
                if (n > CHUNK) throw new Error("WinCE returned an oversized read");
                if (!n) return;
                yield reply.take(n);
            }
        } finally {
            await this.close(handle).catch(() => {});
        }
    }

    async readAll(path, limit, received = () => {}) {
        const bytes = new Uint8Array(limit);
        let offset = 0;
        for await (const chunk of this.read(path)) {
            if (offset + chunk.length > limit) throw new Error("WinCE file exceeds expected size");
            bytes.set(chunk, offset);
            offset += chunk.length;
            await received(chunk.length);
        }
        return bytes.subarray(0, offset);
    }

    async write(handle, data) {
        const reply = await this.call(0x07, join(words(handle, 1, data.length), data, words(0)));
        result(reply, "CeWriteFile");
        if (reply.integer() !== data.length) throw new Error("WinCE reported a short write");
    }

    async verify(path, source, received) {
        const copy = await this.readAll(path, source.length, received);
        const expected = await sha256(source);
        if (copy.length !== source.length || await sha256(copy) !== expected) {
            throw new Error("SHA-256 read-back differed for " + path);
        }
        return expected;
    }

    async upload(source, destination, progress = async () => {}) {
        const staging = DOCUMENTS + "\\" + temporaryStem("AS") + ".TMP";
        const handle = await this.open(staging, 0x40000000, 1);
        let published = false;
        try {
            try {
                for (let offset = 0; offset < source.length; offset += CHUNK) {
                    const chunk = source.subarray(offset, offset + CHUNK);
                    await this.write(handle, chunk);
                    await progress("upload", chunk.length);
                }
            } finally {
                await this.close(handle);
            }
            await progress("verify", 0);
            const sha = await this.verify(staging, source, length => progress("verify", length));
            const reply = await this.call(0x1a, join(optional(staging), optional(destination)));
            result(reply, "Publishing " + destination);
            published = true;
            return sha;
        } finally {
            if (!published) await this.remove(staging).catch(() => {});
        }
    }

    async launch(path, command = null) {
        const args = join(optional(path), optional(command), words(0, 0, 0, 0, 0, 0, 0, 1, 16, 0));
        const reply = await this.call(0x19, args);
        result(reply, "Launching " + path);
        if (reply.integer() !== 1 || reply.integer() !== 16 || reply.integer() !== 1) {
            throw new Error("Invalid WinCE process receipt");
        }
        const process = reply.integer(), thread = reply.integer();
        for (const handle of [thread, process]) if (handle) await this.close(handle);
    }
}
