import {join, Reply, sleep, utf16, words} from "./bytes.mjs";
import {PppLink} from "./ppp.mjs";

export const IPAQ = {vendorId: 0x049f, productId: 0x0003};
const DCCM = 0, RAPI = 1, PING = 0x12345678;
const TX_QUEUE_LIMIT = 12;
const TCP_BUFFER = 65536 + 32;

function withTimeout(promise, ms, message) {
    let timer;
    const timeout = new Promise((_, reject) => {
        timer = setTimeout(() => reject(new Error(message)), ms);
    });
    return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

function passwordPacket(password, key) {
    if (!password || password.length > 127) throw new Error("Enter the iPAQ’s ActiveSync password and reconnect");
    const data = utf16(password).map(byte => byte ^ key);
    return join(Uint8Array.of(data.length & 255, data.length >>> 8), data);
}

function findInterface(configuration) {
    for (const iface of configuration.interfaces) {
        if (iface.alternate.interfaceClass !== 0xff || iface.alternate.alternateSetting !== 0) continue;
        const endpoints = iface.alternate.endpoints.filter(endpoint => endpoint.type === "bulk");
        const input = endpoints.find(endpoint => endpoint.direction === "in");
        const output = endpoints.find(endpoint => endpoint.direction === "out");
        if (input && output && input.packetSize <= 64) return {number: iface.interfaceNumber, input, output};
    }
    throw new Error("No WinCE ActiveSync bulk interface found");
}

export class ActiveSync {
    constructor(device, wasm, report) {
        this.device = device;
        this.wasm = wasm;
        this.report = report;
        const seed = crypto.getRandomValues(new Uint32Array(1))[0];
        this.pointer = wasm.init(seed);
        this.queue = [];
        this.active = true;
        this.epoch = performance.now();
        this.ppp = new PppLink(
            bytes => this.enqueue(bytes),
            bytes => {
                this.staging(bytes.length).set(bytes);
                if (this.wasm.receive_packet(bytes.length) < 0) throw new Error("PPP input queue overflow");
            },
            report,
        );
    }

    static async connect(device, password, report) {
        if (device.vendorId !== IPAQ.vendorId || device.productId !== IPAQ.productId) {
            throw new Error("Select the iPAQ in stock WinCE");
        }
        const response = await fetch(new URL("./tcp.wasm", import.meta.url));
        if (!response.ok) throw new Error("Could not load the WASM TCP stack");
        const {instance} = await WebAssembly.instantiate(await response.arrayBuffer());
        const link = new ActiveSync(device, instance.exports, report);
        try {
            await link.open();
            await link.negotiate(password);
            return link;
        } catch (error) {
            await link.close();
            throw error;
        }
    }

    staging(length) {
        return new Uint8Array(this.wasm.memory.buffer, this.pointer, length);
    }

    dtr(value) {
        return this.device.controlTransferOut({
            requestType: "class", recipient: "interface", request: 0x22,
            index: this.iface.number, value,
        });
    }

    async open() {
        await withTimeout(this.device.open(), 5000, "Opening the USB device timed out");
        if (!this.device.configuration) await this.device.selectConfiguration(1);
        this.iface = findInterface(this.device.configuration);
        await withTimeout(this.device.claimInterface(this.iface.number), 5000, "Claiming ActiveSync timed out");
        this.claimed = true;
        await withTimeout(this.dtr(0), 2000, "Releasing WinCE USB DTR timed out");
        await sleep(1000);
        const asserted = await withTimeout(this.dtr(1), 2000, "Starting the WinCE USB handshake timed out");
        if (asserted.status !== "ok") throw new Error("WinCE rejected the USB handshake");
        this.report("Waiting for the WinCE USB greeting…");
        this.readUsb().catch(error => this.fail(error));
        this.timer = setInterval(() => {
            try { this.pump(); } catch (error) { this.fail(error); }
        }, 5);
    }

    fail(error) {
        if (this.active && !this.error) this.error = error;
    }

    assertActive() {
        if (this.error) throw this.error;
        if (!this.active) throw new Error("USB session closed");
    }

    async close({boot = false} = {}) {
        this.active = false;
        clearInterval(this.timer);
        clearInterval(this.heartbeat);
        if (this.device.opened) {
            try {
                if (this.claimed) {
                    const dtr = withTimeout(this.dtr(0), 1500, "USB DTR release timed out");
                    if (boot) {
                        if ((await dtr).status !== "ok") throw new Error("WinCE USB DTR release failed");
                    } else {
                        await dtr.catch(() => {});
                    }
                }
            } finally {
                await withTimeout(this.device.close(), 2000, "USB close timed out").catch(() => {});
            }
        }
    }

    enqueue(bytes) {
        this.queue.push(bytes);
        if (!this.sending) this.sendUsb().catch(error => this.fail(error));
    }

    async sendUsb() {
        this.sending = true;
        try {
            while (this.active && !this.error && this.queue.length) {
                const frame = this.queue.shift();
                for (let offset = 0; offset < frame.length && this.active; offset += 1024) {
                    const bytes = frame.subarray(offset, offset + 1024);
                    const result = await withTimeout(
                        this.device.transferOut(this.iface.output.endpointNumber, bytes),
                        5000, "USB bulk write timed out",
                    );
                    if (result.status !== "ok" || result.bytesWritten !== bytes.length) {
                        throw new Error("Short ActiveSync USB write");
                    }
                    // Browser timers have a coarser resolution than the native relay.
                    await sleep(1);
                }
                this.pump();
            }
        } finally {
            this.sending = false;
        }
    }

    async readUsb() {
        const ep = this.iface.input;
        while (this.active) {
            const reply = await this.device.transferIn(ep.endpointNumber, ep.packetSize);
            if (!this.active) return;
            if (reply.status !== "ok") throw new Error("ActiveSync USB read " + reply.status);
            this.ppp.feed(new Uint8Array(reply.data.buffer, reply.data.byteOffset, reply.data.byteLength), performance.now());
            this.pump();
        }
    }

    pump() {
        if (!this.active || this.error) return;
        this.ppp.tick(performance.now());
        if (!this.ppp.up) return;
        this.wasm.tick(Math.floor(performance.now() - this.epoch));
        while (this.queue.length < TX_QUEUE_LIMIT) {
            const length = this.wasm.next_packet();
            if (!length) return;
            this.ppp.sendIp(this.staging(length).slice());
        }
    }

    async waitSocket(stream, ms) {
        const until = performance.now() + ms;
        while (performance.now() < until) {
            this.assertActive();
            const status = this.wasm.socket_status(stream);
            if (status > 0) return;
            if (status < 0) throw new Error("WinCE closed a TCP connection");
            await sleep(5);
        }
        throw new Error("Timed out opening the WinCE TCP connection");
    }

    async read(stream, size, timeout = 30000) {
        const data = new Uint8Array(size);
        const deadline = performance.now() + timeout;
        let offset = 0;
        while (offset < size) {
            this.assertActive();
            const count = this.wasm.socket_read(stream, Math.min(size - offset, TCP_BUFFER));
            if (count < 0) throw new Error("WinCE closed a TCP connection");
            if (count) {
                data.set(this.staging(count), offset);
                offset += count;
                this.pump();
            } else {
                if (performance.now() >= deadline) throw new Error("WinCE TCP read timed out");
                await sleep(4);
            }
        }
        return data;
    }

    async write(stream, bytes) {
        const deadline = performance.now() + 20000;
        let offset = 0;
        while (offset < bytes.length) {
            this.assertActive();
            const part = bytes.subarray(offset, offset + TCP_BUFFER);
            this.staging(part.length).set(part);
            const count = this.wasm.socket_write(stream, part.length);
            if (count < 0) throw new Error("WinCE closed a TCP connection");
            offset += count;
            this.pump();
            if (!count) {
                if (performance.now() >= deadline) throw new Error("WinCE TCP write timed out");
                await sleep(4);
            }
        }
    }

    async negotiate(password) {
        await this.waitSocket(DCCM, 55000);
        let key;
        for (;;) {
            const header = new Reply(await this.read(DCCM, 4, 8000)).integer();
            if (header === PING || header === 0) continue;
            if (header >= 512) {
                key = header & 255;
                await this.write(DCCM, passwordPacket(password, key));
            } else if (header >= 36) {
                await this.read(DCCM, header, 8000);
                if (key !== undefined && (await this.read(DCCM, 2, 5000)).every(byte => !byte)) {
                    throw new Error("WinCE rejected the ActiveSync password");
                }
                break;
            } else {
                throw new Error("Unexpected ActiveSync handshake message");
            }
        }
        await this.write(DCCM, words(PING));
        this.heartbeat = setInterval(() => {
            this.write(DCCM, words(PING)).catch(error => this.fail(error));
        }, 5000);
        this.monitorDccm().catch(error => this.fail(error));
        let lastError;
        for (let attempt = 0; attempt < 4; attempt++) {
            const port = 49152 + (crypto.getRandomValues(new Uint16Array(1))[0] % 16000);
            if (!this.wasm.connect_rapi(port)) throw new Error("Could not start the RAPI TCP connection");
            this.pump();
            try {
                await this.waitSocket(RAPI, 4000);
                if (key !== undefined) {
                    await this.write(RAPI, passwordPacket(password, key));
                    if (!(await this.read(RAPI, 1, 5000))[0]) throw new Error("WinCE rejected the RAPI password");
                }
                return;
            } catch (error) {
                lastError = error;
                await sleep(300);
            }
        }
        throw lastError;
    }

    async monitorDccm() {
        while (this.active) {
            const heartbeat = new Reply(await this.read(DCCM, 4, 22000)).integer();
            if (heartbeat !== PING && heartbeat !== 0) throw new Error("Unexpected ActiveSync keepalive");
        }
    }

    send(bytes) { return this.write(RAPI, bytes); }
    receive(size) { return this.read(RAPI, size); }
}
