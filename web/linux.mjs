import {hex, sleep} from "./bytes.mjs";

export const LINUX_USB = {usbVendorId: 0x0525, usbProductId: 0xa4a7};
const identity = "/bin/busybox grep -aqF hp,ipaq-h3970 /sys/firmware/devicetree/base/compatible && test -x /doom/doomctl";
const statusCommand = [
    "/bin/busybox printf 'boot=%s\\nkit=%s\\n' " +
        '"$(/bin/busybox cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo unknown)" ' +
        '"$(/bin/busybox cat /doom/kit-id 2>/dev/null || echo unknown)"',
    'state=$(/doom/doomctl status --machine)',
    'case "$state" in doom=*) echo "$state";; *) ' +
        'if /doom/doomctl status >/dev/null; then ' +
            'read -r pid </tmp/doom.pid; a=on; ' +
            'for pair in nosound:off nomusic:effects nosfx:music; do ' +
                'if /bin/busybox grep -aqF -- "-${pair%:*}" "/proc/$pid/cmdline"; then a=${pair#*:}; break; fi; ' +
            'done; echo "doom=running audio=$a"; ' +
        'else echo "doom=stopped audio=off"; fi;; esac',
    'if /bin/busybox reboot --help >/dev/null 2>&1; then echo reset=yes; else echo reset=no; fi',
].join("; ");

export function isLinuxPort(port) {
    const {usbVendorId, usbProductId} = port.getInfo();
    return usbVendorId === LINUX_USB.usbVendorId && usbProductId === LINUX_USB.usbProductId;
}

export class LinuxConsole {
    static async connect(port) {
        if (!isLinuxPort(port)) throw new Error("Select the iPAQ Linux USB serial port");
        await port.open({baudRate: 115200});
        let console;
        try {
            console = new LinuxConsole(port);
            await console.sync();
            await console.command(identity, 3500);
            return console;
        } catch (error) {
            if (console) await console.close();
            else await port.close().catch(() => {});
            throw error;
        }
    }

    constructor(port) {
        this.port = port;
        this.writer = port.writable.getWriter();
        this.pending = "";
        this.reading = this.read().catch(() => {}).finally(() => { this.disconnected = true; });
    }

    async read() {
        const decoder = new TextDecoder();
        while (!this.closing && this.port.readable) {
            const reader = this.port.readable.getReader();
            this.reader = reader;
            try {
                for (;;) {
                    const {value, done} = await reader.read();
                    if (done) return;
                    this.pending = (this.pending + decoder.decode(value, {stream: true})).slice(-65536);
                }
            } catch (error) {
                if (this.closing || !["BreakError", "BufferOverrunError", "FramingError", "ParityError"].includes(error.name)) throw error;
            } finally {
                reader.releaseLock();
                this.reader = null;
            }
        }
    }

    async write(line, timeout) {
        const bytes = new TextEncoder().encode(line);
        let timer;
        try {
            const deadline = new Promise((_, reject) => {
                timer = setTimeout(() => reject(new Error("Linux USB serial write timed out")), timeout);
            });
            // CDC on the PXA250 uses 64-byte packets. Leave time for the tty to drain
            // between chunks, as the native console does for a recovery UART.
            for (let offset = 0; offset < bytes.length; offset += 64) {
                if (this.disconnected) throw new Error("The Linux USB serial port disconnected");
                await Promise.race([this.writer.write(bytes.subarray(offset, offset + 64)), deadline]);
                await sleep(7);
            }
        } finally {
            clearTimeout(timer);
        }
    }

    async waitReply(reply, timeout) {
        const deadline = performance.now() + timeout;
        do {
            const match = reply.exec(this.pending);
            if (match) return match;
            if (this.disconnected) throw new Error("The Linux USB serial port disconnected");
            await sleep(40);
        } while (performance.now() < deadline);
        this.disconnected = true;
        throw new Error("No reply from the iPAQ Linux console");
    }

