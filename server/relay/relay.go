// Package main is nativeplat's local-wireless relay: it lets DS stations that
// cannot reach each other directly (internet, NAT) share one "room" keyed by
// a PIN, and forwards their datagrams. It knows nothing about the games: the
// payloads are the ARM7 wireless model's own datagrams (games/platinum/pc/src/
// pc_wm.c), carried in shell/src/net.c's framing.
//
// Wire format (little endian), shared with shell/src/net.c:
//
//	header  u32 magic "NPN1" | u8 type | u8 version (1) | u16 port | u32 station id
//	JOIN    (type 3)  header | u8 pinLen | pin bytes            client -> relay, every second
//	ROSTER  (type 4)  header(id 0) | u8 count | count x u32 id   relay -> client, answer to JOIN
//	RDATA   (type 5)  header | u32 dst (0xFFFFFFFF = everyone) | payload   client -> relay
//	DATA    (type 0)  header(src id) | payload                   relay -> client
package main

import (
	"encoding/binary"
	"log"
	"net"
	"sync"
	"time"
)

const (
	magic        = 0x314E504E
	version      = 1
	headerLen    = 12
	typeData     = 0
	typeJoin     = 3
	typeRoster   = 4
	typeRelay    = 5
	broadcastID  = 0xFFFFFFFF
	maxPayload   = 1460
	maxPinLen    = 32
	maxRoom      = 16
	memberExpiry = 15 * time.Second
)

// Limits caps what one source address may send.
type Limits struct {
	PacketsPerSec float64 // sustained datagrams/s per source address
	BytesPerSec   float64 // sustained bytes/s per source address
	Burst         float64 // seconds of the sustained rate that may arrive at once
}

// DefaultLimits are generous for one DS (about 3 datagrams per frame).
var DefaultLimits = Limits{PacketsPerSec: 240, BytesPerSec: 200_000, Burst: 2}

type member struct {
	id   uint32
	addr *net.UDPAddr
	seen time.Time
}

type bucket struct {
	pkts, bytes float64
	last        time.Time
}

// Relay is one listening socket and its rooms.
type Relay struct {
	conn    *net.UDPConn
	limits  Limits
	mu      sync.Mutex
	rooms   map[string]map[uint32]*member
	byAddr  map[string]string // source address -> room PIN
	buckets map[string]*bucket
	now     func() time.Time
	Dropped uint64 // datagrams refused by the rate limit
}

// NewRelay listens on addr ("host:port" or ":port").
func NewRelay(addr string, limits Limits) (*Relay, error) {
	ua, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		return nil, err
	}
	conn, err := net.ListenUDP("udp", ua)
	if err != nil {
		return nil, err
	}
	return &Relay{
		conn:    conn,
		limits:  limits,
		rooms:   map[string]map[uint32]*member{},
		byAddr:  map[string]string{},
		buckets: map[string]*bucket{},
		now:     time.Now,
	}, nil
}

// Addr is the bound address.
func (r *Relay) Addr() *net.UDPAddr { return r.conn.LocalAddr().(*net.UDPAddr) }

// Close stops Serve.
func (r *Relay) Close() error { return r.conn.Close() }

// Serve handles datagrams until Close.
func (r *Relay) Serve() error {
	buf := make([]byte, 2048)
	for {
		n, from, err := r.conn.ReadFromUDP(buf)
		if err != nil {
			return err
		}
		r.handle(buf[:n], from)
	}
}

func (r *Relay) allow(key string, size int) bool {
	now := r.now()
	b := r.buckets[key]
	if b == nil {
		b = &bucket{pkts: r.limits.PacketsPerSec * r.limits.Burst, bytes: r.limits.BytesPerSec * r.limits.Burst, last: now}
		r.buckets[key] = b
	}
	dt := now.Sub(b.last).Seconds()
	b.last = now
	b.pkts = min(b.pkts+dt*r.limits.PacketsPerSec, r.limits.PacketsPerSec*r.limits.Burst)
	b.bytes = min(b.bytes+dt*r.limits.BytesPerSec, r.limits.BytesPerSec*r.limits.Burst)
	if b.pkts < 1 || b.bytes < float64(size) {
		r.Dropped++
		return false
	}
	b.pkts--
	b.bytes -= float64(size)
	return true
}

