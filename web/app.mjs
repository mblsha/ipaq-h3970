import {ActiveSync, IPAQ} from "./transport.mjs";
import {Rapi} from "./rapi.mjs";
import {dumpRom, MAX_KIT_SIZE, uploadDoom} from "./transfers.mjs";

const byId = id => document.getElementById(id);
const connect = byId("connect"), disconnect = byId("disconnect"),
    dump = byId("dump"), upload = byId("upload"),
    image = byId("image"), launcher = byId("launcher"),
    status = byId("status"), meter = byId("progress"), hashes = byId("hashes"),
    amount = byId("amount"), password = byId("password"), device = byId("device"),
    download = byId("download");

let transport, rapi, busy = false, romUrl;
const available = window.isSecureContext && navigator.usb;
const stageLabels = {
    dump: "Dumping WinCE ROM", upload: "Uploading Linux Doom", verify: "Verifying Linux Doom",
    complete: "Transfer complete", failed: "Transfer interrupted",
};
const kib = value => Math.floor(value / 1024).toLocaleString();

function controls() {
    connect.disabled = busy || Boolean(transport) || !available;
    disconnect.disabled = busy || !transport;
    dump.disabled = busy || !rapi;
    upload.disabled = busy || !rapi || !image.files.length || !launcher.files.length;
    password.disabled = busy || Boolean(transport);
    image.disabled = launcher.disabled = busy;
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
        if (transport?.error || (transport && !transport.device.opened)) {
            await endConnection();
        }
    } finally {
        busy = false;
        controls();
    }
}

async function endConnection() {
    const connection = transport;
    transport = rapi = null;
    await connection?.close();
    device.textContent = "Disconnected";
}

connect.addEventListener("click", () => perform(async () => {
    status.textContent = "Select the iPAQ in the USB chooser.";
    const port = await navigator.usb.requestDevice({filters: [IPAQ]});
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
controls();
