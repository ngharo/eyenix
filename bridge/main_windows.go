//go:build !fakegame

// eyenix-bridge.exe runs inside a game's Wine prefix. Tobii's game library
// looks for Tobii's Windows services through named pipes, which only exist
// inside Wine, so this small program provides them and passes on what the
// Eyenix app (outside Wine) sends it: head pose, over UDP on the loopback
// interface. Gaze does not come through here; the library fetches it from the
// app directly over TCP, at the address this program hands out.
//
// It is started and stopped by the app. It also stops by itself when the app
// has been silent for a while, and when the prefix shuts down.
package main

import (
	"flag"
	"fmt"
	"net"
	"os"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

const (
	pipeAccessInbound     = 0x1
	pipeAccessDuplex      = 0x3
	pipeMessage           = 0x4 | 0x2 // message type, message read mode; blocking
	pipeUnlimited         = 255
	fileFlagOverlap       = 0x40000000
	fileFlagFirstInstance = 0x00080000
	errorPipeConnect      = syscall.Errno(535)
	errorMoreData         = syscall.Errno(234)
	errorIOPending        = syscall.Errno(997)
	waitObject0           = 0

	channelFirst    = 0x2711 // the channels a game opens; it waits for a "1" on each
	channelLast     = 0x2714
	channelHeadPose = 0x2712

	replyTimeout = 1000 // ms a game gets to take an answer
	poseTimeout  = 100  // ms it gets to take a head pose before that one is skipped
	poseMaxAge   = 750 * time.Millisecond
	poseInterval = 33 * time.Millisecond
	appSilence   = 10 * time.Second // exit when the app has sent nothing for this long
)

var (
	kernel32             = syscall.NewLazyDLL("kernel32.dll")
	procCreateNamedPipe  = kernel32.NewProc("CreateNamedPipeW")
	procConnectNamedPipe = kernel32.NewProc("ConnectNamedPipe")
	procDisconnectPipe   = kernel32.NewProc("DisconnectNamedPipe")
	procCreateEvent      = kernel32.NewProc("CreateEventW")
	procGetOverlapped    = kernel32.NewProc("GetOverlappedResult")
	ntdll                = syscall.NewLazyDLL("ntdll.dll")
	procSetInfoProcess   = ntdll.NewProc("NtSetInformationProcess")
)

// The newest head pose from the app, shared by every connected game.
var live struct {
	sync.Mutex
	pose headPose
	at   time.Time
}

var lastHeard atomic.Int64 // when the app last sent anything, unix nanoseconds

// say reports an event to the app, which reads our standard output.
func say(what string) { fmt.Println(what) }

var saidOnce sync.Map

func sayOnce(what string) {
	if _, done := saidOnce.LoadOrStore(what, true); !done {
		say(what)
	}
}

// Wine keeps a prefix alive while any ordinary process runs in it. Marking
// ourselves as a system process lets the prefix shut down when the game
// exits, taking us with it.
func becomeSystemProcess() {
	const wineMakeProcessSystem = 1000
	if procSetInfoProcess.Find() != nil {
		return
	}
	self, _ := syscall.GetCurrentProcess()
	var event syscall.Handle
	procSetInfoProcess.Call(uintptr(self), wineMakeProcessSystem, uintptr(unsafe.Pointer(&event)), unsafe.Sizeof(event))
}

func listenForPose(port int) error {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: port})
	if err != nil {
		return err
	}
	go func() {
		buf := make([]byte, 256)
		for {
			n, _, err := conn.ReadFromUDP(buf)
			if err != nil {
				return
			}
			pose, ok := parsePosePacket(buf[:n])
			if !ok {
				continue
			}
			lastHeard.Store(time.Now().UnixNano())
			if pose.posValid || pose.rotValid {
				live.Lock()
				live.pose, live.at = pose, time.Now()
				live.Unlock()
			}
		}
	}()
	return nil
}

// readMessage reads one pipe message. A message longer than the buffer is cut short.
func readMessage(pipe syscall.Handle, buf []byte) ([]byte, bool) {
	var got uint32
	err := syscall.ReadFile(pipe, buf, &got, nil)
	if (err != nil && err != errorMoreData) || got == 0 {
		return nil, false
	}
	return buf[:got], true
}

