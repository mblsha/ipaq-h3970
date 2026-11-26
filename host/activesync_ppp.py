"""Async HDLC and the IPv4 PPP negotiation used by the serial cradle."""

import binascii
import struct
import time

LCP, IPCP, IPV4 = 0xC021, 0x8021, 0x0021
HOST = b"\xc0\xa8\x83\x66"  # 192.168.131.102
DEVICE = b"\xc0\xa8\x83\xc9"  # 192.168.131.201
FLAG, ESCAPE = 0x7E, 0x7D


def fcs(data):
    # crc_hqx operates MSB first. Reflect each input byte and the 16-bit result.
    crc = binascii.crc_hqx(data.translate(REFLECT), 0xFFFF)
    return REFLECT[crc >> 8] | (REFLECT[crc & 255] << 8)


REFLECT = bytes(int(f"{byte:08b}"[::-1], 2) for byte in range(256))


def encode(protocol, payload):
    data = b"\xff\x03" + struct.pack("!H", protocol) + payload
    data += struct.pack("<H", fcs(data) ^ 0xFFFF)
    frame = bytearray([FLAG])
    for byte in data:
        if byte < 0x20 or byte in (FLAG, ESCAPE):
            frame.extend((ESCAPE, byte ^ 0x20))
        else:
            frame.append(byte)
    frame.append(FLAG)
    return frame


class Decoder:
    def __init__(self):
        self.data = bytearray()
        self.escaped = False
        self.receiving = False

    def feed(self, chunk):
        for byte in chunk:
            if byte == FLAG:
                data = bytes(self.data)
                valid = self.receiving and not self.escaped
                self.data.clear()
                self.escaped = False
                self.receiving = True
                if valid and len(data) >= 6 and data[:2] == b"\xff\x03" and fcs(data) == 0xF0B8:
                    yield int.from_bytes(data[2:4], "big"), data[4:-2]
            elif self.receiving:
                if self.escaped:
                    self.data.append(byte ^ 0x20)
                    self.escaped = False
                elif byte == ESCAPE:
                    self.escaped = True
                elif byte >= 0x20:
                    self.data.append(byte)
                if len(self.data) > 2048:
                    self.receiving = False
                    self.data.clear()


def options(data):
    while data:
        if len(data) < 2 or not 2 <= data[1] <= len(data):
            raise ValueError("Malformed PPP configuration option")
        yield data[:data[1]]
        data = data[data[1]:]


class Negotiation:
    def __init__(self, protocol, request, select_options):
        self.protocol = protocol
        self.request = request
        self.select_options = select_options
        self.send_id = 1
        self.sent_at = 0.0
        self.local_ack = False
        self.remote_ack = False
        self.last_acked_request = None

    @property
    def up(self):
        return self.local_ack and self.remote_ack

    def due(self, now):
        if not self.up and now - self.sent_at >= 2:
            # Retransmissions intentionally retain the ID so a delayed Ack is valid.
            self.sent_at = now
            return self.packet(1, self.send_id, self.request)
        return None

    def packet(self, code, identifier, info=b""):
        return encode(self.protocol, struct.pack("!BBH", code, identifier, 4 + len(info)) + info)

    def receive(self, payload):
        if len(payload) < 4:
            return None
        code, identifier, length = struct.unpack_from("!BBH", payload)
        if not 4 <= length <= len(payload):
            return None
        info = payload[4:length]
        if code == 1:  # Configure-Request
            changes = (identifier, info) != self.last_acked_request
            if self.up and changes:
                self.local_ack = False
                self.sent_at = 0.0
                self.send_id = self.send_id % 255 + 1
            reply, body = self.select_options(info)
            self.remote_ack = reply == 2
            if self.remote_ack:
                self.last_acked_request = identifier, info
            return self.packet(reply, identifier, body)
        if identifier == self.send_id:
            if code == 2 and info == self.request:
                self.local_ack = True
            elif code in (3, 4):
                if self.protocol == LCP and self.request:
                    self.request = b""
                    self.send_id = self.send_id % 255 + 1
                    self.sent_at = 0.0
                    self.local_ack = False
                else:
                    raise ValueError(f"Device rejected PPP configuration for protocol {self.protocol:#06x}")
        if code == 5:  # Terminate-Request
            self.local_ack = self.remote_ack = False
            self.sent_at = 0.0
            return self.packet(6, identifier, info)
        if code == 9 and self.protocol == LCP and len(info) >= 4:
            return self.packet(10, identifier, b"\0" * 4 + info[4:])
        return None


