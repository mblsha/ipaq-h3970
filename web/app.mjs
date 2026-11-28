import {ActiveSync, IPAQ} from "./transport.mjs";
import {DOCUMENTS, isCalibration, Rapi} from "./rapi.mjs";
import {dumpRom, MAX_KIT_SIZE, uploadDoom} from "./transfers.mjs";
import {sha256} from "./bytes.mjs";
import {isLinuxPort, LinuxConsole, LINUX_USB} from "./linux.mjs";
import {prepareToday, restoreToday} from "./today.mjs";
import {SharedTerminal} from "./terminal.mjs";

const byId = id => document.getElementById(id);
const connect = byId("connect"), disconnect = byId("disconnect"),
    releasePanel = byId("release-panel"), winCeTools = byId("wince-tools"),
    passwordField = byId("password-field"), install = byId("install"), kitVersion = byId("kit-version"),
    kitRelease = byId("kit-release"),
    doomControls = byId("doom-controls"), linuxActions = byId("linux-actions"),
    audio = byId("audio"), restart = byId("restart"), start = byId("start"), exitDoom = byId("exit-doom"),
    reset = byId("reset"), resetHint = byId("reset-hint"),
    mixPanel = byId("mix"), music = byId("music-volume"), effects = byId("effect-volume"),
    dump = byId("dump"), upload = byId("upload"),
    image = byId("image"), launcher = byId("launcher"),
    status = byId("status"), transfer = byId("transfer"), meter = byId("progress"), hashes = byId("hashes"),
    amount = byId("amount"), password = byId("password"), device = byId("device"),
    download = byId("download"), terminalPanel = byId("terminal-panel"), terminalStatus = byId("terminal-status");

let transport, rapi, linux, kit, busy = false, romUrl;
let snapshot, transition, bootCalibration, paused = false, watchTimer, watchTask, retryAt = 0;
let linuxUsbSeen = false, connectedBefore = false, handledReturnRequest;
let lastLinuxStatus = 0, terminalPending = false;
const sharedTerminal = new SharedTerminal(byId("terminal-screen"), terminalStatus, () => {
    terminalPending = true;
    if (!watchTask && !busy) scheduleDiscovery();
});
const calibrationKey = "h3970-boot-calibration";
const transitionKey = "h3970-pending-return";
const calibrationGrace = 120000;
const available = window.isSecureContext && navigator.usb;
const serial = window.isSecureContext && navigator.serial;
const linuxUsb = {vendorId: LINUX_USB.usbVendorId, productId: LINUX_USB.usbProductId};
const kitFiles = ["DOOMUSB.BIN", "DOOMUSB.EXE"];
const stageLabels = {
    dump: "Dumping WinCE ROM", upload: "Uploading Linux Doom", verify: "Verifying Linux Doom",
    complete: "Transfer complete", failed: "Transfer interrupted",
};
const kib = value => Math.floor(value / 1024).toLocaleString();

function readSession(key) {
    try { return JSON.parse(sessionStorage.getItem(key)); } catch { return null; }
}

function saveSession(key, value) {
    try {
        if (value) sessionStorage.setItem(key, JSON.stringify(value));
        else sessionStorage.removeItem(key);
    } catch {}
}

function setTransition(next) {
    transition = next;
    saveSession(transitionKey, next);
}

function showWaiting() {
    if (transition) {
        device.textContent = transition.target === "linux" ? "Booting Linux…" : "Returning to WinCE…";
    } else {
        device.textContent = connectedBefore ? "Waiting to reconnect to the iPAQ…" : "No iPAQ connected";
    }
}

function calibrationForBoot() {
    const saved = bootCalibration ?? readSession(calibrationKey);
    return snapshot?.boot && snapshot.boot !== "unknown" && saved?.boot === snapshot.boot &&
        isCalibration(saved.value) ? saved : null;
}

