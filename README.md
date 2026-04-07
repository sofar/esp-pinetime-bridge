# PineTime Reminder Bridge

Manage reminders, appointments, and events on a PineTime smartwatch. A Go API server holds the schedule, an ESP32 bridge syncs it over BLE, and a custom InfiniTime firmware stores and fires reminders independently on the watch.

See [ARCHITECTURE.md](ARCHITECTURE.md) for design details, data models, and protocol documentation.

## Prerequisites

- **Go 1.22+**
- **ARM GCC toolchain** (`arm-none-eabi-gcc` 12+)
- **nRF5 SDK 15.3.0** (`nRF5_SDK_15.3.0_59ac345`)
- **CMake 3.22+**
- **ESPHome 2025.10+**
- **Python 3** with `adafruit-nrfutil` (`pip install adafruit-nrfutil`)

## Clone

```bash
git clone <repo-url> esp-pinetime-bridge
cd esp-pinetime-bridge
git submodule update --init
```

---

## API Server

```bash
cd server
go build -o pinetime-bridge-server .
./pinetime-bridge-server -addr :8080 -db pinetime-bridge.db
```

The web UI is served at `http://localhost:8080/`. Use it to manage users, reminders, view logs, pair watches, and trigger firmware updates.

### Web UI tabs

- **Reminders** — create, edit, delete reminders with recurrence and priority
- **Notify** — send one-off push notifications to the watch
- **Logs** — real-time bridge and system logs (auto-refreshes every 10s)
- **Equipment** — watch status, battery chart (30 days), pairing, firmware updates (auto-refreshes every 30s)

### Quick test

```bash
# Create a user
curl -s -X POST http://localhost:8080/api/users \
  -H 'Content-Type: application/json' \
  -d '{"name":"Alice","watch_mac":"AA:BB:CC:DD:EE:FF","bridge_id":"bridge-01"}'

# Create a daily 9:00 AM reminder
curl -s -X POST http://localhost:8080/api/users/1/reminders \
  -H 'Content-Type: application/json' \
  -d '{"reminder_id":0,"hours":9,"minutes":0,"recurrence":128,"priority":2,"message":"Morning reminder","enabled":true}'
```

---

## InfiniTime Firmware

The custom firmware lives in the `InfiniTime/` submodule on the `sofar/reminders` branch.

**Changes from stock 1.16.0:**
- Removed Music, Navigation, Weather services (RAM savings for reminder system)
- Added BLE Reminder Service (5 characteristics) + ReminderController
- Added Reminders app in launcher (shows sync time, count, next reminder)
- Added quiet hours feature (cherry-picked from upstream PR #2420)
- High-priority reminders exit quiet hours automatically
- LFS file handles moved to BSS to prevent SystemTask stack overflow
- Reminder file save routed through DisplayApp (3200B stack) instead of SystemTask (1400B)
- Git short ref included in firmware version string

### Building

```bash
cd InfiniTime
cmake -S . -B build \
  -DARM_NONE_EABI_TOOLCHAIN_PATH=/usr \
  -DNRF5_SDK_PATH=/path/to/nRF5_SDK_15.3.0_59ac345 \
  -DBUILD_DFU=1
cmake --build build -j$(nproc) --target pinetime-mcuboot-app
```

**Important:** Use `pinetime-mcuboot-app` target (not `pinetime-app`) to generate the DFU zip. After git commits, reconfigure with `cmake -S . -B build ...` to update the git hash before rebuilding.

DFU zip: `build/src/pinetime-mcuboot-app-dfu-1.16.0.zip`

### Flashing via DFU (over BLE through the bridge)

1. Upload firmware via the web UI or API:
   ```bash
   curl -X POST http://localhost:8080/api/firmware \
     -F "file=@build/src/pinetime-mcuboot-app-dfu-1.16.0.zip"
   ```
2. Trigger DFU from the web UI Equipment tab, or:
   ```bash
   curl -X POST http://localhost:8080/api/bridges/bridge-01/pairing \
     -H 'Content-Type: application/json' -d '{"state":"dfu"}'
   ```
3. The bridge downloads firmware, streams to watch, watch reboots automatically.

---

## ESPHome Bridge

**Hardware:** [Waveshare ESP32-S3-Touch-AMOLED-1.8](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8) (ESP32-S3, 8MB PSRAM, 1.8" AMOLED 368x448, touch, ES8311 audio codec, speaker)

See [docs/hardware-pinout.md](docs/hardware-pinout.md) for GPIO assignments.

### Configuration

Create `esphome/secrets.yaml`:

```yaml
wifi_ssid: "YourSSID"
wifi_password: "YourPassword"
ota_password: "your-ota-password"
watch_mac: "AA:BB:CC:DD:EE:FF"
api_base_url: "http://192.168.1.100:8080"
user_id: "1"
bridge_id: "bridge-01"
```

### Building and flashing

```bash
cd esphome
esphome run pinetime-bridge.yaml                              # USB serial
esphome run pinetime-bridge.yaml --device pinetime-bridge.local  # OTA
```

### Display

- **Default:** Large clock + date
- **Swipe right:** Debug status page (reminders, watch, bridge info)
- **Swipe left:** Back to clock
- **Swipe up/down:** Volume control (persists across reboots)
- **Events:** Full-screen notifications with MDI icons and audio chimes

### Event notifications (visual + audio)

| Event | Icon | Sound |
|-------|------|-------|
| Reminder fired | bell-ring | Warning chime |
| Watch battery <60% | battery-low | Warning (repeats hourly 8am-8pm) |
| Reminders syncing | sync | Info ding |
| User returns | home | Welcome jingle |
| Server down/restored | server-off/network | Error/positive |
| DFU started/complete/failed | download/check/alert | Info/positive/error |
| Watch paired | bluetooth-connect | Positive |
| Reminder acknowledged | bell-check | Info ding |

### Audio

Speaker audio uses the raw ESP-IDF I2S driver (not ESPHome's speaker component) with the ES8311 codec. Tones are generated from RTTTL via `tools/rtttl2pcm.py` and compiled as C arrays.

```bash
python3 tools/rtttl2pcm.py   # Regenerate tones
```

---

## InfiniSim (Desktop Testing)

```bash
cmake -DInfiniTime_DIR=../InfiniTime -S InfiniSim -B InfiniSim/build
cmake --build InfiniSim/build -j$(nproc)
./InfiniSim/build/infinisim
```

Note: InfiniSim's CMakeLists and NimbleController stub must be updated to match firmware changes (removed services, added ReminderController).

---

## Operational Notes

- **Bridge reboots** are safe — paired address restored from NVS, reconnects automatically.
- **Watch reboots** clear reminder storage — bridge detects hash mismatch and re-syncs.
- **Post-DFU grace period** — bridge waits 2 minutes before syncing reminders to let firmware stabilize.
- **DFU safety** — bridge skips all DFU characteristic access when no DFU is active (prevents "firmware disabled" notification crash).
- **Quiet hours** — high-priority reminders (e.g., wake-up alarms) exit quiet hours and block re-entry until the configured end time.
- **Poll interval** configurable at runtime (default 60s, range 10-3600).
