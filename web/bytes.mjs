export const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
export const ascii = value => new TextEncoder().encode(value);
export const view = bytes => new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);

export function join(...parts) {
    const bytes = new Uint8Array(parts.reduce((size, part) => size + part.length, 0));
    let offset = 0;
    for (const part of parts) {
        bytes.set(part, offset);
        offset += part.length;
    }
    return bytes;
}

export function words(...values) {
    const bytes = new Uint8Array(4 * values.length);
    const data = view(bytes);
    values.forEach((value, index) => data.setUint32(index * 4, value, true));
    return bytes;
}

export function hex(bytes) {
    return Array.from(bytes, b => b.toString(16).padStart(2, "0")).join("");
}

export async function sha256(bytes) {
    return hex(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)));
}

export function utf16(value) {
    if (value.includes("\0")) throw new Error("Unexpected null in WinCE string");
    const bytes = new Uint8Array((value.length + 1) * 2);
    const data = view(bytes);
    for (let offset = 0; offset < value.length; offset++) {
        data.setUint16(offset * 2, value.charCodeAt(offset), true);
    }
    return bytes;
}

export class Reply {
    constructor(bytes) {
        this.bytes = bytes;
        this.offset = 0;
    }
    get remaining() {
        return this.bytes.length - this.offset;
    }
    take(size) {
        if (size > this.remaining) throw new Error("Truncated WinCE reply");
        const start = this.offset;
        this.offset += size;
        return this.bytes.subarray(start, this.offset);
    }
    integer() {
        return view(this.take(4)).getUint32(0, true);
    }
}
