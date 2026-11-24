# Tobii Eye Tracker 5 (2104:0313) — protocol notes

Derived from usbmon captures of the Windows Tobii service (captures/, not in git).
Everything here is observed behaviour; names are guesses unless stated.

## USB

- Interface 0 (vendor): bulk OUT 0x05 = requests, bulk IN 0x83 = responses and
  notifications. Bulk OUT 0x04 unused so far.
- Interfaces 1–2: UVC, 642x480, proprietary format `e39e1ba2-…`. Unused so far.
- Vendor control requests (recipient interface 0):
  - `0xC1 / 0x46`, wValue 1, 8 bytes in: device clock, u64 LE microseconds since
    power-on. Sent in bursts of 4 (clock sync).
  - `0x41 / 0x30` (24 bytes out), `0xC1 / 0x30`, `0x41 / 0x41`, `0xC0 / 0x99`:
    seen once at attach, not decoded.

## Bulk framing

    u32 LE  kind      0 = host->device, 1 = device->host
    u32 LE  length    requests: bytes after this prefix; responses: whole message
    u32 BE  magic     0x51 request, 0x52 response, 0x53 stream data, 0x4e notification
    u32 BE  seq       request sequence, echoed in response; 0 for 0x53/0x4e
    u32 BE  ?         0 in requests, 1 in responses
    u32 BE  op
    u32 BE  status?   0 normally
    u32 BE  payload length
    payload

Fragmentation (verified: lib output is byte-identical to Windows for the
318 kB calibration upload):

- Device to host: the first envelope holds the header and the start of the
  payload. If the header's payload length is larger, the rest follows in
  further envelopes (kind 1, own length) that carry raw payload bytes only.
- Host to device: the message (header + payload) is cut into slices of at most
  4087 bytes, each sent as its own bulk transfer with its own envelope
  (kind 0, slice length), so no packet exceeds 4095 bytes.

Payload: 2 bytes (0), then items of `u8 type, u32 BE length, value`:

| type | meaning |
|---|---|
| 1, 2, 3 | u32 |
| 4 | s64 fixed point, value / 2^42 |
| 5 | struct header: u16 member count, u16 struct id |
| 6 | u64 |
| 0x14 | string (inner u32 length + bytes) |
| 0x15 | blob (inner u32 length + bytes) |
| 0x17 | seen in subscribe, 4 bytes, 0 |

Struct ids: 8000 = 2D point, 8001 = 3D point, 3000 = gaze sample, 3001 = (column id, value).

## Operations seen

| op | dec | guess |
|---|---|---|
| 0x3e8 | 1000 | hello / protocol negotiation (first request) |
| 0x640 | 1600 | ? |
| 0x58c | 1420 | device info (serial, model `IS5_Large_Eyetracker_5`, …) |
| 0x532, 0x546, 0x547, 0x5d2, 0x5b4, 0x686, 0x4b0, 0x596, 0x578, 0x83e, 0x672, 0x848, 0xc62, 0x6a4 | | capability / config queries at attach |
| 0x76c | 1900 | auth step 1: host sends id 0x2712; device returns 256-byte blob |
| 0x777 | 1911 | auth step 2: host sends 16-byte + 4-byte blobs; empty OK |
| 0x77b | 1915 | auth related (id 0x2712) |
| 0x5a0 | 1440 | set display area: three 3D points in mm; device echoes as notification 0x5aa |
| 0x4c4 | 1220 | subscribe to stream (stream id, 0) |
| 0x4ce | 1230 | unsubscribe (stream id) |
| 0x3f2 | 1010 | calibration start |
| 0x424 | 1060 | calibration clear |
| 0x406 | 1030 | add calibration point: fx x, fx y (normalized display coords), u32 3 (both eyes?). Response arrives ~0.6 s later, when collection is done |
| 0x438 | 1080 | discard calibration point (same x, y, 3); followed by a repeat of 0x406 |
| 0x42e | 1070 | compute and apply; device sends notification 0xc94 (u32, calibration id/hash?) first |
| 0x44c | 1100 | get calibration blob (type 0x15). 1.4 kB default, ~47 kB after 1 point, 318 kB after 7. Blob contains "human" |
| 0x3fc | 1020 | calibration stop |
| 0x456 | 1110 | set calibration: host sends the blob back (318 kB request) |
| 0x460 | 1120 | after calibration, 713-byte reply; per-point results? |
| 0xc58 | 3160 | u32 3; echoed as notification 0xc6c. Unknown |

Calibration as run by Tobii Experience (captures/05, second attempt), gaze
stream 0x500 subscribed throughout:

    start, clear
    add (0.5,0.5); compute; get blob
    add (0.1,0.9); add (0.5,0.1); add (0.9,0.9); compute; get blob
    add (0.1,0.1); add (0.9,0.1); discard (0.9,0.1); add (0.9,0.1); add (0.5,0.9); compute; get blob
    stop; get blob; 0x460; set blob (0x456)

## Streams

- 0x504: presence. Columns: timestamp, state (1 / 2).
- 0x500: gaze, ~33 Hz (30.2 ms), 1724 bytes, 39 columns. Known columns:

| col | value |
|---|---|
| 1 | timestamp, device clock µs |
| 2 / 8 | left / right eye position, mm (tracker coordinates) |
| 3 / 9 | left / right eye position, normalized in track box |
| 4 / 10 | left / right gaze point 3D, mm |
| 5 / 11 | left / right gaze point 2D, normalized on display (0..1) |
| 6 / 12 | left / right pupil? (u32) |
| 7 / 13 | left / right validity? (0 = valid) |
| 28 | combined gaze point 2D, normalized; (-1, -1) when invalid |
| 27, 22, 29, … | validity flags for neighbouring columns (1 = valid) |

