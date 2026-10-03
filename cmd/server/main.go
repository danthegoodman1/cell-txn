// Command server serves cell-tnx over the MySQL protocol.
//
//	server -addr 127.0.0.1:3307 -mode cell+delta -dir ./data [-pprof 127.0.0.1:6060]
package main

import (
	"flag"
	"fmt"
	"net/http"
	_ "net/http/pprof"
	"os"
	"os/signal"
	"runtime"
	"runtime/debug"

	"cell-tnx/sqlgms"
	"cell-tnx/txn"
)

func main() {
	if os.Getenv("GOGC") == "" {
		// The heap is many small long-lived objects; collect less often.
		debug.SetGCPercent(400)
	}
	addr := flag.String("addr", "127.0.0.1:3307", "listen address")
	mode := flag.String("mode", "cell+delta", "row, cell or cell+delta")
	bucketBits := flag.Uint("bucketbits", 4, "low key bits each bucket covers")
	dir := flag.String("dir", "", "data directory; empty keeps everything in memory")
	nosync := flag.Bool("nosync", false, "acknowledge commits before they sync")
	pprofAddr := flag.String("pprof", "", "serve net/http/pprof on this address, with mutex sampling, e.g. 127.0.0.1:6060")
	flag.Parse()
	if *pprofAddr != "" {
		runtime.SetMutexProfileFraction(10)
		go func() {
			if err := http.ListenAndServe(*pprofAddr, nil); err != nil {
				fmt.Fprintln(os.Stderr, "pprof:", err)
			}
		}()
	}

	m, ok := map[string]txn.Mode{"row": txn.Row, "cell": txn.Cell, "cell+delta": txn.CellDelta}[*mode]
	if !ok {
		fmt.Fprintln(os.Stderr, "unknown mode", *mode)
		os.Exit(2)
	}
	s, err := sqlgms.Serve(sqlgms.ServerConfig{Addr: *addr, Mode: m, BucketBits: *bucketBits, Dir: *dir, NoSync: *nosync})
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt)
	go func() {
		<-stop
		s.Close()
		os.Exit(0)
	}()
	fmt.Printf("cell-tnx listening on %s (mode %s, durable=%v)\n", *addr, *mode, *dir != "")
	if err := s.Start(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