function controls() {
    const connected = Boolean(transport || linux);
    const connecting = busy || (watchTask && !rapi && !linux);
    const target = transition?.target;
    const mode = target ?? (linuxUsbSeen ? "linux" : undefined);
    const canConnect = mode === "linux" ? serial : mode === "wince" ? available : available || serial;
    const inLinux = Boolean(linux && snapshot);
    const running = inLinux && snapshot.doom === "running";
    connect.textContent = mode === "linux" ? "Allow Linux serial access" :
        mode === "wince" ? "Allow WinCE USB access" : "Connect iPAQ";
    connect.disabled = connecting || connected || !canConnect;
    connect.hidden = connected;
    disconnect.disabled = busy || !(transport || linux);
    disconnect.hidden = !connected;
    releasePanel.hidden = Boolean(linux || transition) || !available;
    install.disabled = connecting || releasePanel.hidden || !kit;
    winCeTools.hidden = Boolean(linux) || target === "linux" || !available;
    dump.disabled = busy || !rapi;
    upload.disabled = busy || !rapi || !image.files.length || !launcher.files.length;
    password.disabled = busy || Boolean(transport);
    passwordField.hidden = Boolean(transport);
    image.disabled = launcher.disabled = busy;
    doomControls.hidden = !running;
    audio.disabled = restart.disabled = busy || !running;
    exitDoom.hidden = !running || !snapshot.terminal;
    exitDoom.disabled = busy || exitDoom.hidden;
    audio.setAttribute("aria-pressed", String(running && snapshot.audio !== "off"));
    mixPanel.hidden = !running || !snapshot.mix;
    music.disabled = effects.disabled = busy || mixPanel.hidden;
    linuxActions.hidden = !inLinux;
    terminalPanel.hidden = !inLinux || running || !snapshot.terminal;
    if (terminalPanel.hidden) sharedTerminal.stop();
    start.hidden = !inLinux || running;
    start.disabled = busy || start.hidden;
    const canReturn = inLinux && snapshot.reset && available && calibrationForBoot();
    reset.disabled = busy || Boolean(transition) || !canReturn;
    resetHint.hidden = !inLinux || Boolean(canReturn);
    resetHint.textContent = resetHint.hidden ? "" :
        !snapshot.reset ? "This kit cannot reset to WinCE." :
        !available ? "WinCE recovery needs a browser with WebUSB access." :
        "The iPAQ needs a saved touch calibration. Boot the kit from calibrated WinCE to capture it.";
}

function reportProgress({stage, done, total}) {
    transfer.hidden = false;
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
        status.textContent = error.name === "NotFoundError"
            ? "No iPAQ was selected. Check its USB connection and try again."
            : error.message || String(error);
        if (transport?.error || (transport && !transport.device.opened) || linux?.disconnected) {
            await endConnection();
            showWaiting();
            scheduleDiscovery();
        }
    } finally {
        busy = false;
        transfer.hidden = true;
        controls();
    }
}

async function endConnection(options) {
    const connection = transport;
    const console = linux;
    sharedTerminal.stop();
    terminalPending = false;
    transport = rapi = linux = snapshot = null;
    await connection?.close(options);
    await console?.close();
}

const isWinCe = port => port.vendorId === IPAQ.vendorId && port.productId === IPAQ.productId;
const isLinuxUsb = port => port.vendorId === linuxUsb.vendorId && port.productId === linuxUsb.productId;

function releasedStem() {
    return "DOOM-" + kit.sha256["DOOMUSB.EXE"].slice(0, 12).toUpperCase();
}

async function inspectWinCe(client, build) {
    const names = await client.list(6000);
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
    let restore;
    if (transition?.target === "wince" && transition.calibration?.serial &&
        transition.calibration.serial !== port.serialNumber) {
        throw new Error("Select the same iPAQ to restore its calibration");
    }
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
            status.textContent = snapshot.failure ? "Last launcher report " + snapshot.failure : "";
            if (transition?.target === "wince") {
                restore = transition.calibration;
            }
        }
        if (!restore) setTransition(null);
    } catch (error) {
        await endConnection();
        throw error;
    }
    if (restore) {
        status.textContent = "Restoring calibration and opening WinCE Today…";
        try {
            await restoreToday(rapi, restore.value);
            setTransition(null);
            status.textContent = "WinCE Today is ready";
        } catch (error) {
            if (transport.error || !transport.device.opened) {
                await endConnection();
                throw error;
            }
            setTransition(null);
            status.textContent = "WinCE is connected. " + error.message;
        }
    }
}

