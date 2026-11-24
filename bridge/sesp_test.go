package main

import (
	"encoding/hex"
	"math"
	"testing"
)

// Bytes produced by tobii-faithful's C encoder for the same inputs.
const (
	goldenInitialize  = "736573704c0000003f65737004000000c2ffffff070000000800000003000000ecffffff010101010101010101010101010101002400140000000400050006000700080009000a000b000c000d000a000d0004000c000800"
	goldenStatus      = "736573702a0000005965737004000000e4ffffff090000000800000005000000fcffffff06000400000000000a000d0004000c000800"
	goldenHeadPose    = "736573706e0000001d65737004000000a0ffffff2a0000000800000008000000ccffffff34120000bc9a7856000048c10000f24100c01944cdcccc3dcdcc4cbecdcc4c3d010000000100000001000000010000001c003400040008000c001000140018001c002000240028002c0030000a000d0004000c000800"
	goldenListDevices = "73657370ea000000996573700400000024ffffff03000000080000000c0000003affffff0400000001000000040000005cffffff1c0000003c0000004c00000064000000780000007c000000010000001e000000746f6269692d7474703a2f2f49533546462d31303032303030303030303000000d00000045796520547261636b65722035000000160000004953355f4c617267655f457965747261636b65725f3500001200000049533546462d313030323030303030303030000003000000495335000a00000030326131613661393737000012002000040008000c001000140018001c000600080004000a000d0004000c000800" // serial IS5FF-100200000000
)

func same(t *testing.T, what string, got []byte, want string) {
	t.Helper()
	if hex.EncodeToString(got) != want {
		t.Errorf("%s:\n got %x\nwant %s", what, got, want)
	}
}

func TestMessagesMatchTheReferenceEncoder(t *testing.T) {
	same(t, "initialize", initializeReply(7), goldenInitialize)
	same(t, "status", statusReply(9, 5), goldenStatus)
	same(t, "head pose", headPoseMessage(42, headPose{
		timestampUs: 0x123456789abc, x: -12.5, y: 30.25, z: 615,
		pitch: 0.1, yaw: -0.2, roll: 0.05, posValid: true, rotValid: true,
	}), goldenHeadPose)
	same(t, "list devices", listDevicesReply(3, config{serial: "IS5FF-100200000000"}), goldenListDevices)
}

// table reads a FlatBuffer table's field the way the game library would.
type table struct {
	p   []byte
	obj int
}

func (t table) field(i int) int { // offset of field i within the payload, 0 if absent
	vt := t.obj - int(int32(le.Uint32(t.p[t.obj:])))
	if 4+2*i >= int(le.Uint16(t.p[vt:])) {
		return 0
	}
	off := int(le.Uint16(t.p[vt+4+2*i:]))
	if off == 0 {
		return 0
	}
	return t.obj + off
}
func (t table) u32(i int) uint32 { return le.Uint32(t.p[t.field(i):]) }
func (t table) follow(i int) int { f := t.field(i); return f + int(le.Uint32(t.p[f:])) }
func (t table) text(i int) string {
	at := t.follow(i)
	return string(t.p[at+4 : at+4+int(le.Uint32(t.p[at:]))])
}

// openMessage checks the frame and returns the message table inside the root.
func openMessage(t *testing.T, framed []byte, wantSeq uint32, wantKind uint8) table {
	t.Helper()
	payloads := splitFrames(framed)
	if len(payloads) != 1 {
		t.Fatalf("expected one frame, got %d", len(payloads))
	}
	p := payloads[0]
	if le.Uint32(framed[8:]) != uint32(len(p))^lengthMask {
		t.Fatal("bad length check word")
	}
	seq, kind, ok := rootType(p)
	if !ok || seq != wantSeq || kind != wantKind {
		t.Fatalf("root: ok=%v seq=%d kind=%d", ok, seq, kind)
	}
	root := table{p, int(le.Uint32(p))}
	return table{p, root.follow(2)}
}

func TestDisplayInfoNamesTheScreen(t *testing.T) {
	c := config{displayDeviceID: `display#default_monitor#0000&0000`, displayWidth: 2560, displayHeight: 1440}
	m := openMessage(t, displayInfoReply(5, c), 5, replyDisplayInfo)
	if got := m.text(1); got != `DISPLAY\DEFAULT_MONITOR\0000&0000` {
		t.Errorf("device id %q", got)
	}
	if m.u32(2) != 2560 {
		t.Errorf("width %d", m.u32(2))
	}
	// See the note on displayInfoReply: the height field holds the id's length.
	if m.u32(3) != uint32(len(m.text(1))) {
		t.Errorf("height field %d: layout no longer matches the reference encoder", m.u32(3))
	}
}

