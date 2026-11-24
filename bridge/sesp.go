// The message formats in this file were worked out by the tobii-faithful project
// (https://github.com/SciFi-Bob/tobii-faithful v1.1.8, src/wine/sesp_codec.c and
// src/wine/sesp-bridge.c, Copyright its authors, Apache License 2.0; see
// licenses/Apache-2.0.txt). This is a rewrite in Go, not a copy.

package main

import (
	"bytes"
	"encoding/binary"
	"math"
	"strings"
)

// Tobii's game library talks to "stream engine services" in frames:
//
//	"sesp", u32 payload length, u32 length ^ 0x70736573, payload
//
// all little-endian. The payload is a FlatBuffer whose root table holds a
// sequence number, a message type and a nested table with the message itself.

const (
	frameHeader = 12
	lengthMask  = 0x70736573 // "sesp" as a little-endian number

	// Requests the library makes, and the type of the answer to each.
	reqInitialize  = 2
	reqDisplayInfo = 9
	reqListDevices = 0x0b

	replyInitialize  = 3
	replyHeadPose    = 8
	replyDisplayInfo = 10
	replyListDevices = 0x0c

	maxFrame = 1 << 20
)

var le = binary.LittleEndian

// Settings the answers are built from.
type config struct {
	serial          string // the tracker's serial number
	displayName     string // Windows name of the screen the tracker is on
	displayDeviceID string
	displayWidth    uint32 // pixels
	displayHeight   uint32
}

// Head pose as the library wants it: millimetres and radians.
type headPose struct {
	timestampUs      uint64
	x, y, z          float32
	pitch, yaw, roll float32
	posValid         bool
	rotValid         bool
}

func frame(payload []byte) []byte {
	out := make([]byte, frameHeader+len(payload))
	copy(out, "sesp")
	le.PutUint32(out[4:], uint32(len(payload)))
	le.PutUint32(out[8:], uint32(len(payload))^lengthMask)
	copy(out[frameHeader:], payload)
	return out
}

// splitFrames returns the payloads of the complete frames at the start of data.
func splitFrames(data []byte) [][]byte {
	var out [][]byte
	for len(data) >= frameHeader && string(data[:4]) == "sesp" {
		n := le.Uint32(data[4:])
		if n > maxFrame || uint64(n) > uint64(len(data)-frameHeader) {
			break
		}
		out = append(out, data[frameHeader:frameHeader+int(n)])
		data = data[frameHeader+int(n):]
	}
	return out
}

// rootType reads the sequence number and message type from a payload.
func rootType(p []byte) (seq uint32, kind uint8, ok bool) {
	if len(p) < 24 {
		return
	}
	obj := int64(le.Uint32(p))
	if obj > int64(len(p))-16 {
		return
	}
	vt := obj - int64(int32(le.Uint32(p[obj:])))
	if vt < 0 || vt+10 > int64(len(p)) {
		return
	}
	vtLen := int64(le.Uint16(p[vt:]))
	if vtLen < 10 || vt+vtLen > int64(len(p)) {
		return
	}
	seqOff, typeOff := int64(le.Uint16(p[vt+4:])), int64(le.Uint16(p[vt+6:]))
	if seqOff == 0 || typeOff == 0 || obj+seqOff+4 > int64(len(p)) || obj+typeOff >= int64(len(p)) {
		return
	}
	return le.Uint32(p[obj+seqOff:]), p[obj+typeOff], true
}

// Every message starts the same way: the root table at offset 4 pointing at
// the message's own table at offset 20. wrap fills that in, and the root's
// field table, which sits in the last ten bytes. Call it last: in some
// messages the root's field table is laid over the unused tail of the
// message's own, exactly as the reference encoder does.
func wrap(p []byte, seq uint32, kind uint8, innerVT int) {
	const root, inner = 4, 20
	rootVT := len(p) - 10
	le.PutUint32(p[0:], root)
	le.PutUint32(p[root:], uint32(int32(root-rootVT)))
	le.PutUint32(p[root+4:], seq)
	le.PutUint32(p[root+8:], inner-(root+8))
	p[root+12] = kind
	le.PutUint32(p[inner:], uint32(int32(inner-innerVT)))
	for i, v := range []uint16{10, 13, 4, 12, 8} {
		le.PutUint16(p[rootVT+2*i:], v)
	}
}