function showWinCe() {
    device.textContent = "WinCE build " + snapshot.build + " · " + snapshot.staged;
}

async function refreshLinux() {
    const previousMix = snapshot?.mix;
    snapshot = await linux.snapshot();
    lastLinuxStatus = Date.now();
    snapshot.mix?.forEach((level, index) => {
        if (previousMix?.[index] !== level) setSlider(index, level);
    });
    const kitLabel = snapshot.kit === "unknown" ? "" : " " + snapshot.kit;
    const audio = {off: "Muted", on: "Audio on", effects: "Effects only", music: "Music only"};
    device.textContent = "H3970 Linux" + kitLabel + " · Doom " + snapshot.doom +
        (snapshot.doom === "running" ? " · " + audio[snapshot.audio] : "");
}

async function refreshSharedTerminal() {
    terminalPending = false;
    controls();
    if (terminalPanel.hidden) return;
    try {
        terminalPending = await sharedTerminal.update(linux, snapshot.boot);
    } catch (error) {
        sharedTerminal.stop();
        if (linux.disconnected) throw error;
        terminalStatus.textContent = error.message;
    }
}

const sliders = [[music, byId("music-level")], [effects, byId("effect-level")]];

function setSlider(index, value) {
    const [control, label] = sliders[index];
    control.value = value;
    label.textContent = value + "%";
}

sliders.forEach(([control], index) => {
    control.addEventListener("input", () => setSlider(index, control.value));
    control.addEventListener("change", () => {
        const mix = sliders.map(([input]) => Number(input.value));
        return perform(async () => {
            try {
                status.textContent = "Changing Doom volume…";
                await requireLinux().setMix(mix);
                await refreshLinux();
                status.textContent = "Doom volume updated";
            } catch (error) {
                snapshot?.mix?.forEach((level, channel) => setSlider(channel, level));
                throw error;
            }
        });
    });
});

function requireLinux() {
    if (!linux || linux.disconnected) throw new Error("Reconnect the iPAQ in Linux first");
    return linux;
}

async function openLinux(port) {
    linux = await LinuxConsole.connect(port);
    let backupError;
    try {
        await refreshLinux();
        const handoff = transition?.target === "linux" && transition.kit === snapshot.kit &&
            isCalibration(transition.calibration?.value) ? transition.calibration : null;
        const stored = await linux.readWinCeCalibration();
        const saved = stored ?? handoff ?? calibrationForBoot();
        if (saved && snapshot.boot !== "unknown") {
            bootCalibration = {...saved, serial: saved.serial || handoff?.serial, boot: snapshot.boot};
            saveSession(calibrationKey, bootCalibration);
            if (!stored || (!stored.serial && bootCalibration.serial)) {
                try {
                    await linux.saveWinCeCalibration(bootCalibration);
                } catch (error) {
                    if (linux.disconnected) throw error;
                    backupError = error;
                }
            }
        }
    } catch (error) {
        await endConnection();
        throw error;
    }
    if (transition?.target === "linux" && snapshot.kit !== "unknown" && transition.kit !== snapshot.kit) {
        status.textContent = "Linux answered with a different kit than requested.";
    } else if (transition?.target === "wince") {
        status.textContent = transition.boot !== "unknown" && transition.boot === snapshot.boot
            ? "Linux still answers on the previous boot. WinCE reset was not confirmed."
            : "Linux answered. WinCE reset was not confirmed.";
    } else if (backupError) {
        status.textContent = "Could not save calibration on the iPAQ. Keep this tab open to return to WinCE.";
    } else {
        status.textContent = "";
    }
    setTransition(null);
}

async function permittedDevices() {
    const [winDevices, serialPorts] = await Promise.all([
        available ? navigator.usb.getDevices() : [],
        serial ? serial.getPorts() : [],
    ]);
    linuxUsbSeen = Boolean(serial && winDevices.some(isLinuxUsb) && !winDevices.some(isWinCe));
    return [
        ...winDevices.filter(isWinCe).map(port => ({mode: "wince", port})),
        ...serialPorts.filter(isLinuxPort).map(port => ({mode: "linux", port})),
    ];
}