- 0x50e: infrared camera image, one per gaze sample (~33 Hz), ~78.6 kB. Streams without
  realm authentication (verified from Linux). Same sample/column layout as gaze:

| col | value |
|---|---|
| 1 | timestamp, device clock µs (u64); within ~10 ms of the matching gaze sample |
| 2 | bits per pixel (8) |
| 3 | width (280) |
| 4 | stride, bytes per row (280) |
| 5 | height (280) |
| 6 | blob: greyscale pixels, top row first |

  Columns 3–5 are all 280 on this device, so which is which follows tobii-faithful's reading.
  Frames are pictures of the user's face: never commit or log them.

## Prior art: tobii-linux (njmill, Apache-2.0)

Cloned at ./tobii-linux (git-ignored). `tools/app/tobii-ttp-mux.c` is a working
libusb reader for this device. What it adds to the above:

- Names: the protocol is TTP; 0x640 = query realm, 0x76c = open realm,
  0x776 = realm response, 0x596 = get display area.
- Startup it uses: control `0x41 / 0x41` (and `0x41 / 0x42` on close), hello,
  query realm, open realm, then subscribe 0x500. If query realm returns type 0
  it sends no auth response at all.
- Realm auth, when the type is non-zero: HMAC-MD5 over the challenge with a
  fixed key, sent with op 0x776.
- More streams: 0x501 / 0x508 / 0x50e in-band IR images, 0x1771 sync.
- Gaze column names (see `decode_gaze_sample`).
- It does not use on-device calibration; it fits its own screen/gaze mapping.

Checked against captures/03: our tracker answers query realm with 0, the
challenge differs every session (3 seen), and Windows opens realm 0x2712 and
answers with op 0x777 (16-byte digest + constant 4-byte blob 816d4df2). The
tobii-linux key does not reproduce that digest (HMAC-MD5/SHA1/SHA256 over the
obvious slices of the challenge), so realm 0x2712 uses something else.

## Verified from Linux (tools/probe.py, VM off, captures/07)

No realm authentication was performed in any of these; all returned status 0:

- hello, query realm, subscribe 0x500: gaze arrives at ~33 Hz with valid data.
- get display area (0x596), set display area (0x5a0) with the same values.
- calibration start (0x3f2) and stop (0x3fc), no points added.

Open realm with type 0 (as tobii-linux sends it) returns status 0x20000503 and
is not needed. This also confirms the fifth header word is a status code.

## Display geometry sent by Windows

All five set-display-area requests in captures/04 and 05 fit one model: the
screen plane is tilted 20.00 degrees from the tracker's y axis, and the bottom
edge of a full-height picture sits 8.59 mm above the tracker origin along the
screen and 6.42 mm behind it. Width and horizontal offset come from the user's
alignment step. `lib/src/display.cpp` implements this; the 184 mm mark spacing
it relies on comes from tobii-linux. A by-eye alignment on a 27" monitor gave
597 x 336 mm, the panel's real size, which supports that figure.

Verified from Linux since: the full calibration sequence (add point, compute,
get blob) and the calibration upload (0x456) all work without realm
authentication.

The calibration read back from Linux after the Windows session was the 1469-byte
default, not the 318 kB one Windows uploaded. Either it does not survive a
session, or our start/stop probe reset it. Plan to upload it at every start.

## How games reach the tracker (Tobii game integration)

Worked out by the tobii-faithful project; implemented here in `src/gameserver.cpp` and
`bridge/`. Head tracking is confirmed working in Star Citizen; gaze has only been checked
against a stand-in for a game (`bin/test-bridge` and a plain TCP client).

- The game loads `tobii_gameintegration_x64.dll`, which looks for Tobii's services through
  named pipes: `streamengineservices`, `ETDefaultPIPE`, `TOBII-<host>`, `TOBIIPRP-<serial>`.
- `ETDefaultPIPE` (duplex): any request is answered with u32 1, u32 1 and the service's
  address as a NUL-padded 512-byte string. We answer `127.0.0.1`.
- Gaze: the library connects to that address on TCP 4455 and speaks the same protocol as
  the tracker does over USB, without the 8-byte envelope: 24-byte header, then payload.
  It asks 0x3e8, 0x640, 0x58c, 0x532, 0x546, 0x4b0, 0x596, …, then subscribes to 0x500
  and 0x504. The display area it is given is in pixels: (-w/2, h, 0), (w/2, h, 0),
  (-w/2, 0, 0).
- Head pose: on `streamengineservices` (inbound, message mode) the first message is a u32
  channel (0x2711–0x2714) and the name of a pipe the game created; answers go to that
  pipe. Each channel is greeted with u32 1. Requests and answers are "sesp" frames
  ("sesp", u32 length, u32 length ^ 0x70736573, FlatBuffer payload; little-endian) whose
  root table has a sequence number, a type and the message. Request 2 → answer 3
  (capabilities), 9 → 10 (display: device id, width, height), 0x0b → 0x0c (device list);
  others are acknowledged with type + 1. On channel 0x2712 head poses (type 8) are
  pushed ~30 times a second: u64 timestamp µs, position x y z mm, rotation pitch yaw roll
  radians (float32), and four validity flags.

## Open questions

- Do add point / compute / get blob / set blob also work without realm 0x2712?
  Start and stop do; the rest is untested.
- Remaining columns of stream 0x500; meaning of ops in the attach query list.