func header(typ byte, id uint32) []byte {
	h := make([]byte, headerLen, headerLen+maxPayload)
	binary.LittleEndian.PutUint32(h, magic)
	h[4] = typ
	h[5] = version
	binary.LittleEndian.PutUint32(h[8:], id)
	return h
}

func (r *Relay) handle(p []byte, from *net.UDPAddr) {
	if len(p) < headerLen || binary.LittleEndian.Uint32(p) != magic || p[5] != version {
		return
	}
	id := binary.LittleEndian.Uint32(p[8:]) & 0xFFFFFF
	if id == 0 {
		return
	}
	key := from.String()
	r.mu.Lock()
	defer r.mu.Unlock()
	if !r.allow(key, len(p)) {
		return
	}
	r.expire()
	switch p[4] {
	case typeJoin:
		if len(p) < headerLen+1 || int(p[headerLen]) > maxPinLen || len(p) < headerLen+1+int(p[headerLen]) {
			return
		}
		pin := string(p[headerLen+1 : headerLen+1+int(p[headerLen])])
		if pin == "" {
			return
		}
		if old, ok := r.byAddr[key]; ok && old != pin {
			r.leave(old, key)
		}
		room := r.rooms[pin]
		if room == nil {
			room = map[uint32]*member{}
			r.rooms[pin] = room
		}
		m := room[id]
		if m == nil {
			if len(room) >= maxRoom {
				return
			}
			m = &member{id: id}
			room[id] = m
			log.Printf("room %q: station %06x joined from %s (%d in room)", pin, id, key, len(room))
		}
		m.addr = from
		m.seen = r.now()
		r.byAddr[key] = pin
		r.sendRoster(room, m)
	case typeRelay:
		if len(p) < headerLen+4 || len(p)-headerLen-4 > maxPayload {
			return
		}
		pin, ok := r.byAddr[key]
		if !ok {
			return
		}
		room := r.rooms[pin]
		src := room[id]
		if src == nil || src.addr.String() != key {
			return // only a joined station may speak for its id
		}
		src.seen = r.now()
		dst := binary.LittleEndian.Uint32(p[headerLen:])
		out := append(header(typeData, id), p[headerLen+4:]...)
		for _, m := range room {
			if m.id == id || (dst != broadcastID && m.id != dst) {
				continue
			}
			_, _ = r.conn.WriteToUDP(out, m.addr)
		}
	}
}

func (r *Relay) sendRoster(room map[uint32]*member, to *member) {
	out := header(typeRoster, 0)
	out = append(out, 0)
	n := 0
	for id := range room {
		if id == to.id {
			continue
		}
		out = binary.LittleEndian.AppendUint32(out, id)
		n++
	}
	out[headerLen] = byte(n)
	_, _ = r.conn.WriteToUDP(out, to.addr)
}

func (r *Relay) leave(pin, key string) {
	for id, m := range r.rooms[pin] {
		if m.addr.String() == key {
			delete(r.rooms[pin], id)
		}
	}
	if len(r.rooms[pin]) == 0 {
		delete(r.rooms, pin)
	}
	delete(r.byAddr, key)
}

func (r *Relay) expire() {
	now := r.now()
	for pin, room := range r.rooms {
		for id, m := range room {
			if now.Sub(m.seen) > memberExpiry {
				log.Printf("room %q: station %06x left", pin, id)
				delete(room, id)
				delete(r.byAddr, m.addr.String())
			}
		}
		if len(room) == 0 {
			delete(r.rooms, pin)
		}
	}
	for key, b := range r.buckets {
		if now.Sub(b.last) > time.Minute {
			delete(r.buckets, key)
		}
	}
}
