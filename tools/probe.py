#!/usr/bin/env python3
"""Probe: what does the tracker allow without realm authentication?"""
import sys, time
from ttp import *

t = Tracker()
try:
    print("device clock us:", t.clock_us())
    r = t.request(OP_HELLO, HELLO_PAYLOAD); print("hello", r, r.payload.hex())
    r = t.request(OP_QUERY_REALM, b"\0\0"); print("query_realm", r, r.payload.hex())
    realm = int.from_bytes(parse_items(r.payload)[0][1], "big")
    r = t.request(OP_OPEN_REALM, b"\0\0" + u32(realm) + b"\0"); print("open_realm", realm, r, r.payload.hex()[:80])

    r = t.subscribe(STREAM_GAZE); print("subscribe gaze", r)
    n, valid, t0, last = 0, 0, time.time(), None
    while time.time() - t0 < 5:
        m = t.pending.pop(0) if t.pending else t.read(500)
        if m is None: continue
        if m.magic == STREAM and m.op == STREAM_GAZE:
            c = columns(m.payload); n += 1
            g = c.get(28)
            if g and g[0] != -1.0: valid += 1; last = (c.get(1), g, c.get(2), c.get(8))
            if n % 30 == 1: print("  sample", n, "ts", c.get(1), "gaze", g, "Lpos", c.get(2))
        else:
            print("  other", m, m.payload.hex()[:60])
    print(f"gaze samples: {n} in 5 s, valid gaze: {valid}, last valid: {last}")

    r = t.request(OP_GET_DISPLAY_AREA); print("get_display_area", r, [round(fx(v), 1) for ty, v in parse_items(r.payload) if ty == 4])
    area = r.payload
    if "--write" in sys.argv:
        r = t.request(OP_SET_DISPLAY_AREA, area); print("set_display_area (same values)", r)
        r = t.request(OP_CAL_START); print("calibration start", r)
        r = t.request(OP_CAL_STOP); print("calibration stop", r)
    r = t.unsubscribe(STREAM_GAZE); print("unsubscribe", r)
finally:
    t.close()
