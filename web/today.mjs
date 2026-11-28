import {sleep, utf16, view} from "./bytes.mjs";
import {DOCUMENTS} from "./rapi.mjs";

const DESKTOP = "DesktopExplorerWindow";
const path = name => DOCUMENTS + "\\" + name;

async function run(rapi, stem, {size, suffix = "BIN", calibration} = {}) {
    const exe = path(stem + ".EXE");
    const report = size && path(stem + "." + suffix);
    const touch = calibration && path("RT83CA.DAT");
    const occupied = await rapi.list();
    if ([exe, report, touch].some(name => name && occupied.has(name))) {
        throw new Error("A WinCE Today helper file already exists");
    }
    const response = await fetch(new URL("./today/" + stem + ".EXE", import.meta.url));
    if (!response.ok) throw new Error("WinCE Today helper could not be loaded");
    const cleanup = [];
    try {
        await rapi.upload(new Uint8Array(await response.arrayBuffer()), exe);
        cleanup.push(exe);
        if (touch) {
            await rapi.upload(utf16(calibration), touch);
            cleanup.push(touch);
        }
        if (report) cleanup.push(report);
        await rapi.launch(exe);
        if (!report) {
            await sleep(3000);
            return;
        }
        const deadline = performance.now() + 10000;
        do {
            try {
                const bytes = await rapi.readAll(report, size);
                if (bytes.length === size) return bytes;
            } catch (error) {
                if (error.code !== 2 && error.code !== 32) throw error;
            }
            await sleep(300);
        } while (performance.now() < deadline);
        throw new Error("WinCE Today helper did not return a receipt");
    } finally {
        for (const name of cleanup.reverse()) await rapi.remove(name).catch(() => {});
    }
}

async function foreground(rapi) {
    const report = await run(rapi, "RT83WN", {size: 516});
    const name = new TextDecoder("utf-16le").decode(report.subarray(0, 256)).split("\0", 1)[0];
    if (name === DESKTOP && view(report).getUint32(512, true) !== 1) {
        throw new Error("WinCE desktop is not in the foreground");
    }
    return name;
}

export async function restoreToday(rapi, calibration) {
    let active = await foreground(rapi);
    if (active === "Welcome") {
        if (!calibration) throw new Error("No saved touch calibration. Complete setup on the iPAQ once.");
        const receipt = view(await run(rapi, "RT83GO", {size: 268, calibration}));
        if (receipt.getInt32(0, true) !== 1 || receipt.getInt32(4, true) !== 0 || receipt.getInt32(8, true) !== 1 ||
            await rapi.readCalibration() !== calibration) {
            throw new Error("WinCE did not confirm the restored calibration");
        }
        active = await foreground(rapi);
    }
    if (active === "SHOWBMP") {
        const receipt = view(await run(rapi, "RT83TP", {size: 4}));
        if (receipt.getUint32(0, true) !== 7) throw new Error("WinCE did not dismiss its splash screen");
        active = await foreground(rapi);
    }
    if (active !== DESKTOP) throw new Error("Finish setup on the iPAQ. WinCE foreground " + (active || "unknown"));

    await run(rapi, "RT83TD");
    if (await foreground(rapi) !== DESKTOP) throw new Error("The WinCE desktop could not be opened");
    const bitmap = await run(rapi, "RT83SC", {size: 230454, suffix: "BMP"});
    const header = view(bitmap);
    if (header.getUint16(0, true) !== 0x4d42 || header.getUint32(2, true) !== bitmap.length ||
        header.getUint32(10, true) !== 54 || header.getUint32(14, true) !== 40 ||
        header.getInt32(18, true) !== 240 || header.getInt32(22, true) !== 320 ||
        header.getUint16(26, true) !== 1 || header.getUint16(28, true) !== 24) {
        throw new Error("WinCE returned an invalid screen capture");
    }
    const body = bitmap.subarray(54 + 32 * 720, 54 + 290 * 720);
    if (!body.some((byte, index) => byte !== body[index % 3])) throw new Error("The WinCE Today panel is blank");
    return bitmap;
}