function expectedDevice({mode, port}) {
    if (mode !== transition.target) return false;
    const serialNumber = mode === "wince" && transition.calibration?.serial;
    return !serialNumber || port.serialNumber === serialNumber;
}

async function openDevice({mode, port}) {
    if (mode === "wince") {
        const pass = password.value;
        password.value = "";
        await openWinCe(port, pass);
    } else {
        await openLinux(port);
    }
    connectedBefore = true;
}

async function connectDevice(mode = transition?.target) {
    const candidates = await permittedDevices();
    const matches = transition ? candidates.filter(expectedDevice) :
        mode ? candidates.filter(candidate => candidate.mode === mode) : candidates;
    if (matches.length === 1) {
        await openDevice(matches[0]);
        return;
    }
    mode ??= !serial ? "wince" : !available || linuxUsbSeen ? "linux" : undefined;
    if (!mode) {
        const port = await navigator.usb.requestDevice({filters: [IPAQ, linuxUsb]});
        if (isWinCe(port)) {
            await openDevice({mode: "wince", port});
        } else if (isLinuxUsb(port)) {
            linuxUsbSeen = true;
            status.textContent = "Linux detected. Allow serial access to connect.";
            if (navigator.userActivation?.isActive) await connectDevice("linux");
        }
        return;
    }
    const serialNumber = mode === "wince" && transition?.calibration?.serial;
    const filter = serialNumber ? {...IPAQ, serialNumber} : IPAQ;
    const port = mode === "wince"
        ? await navigator.usb.requestDevice({filters: [filter]})
        : await serial.requestPort({filters: [LINUX_USB]});
    await openDevice({mode, port});
}

connect.addEventListener("click", () => perform(async () => {
    paused = false;
    await connectDevice();
}));

disconnect.addEventListener("click", () => perform(async () => {
    paused = true;
    connectedBefore = false;
    setTransition(null);
    await endConnection();
    device.textContent = "Disconnected";
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
    if (!rapi) await connectDevice("wince");
    hashes.hidden = true;
    status.textContent = "Downloading Doom " + kit.tag + "…";
    const [bin, exe] = await Promise.all(kitFiles.map(fetchReleasedFile));
    const stem = releasedStem();
    await uploadDoom(rapi, bin, exe, reportProgress, stem);
    let calibration;
    try {
        calibration = {value: await rapi.readCalibration(), serial: transport.device.serialNumber};
    } catch (error) {
        if (transport.error || !transport.device.opened) throw error;
    }
    status.textContent = "Starting Linux Doom…";
    setTransition({target: "linux", kit: kit.tag, calibration, failure: snapshot?.failure, deadline: Date.now() + 35000});
    let acknowledged = false;
    try {
        await rapi.launch(DOCUMENTS + "\\" + stem + ".EXE");
        acknowledged = true;
    } catch (error) {
        if (!transport?.error && transport?.device.opened) {
            setTransition(null);
            throw error;
        }
    } finally {
        if (transition) await endConnection({boot: true}).catch(() => {});
    }
    status.textContent = acknowledged ? "Waiting for a Linux status reply…" :
        "Launcher acknowledgment lost. Checking the iPAQ state…";
    showWaiting();
    scheduleDiscovery(250);
}));

audio.addEventListener("click", () => perform(async () => {
    if (snapshot?.doom !== "running") return;
    const enabled = snapshot.audio === "off";
    status.textContent = "Changing Doom audio…";
    await requireLinux().setAudio(enabled);
    await refreshLinux();
    status.textContent = enabled ? "Doom audio enabled" : "Doom audio disabled";
}));

exitDoom.addEventListener("click", () => perform(async () => {
    status.textContent = "Exiting Doom…";
    await requireLinux().stopDemo();
    await refreshLinux();
    await refreshSharedTerminal();
    if (!terminalPanel.hidden) sharedTerminal.focus();
    status.textContent = "";
}));

for (const button of [start, restart]) {
    button.addEventListener("click", () => perform(async () => {
        const running = snapshot?.doom === "running";
        status.textContent = running ? "Restarting the Doom demo…" : "Starting the Doom demo…";
        if (!running) sharedTerminal.stop();
        await requireLinux().restartDemo();
        await refreshLinux();
        status.textContent = running ? "Doom demo restarted" : "Doom demo started";
    }));
}

