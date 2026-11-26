import {ActiveSync, IPAQ} from "./transport.mjs";
import {DOCUMENTS, Rapi} from "./rapi.mjs";
import {dumpRom, MAX_KIT_SIZE, uploadDoom} from "./transfers.mjs";
import {sha256, sleep} from "./bytes.mjs";
import {isLinuxPort, LinuxConsole, LINUX_USB} from "./linux.mjs";

const byId = id => document.getElementById(id);
const connect = byId("connect"), disconnect = byId("disconnect"),
    connectLinux = byId("connect-linux"), install = byId("install"), kitVersion = byId("kit-version"),
    kitRelease = byId("kit-release"),
    audioOn = byId("audio-on"), audioOff = byId("audio-off"), reset = byId("reset"),
    dump = byId("dump"), upload = byId("upload"),
    image = byId("image"), launcher = byId("launcher"),
    status = byId("status"), meter = byId("progress"), hashes = byId("hashes"),
    amount = byId("amount"), password = byId("password"), device = byId("device"),
    download = byId("download");

let transport, rapi, linux, kit, busy = false, romUrl;
const available = window.isSecureContext && navigator.usb;
const serial = window.isSecureContext && navigator.serial;
const kitFiles = ["DOOMUSB.BIN", "DOOMUSB.EXE"];
const stageLabels = {
    dump: "Dumping WinCE ROM", upload: "Uploading Linux Doom", verify: "Verifying Linux Doom",
    complete: "Transfer complete", failed: "Transfer interrupted",
};
const kib = value => Math.floor(value / 1024).toLocaleString();

function controls() {
    connect.disabled = busy || Boolean(transport || linux) || !available;
    connectLinux.disabled = busy || Boolean(transport || linux) || !serial;
    disconnect.disabled = busy || !(transport || linux);
    install.disabled = busy || Boolean(linux) || !available || !kit;
    dump.disabled = busy || !rapi;
    upload.disabled = busy || !rapi || !image.files.length || !launcher.files.length;
    password.disabled = busy || Boolean(transport);
    image.disabled = launcher.disabled = busy;
    audioOn.disabled = audioOff.disabled = reset.disabled = busy || !linux;
}

function reportProgress({stage, done, total}) {
    if (status.dataset.stage !== stage) {
        status.textContent = stageLabels[stage];
        status.dataset.stage = stage;
    }
    meter.max = total;
    meter.value = done;
    amount.textContent = kib(done) + " of " + kib(total) + " KiB";
    meter.setAttribute("aria-valuetext", amount.textContent);
}

async function perform(action) {
    if (busy) return;
    busy = true;
    controls();
    delete status.dataset.stage;
    try {
        await action();
    } catch (error) {
        status.textContent = error.message || String(error);
        if (transport?.error || (transport && !transport.device.opened) || linux?.disconnected) {
            await endConnection();
        }
    } finally {
        busy = false;
        controls();
    }
}

async function endConnection(options) {
    const connection = transport;
    const console = linux;
    transport = rapi = linux = null;
    device.textContent = "Disconnected";
    await connection?.close(options);
    await console?.close();
}

const isWinCe = port => port.vendorId === IPAQ.vendorId && port.productId === IPAQ.productId;

async function openWinCe() {
    status.textContent = "Select the iPAQ in the USB chooser.";
    const ports = (await navigator.usb.getDevices()).filter(isWinCe);
    const port = ports.length === 1 ? ports[0] : await navigator.usb.requestDevice({filters: [IPAQ]});
    try {
        transport = await ActiveSync.connect(port, password.value, message => { status.textContent = message; });
        const client = new Rapi(transport);
        const version = await client.version();
        if (version.major !== 3 || version.minor !== 0 || version.build !== 11171) {
            throw new Error("WinCE build 11171 is required for these H3970 helpers");
        }
        rapi = client;
        device.textContent = "Connected to WinCE build " + version.build;
        status.textContent = "Ready to transfer";
    } catch (error) {
        await endConnection();
        throw error;
    } finally {
        password.value = "";
    }
}

async function openLinux(port) {
    try {
        linux = await LinuxConsole.connect(port);
    } catch (error) {
        // The cold image briefly disconnects its CDC gadget during startup.
        const deadline = performance.now() + 6000;
        do {
            status.textContent = "Waiting for Linux USB to reconnect…";
            await sleep(650);
            const ports = (await serial.getPorts()).filter(isLinuxPort);
            if (ports.length === 1) {
                try {
                    linux = await LinuxConsole.connect(ports[0]);
                    break;
                } catch {
                    // Wait for the re-enumerated port to finish starting.
                }
            }
        } while (performance.now() < deadline);
        if (!linux) throw new Error(error.message + ". Reconnect the USB cable, then choose Connect Linux again.");
    }
    device.textContent = "Connected to H3970 Linux";
    status.textContent = "Doom controls are ready";
}

connect.addEventListener("click", () => perform(openWinCe));

connectLinux.addEventListener("click", () => perform(async () => {
    const ports = (await serial.getPorts()).filter(isLinuxPort);
    const port = ports.length === 1 ? ports[0] : await serial.requestPort({filters: [LINUX_USB]});
    await openLinux(port);
}));

disconnect.addEventListener("click", () => perform(async () => {
    await endConnection();
    status.textContent = "USB session closed";
}));

