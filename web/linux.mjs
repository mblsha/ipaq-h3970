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
            await console.write("\n", 1000);
            await sleep(60);
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
        this.reader = port.readable.getReader();
        this.writer = port.writable.getWriter();
        this.pending = "";
        this.reading = this.read().catch(() => {}).finally(() => { this.disconnected = true; });
    }

    async read() {
        const decoder = new TextDecoder();
        for (;;) {
            const {value, done} = await this.reader.read();
            if (done) return;
            this.pending = (this.pending + decoder.decode(value, {stream: true})).slice(-65536);
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

    async command(shell, timeout = 12000) {
        if (this.disconnected) throw new Error("The Linux USB serial port disconnected");
        const nonce = hex(crypto.getRandomValues(new Uint8Array(8)));
        const reply = new RegExp("^H3970_" + nonce + ":BEGIN\\r?\\n([\\s\\S]*?)^H3970_" + nonce + ":(\\d+)\\r?$", "m");
        const print = "/bin/busybox printf '\\nH3970_%s:%s\\n' " + nonce;
        const line = "\n" + print + " BEGIN; " + shell + "; " + print + " \"$?\"\n";
        this.pending = "";
        await this.write(line, Math.min(timeout, 2000));
        const deadline = performance.now() + timeout;
        do {
            const match = reply.exec(this.pending);
            if (match) {
                if (match[2] !== "0") throw new Error("The iPAQ Linux command failed (" + match[2] + ")");
                return match[1].replaceAll("\r", "").trim();
            }
            if (this.disconnected) throw new Error("The Linux USB serial port disconnected");
            await sleep(40);
        } while (performance.now() < deadline);
        throw new Error("No reply from the iPAQ Linux console");
    }

    async snapshot() {
        let output = await this.command("if test -x /doom/device-status; then /doom/device-status; else echo legacy; fi", 3500);
        if (/^legacy$/m.test(output)) output = await this.command(statusCommand, 5000);
        const boot = /^boot=([a-f0-9-]{36}|unknown)$/m.exec(output)?.[1];
        const kit = /^kit=([a-zA-Z0-9.+-]{1,64})$/m.exec(output)?.[1];
        const doom = /^doom=(running|stopped) audio=(off|on|effects|music)$/m.exec(output);
        const reset = /^reset=(yes|no)$/m.exec(output)?.[1];
        if (!boot || !kit || !doom || !reset) throw new Error("Linux returned an invalid Doom status");
        return {boot, kit, doom: doom[1], audio: doom[2], reset: reset === "yes"};
    }

    async setAudio(enabled) {
        const muted = "read -r pid </tmp/doom.pid && /bin/busybox grep -aqF -- -nosound \"/proc/$pid/cmdline\"";
        const matches = enabled ? "! ( " + muted + " )" : "( " + muted + " )";
        await this.command("if /doom/doomctl status >/dev/null && " + matches +
            "; then :; else /doom/doomctl stop && /doom/doomctl start " +
            (enabled ? "--audio" : "0") + "; fi");
        await sleep(400);
        await this.command("/doom/doomctl status >/dev/null && " + matches);
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

    async close() {
        await this.reader.cancel().catch(() => {});
        await this.reading;
        this.reader.releaseLock();
        this.writer.releaseLock();
        await this.port.close().catch(() => {});
    }
}
