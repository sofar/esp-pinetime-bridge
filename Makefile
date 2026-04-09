.PHONY: server

server:
	cd server && go build -o pinetime-bridge-server .
