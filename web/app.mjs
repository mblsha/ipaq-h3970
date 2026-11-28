import {ActiveSync, IPAQ} from "./transport.mjs";
import {DOCUMENTS, Rapi} from "./rapi.mjs";
import {dumpRom, MAX_KIT_SIZE, uploadDoom} from "./transfers.mjs";
import {sha256} from "./bytes.mjs";
import {isLinuxPort, LinuxConsole, LINUX_USB} from "./linux.mjs";

const byId = id => document.getElementById(id);
const connect = byId("connect"), disconnect = byId("disconnect"),
    connectLinux = byId("connect-linux"), install = byId("install"), kitVersion = byId("kit-version"),
    kitRelease = byId("kit-release"),
    audioOn = byId("audio-on"), audioOff = byId("audio-off"),
    restart = byId("restart"), reset = byId("reset"),
    dump = byId("dump"), upload = byId("upload"),
    image = byId("image"), launcher = byId("launcher"),
    status = byId("status"), meter = byId("progress"), hashes = byId("hashes"),
    amount = byId("amount"), password = byId("password"), device = byId("device"),
    download = byId("download");

let transport, rapi, linux, kit, busy = false, romUrl;
let snapshot, transition, paused = false, watchTimer, watchTask, retryAt = 0;
const available = window.isSecureContext && navigator.usb;
const serial = window.isSecureContext && navigator.serial;
const kitFiles = ["DOOMUSB.BIN", "DOOMUSB.EXE"];
const stageLabels = {
    dump: "Dumping WinCE ROM", upload: "Uploading Linux Doom", verify: "Verifying Linux Doom",
    complete: "Transfer complete", failed: "Transfer interrupted",
};
const kib = value => Math.floor(value / 1024).toLocaleString();

function controls() {
    const connecting = watchTask && !rapi && !linux;
    connect.disabled = busy || connecting || Boolean(transport || linux) || !available;
    connectLinux.disabled = busy || connecting || Boolean(transport || linux) || !serial;
    disconnect.disabled = busy || !(transport || linux);
    install.disabled = busy || connecting || Boolean(linux || transition) || !available || !kit;
    dump.disabled = busy || !rapi;
    upload.disabled = busy || !rapi || !image.files.length || !launcher.files.length;
    password.disabled = busy || Boolean(transport);
    image.disabled = launcher.disabled = busy;
    audioOn.disabled = audioOff.disabled = restart.disabled = busy || !linux || !snapshot;
    reset.disabled = busy || !linux || !snapshot?.reset;
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
        if (watchTask) await watchTask;
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
    transport = rapi = linux = snapshot = null;
    device.textContent = "Disconnected";
    await connection?.close(options);
    await console?.close();
}

const isWinCe = port => port.vendorId === IPAQ.vendorId && port.productId === IPAQ.productId;

function releasedStem() {
    return "DOOM-" + kit.sha256["DOOMUSB.EXE"].slice(0, 12).toUpperCase();
}

async function inspectWinCe(client, build) {
    const names = await client.list();
    const pairs = [...names].flatMap(([path, entry]) => {
        if (entry.directory || !entry.size || !/\\DOOM(?:USB|-[A-F0-9]{12})\.EXE$/.test(path)) return [];
        const bin = names.get(path.replace(/\.EXE$/, ".BIN"));
        return bin && !bin.directory && bin.size ? [path.slice(DOCUMENTS.length + 1, -4)] : [];
    });
    const report = DOCUMENTS + "\\DOOMN3-FAIL.TXT";
    const entry = names.get(report);
    const failure = entry && !entry.directory && entry.size <= 256
        ? new TextDecoder("utf-16le").decode(await client.readAll(report, 256)).replace(/\0+$/, "") : "";
    const current = kit && pairs.includes(releasedStem());
    let staged = "No complete Doom kit staged";
    if (current) staged = "Release files present";
    else if (pairs.length) staged = "Staged Doom kits " + pairs.length;
    return {build, staged, failure};
}

