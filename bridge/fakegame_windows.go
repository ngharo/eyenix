//go:build fakegame

// A stand-in for a game, used by bin/test-bridge to check eyenix-bridge.exe
// under Wine. It does what Tobii's game library does: asks where the tracker's
// service is, opens the head pose channel, makes a few requests and reads head
// poses. It prints what it saw, one "key value" per line, and exits non-zero
// if anything is missing.
package main

import (
	"fmt"
	"math"
	"os"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

var (
	kernel32             = syscall.NewLazyDLL("kernel32.dll")
	procCreateNamedPipe  = kernel32.NewProc("CreateNamedPipeW")
	procConnectNamedPipe = kernel32.NewProc("ConnectNamedPipe")
)

func fail(format string, args ...any) {
	fmt.Printf("FAIL "+format+"\n", args...)
	os.Exit(1)
}

func open(name string, access uint32) syscall.Handle {
	path, _ := syscall.UTF16PtrFromString(`\\.\pipe\` + name)
	var h syscall.Handle
	var err error
	for try := 0; try < 100; try++ { // the bridge may still be starting
		h, err = syscall.CreateFile(path, access, 0, nil, syscall.OPEN_EXISTING, 0, 0)
		if err == nil {
			return h
		}
		time.Sleep(100 * time.Millisecond)
	}
	fail("cannot open pipe %s: %v", name, err)
	return 0
}

func read(h syscall.Handle) []byte {
	buf := make([]byte, 65536)
	var got uint32
	if err := syscall.ReadFile(h, buf, &got, nil); err != nil || got == 0 {
		fail("read: %v", err)
	}
	return buf[:got]
}

func write(h syscall.Handle, data []byte) {
	var wrote uint32
	if err := syscall.WriteFile(h, data, &wrote, nil); err != nil {
		fail("write: %v", err)
	}
}

func main() {
	// 1. Where is the service?
	address := open("ETDefaultPIPE", syscall.GENERIC_READ|syscall.GENERIC_WRITE)
	write(address, []byte{1, 0, 0, 0})
	answer := read(address)
	if len(answer) != 8+0x200 {
		fail("address answer is %d bytes", len(answer))
	}
	fmt.Println("host", strings.TrimRight(string(answer[8:]), "\x00"))
	syscall.CloseHandle(address)

	// 2. Our own pipe, then announce it on the head pose channel.
	const mine = "client_streamengineservices_0000beef_00006ffffee90001"
	path, _ := syscall.UTF16PtrFromString(`\\.\pipe\` + mine)
	inbox, _, err := procCreateNamedPipe.Call(uintptr(unsafe.Pointer(path)), 0x1, 0x4|0x2, 1, 65536, 65536, 0, 0)
	if syscall.Handle(inbox) == syscall.InvalidHandle {
		fail("create own pipe: %v", err)
	}
	services := open("streamengineservices", syscall.GENERIC_WRITE)
	write(services, append([]byte{0x12, 0x27, 0, 0}, (mine+"\x00")...))
	procConnectNamedPipe.Call(inbox, 0)
	in := syscall.Handle(inbox)

	if hello := read(in); len(hello) != 4 || le.Uint32(hello) != 1 {
		fail("channel greeting % x", hello)
	}
	fmt.Println("greeting ok")

	// 3. Requests. Head poses arrive in between, so skip those while waiting.
	ask := func(kind uint8, seq uint32) []byte {
		write(services, statusReply(seq, kind)) // a request has the same layout
		for {
			for _, payload := range splitFrames(read(in)) {
				if s, k, ok := rootType(payload); ok && k != replyHeadPose {
					if s != seq {
						fail("answer to request %d has sequence %d", seq, s)
					}
					fmt.Printf("request %#x answer %#x\n", kind, k)
					return payload
				}
			}
		}
	}
	ask(reqInitialize, 1)
	devices := ask(reqListDevices, 2)
	if at := strings.Index(string(devices), "tobii-ttp://"); at >= 0 {
		fmt.Println("device", strings.SplitN(string(devices[at:]), "\x00", 2)[0])
	}
	ask(reqDisplayInfo, 3)
	ask(0x14, 4)

	// 4. Head poses for two seconds; report the last one.
	deadline := time.Now().Add(2 * time.Second)
	count, valid := 0, 0
	var last [6]float32
	for time.Now().Before(deadline) {
		for _, p := range splitFrames(read(in)) {
			if _, k, ok := rootType(p); !ok || k != replyHeadPose {
				continue
			}
			count++
			const inner = 20
			for i := range last {
				last[i] = math.Float32frombits(le.Uint32(p[inner+12+4*i:]))
			}
			if le.Uint32(p[inner+36:]) == 1 && le.Uint32(p[inner+40:]) == 1 {
				valid++
			}
		}
	}
	fmt.Println("poses", count)
	fmt.Println("valid", valid)
	fmt.Printf("last %.2f %.2f %.2f %.3f %.3f %.3f\n", last[0], last[1], last[2], last[3], last[4], last[5])
	if count < 30 {
		fail("only %d head poses in two seconds", count)
	}
}
