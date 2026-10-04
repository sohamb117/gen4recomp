# Internet play through a relay

Local wireless (Union Room, link battles and trades, the Underground) normally
finds partners on the LAN. Over the internet, players behind NAT cannot reach
each other directly, so both connect to a small relay and pick the same room
PIN. The relay forwards the stations' datagrams within a room and knows
nothing about the games.

## Running the relay

```sh
cd server/relay
go test ./...
go build -o np-relay .
./np-relay -listen :2020          # UDP; open/forward this port on the server
```

Flags: `-listen` (default `:2020`), `-pps` and `-bps` (per-source rate limit,
default 240 datagrams/s and 200 kB/s, about three times what one linked DS
sends). Rooms hold up to 16 stations; a station that sends nothing for 15 s
leaves its room. The relay only forwards datagrams from the address that
joined with that station id.

## Using it

- App: Options -> Local wireless: relay `host:2020` and a room PIN (both
  players use the same PIN).
- Headless: `np_headless platinum rom.nds --net 2009 --net-relay host:2020
  --net-pin 4242 ...`

## Protocol

Shared by `server/relay/relay.go` and `shell/src/net.c` (little endian):

| Datagram | Layout |
|---|---|
| header | `u32 "NPN1"`, `u8 type`, `u8 version 1`, `u16 port`, `u32 station id` |
| JOIN (3) | header, `u8 pinLen`, PIN; every second, also the keepalive |
| ROSTER (4) | header (id 0), `u8 count`, `count x u32` other station ids |
| RDATA (5) | header, `u32 dst` (`0xFFFFFFFF` = everyone), payload; to the relay |
| DATA (0) | header with the sender's id, payload; from the relay |

The payloads are the ARM7 wireless model's datagrams
(`games/platinum/pc/src/pc_wm.c`), which resend until acknowledged, so the
relay never retries anything.