    async sync() {
        const nonce = hex(crypto.getRandomValues(new Uint8Array(4)));
        const reply = new RegExp("^H3970_" + nonce + "\\r?$", "m");
        // Check a short message in both directions before sending shell commands.
        await this.write("\n", 1000);
        await sleep(120);
        await this.write("printf '\\nH3970_%s\\n' " + nonce + "\n", 1000);
        await this.waitReply(reply, 2500);
    }

    async command(shell, timeout = 12000) {
        const nonce = hex(crypto.getRandomValues(new Uint8Array(8)));
        const reply = new RegExp("^H3970_" + nonce + ":BEGIN\\r?\\n([\\s\\S]*?)^H3970_" + nonce + ":(\\d+)\\r?$", "m");
        const print = "/bin/busybox printf '\\nH3970_%s:%s\\n' " + nonce;
        this.pending = "";
        await this.write("\n" + print + " BEGIN; " + shell + "; " + print + " \"$?\"\n", Math.min(timeout, 2000));
        const match = await this.waitReply(reply, timeout);
        if (match[2] !== "0") throw new Error("The iPAQ Linux command failed (" + match[2] + ")");
        return match[1].replaceAll("\r", "").trim();
    }

    async snapshot() {
        let output = await this.command("if test -x /doom/device-status; then /doom/device-status; else echo legacy; fi", 3500);
        if (/^legacy$/m.test(output)) output = await this.command(statusCommand, 5000);
        const boot = /^boot=([a-f0-9-]{36}|unknown)$/m.exec(output)?.[1];
        const kit = /^kit=([a-zA-Z0-9.+-]{1,64})$/m.exec(output)?.[1];
        const doom = /^doom=(running|stopped) audio=(off|on|effects|music)$/m.exec(output);
        const reset = /^reset=(yes|no)$/m.exec(output)?.[1];
        const rawMix = /^mix=(\d{1,3}),(\d{1,3})$/m.exec(output);
        const mix = rawMix && rawMix.slice(1).map(Number);
        if (!boot || !kit || !doom || !reset) throw new Error("Linux returned an invalid Doom status");
        if (mix && mix.some(level => level > 100)) throw new Error("Linux returned invalid volume levels");
        return {boot, kit, doom: doom[1], audio: doom[2], mix, reset: reset === "yes"};
    }

    async setMix([music, effects]) {
        for (const level of [music, effects]) {
            if (!Number.isInteger(level) || level < 0 || level > 100) throw new Error("Volume must be between 0 and 100");
        }
        const result = await this.command("/doom/doomctl mix " + music + " " + effects);
        if (!result.split("\n").includes("mix=" + music + "," + effects)) {
            throw new Error("The iPAQ did not confirm the volume change");
        }
    }

    async setAudio(enabled) {
        const audio = enabled ? "on" : "off";
        const state = await this.snapshot();
        if (state.doom !== "running" || state.audio !== audio) await this.restartDemo(audio);
    }

    async reboot() {
        try {
            await this.command("/bin/busybox reboot --help >/dev/null 2>&1");
        } catch (error) {
            if (!this.disconnected) {
                throw new Error("Could not start a software reset. The released browser kit includes the reset command.", {cause: error});
            }
            throw error;
        }
        await this.command("/bin/busybox sync && { ( /bin/busybox sleep 1; /bin/busybox reboot -f ) </dev/null >/dev/null 2>&1 & }");
    }

    async restartDemo(audio) {
        audio ??= (await this.snapshot()).audio;
        const option = {on: "--audio", off: "0", effects: "--effects", music: "--music"}[audio];
        await this.command("/doom/doomctl stop && /doom/doomctl start " + option);
        await sleep(400);
        const state = await this.snapshot();
        if (state.doom !== "running" || state.audio !== audio) {
            throw new Error("Doom did not restart in the requested audio mode");
        }
    }

    async close() {
        this.closing = true;
        await this.reader?.cancel().catch(() => {});
        await this.reading;
        this.writer.releaseLock();
        await this.port.close().catch(() => {});
    }
}
