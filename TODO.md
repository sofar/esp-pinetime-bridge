# TODO

## 1. Docker build system

Build both firmwares in official compiler containers — no local toolchain needed.

- [ ] Dockerfile for InfiniTime (ARM cross-compile with `arm-none-eabi-gcc`)
- [ ] Dockerfile for ESPHome bridge (`esphome/esphome` base image)
- [ ] Top-level `Makefile` with targets:
  - `make firmware` — build InfiniTime DFU zip
  - `make bridge` — build ESPHome bridge firmware
  - `make flash-bridge` — OTA flash bridge
  - `make dfu` — upload DFU zip to server and trigger watch update
- [ ] CI-friendly: clone + `make` on any Linux box

## 2. Web-integrated build and flash

Single web page to build, upload, and flash — no CLI after initial deploy.

- [ ] Server API endpoint to trigger InfiniTime build (shells out to Docker)
- [ ] Server API endpoint to trigger ESPHome build
- [ ] Stream build logs to web UI in real-time
- [ ] Auto-upload built firmware to server
- [ ] One-click DFU from build output
- [ ] One-click OTA bridge flash from build output

## 3. Increase reminder capacity

Target: 40 reminders with 64-character messages (currently 10 × 32).

- [ ] Test incrementally: 15, 20, 25, 30, 35, 40 reminders
- [ ] Each step: build, DFU, run settings crash test (sleep 3x, brightness 3x, flashlight)
- [ ] If RAM too tight at 40 × 72 bytes (2880 bytes), explore:
  - Remove more unused apps (Paint, Paddle, Twos, Metronome, Dice, Calculator)
  - Dynamic heap allocation instead of static array
  - Reduce other BSS consumers
- [ ] Update bridge `WatchReminder` struct to match new sizes
- [ ] Update `rtttl2pcm.py` tone count comment if MaxReminders changes
- [ ] Update ARCHITECTURE.md with final capacity