async function returnToWinCe(request) {
    const connection = requireLinux();
    if (!available || !snapshot?.reset) throw new Error("WinCE recovery needs WebUSB and a kit with reset support");
    const calibration = await connection.readWinCeCalibration() ?? calibrationForBoot();
    if (!calibration) throw new Error("Reset requires saved calibration from this Linux boot");
    await prepareToday();
    if (request) await connection.claimWinCeReturn(request);
    status.textContent = "Resetting the iPAQ to WinCE…";
    setTransition({target: "wince", boot: snapshot.boot, calibration, deadline: Date.now() + 20000});
    try {
        await connection.reboot();
    } catch (error) {
        if (!connection.disconnected) {
            setTransition(null);
            throw error;
        }
    }
    await endConnection();
    showWaiting();
    status.textContent = "Waiting for a WinCE status reply…";
    scheduleDiscovery(250);
}

reset.addEventListener("click", () => perform(() => returnToWinCe(snapshot?.returnRequest)));

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
            showWaiting();
            status.textContent = "USB session lost. Checking the iPAQ state…";
        } else if (rapi) {
            snapshot = await inspectWinCe(rapi, snapshot.build);
            showWinCe();
            return;
        } else {
            if (Date.now() - lastLinuxStatus >= 1100) await refreshLinux();
            const request = snapshot.returnRequest;
            if (!busy && request && request !== handledReturnRequest) {
                handledReturnRequest = request;
                try {
                    await returnToWinCe(request);
                } catch (error) {
                    if (linux?.disconnected) throw error;
                    status.textContent = error.message;
                }
                return;
            }
            await refreshSharedTerminal();
            return;
        }
    }
    const candidates = await permittedDevices();
    let matches = candidates;
    if (transition) {
        const expected = candidates.filter(expectedDevice);
        if (expected.length || Date.now() < transition.deadline) matches = expected;
    }
    if (matches.length !== 1) {
        if (transition && Date.now() >= transition.deadline + calibrationGrace) {
            status.textContent = "The iPAQ did not answer after the switch. Check the USB cable and reconnect.";
            connectedBefore = true;
            setTransition(null);
        }
        if (matches.length > 1) {
            device.textContent = "Multiple iPAQ connections found. Choose the device to use.";
        } else {
            showWaiting();
        }
        return;
    }
    if (Date.now() < retryAt) return;
    const selected = matches[0];
    const label = selected.mode === "wince" ? "WinCE" : "Linux USB";
    device.textContent = label + " detected · Checking for a reply…";
    try {
        await openDevice(selected);
    } catch (error) {
        device.textContent = label + " detected · No reply";
        status.textContent = error.message;
        retryAt = Date.now() + 4000;
        if (/ActiveSync password|RAPI password/.test(error.message)) paused = true;
        if (transition && Date.now() >= transition.deadline + calibrationGrace) {
            setTransition(null);
        }
    }
}

function scheduleDiscovery(delay = 0) {
    clearTimeout(watchTimer);
    if (!available && !serial) return;
    watchTimer = setTimeout(async () => {
        if (!paused && !busy && !watchTask) {
            watchTask = discover().catch(async error => {
                await endConnection();
                showWaiting();
                status.textContent = error.message || String(error);
            }).finally(() => {
                watchTask = null;
                controls();
            });
            controls();
            await watchTask;
        }
        const delay = transition ? 500 : !terminalPanel.hidden ? (terminalPending ? 0 : 180) : linux ? 1200 : 3500;
        scheduleDiscovery(delay);
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
const pending = readSession(transitionKey);
if (pending?.deadline + calibrationGrace > Date.now() &&
    (pending.target === "linux" || (pending.target === "wince" && isCalibration(pending.calibration?.value)))) {
    transition = pending;
    showWaiting();
    status.textContent = pending.target === "linux" ? "Waiting for a Linux status reply…" : "Waiting for a WinCE status reply…";
}
controls();
loadRelease();
scheduleDiscovery();
