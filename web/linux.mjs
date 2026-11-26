import {hex, sleep} from "./bytes.mjs";

export const LINUX_USB = {usbVendorId: 0x0525, usbProductId: 0xa4a7};
const identity = "/bin/busybox grep -aqF hp,ipaq-h3970 /sys/firmware/devicetree/base/compatible && test -x /doom/doomctl";

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
            await port.setSignals({dataTerminalReady: true, requestToSend: true});
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

    async command(shell, timeout = 12000) {
        const nonce = hex(crypto.getRandomValues(new Uint8Array(8)));
        const reply = new RegExp("^H3970_" + nonce + ":(\\d+)\\r?$", "m");
        const line = "\n" + shell + "; /bin/busybox printf '\\nH3970_%s:%s\\n' " + nonce + " \"$?\"\n";
        this.pending = "";
        await this.writer.write(new TextEncoder().encode(line));
        const deadline = performance.now() + timeout;
        do {
            const match = reply.exec(this.pending);
            if (match) {
                if (match[1] !== "0") throw new Error("The iPAQ Linux command failed (" + match[1] + ")");
                return;
            }
            if (this.disconnected) throw new Error("The Linux USB serial port disconnected");
            await sleep(40);
        } while (performance.now() < deadline);
        throw new Error("No reply from the iPAQ Linux console");
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