def lcp_options(data):
    rejected = []
    suggested = []
    for option in options(data):
        if option[0] == 1 and len(option) == 4:
            if int.from_bytes(option[2:], "big") < 1500:
                suggested.append(b"\x01\x04\x05\xdc")
        elif len(option) != {2: 6, 5: 6}.get(option[0]):
            rejected.append(option)
    if rejected:
        return 4, b"".join(rejected)
    if suggested:
        return 3, b"".join(suggested)
    return 2, data


def ipcp_options(data):
    rejected = []
    suggested = []
    has_address = False
    for option in options(data):
        kind = option[0]
        if len(option) != 6 or kind not in (3, 129, 131):
            rejected.append(option)
            continue
        if kind == 3:
            has_address = True
        expected = DEVICE if kind == 3 else HOST
        if option[2:] != expected:
            suggested.append(option[:2] + expected)
    if rejected:
        return 4, b"".join(rejected)
    if not has_address:
        suggested.append(b"\x03\x06" + DEVICE)
    if suggested:
        return 3, b"".join(suggested)
    return 2, data


class Link:
    """Feed one serial stream and pass negotiated IPv4 datagrams to a backend."""

    def __init__(self, write_serial, write_network, report):
        self.write_serial = write_serial
        self.write_network = write_network
        self.report = report
        self.decoder = Decoder()
        self.lcp = Negotiation(LCP, b"\x01\x04\x05\xdc", lcp_options)
        self.ipcp = Negotiation(IPCP, b"\x03\x06" + HOST, ipcp_options)
        self.handshake = bytearray()
        self.started = False
        self.link_reported = False
        self.network_reported = False
        self.reject_id = 0

    def tick(self):
        if not self.started:
            return
        now = time.monotonic()
        active = self.ipcp if self.lcp.up else self.lcp
        frame = active.due(now)
        if frame:
            self.write_serial(frame)

    def serial_input(self, data):
        if not self.started:
            self.handshake.extend(data)
            start = self.handshake.find(b"CLIENT")
            if start < 0:
                del self.handshake[:-5]
                return
            data = bytes(self.handshake[start + 6:])
            self.handshake.clear()
            self.write_serial(b"CLIENTSERVER")
            self.report("Received WinCE CLIENT handshake")
            self.started = True
            self.tick()

        for protocol, payload in self.decoder.feed(data):
            if protocol == LCP:
                response = self.lcp.receive(payload)
                if self.lcp.up and not self.link_reported:
                    self.report("PPP link negotiated")
                    self.link_reported = True
                if not self.lcp.up and self.ipcp.up:
                    self.ipcp = Negotiation(IPCP, b"\x03\x06" + HOST, ipcp_options)
                    self.network_reported = False
            elif protocol == IPCP and self.lcp.up:
                response = self.ipcp.receive(payload)
                if self.ipcp.up and not self.network_reported:
                    self.report("PPP IPv4 addresses negotiated")
                    self.network_reported = True
            elif protocol == IPV4:
                if self.lcp.up and self.ipcp.up:
                    self.write_network(payload)
                continue
            elif self.lcp.up:
                self.reject_id = self.reject_id % 255 + 1
                response = self.lcp.packet(8, self.reject_id, struct.pack("!H", protocol) + payload[:1494])
            else:
                continue
            if response:
                self.write_serial(response)
            self.tick()

    def network_input(self, data):
        if self.lcp.up and self.ipcp.up and len(data) <= 1500:
            self.write_serial(encode(IPV4, data))
