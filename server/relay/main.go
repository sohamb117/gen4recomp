package main

import (
	"flag"
	"log"
)

func main() {
	listen := flag.String("listen", ":2020", "UDP address to listen on")
	pps := flag.Float64("pps", DefaultLimits.PacketsPerSec, "datagrams per second allowed per source address")
	bps := flag.Float64("bps", DefaultLimits.BytesPerSec, "bytes per second allowed per source address")
	flag.Parse()

	r, err := NewRelay(*listen, Limits{PacketsPerSec: *pps, BytesPerSec: *bps, Burst: DefaultLimits.Burst})
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("nativeplat relay listening on udp %s", r.Addr())
	log.Fatal(r.Serve())
}
