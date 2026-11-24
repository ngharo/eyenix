#!/usr/bin/env python3
"""Dump Tobii EyeChip bulk-channel messages from a usbmon pcapng (via tshark).

usage: ttp_dump.py capture.pcapng [--raw]
Framing as observed: 8-byte LE prefix (kind, length), then 6 x u32 BE header:
magic(0x51 req / 0x52 rsp / other), seq, ?, op, ?, payload_len; then TLV payload.
"""
import subprocess, struct, sys, collections

def urbs(path):
    out = subprocess.run(
        ["tshark", "-r", path, "-Y", "usb.transfer_type==0x03 && usb.data_len>0",
         "-T", "fields", "-e", "frame.time_relative", "-e", "usb.endpoint_address", "-e", "usb.capdata"],
        capture_output=True, text=True).stdout
    for line in out.splitlines():
        t, ep, data = (line.split("\t") + ["", ""])[:3]
        if data:
            yield float(t), int(ep, 16), bytes.fromhex(data.replace(":", ""))

def tlv(buf):
    """Best-effort parse: u32 type, u32 len, value."""
    items, i = [], 0
    while i + 8 <= len(buf):
        ty, ln = struct.unpack_from(">II", buf, i)
        if ln > len(buf) - i - 8:
            items.append(("?", buf[i:].hex())); break
        items.append((ty, buf[i + 8:i + 8 + ln])); i += 8 + ln
    return items

if __name__ == "__main__":
    path = sys.argv[1]; raw = "--raw" in sys.argv
    stats = collections.Counter()
    for t, ep, d in urbs(path):
        if len(d) < 32:
            print(f"{t:9.4f} ep{ep:02x} short {d.hex()}"); continue
        kind, ln = struct.unpack_from("<II", d, 0)
        magic, seq, a, op, b, plen = struct.unpack_from(">6I", d, 8)
        stats[(ep, kind, magic, op, len(d))] += 1
        if raw:
            print(f"{t:9.4f} ep{ep:02x} kind={kind} len={ln} urb={len(d)} magic={magic:#x} seq={seq} a={a} op={op:#x} b={b} plen={plen} {d[32:32+96].hex()}")
    if not raw:
        for k, n in sorted(stats.items(), key=lambda kv: -kv[1]):
            ep, kind, magic, op, l = k
            print(f"{n:6d}  ep{ep:02x} kind={kind} magic={magic:#x} op={op:#x} urblen={l}")