// A game's own pipe, which answers and head poses are written to.
type client struct {
	sync.Mutex
	handle syscall.Handle
}

// write gives the game a limited time to take the data, so one that has
// stopped reading cannot hold us up. False means the pipe is broken.
func (c *client) write(data []byte, timeoutMs uint32) bool {
	c.Lock()
	defer c.Unlock()
	event, _, _ := procCreateEvent.Call(0, 1, 0, 0)
	if event == 0 {
		return false
	}
	defer syscall.CloseHandle(syscall.Handle(event))
	ov := syscall.Overlapped{HEvent: syscall.Handle(event)}
	var wrote uint32
	err := syscall.WriteFile(c.handle, data, &wrote, &ov)
	if err == nil {
		return true
	}
	if err != errorIOPending {
		return false
	}
	state, _ := syscall.WaitForSingleObject(ov.HEvent, timeoutMs)
	if state != waitObject0 {
		syscall.CancelIoEx(c.handle, &ov)
	}
	// Wait for the outcome either way: the Overlapped must not go away while the write is live.
	ok, _, _ := procGetOverlapped.Call(uintptr(c.handle), uintptr(unsafe.Pointer(&ov)), uintptr(unsafe.Pointer(&wrote)), 1)
	return ok != 0 || state != waitObject0 // a timed-out write is skipped, not fatal
}

func (c *client) streamHeadPose(stop <-chan struct{}) {
	ticker := time.NewTicker(poseInterval)
	defer ticker.Stop()
	start := time.Now()
	for seq := uint32(1); ; seq++ {
		select {
		case <-stop:
			return
		case <-ticker.C:
		}
		live.Lock()
		pose, fresh := live.pose, time.Since(live.at) <= poseMaxAge
		live.Unlock()
		if !fresh {
			// Nobody in view: keep the last pose but say it cannot be trusted.
			pose.posValid, pose.rotValid = false, false
			if pose.z == 0 {
				pose.z = 600
			}
		}
		pose.timestampUs = uint64(time.Since(start).Microseconds())
		if !c.write(headPoseMessage(seq, pose), poseTimeout) {
			return
		}
	}
}