async function openWinCe(port, pass = "") {
    try {
        transport = await ActiveSync.connect(port, pass, message => { status.textContent = message; });
        const client = new Rapi(transport);
        const version = await client.version();
        if (version.major !== 3 || version.minor !== 0 || version.build !== 11171) {
            throw new Error("WinCE build 11171 is required for these H3970 helpers");
        }
        snapshot = await inspectWinCe(client, version.build);
        rapi = client;
        showWinCe();
        if (transition?.target === "linux") {
            status.textContent = snapshot.failure && snapshot.failure !== transition.failure
                ? "Linux launcher reported " + snapshot.failure : "WinCE answered again. Linux boot was not confirmed.";
        } else {
            status.textContent = transition ? "Returned to WinCE" : "WinCE connection ready";
            if (snapshot.failure) status.textContent += " · Last launcher report " + snapshot.failure;
        }
        transition = null;
    } catch (error) {
        await endConnection();
        throw error;
    }
}

function showWinCe() {
    device.textContent = "WinCE build " + snapshot.build + " · " + snapshot.staged;
}

async function refreshLinux() {
    snapshot = await linux.snapshot();
    const kitLabel = snapshot.kit === "unknown" ? "" : " " + snapshot.kit;
    const audio = {off: "Muted", on: "Audio on", effects: "Effects only", music: "Music only"};
    device.textContent = "H3970 Linux" + kitLabel + " · Doom " + snapshot.doom +
        (snapshot.doom === "running" ? " · " + audio[snapshot.audio] : "");
}

function requireLinux() {
    if (!linux || linux.disconnected) throw new Error("Reconnect the iPAQ in Linux first");
    return linux;
}

async function openLinux(port) {
    linux = await LinuxConsole.connect(port);
    try {
        await refreshLinux();
    } catch (error) {
        await endConnection();
        throw error;
    }
    if (transition?.target === "linux" && snapshot.kit !== "unknown" && transition.kit !== snapshot.kit) {
        status.textContent = "Linux answered with a different kit than requested.";
    } else if (transition?.target === "wince") {
        status.textContent = "Linux still answers on the previous boot. WinCE reset was not confirmed.";
    } else {
        status.textContent = "Linux connection ready";
    }
    transition = null;
}

async function chooseWinCe() {
    const ports = (await navigator.usb.getDevices()).filter(isWinCe);
    if (ports.length === 1) return ports[0];
    status.textContent = "Select the iPAQ in the USB chooser.";
    return navigator.usb.requestDevice({filters: [IPAQ]});
}

async function connectWithPassword() {
    const pass = password.value;
    password.value = "";
    await openWinCe(await chooseWinCe(), pass);
}

connect.addEventListener("click", () => perform(async () => {
    paused = false;
    await connectWithPassword();
}));

connectLinux.addEventListener("click", () => perform(async () => {
    paused = false;
    const ports = (await serial.getPorts()).filter(isLinuxPort);
    const port = ports.length === 1 ? ports[0] : await serial.requestPort({filters: [LINUX_USB]});
    await openLinux(port);
}));

disconnect.addEventListener("click", () => perform(async () => {
    paused = true;
    transition = null;
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
    paused = false;
    if (!rapi) await connectWithPassword();
    hashes.hidden = true;
    status.textContent = "Downloading Doom " + kit.tag + "…";
    const [bin, exe] = await Promise.all(kitFiles.map(fetchReleasedFile));
    const stem = releasedStem();
    await uploadDoom(rapi, bin, exe, reportProgress, stem);
    status.textContent = "Starting Linux Doom…";
    transition = {target: "linux", kit: kit.tag, failure: snapshot?.failure, deadline: Date.now() + 35000};
    let acknowledged = false;
    try {
        await rapi.launch(DOCUMENTS + "\\" + stem + ".EXE");
        acknowledged = true;
    } catch (error) {
        if (!transport?.error && transport?.device.opened) {
            transition = null;
            throw error;
        }
    } finally {
        if (transition) await endConnection({boot: true}).catch(() => {});
    }
    status.textContent = acknowledged ? "Waiting for a Linux status reply…" :
        "Launcher acknowledgment lost. Checking the iPAQ state…";
    device.textContent = "Booting Linux…";
    scheduleDiscovery(250);
}));

for (const [button, enabled] of [[audioOn, true], [audioOff, false]]) {
    button.addEventListener("click", () => perform(async () => {
        status.textContent = "Changing Doom audio…";
        await requireLinux().setAudio(enabled);
        await refreshLinux();
        status.textContent = enabled ? "Doom audio enabled" : "Doom audio disabled";
    }));
}