func TestListDevicesDescribesTheTracker(t *testing.T) {
	for _, serial := range []string{"IS5FF-100200000000", "X", "a-serial-of-another-length"} {
		m := openMessage(t, listDevicesReply(3, config{serial: serial}), 3, replyListDevices)
		vector := m.follow(0)
		if le.Uint32(m.p[vector:]) != 1 {
			t.Fatal("expected one device")
		}
		d := table{m.p, vector + 4 + int(le.Uint32(m.p[vector+4:]))}
		got := []string{d.text(0), d.text(1), d.text(2), d.text(3), d.text(4), d.text(5)}
		want := []string{"tobii-ttp://" + serial, "Eye Tracker 5", "IS5_Large_Eyetracker_5", serial, "IS5", "02a1a6a977"}
		for i := range want {
			if got[i] != want[i] {
				t.Errorf("serial %q field %d: %q, want %q", serial, i, got[i], want[i])
			}
		}
		if d.u32(6) != 1 {
			t.Error("device not marked present")
		}
	}
}

func TestRepliesFollowTheRequestType(t *testing.T) {
	c := config{serial: "S"}
	request := func(kind uint8) []byte { return splitFrames(statusReply(11, kind))[0] } // same layout as a request
	for kind, want := range map[uint8]uint8{reqInitialize: replyInitialize, reqDisplayInfo: replyDisplayInfo,
		reqListDevices: replyListDevices, 4: 5, 0x32: 0x33, 0x77: replyInitialize} {
		reply := replyTo(request(kind), c)
		seq, got, ok := rootType(splitFrames(reply)[0])
		if !ok || seq != 11 || got != want {
			t.Errorf("request %#x: reply type %#x seq %d, want %#x", kind, got, seq, want)
		}
	}
}

func TestBadInputIsRefused(t *testing.T) {
	good := statusReply(1, 4)
	for n := 0; n < len(good); n++ { // cut short anywhere: no frame, no crash
		if len(splitFrames(good[:n])) != 0 {
			t.Fatalf("accepted a frame cut to %d bytes", n)
		}
		rootType(good[frameHeader:][:max(0, n-frameHeader)])
	}
	huge := append([]byte("sesp\xff\xff\xff\x7f"), make([]byte, 64)...)
	if len(splitFrames(huge)) != 0 {
		t.Error("accepted an absurd length")
	}
	wild := make([]byte, 64)
	for i := range wild {
		wild[i] = 0xff
	}
	if _, _, ok := rootType(wild); ok {
		t.Error("accepted wild offsets")
	}
	if replyTo(wild, config{}) != nil {
		t.Error("answered garbage")
	}
	if two := splitFrames(append(append([]byte{}, good...), good...)); len(two) != 2 {
		t.Errorf("two frames back to back: got %d", len(two))
	}
}

func TestClientPipeName(t *testing.T) {
	msg := append([]byte{0x12, 0x27, 0, 0}, "client_streamengineservices_0000abcd_00006ffffee90001\x00junk"...)
	channel, name, ok := clientPipeName(msg)
	if !ok || channel != 0x2712 || name != "client_streamengineservices_0000abcd_00006ffffee90001" {
		t.Errorf("got %v %#x %q", ok, channel, name)
	}
	for _, bad := range []string{"", "no-terminator", "..\\..\\other\x00", "a/b\x00", "sp ace\x00"} {
		if _, _, ok := clientPipeName(append([]byte{1, 0, 0, 0}, bad...)); ok {
			t.Errorf("accepted %q", bad)
		}
	}
}

func TestPosePacket(t *testing.T) {
	b := make([]byte, posePacketSize)
	copy(b, "EYNX")
	le.PutUint32(b[4:], 3)
	for i, v := range []float32{-10, 20, 600, 0.1, 0.2, float32(math.NaN())} {
		le.PutUint32(b[8+4*i:], math.Float32bits(v))
	}
	h, ok := parsePosePacket(b)
	if !ok || !h.posValid || !h.rotValid || h.x != -10 || h.z != 600 || h.yaw != 0.2 || h.roll != 0 {
		t.Errorf("got %+v %v", h, ok)
	}
	if _, ok := parsePosePacket(b[:31]); ok {
		t.Error("accepted a short packet")
	}
	b[0] = 'X'
	if _, ok := parsePosePacket(b); ok {
		t.Error("accepted a foreign packet")
	}
}

func TestLicenceMarker(t *testing.T) {
	if !containsLicence([]byte("xx--SE-SIGNATURE--yy")) || !containsLicence([]byte("-\x00-\x00S\x00E\x00-\x00S\x00I\x00G\x00N\x00A\x00T\x00U\x00R\x00E\x00-\x00-\x00")) {
		t.Error("missed a licence")
	}
	if containsLicence([]byte("nothing here")) {
		t.Error("false alarm")
	}
}
