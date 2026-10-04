package main

import (
	"encoding/binary"
	"net"
	"testing"
	"time"
)

type client struct {
	t    *testing.T
	conn *net.UDPConn
	id   uint32
}

func dial(t *testing.T, r *Relay, id uint32) *client {
	t.Helper()
	conn, err := net.DialUDP("udp", nil, r.Addr())
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.Close() })
	return &client{t: t, conn: conn, id: id}
}

func (c *client) send(p []byte) {
	if _, err := c.conn.Write(p); err != nil {
		c.t.Fatal(err)
	}
}

func (c *client) join(pin string) {
	p := append(header(typeJoin, c.id), byte(len(pin)))
	c.send(append(p, pin...))
}

func (c *client) relay(dst uint32, payload string) {
	p := binary.LittleEndian.AppendUint32(header(typeRelay, c.id), dst)
	c.send(append(p, payload...))
}

// recv returns the next datagram of type typ, or nil after the timeout.
func (c *client) recv(typ byte, timeout time.Duration) []byte {
	buf := make([]byte, 2048)
	deadline := time.Now().Add(timeout)
	for {
		_ = c.conn.SetReadDeadline(deadline)
		n, err := c.conn.Read(buf)
		if err != nil {
			return nil
		}
		if n >= headerLen && buf[4] == typ {
			return append([]byte(nil), buf[:n]...)
		}
	}
}

func start(t *testing.T, limits Limits) *Relay {
	t.Helper()
	r, err := NewRelay("127.0.0.1:0", limits)
	if err != nil {
		t.Fatal(err)
	}
	go r.Serve()
	t.Cleanup(func() { r.Close() })
	return r
}

func TestRoomForwarding(t *testing.T) {
	r := start(t, DefaultLimits)
	a, b, c := dial(t, r, 0x111111), dial(t, r, 0x222222), dial(t, r, 0x333333)
	a.join("1234")
	if p := a.recv(typeRoster, time.Second); p == nil || p[headerLen] != 0 {
		t.Fatalf("first member's roster: %v", p)
	}
	b.join("1234")
	p := b.recv(typeRoster, time.Second)
	if p == nil || p[headerLen] != 1 || binary.LittleEndian.Uint32(p[headerLen+1:]) != a.id {
		t.Fatalf("second member's roster should list a: %v", p)
	}
	c.join("9999") // another room

	a.relay(broadcastID, "beacon")
	got := b.recv(typeData, time.Second)
	if got == nil || binary.LittleEndian.Uint32(got[8:]) != a.id || string(got[headerLen:]) != "beacon" {
		t.Fatalf("b should get a's broadcast with a's id: %q", got)
	}
	if c.recv(typeData, 200*time.Millisecond) != nil {
		t.Fatal("a datagram crossed rooms")
	}
	b.relay(a.id, "unicast")
	if got := a.recv(typeData, time.Second); got == nil || string(got[headerLen:]) != "unicast" {
		t.Fatalf("a should get b's unicast: %q", got)
	}
}

func TestSpoofedIDDropped(t *testing.T) {
	r := start(t, DefaultLimits)
	a, b := dial(t, r, 0x111111), dial(t, r, 0x222222)
	a.join("pin")
	a.recv(typeRoster, time.Second)
	b.join("pin")
	b.recv(typeRoster, time.Second)
	// b claims to be a: refused, since a's id belongs to another address.
	p := binary.LittleEndian.AppendUint32(header(typeRelay, a.id), broadcastID)
	b.send(append(p, "spoof"...))
	if got := a.recv(typeData, 200*time.Millisecond); got != nil {
		t.Fatalf("spoofed datagram forwarded: %q", got)
	}
}

func TestRateLimit(t *testing.T) {
	r := start(t, Limits{PacketsPerSec: 10, BytesPerSec: 1e6, Burst: 1})
	a, b := dial(t, r, 0x111111), dial(t, r, 0x222222)
	a.join("rl")
	b.join("rl")
	b.recv(typeRoster, time.Second)
	for range 100 {
		a.relay(b.id, "x")
	}
	n := 0
	for b.recv(typeData, 200*time.Millisecond) != nil {
		n++
	}
	if n == 0 || n > 15 {
		t.Fatalf("expected the burst (about 10) through, got %d", n)
	}
}
