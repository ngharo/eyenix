"""Minimal TTP client for the Tobii Eye Tracker 5 (2104:0313) over libusb.

Protocol details: docs/protocol-notes.md. Startup sequence follows
tobii-linux (njmill, Apache-2.0) tools/app/tobii-ttp-mux.c.
"""
import struct
import usb.core
import usb.util

VID, PID = 0x2104, 0x0313
EP_OUT, EP_IN = 0x05, 0x83
REQ, RSP, STREAM, NOTIFY = 0x51, 0x52, 0x53, 0x4E

OP_HELLO, OP_QUERY_REALM, OP_OPEN_REALM = 0x3E8, 0x640, 0x76C
OP_SUBSCRIBE, OP_UNSUBSCRIBE = 0x4C4, 0x4CE
OP_GET_DISPLAY_AREA, OP_SET_DISPLAY_AREA = 0x596, 0x5A0
OP_CAL_START, OP_CAL_STOP = 0x3F2, 0x3FC
STREAM_GAZE = 0x500

HELLO_PAYLOAD = bytes.fromhex(
    "0000170000002800000009000100000001000100010002000100030001000400010005000100060001000700010008")


def u32(v):
    return b"\x02" + struct.pack(">II", 4, v)


def parse_items(payload):
    """Payload -> list of (type, value bytes)."""
    out, i = [], 2
    while i + 5 <= len(payload):
        ty, ln = payload[i], struct.unpack_from(">I", payload, i + 1)[0]
        out.append((ty, payload[i + 5:i + 5 + ln]))
        i += 5 + ln
    return out


def fx(v):
    return struct.unpack(">q", v)[0] / 2 ** 42


def columns(payload):
    """Decode a stream sample into {column id: int | [floats]}."""
    it, col, i = parse_items(payload), {}, 1
    while i + 2 < len(it) + 1 and i < len(it):
        if it[i][0] == 5 and it[i][1][2:] == b"\x0b\xb9" and i + 2 <= len(it) - 1 + 1:
            cid = int.from_bytes(it[i + 1][1], "big")
            j = i + 2
            if j < len(it) and it[j][0] == 5:
                n = struct.unpack(">H", it[j][1][:2])[0]
                col[cid] = [fx(x[1]) for x in it[j + 1:j + 1 + n]]
                i = j + 1 + n
            elif j < len(it):
                col[cid] = int.from_bytes(it[j][1], "big")
                i = j + 1
            else:
                break
        else:
            i += 1
    return col


class Message:
    def __init__(self, raw):
        self.magic, self.seq, self.flag, self.op, self.status, self.plen = struct.unpack_from(">6I", raw, 8)
        self.payload = raw[32:]

    def __repr__(self):
        return (f"<magic={self.magic:#x} seq={self.seq} op={self.op:#x} "
                f"status={self.status:#x} plen={self.plen}>")


class Tracker:
    def __init__(self):
        self.dev = usb.core.find(idVendor=VID, idProduct=PID)
        if self.dev is None:
            raise RuntimeError("tracker not found")
        usb.util.claim_interface(self.dev, 0)
        self.dev.ctrl_transfer(0x41, 0x41, 0, 0, None, 1000)  # session open
        self.seq = 0
        self.buf = b""
        self.pending = []  # stream / notification messages seen while waiting

    def close(self):
        try:
            self.dev.ctrl_transfer(0x41, 0x42, 0, 0, None, 500)  # session close
        finally:
            usb.util.release_interface(self.dev, 0)
            usb.util.dispose_resources(self.dev)

    def clock_us(self):
        return struct.unpack("<Q", bytes(self.dev.ctrl_transfer(0xC1, 0x46, 1, 0, 8, 1000)))[0]

    def read(self, timeout_ms=1000):
        """Next complete message, or None on timeout."""
        while True:
            if len(self.buf) >= 8:
                total = struct.unpack_from("<I", self.buf, 4)[0]
                if 32 <= total <= len(self.buf):
                    raw, self.buf = self.buf[:total], self.buf[total:]
                    return Message(raw)
            try:
                self.buf += bytes(self.dev.read(EP_IN, 16384, timeout_ms))
            except usb.core.USBTimeoutError:
                return None

    def request(self, op, payload=b"", timeout_ms=3000):
        self.seq += 1
        body = struct.pack(">6I", REQ, self.seq, 0, op, 0, len(payload)) + payload
        self.dev.write(EP_OUT, struct.pack("<II", 0, len(body)) + body, 1000)
        while True:
            m = self.read(timeout_ms)
            if m is None:
                raise TimeoutError(f"no response to op {op:#x}")
            if m.magic == RSP and m.seq == self.seq:
                return m
            self.pending.append(m)

    def subscribe(self, stream):
        return self.request(OP_SUBSCRIBE, b"\0\0" + u32(stream) + b"\x17" + struct.pack(">II", 4, 0))

    def unsubscribe(self, stream):
        return self.request(OP_UNSUBSCRIBE, b"\0\0" + u32(stream))