// serveGame handles one connection to the services pipe. The game announces a
// channel and the name of a pipe of its own, which answers go to.
func serveGame(pipe syscall.Handle, cfg config) {
	buf := make([]byte, 65536)
	first, ok := readMessage(pipe, buf)
	if !ok {
		return
	}
	if containsLicence(first) {
		sayOnce("licence")
	}
	channel, name, ok := clientPipeName(first)
	if !ok {
		return
	}
	path, _ := syscall.UTF16PtrFromString(`\\.\pipe\` + name)
	handle, err := syscall.CreateFile(path, syscall.GENERIC_WRITE, 0, nil, syscall.OPEN_EXISTING,
		syscall.FILE_ATTRIBUTE_NORMAL|fileFlagOverlap, 0)
	if err != nil {
		return
	}
	c := &client{handle: handle}
	defer syscall.CloseHandle(handle)

	if channel >= channelFirst && channel <= channelLast {
		c.write([]byte{1, 0, 0, 0}, replyTimeout)
	}
	if channel == channelHeadPose {
		sayOnce("headpose")
		stop := make(chan struct{})
		done := make(chan struct{})
		go func() { c.streamHeadPose(stop); close(done) }()
		defer func() { close(stop); <-done }()
	}

	for {
		msg, ok := readMessage(pipe, buf)
		if !ok {
			return
		}
		if containsLicence(msg) {
			sayOnce("licence")
		}
		for _, payload := range splitFrames(msg) {
			if reply := replyTo(payload, cfg); reply != nil {
				c.write(reply, replyTimeout)
			}
		}
	}
}

// serveAddress answers the pipe that tells the library where the tracker's
// service listens: two u32 ones, then the address in a fixed 512-byte field.
func serveAddress(pipe syscall.Handle, host string) {
	if _, ok := readMessage(pipe, make([]byte, 64)); !ok {
		return
	}
	out := make([]byte, 8+0x200)
	le.PutUint32(out[0:], 1)
	le.PutUint32(out[4:], 1)
	copy(out[8:len(out)-1], host)
	var wrote uint32
	syscall.WriteFile(pipe, out, &wrote, nil)
	syscall.FlushFileBuffers(pipe)
}

// serveNothing is for pipes that only need to exist and accept a connection.
func serveNothing(pipe syscall.Handle) { readMessage(pipe, make([]byte, 4096)) }

// listen creates the named pipe and hands each connection to serve.
func listen(name string, access uintptr, serve func(syscall.Handle)) error {
	path, err := syscall.UTF16PtrFromString(`\\.\pipe\` + name)
	if err != nil {
		return err
	}
	create := func(extra uintptr) (syscall.Handle, error) {
		h, _, err := procCreateNamedPipe.Call(uintptr(unsafe.Pointer(path)), access|extra, pipeMessage, pipeUnlimited, 65536, 65536, 0, 0)
		if syscall.Handle(h) == syscall.InvalidHandle {
			return 0, err
		}
		return syscall.Handle(h), nil
	}
	// Fails if the pipe exists already, so two copies cannot serve one prefix.
	pipe, err := create(fileFlagFirstInstance)
	if err != nil {
		return fmt.Errorf("pipe %s: %w", name, err)
	}
	go func() {
		for {
			ok, _, err := procConnectNamedPipe.Call(uintptr(pipe), 0)
			if ok != 0 || err == errorPipeConnect {
				sayOnce("connected")
				go func(p syscall.Handle) {
					serve(p)
					procDisconnectPipe.Call(uintptr(p))
					syscall.CloseHandle(p)
				}(pipe)
			} else {
				syscall.CloseHandle(pipe)
			}
			for {
				if pipe, err = create(0); err == nil {
					break
				}
				time.Sleep(200 * time.Millisecond)
			}
		}
	}()
	return nil
}

func main() {
	var cfg config
	var width, height uint
	flag.StringVar(&cfg.serial, "serial", "IS5FF-100200000000", "the tracker's serial number")
	flag.StringVar(&cfg.displayName, "display-name", `\\.\DISPLAY1`, "Windows name of the tracker's screen")
	flag.StringVar(&cfg.displayDeviceID, "display-device-id", `DISPLAY\DEFAULT_MONITOR\0000&0000`, "Windows device id of the tracker's screen")
	flag.UintVar(&width, "display-width", 1920, "width of the tracker's screen in pixels")
	flag.UintVar(&height, "display-height", 1080, "height of the tracker's screen in pixels")
	host := flag.String("host", "127.0.0.1", "where the app serves gaze")
	port := flag.Int("pose-port", 4243, "UDP port head pose arrives on")
	flag.Parse()
	cfg.displayWidth, cfg.displayHeight = uint32(width), uint32(height)

	becomeSystemProcess()
	lastHeard.Store(time.Now().UnixNano())

	if err := listenForPose(*port); err != nil {
		fmt.Fprintln(os.Stderr, "eyenix-bridge:", err)
		os.Exit(1)
	}
	pipes := []struct {
		name   string
		access uintptr
		serve  func(syscall.Handle)
	}{
		{"streamengineservices", pipeAccessInbound, func(p syscall.Handle) { serveGame(p, cfg) }},
		{"ETDefaultPIPE", pipeAccessDuplex, func(p syscall.Handle) { serveAddress(p, *host) }},
		{"TOBII-" + *host, pipeAccessInbound, serveNothing},
		{"TOBIIPRP-" + cfg.serial, pipeAccessInbound, serveNothing},
	}
	for _, p := range pipes {
		if err := listen(p.name, p.access, p.serve); err != nil {
			// Usually another copy already serving this prefix.
			fmt.Fprintln(os.Stderr, "eyenix-bridge:", err)
			os.Exit(1)
		}
	}
	say("ready")

	for {
		time.Sleep(time.Second)
		if time.Since(time.Unix(0, lastHeard.Load())) > appSilence {
			return
		}
	}
}