dump.addEventListener("click", () => perform(async () => {
    hashes.hidden = true;
    download.hidden = true;
    if (romUrl) URL.revokeObjectURL(romUrl);
    status.textContent = "Starting the WinCE dumper…";
    const result = await dumpRom(rapi, reportProgress);
    romUrl = URL.createObjectURL(new Blob([result.data], {type: "application/octet-stream"}));
    download.href = romUrl;
    download.hidden = false;
    hashes.textContent = "SHA-256  " + result.sha256;
    hashes.hidden = false;
    status.textContent = "ROM verified and ready to save";
}));

upload.addEventListener("click", () => perform(async () => {
    const [bin, exe] = [image.files[0], launcher.files[0]];
    if (bin.size + exe.size > MAX_KIT_SIZE) throw new Error("Doom kit exceeds 128 MiB");
    hashes.hidden = true;
    status.textContent = "Preparing the Doom kit…";
    const result = await uploadDoom(rapi, new Uint8Array(await bin.arrayBuffer()),
        new Uint8Array(await exe.arrayBuffer()), reportProgress);
    hashes.textContent = Object.entries(result).map(([file, hash]) => file + "  " + hash).join("\n");
    hashes.hidden = false;
    status.textContent = "Doom kit verified. WinCE is still running.";
}));

async function fetchReleasedFile(name) {
    const response = await fetch(new URL("./kit/" + name + "?v=" + encodeURIComponent(kit.tag), import.meta.url));
    if (!response.ok) throw new Error("Could not download " + name);
    const bytes = new Uint8Array(await response.arrayBuffer());
    if (await sha256(bytes) !== kit.sha256[name]) throw new Error("Download checksum failed for " + name);
    return bytes;
}

install.addEventListener("click", () => perform(async () => {
    if (!rapi) await openWinCe();
    hashes.hidden = true;
    status.textContent = "Downloading Doom " + kit.tag + "…";
    const [bin, exe] = await Promise.all(kitFiles.map(fetchReleasedFile));
    const stem = "DOOM-" + kit.sha256["DOOMUSB.EXE"].slice(0, 12).toUpperCase();
    await uploadDoom(rapi, bin, exe, reportProgress, stem);
    status.textContent = "Starting Linux Doom…";
    await rapi.launch(DOCUMENTS + "\\" + stem + ".EXE");
    let usbError;
    try {
        await endConnection({boot: true});
    } catch (error) {
        usbError = error;
    }
    status.textContent = "Launcher started. Waiting for the iPAQ to boot Linux…";
    await sleep(21000);
    if (serial) {
        const ports = (await serial.getPorts()).filter(isLinuxPort);
        if (ports.length === 1) {
            await openLinux(ports[0]);
            return;
        }
    }
    const next = "When Doom appears, choose Connect Linux. If it stayed in WinCE, unplug USB and launch " +
        stem + ".EXE from My Documents.";
    status.textContent = usbError ? usbError.message + ". " + next : next;
}));

for (const [button, enabled] of [[audioOn, true], [audioOff, false]]) {
    button.addEventListener("click", () => perform(async () => {
        status.textContent = "Changing Doom audio…";
        await linux.setAudio(enabled);
        status.textContent = enabled ? "Doom audio enabled" : "Doom audio disabled";
    }));
}

reset.addEventListener("click", () => perform(async () => {
    status.textContent = "Resetting the iPAQ to WinCE…";
    await linux.reboot();
    const deadline = performance.now() + 25000;
    do {
        if (linux.disconnected && available && (await navigator.usb.getDevices()).some(isWinCe)) {
            await endConnection();
            status.textContent = "WinCE is back. Choose Connect WinCE to use the USB tools.";
            return;
        }
        await sleep(400);
    } while (performance.now() < deadline);
    const disconnected = linux.disconnected;
    await endConnection();
    status.textContent = disconnected
        ? "Linux disconnected. Once WinCE appears on the iPAQ, choose Connect WinCE."
        : "No USB switch was detected. Check the iPAQ screen before retrying.";
}));

async function loadRelease() {
    try {
        const response = await fetch(new URL("./kit/kit.json", import.meta.url), {cache: "no-cache"});
        if (!response.ok) throw new Error("Release manifest unavailable");
        const manifest = await response.json();
        if (!/^v[0-9][0-9A-Za-z.+-]*$/.test(manifest.tag) ||
            !kitFiles.every(name => /^[a-f0-9]{64}$/.test(manifest.sha256?.[name]))) {
            throw new Error("Invalid release manifest");
        }
        kit = manifest;
        kitVersion.textContent = manifest.tag + " · Doom 1.9 shareware";
        kitRelease.href = "https://github.com/mblsha/ipaq-h3970/releases/tag/" + encodeURIComponent(manifest.tag);
        kitRelease.hidden = false;
    } catch {
        kitVersion.textContent = "No released kit available. You can upload your own kit.";
    }
    controls();
}

image.addEventListener("change", controls);
launcher.addEventListener("change", controls);
if (available) {
    navigator.usb.addEventListener("disconnect", event => {
        if (event.device === transport?.device) {
            endConnection().finally(controls);
            status.textContent = "iPAQ disconnected";
        }
    });
} else {
    status.textContent = "Open this page in a WebUSB-capable browser over HTTPS or localhost.";
}
if (serial) {
    serial.addEventListener("disconnect", event => {
        if (!busy && (event.port ?? event.target) === linux?.port) {
            endConnection().finally(controls);
            status.textContent = "Linux USB disconnected";
        }
    });
}
controls();
loadRelease();
