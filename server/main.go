package main

import (
	"embed"
	"flag"
	"io/fs"
	"log"
	"net/http"
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
	log.Printf("Starting server on %s", *addr)
	log.Fatal(server.ListenAndServe())
}
