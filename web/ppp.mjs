import {ascii, join} from "./bytes.mjs";

const LCP = 0xc021, IPCP = 0x8021, IPV4 = 0x0021;
const FLAG = 0x7e, ESCAPE = 0x7d;
const HOST = Uint8Array.of(192, 168, 131, 102);
const DEVICE = Uint8Array.of(192, 168, 131, 201);
const MRU = Uint8Array.of(1, 4, 0x05, 0xdc);
const equal = (left, right) => left.length === right.length && left.every((v, i) => v === right[i]);

const table = Uint16Array.from({length: 256}, (_, byte) => {
    for (let bit = 0; bit < 8; bit++) byte = (byte >>> 1) ^ ((byte & 1) ? 0x8408 : 0);
    return byte;
});

function fcs(data) {
    let crc = 0xffff;
    for (const byte of data) crc = (crc >>> 8) ^ table[(crc ^ byte) & 0xff];
    return crc;
}

export function encode(protocol, payload) {
    const body = join(Uint8Array.of(0xff, 3, protocol >>> 8, protocol & 0xff), payload);
    const checksum = fcs(body) ^ 0xffff;
    const packet = [FLAG];
    for (const byte of join(body, Uint8Array.of(checksum & 0xff, checksum >>> 8))) {
        if (byte < 0x20 || byte === FLAG || byte === ESCAPE) packet.push(ESCAPE, byte ^ 0x20);
        else packet.push(byte);
    }
    packet.push(FLAG);
    return Uint8Array.from(packet);
}

function choose(protocol, payload) {
    const reject = [], suggest = [];
    let address = false;
    for (let offset = 0; offset < payload.length;) {
        const type = payload[offset], size = payload[offset + 1];
        if (!size || size < 2 || offset + size > payload.length) throw new Error("Malformed PPP option");
        const opt = payload.subarray(offset, offset + size);
        offset += size;
        if (protocol === LCP) {
            if (type === 1 && size === 4) {
                if ((opt[2] * 256 + opt[3]) < 1500) suggest.push(MRU);
            } else if ((type !== 2 && type !== 5) || size !== 6) {
                reject.push(opt);
            }
        } else if (size !== 6 || ![3, 129, 131].includes(type)) {
            reject.push(opt);
        } else {
            const expected = type === 3 ? DEVICE : HOST;
            if (type === 3) address = true;
            if (!equal(opt.subarray(2), expected)) suggest.push(join(opt.subarray(0, 2), expected));
        }
    }
    if (protocol === IPCP && !address) suggest.push(join(Uint8Array.of(3, 6), DEVICE));
    if (reject.length) return [4, join(...reject)];
    return suggest.length ? [3, join(...suggest)] : [2, payload];
}

class Negotiation {
    constructor(protocol, request) {
        this.protocol = protocol;
        this.request = request;
        this.id = 1;
        this.nextSend = 0;
        this.localAck = this.remoteAck = false;
    }
    get up() {
        return this.localAck && this.remoteAck;
    }
    frame(code, id, payload = new Uint8Array()) {
        const length = payload.length + 4;
        return encode(this.protocol, join(Uint8Array.of(code, id, length >>> 8, length & 0xff), payload));
    }
    sendRequest(now) {
        if (this.up || now < this.nextSend) return;
        this.nextSend = now + 2000;
        return this.frame(1, this.id, this.request);
    }
    receive(packet) {
        const [code, id, high, low] = packet;
        const length = high * 256 + low;
        if (!(length >= 4 && length <= packet.length)) return;
        const data = packet.subarray(4, length);
        if (code === 1) {
            if (this.up && (this.lastId !== id || !equal(this.lastRequest, data))) {
                this.localAck = false;
                this.nextSend = 0;
                this.id = this.id % 255 + 1;
            }
            const [response, payload] = choose(this.protocol, data);
            this.remoteAck = response === 2;
            if (this.remoteAck) {
                this.lastId = id;
                this.lastRequest = data;
            }
            return this.frame(response, id, payload);
        }
        if (code === 2 && id === this.id && equal(data, this.request)) this.localAck = true;
        if ((code === 3 || code === 4) && id === this.id) {
            if (this.protocol !== LCP || this.request.length === 0) throw new Error("WinCE rejected PPP configuration");
            this.request = new Uint8Array();
            this.id = this.id % 255 + 1;
            this.localAck = false;
            this.nextSend = 0;
        }
        if (code === 5) {
            this.localAck = this.remoteAck = false;
            this.nextSend = 0;
            return this.frame(6, id, data);
        }
        if (code === 9 && this.protocol === LCP && data.length >= 4) {
            return this.frame(10, id, join(new Uint8Array(4), data.subarray(4)));
        }
    }
}

