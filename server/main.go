package main

import (
	"context"
	"embed"
	"flag"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/sofar/pinetime-bridge-server/handlers"
	"github.com/sofar/pinetime-bridge-server/store"
)

//go:embed web
var webFS embed.FS

func main() {
	addr := flag.String("addr", ":8080", "listen address")
	dbPath := flag.String("db", "pinetime-bridge.db", "SQLite database path")
	flag.Parse()

	db := store.Open(*dbPath)
	defer db.Close()
	db.Migrate()

	h := handlers.New(db, "firmware")
	h.ServerLog(0, "info", "Server started")
	mux := http.NewServeMux()
	h.Register(mux)

	// Serve embedded web UI
	webContent, _ := fs.Sub(webFS, "web")
	mux.Handle("GET /", http.FileServer(http.FS(webContent)))

	server := &http.Server{
		Addr:         *addr,
		Handler:      mux,
		ReadTimeout:  15 * time.Second,
		WriteTimeout: 30 * time.Second,
		IdleTimeout:  60 * time.Second,
	}
	// Graceful shutdown: log server stop on SIGINT/SIGTERM
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt, syscall.SIGTERM)
	go func() {
		<-stop
		h.ServerLog(0, "info", "Server shutting down")
		server.Shutdown(context.Background())
	}()

	log.Printf("Starting server on %s", *addr)
	if err := server.ListenAndServe(); err != http.ErrServerClosed {
		log.Fatal(err)
	}
}