restart.addEventListener("click", () => perform(async () => {
    status.textContent = "Restarting the Doom demo…";
    await requireLinux().restartDemo();
    await refreshLinux();
    status.textContent = "Doom demo restarted";
}));

reset.addEventListener("click", () => perform(async () => {
    const connection = requireLinux();
    status.textContent = "Resetting the iPAQ to WinCE…";
    transition = {target: "wince", boot: snapshot.boot, deadline: Date.now() + 20000};
    try {
        await connection.reboot();
    } catch (error) {
        if (!connection.disconnected) {
            transition = null;
            throw error;
        }
    }
    await endConnection();
    device.textContent = "Returning to WinCE…";
    status.textContent = "Waiting for a WinCE status reply…";
    scheduleDiscovery(250);
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

async function discover() {
    if (rapi || linux) {
        if (transport?.error || linux?.disconnected) {
            await endConnection();
            status.textContent = "USB session lost. Checking the iPAQ state…";
            return;
        }
        if (rapi) {
            snapshot = await inspectWinCe(rapi, snapshot.build);
            showWinCe();
        } else {
            await refreshLinux();
        }
        return;
    }
    const [winDevices, serialPorts] = await Promise.all([
        available ? navigator.usb.getDevices() : [],
        serial ? serial.getPorts() : [],
    ]);
    const candidates = [
        ...winDevices.filter(isWinCe).map(port => ({mode: "wince", port})),
        ...serialPorts.filter(isLinuxPort).map(port => ({mode: "linux", port})),
    ];
    if (candidates.length > 1) {
        device.textContent = "Multiple iPAQ interfaces found. Choose a connection.";
        return;
    }
    const selected = candidates[0];
    if (transition && Date.now() < transition.deadline && selected?.mode !== transition.target) return;
    if (!selected) {
        device.textContent = "Disconnected";
        if (transition) {
            status.textContent = transition.target === "linux"
                ? "Linux boot unconfirmed. If Doom appeared, choose Connect Linux."
                : "WinCE return unconfirmed. If WinCE appeared, choose Connect WinCE.";
            transition = null;
        }
        return;
    }
    if (Date.now() < retryAt) return;
    const label = selected.mode === "wince" ? "WinCE" : "Linux USB";
    device.textContent = label + " detected · Checking for a reply…";
    try {
        if (selected.mode === "wince") await openWinCe(selected.port);
        else await openLinux(selected.port);
    } catch (error) {
        device.textContent = label + " detected · No reply";
        status.textContent = error.message;
        retryAt = Date.now() + 4000;
        if (/ActiveSync password|RAPI password/.test(error.message)) paused = true;
        if (transition && Date.now() >= transition.deadline) transition = null;
    }
}

function scheduleDiscovery(delay = 0) {
    clearTimeout(watchTimer);
    if (!available && !serial) return;
    watchTimer = setTimeout(async () => {
        if (!paused && !busy && !watchTask) {
            watchTask = discover().catch(async error => {
                await endConnection();
                status.textContent = error.message || String(error);
            }).finally(() => {
                watchTask = null;
                controls();
            });
            controls();
            await watchTask;
        }
        scheduleDiscovery(transition ? 500 : 3500);
    }, delay);
}

function deviceChanged() {
    retryAt = 0;
    scheduleDiscovery();
}

image.addEventListener("change", controls);
launcher.addEventListener("change", controls);
if (available) {
    navigator.usb.addEventListener("connect", deviceChanged);
    navigator.usb.addEventListener("disconnect", event => {
        if (event.device === transport?.device) transport.fail(new Error("WinCE USB disconnected"));
        deviceChanged();
    });
}
if (serial) {
    serial.addEventListener("connect", deviceChanged);
    serial.addEventListener("disconnect", event => {
        if ((event.port ?? event.target) === linux?.port) linux.disconnected = true;
        deviceChanged();
    });
}
if (!available && !serial) {
    status.textContent = "Open this page in a WebUSB-capable browser over HTTPS or localhost.";
}
document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "visible") deviceChanged();
});
controls();
loadRelease().finally(() => scheduleDiscovery());