// The answer to "initialize": fifteen capability flags, all on.
func initializeReply(seq uint32) []byte {
	const inner, innerVT = 20, 40
	p := make([]byte, 76)
	for i := 1; i <= 15; i++ {
		p[inner+3+i] = 1
		le.PutUint16(p[innerVT+4+2*i:], uint16(3+i))
	}
	le.PutUint16(p[innerVT:], 36)
	le.PutUint16(p[innerVT+2:], 20)
	wrap(p, seq, replyInitialize, innerVT)
	return frame(p)
}

// An empty answer of the given type: "done".
func statusReply(seq uint32, kind uint8) []byte {
	const innerVT = 24
	p := make([]byte, 42)
	le.PutUint16(p[innerVT:], 6)
	le.PutUint16(p[innerVT+2:], 4)
	wrap(p, seq, kind, innerVT)
	return frame(p)
}

func headPoseMessage(seq uint32, h headPose) []byte {
	const inner, innerSize = 20, 52
	innerVT := inner + innerSize
	p := make([]byte, innerVT+28+10)
	le.PutUint32(p[inner+4:], uint32(h.timestampUs>>32))
	le.PutUint32(p[inner+8:], uint32(h.timestampUs))
	for i, v := range []float32{h.x, h.y, h.z, h.pitch, h.yaw, h.roll} {
		le.PutUint32(p[inner+12+4*i:], math.Float32bits(v))
	}
	flag := func(b bool) uint32 {
		if b {
			return 1
		}
		return 0
	}
	le.PutUint32(p[inner+36:], flag(h.posValid))
	for i := 0; i < 3; i++ {
		le.PutUint32(p[inner+40+4*i:], flag(h.rotValid))
	}
	le.PutUint16(p[innerVT:], 28)
	le.PutUint16(p[innerVT+2:], innerSize)
	for i := 0; i < 12; i++ {
		le.PutUint16(p[innerVT+4+2*i:], uint16(4+4*i))
	}
	wrap(p, seq, replyHeadPose, innerVT)
	return frame(p)
}

func align4(n int) int { return (n + 3) &^ 3 }

// putString writes a length-prefixed, NUL-terminated string and returns where the next thing goes.
func putString(p []byte, at int, s string) int {
	le.PutUint32(p[at:], uint32(len(s)))
	copy(p[at+4:], s)
	return align4(at + 4 + len(s) + 1)
}

