import {ascii, join, Reply, sha256, sleep, view, words} from "./bytes.mjs";
import {DOCUMENTS, temporaryStem} from "./rapi.mjs";

export const ROM_SIZE = 48 * 1024 * 1024;
export const MAX_KIT_SIZE = 128 * 1024 * 1024;
const SLICE = 2 * 1024 * 1024;
const RECORD = 128 * 1024, BLOCK = 16 * 1024;
const STAGES = {dump: 1, upload: 2, verify: 3, complete: 4, failed: 5};
const ROM2 = 0x324d4f52;

async function asset(name) {
    const response = await fetch(new URL(name, import.meta.url));
    if (!response.ok) throw new Error("Missing WinCE transfer helper " + name);
    return new Uint8Array(await response.arrayBuffer());
}

async function temporary(rapi, prefix, extensions, transfer) {
    const stem = DOCUMENTS + "\\" + temporaryStem(prefix);
    const paths = Object.fromEntries(extensions.map(ext => [ext, stem + "." + ext]));
    const values = Object.values(paths);
    const names = await rapi.list();
    if (values.some(path => names.has(path))) throw new Error("Temporary WinCE name already exists");
    try {
        return await transfer(paths);
    } finally {
        for (const path of values) await rapi.remove(path).catch(() => {});
    }
}

async function waitFile(rapi, paths, timeout) {
    const deadline = performance.now() + timeout;
    do {
        const files = await rapi.list();
        const path = paths.find(name => files.has(name));
        if (path) return path;
        await sleep(250);
    } while (performance.now() < deadline);
    throw new Error("Timed out waiting for the progress or dump helper on WinCE");
}

async function withProgress(rapi, initial, total, report, transfer) {
    const helper = await asset("TRANSFER.EXE");
    return temporary(rapi, "TP", ["EXE", "PRG", "ACK"], async paths => {
        await rapi.upload(helper, paths.EXE);
        const handle = await rapi.open(paths.PRG, 0x40000000, 1, 1);
        let stage, last = 0, done = 0;
        const amounts = {dump: 0, upload: 0, verify: 0};
        async function update(phase, count, force = false) {
            if (phase in amounts) done = amounts[phase] += count;
            else if (phase === "complete") done = total;
            report({stage: phase, done, total});
            if (force || phase !== stage || performance.now() - last >= 2000) {
                await rapi.write(handle, join(ascii("USB1"), words(STAGES[phase], done, total)));
                stage = phase;
                last = performance.now();
            }
        }
        let acknowledged = false;
        try {
            await update(initial, 0, true);
            await rapi.launch(paths.EXE);
            await waitFile(rapi, [paths.ACK], 15000);
            await rapi.remove(paths.ACK);
            acknowledged = true;
            const value = await transfer(update);
            await update("complete", 0, true);
            await waitFile(rapi, [paths.ACK], 8000);
            return value;
        } catch (error) {
            if (acknowledged) {
                await (async () => {
                    await update("failed", 0, true);
                    await waitFile(rapi, [paths.ACK], 6000);
                })().catch(() => {});
            }
            throw error;
        } finally {
            await rapi.close(handle).catch(() => {});
        }
    });
}

const crcTable = Uint32Array.from({length: 256}, (_, value) => {
    for (let bit = 0; bit < 8; bit++) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
    return value;
});

function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) crc = (crc >>> 8) ^ crcTable[(byte ^ crc) & 255];
    return (~crc) >>> 0;
}