export class PppLink {
    constructor(send, receive, report) {
        this.send = send;
        this.receive = receive;
        this.report = report;
        this.greeting = "";
        this.lcp = new Negotiation(LCP, MRU);
        this.ipcp = new Negotiation(IPCP, join(Uint8Array.of(3, 6), HOST));
        this.frame = [];
        this.foundClient = false;
        this.receiving = this.escaped = false;
        this.reported = false;
        this.rejectId = 0;
    }
    get up() {
        return this.lcp.up && this.ipcp.up;
    }
    tick(now) {
        if (!this.foundClient) return;
        const request = (this.lcp.up ? this.ipcp : this.lcp).sendRequest(now);
        if (request) this.send(request);
    }
    sendIp(payload) {
        if (this.up) this.send(encode(IPV4, payload));
    }
    process(frame, now) {
        if (frame.length < 6 || frame[0] !== 0xff || frame[1] !== 3 || fcs(frame) !== 0xf0b8) return;
        const protocol = frame[2] * 256 + frame[3], payload = Uint8Array.from(frame.slice(4, -2));
        let response;
        if (protocol === LCP) {
            response = this.lcp.receive(payload);
            if (!this.lcp.up && this.ipcp.up) {
                this.ipcp = new Negotiation(IPCP, join(Uint8Array.of(3, 6), HOST));
            }
        } else if (protocol === IPCP) {
            response = this.ipcp.receive(payload);
        } else if (protocol === IPV4) {
            if (this.up) this.receive(payload);
            return;
        } else if (this.lcp.up) {
            this.rejectId = this.rejectId % 255 + 1;
            response = this.lcp.frame(8, this.rejectId, join(frameProtocol(protocol), payload.subarray(0, 1494)));
        }
        if (response) this.send(response);
        if (this.up && !this.reported) {
            this.reported = true;
            this.report("PPP negotiated. Waiting for ActiveSync…");
        }
        this.tick(now);
    }
    feed(bytes, now) {
        for (const byte of bytes) {
            if (!this.foundClient) {
                this.greeting = (this.greeting + String.fromCharCode(byte)).slice(-6);
                if (this.greeting === "CLIENT") {
                    this.foundClient = true;
                    this.report("WinCE USB greeting received. Negotiating PPP…");
                    this.send(ascii("CLIENTSERVER"));
                    this.tick(now);
                }
                continue;
            }
            if (byte === FLAG) {
                const frame = this.frame;
                const valid = this.receiving && !this.escaped;
                this.frame = [];
                this.receiving = true;
                this.escaped = false;
                if (valid) this.process(frame, now);
            } else if (this.receiving) {
                if (this.escaped) {
                    this.frame.push(byte ^ 0x20);
                    this.escaped = false;
                } else if (byte === ESCAPE) {
                    this.escaped = true;
                } else if (byte >= 0x20) {
                    this.frame.push(byte);
                }
                if (this.frame.length > 2048) {
                    this.receiving = false;
                    this.frame = [];
                }
            }
        }
    }
}

function frameProtocol(protocol) {
    return Uint8Array.of(protocol >>> 8, protocol & 0xff);
}