// Which screen the tracker is attached to, and its size in pixels.
//
// Known quirk, kept on purpose: the reference encoder puts the device id
// string where the height field lives, so a reader sees the id's length as
// the height. Games are known to work with exactly these bytes and nothing is
// known about how they react to the "right" ones, so this stays byte-for-byte
// the same until it can be tried against a game.
func displayInfoReply(seq uint32, c config) []byte {
	// The library matches this against Windows' monitor device ids.
	id := strings.ToUpper(strings.ReplaceAll(c.displayDeviceID, "#", `\`))
	const inner, idAt = 20, 36
	innerVT := align4(idAt + 4 + len(id) + 1)
	p := make([]byte, innerVT+12+10)
	le.PutUint32(p[inner+8:], idAt-(inner+8))
	le.PutUint32(p[inner+12:], c.displayWidth)
	le.PutUint32(p[inner+16:], c.displayHeight)
	putString(p, idAt, id)
	for i, v := range []uint16{12, 20, 4, 8, 12, 16} {
		le.PutUint16(p[innerVT+2*i:], v)
	}
	wrap(p, seq, replyDisplayInfo, innerVT)
	return frame(p)
}

// The one tracker we have.
func listDevicesReply(seq uint32, c config) []byte {
	const inner, vector, device, stringsAt = 20, 28, 36, 68
	texts := []string{
		"tobii-ttp://" + c.serial, // address
		"Eye Tracker 5",           // name
		"IS5_Large_Eyetracker_5",  // model
		c.serial,
		"IS5",        // family
		"02a1a6a977", // firmware version
	}
	size := stringsAt
	for _, s := range texts {
		size = align4(size + 4 + len(s) + 1)
	}
	deviceVT, innerVT := size, size+18
	p := make([]byte, innerVT+6+10)
	le.PutUint32(p[inner+4:], vector-(inner+4))
	le.PutUint32(p[vector:], 1)
	le.PutUint32(p[vector+4:], device-(vector+4))
	le.PutUint32(p[device:], uint32(int32(device-deviceVT)))
	at := stringsAt
	for i, s := range texts {
		field := device + 4 + 4*i
		le.PutUint32(p[field:], uint32(at-field))
		at = putString(p, at, s)
	}
	le.PutUint32(p[device+28:], 1)
	for i, v := range []uint16{18, 32, 4, 8, 12, 16, 20, 24, 28} {
		le.PutUint16(p[deviceVT+2*i:], v)
	}
	for i, v := range []uint16{6, 8, 4} {
		le.PutUint16(p[innerVT+2*i:], v)
	}
	wrap(p, seq, replyListDevices, innerVT)
	return frame(p)
}

// replyTo answers one request payload, or returns nil if it cannot be read.
func replyTo(payload []byte, c config) []byte {
	seq, kind, ok := rootType(payload)
	if !ok {
		return nil
	}
	switch kind {
	case reqInitialize:
		return initializeReply(seq)
	case reqDisplayInfo:
		return displayInfoReply(seq, c)
	case reqListDevices:
		return listDevicesReply(seq, c)
	case 4, 6, 0x0d, 0x10, 0x14, 0x16, 0x1a, 0x21, 0x23, 0x32:
		// Subscriptions and settings: the answer's type is the request's plus one.
		return statusReply(seq, kind+1)
	}
	return statusReply(seq, replyInitialize)
}

// Tobii licences carry this marker, as ASCII or UTF-16.
func containsLicence(data []byte) bool {
	const marker = "--SE-SIGNATURE--"
	wide := make([]byte, 0, 2*len(marker))
	for i := 0; i < len(marker); i++ {
		wide = append(wide, marker[i], 0)
	}
	return bytes.Contains(data, []byte(marker)) || bytes.Contains(data, wide)
}

// What the app sends us over UDP, 30 or so times a second:
//
//	"EYNX", u32 flags (1: position valid, 2: rotation valid),
//	six float32: x y z in millimetres, pitch yaw roll in radians
//
// With neither flag set it is only a sign that the app is still there.
const posePacketSize = 32

func parsePosePacket(b []byte) (h headPose, ok bool) {
	if len(b) != posePacketSize || string(b[:4]) != "EYNX" {
		return
	}
	flags := le.Uint32(b[4:])
	var v [6]float32
	for i := range v {
		v[i] = math.Float32frombits(le.Uint32(b[8+4*i:]))
		if math.IsNaN(float64(v[i])) || math.IsInf(float64(v[i]), 0) {
			v[i] = 0
		}
	}
	return headPose{x: v[0], y: v[1], z: v[2], pitch: v[3], yaw: v[4], roll: v[5],
		posValid: flags&1 != 0, rotValid: flags&2 != 0}, true
}

// clientPipeName picks the pipe name a game announces out of its first message:
// u32 channel, then the name, NUL-terminated.
func clientPipeName(msg []byte) (channel uint32, name string, ok bool) {
	if len(msg) < 6 {
		return
	}
	end := bytes.IndexByte(msg[4:], 0)
	if end <= 0 || end > 200 {
		return
	}
	name = string(msg[4 : 4+end])
	for _, r := range name {
		// A plain pipe name only: nothing that could point anywhere else.
		if !(r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || strings.ContainsRune("_-.{}", r)) {
			return
		}
	}
	return le.Uint32(msg), name, true
}