export async function decodeRom(chunks, offset, size, advance) {
    const unpacked = new Uint8Array(size);
    let done = 0, pending = new Uint8Array();
    for await (const chunk of chunks) {
        pending = join(pending, chunk);
        while (done < size && pending.length >= 4) {
            const flags = view(pending).getUint32(0, true);
            if (flags > 255) throw new Error("Invalid WinCE NOR block flags");
            let length = 4;
            for (let block = 0; block < 8; block++) length += (flags & (1 << block)) ? 4 : BLOCK;
            if (pending.length < length) break;
            let cursor = 4;
            for (let block = 0; block < 8; block++) {
                const start = done + block * BLOCK;
                if (flags & (1 << block)) {
                    const word = pending.subarray(cursor, cursor + 4);
                    for (let pos = start; pos < start + BLOCK; pos += 4) unpacked.set(word, pos);
                    cursor += 4;
                } else {
                    unpacked.set(pending.subarray(cursor, cursor + BLOCK), start);
                    cursor += BLOCK;
                }
            }
            pending = pending.subarray(length);
            done += RECORD;
            await advance("dump", RECORD);
        }
        if (done === size && pending.length > 16) throw new Error("WinCE returned an oversized NOR slice");
    }
    if (done !== size || pending.length !== 16) throw new Error("WinCE returned a short NOR slice");
    const receipt = new Reply(pending);
    if (receipt.integer() !== ROM2 || receipt.integer() !== offset ||
        receipt.integer() !== size || receipt.integer() !== crc32(unpacked)) {
        throw new Error("NOR slice CRC or address mismatch at 0x" + offset.toString(16));
    }
    return unpacked;
}

function hex32(value) {
    return value.toString(16).padStart(8, "0");
}

export async function dumpRom(rapi, report, {start = 0, size = ROM_SIZE} = {}) {
    if (!Number.isInteger(start) || !Number.isInteger(size) ||
        start < 0 || size <= 0 || start + size > ROM_SIZE || (start | size) % RECORD) {
        throw new Error("Invalid H3970 NOR range");
    }
    const helper = await asset("DUMPUSB.EXE");
    return temporary(rapi, "ND", ["EXE", "BIN", "PRT", "ERR"], async paths => {
        await rapi.upload(helper, paths.EXE);
        return withProgress(rapi, "dump", size, report, async update => {
            const dump = new Uint8Array(size);
            for (let done = 0; done < size; done += SLICE) {
                const length = Math.min(SLICE, size - done);
                const offset = start + done;
                await rapi.launch(paths.EXE, "/dump:" + hex32(offset) + ":" + hex32(length));
                const found = await waitFile(rapi, [paths.ERR, paths.BIN], 35000);
                if (found === paths.ERR) {
                    const receipt = new Reply(await rapi.readAll(found, 12));
                    if (receipt.integer() !== ROM2) throw new Error("Invalid WinCE dump error receipt");
                    throw new Error("WinCE could not read NOR (step " + receipt.integer() + ", error " + receipt.integer() + ")");
                }
                dump.set(await decodeRom(rapi.read(paths.BIN), offset, length, update), done);
                await rapi.remove(paths.BIN);
            }
            return {data: dump, sha256: await sha256(dump)};
        });
    });
}

export async function uploadDoom(rapi, image, launcher, report, stem = "DOOMUSB") {
    const total = image.length + launcher.length;
    if (!image.length || !launcher.length || total > MAX_KIT_SIZE) throw new Error("Invalid Doom kit size");
    const files = [
        [DOCUMENTS + "\\" + stem + ".BIN", image],
        [DOCUMENTS + "\\" + stem + ".EXE", launcher],
    ];
    const existing = await rapi.list();
    for (const [path, bytes] of files) {
        const remote = existing.get(path);
        if (remote && (remote.directory || remote.size !== bytes.length)) {
            throw new Error("WinCE already contains a different " + path.split("\\").at(-1));
        }
    }
    const initial = files.every(([path]) => existing.has(path)) ? "verify" : "upload";
    return withProgress(rapi, initial, total, report, async update => {
        const hashes = {};
        // Verify pre-existing files before copying any new file.
        for (const [path, bytes] of files.filter(([name]) => existing.has(name))) {
            hashes[path.split("\\").at(-1)] = await rapi.verify(path, bytes, count => update("verify", count));
        }
        for (const [path, bytes] of files.filter(([name]) => !existing.has(name))) {
            hashes[path.split("\\").at(-1)] = await rapi.upload(bytes, path, update);
        }
        return hashes;
    });
}
