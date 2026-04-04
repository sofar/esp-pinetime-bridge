# PineTime Reminder Bridge

Manage reminders, appointments, and events on a PineTime smartwatch. A Go API server holds the schedule, an ESP32 bridge syncs it over BLE, and a custom InfiniTime firmware stores and fires reminders independently on the watch.

See [ARCHITECTURE.md](ARCHITECTURE.md) for design details, data models, and protocol documentation.

## Prerequisites

- **Go 1.22+**
- **ARM GCC toolchain** (`gcc-arm-none-eabi-10.3-2021.10`)
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

# Create a weekday-only 7:30 AM reminder (Mon-Fri = 0x3E)
curl -s -X POST http://localhost:8080/api/users/1/reminders \
  -H 'Content-Type: application/json' \
  -d '{"reminder_id":1,"hours":7,"minutes":30,"recurrence":62,"priority":1,"message":"Bus time!","enabled":true}'

# Send a one-off notification
curl -s -X POST http://localhost:8080/api/users/1/notifications \
  -H 'Content-Type: application/json' \
  -d '{"message":"Lunch is ready!","priority":1}'
```

---

## InfiniTime Firmware

The custom firmware lives in the `InfiniTime/` submodule on the `reminder-service` branch.

### Building

```bash
cd InfiniTime

# Download nRF5 SDK if not already present
wget -q https://nsscprodmedia.blob.core.windows.net/prod/software-and-other-downloads/sdks/nrf5/binaries/nrf5sdk1530059ac345.zip -O /tmp/nrf5sdk.zip
unzip -q /tmp/nrf5sdk.zip -d .

# Configure (adjust toolchain path as needed)
cmake -S . -B build \
  -DARM_NONE_EABI_TOOLCHAIN_PATH=/usr/local \
  -DNRF5_SDK_PATH=nRF5_SDK_15.3.0_59ac345 \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_DFU=1

# Build
cmake --build build -j$(nproc) --target pinetime-mcuboot-app
```

### Generating the DFU zip

The CMake build does not always regenerate the DFU zip. Generate it manually:

```bash
cd build/src
adafruit-nrfutil dfu genpkg \
  --dev-type 0x0052 \
  --application pinetime-mcuboot-app-image-1.16.0.hex \
  pinetime-mcuboot-app-dfu-1.16.0.zip
```

### Flashing via DFU (over BLE through the bridge)

1. Copy the DFU `.zip` contents (`.bin` and `.dat`) to `server/firmware/`
2. Start the API server
3. Upload firmware via the web UI or API:
   ```bash
   curl -X POST http://localhost:8080/api/firmware \
     -F "file=@build/src/pinetime-mcuboot-app-dfu-1.16.0.zip"
   ```
4. Trigger DFU from the web UI, or:
   ```bash
   curl -X POST http://localhost:8080/api/bridges/bridge-01/pairing \
     -H 'Content-Type: application/json' \
     -d '{"state":"dfu"}'
   ```
5. The bridge downloads the firmware, connects to the watch, and streams it. The watch reboots automatically when complete.

### Flashing via SWD (wired, for initial setup)

If you have an SWD debugger (J-Link, ST-Link, or BlackMagic Probe):

```bash
# Using OpenOCD
openocd -f interface/cmsis-dap.cfg -f target/nrf52.cfg \
  -c "program build/src/pinetime-mcuboot-app-image-1.16.0.bin 0x8000 verify reset exit"
```

---

## ESPHome Bridge

**Hardware:** Waveshare ESP32-S3-Touch-AMOLED-1.8

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

Set `watch_mac` to any valid MAC for initial setup — the actual watch address is configured through the pairing flow.

### Building and flashing

```bash
cd esphome

# First flash (USB serial)
esphome run pinetime-bridge.yaml

# Subsequent updates (OTA over WiFi)
esphome run pinetime-bridge.yaml --device pinetime-bridge.local
```

### Monitoring logs

```bash
esphome logs pinetime-bridge.yaml
# or
esphome logs pinetime-bridge.yaml --device pinetime-bridge.local
```

### Initial watch pairing

1. Make sure the API server is running and the bridge is connected to WiFi.
2. Open the bridge web UI at `http://pinetime-bridge.local/` or the API web UI at `http://<server>:8080/`.
3. The bridge continuously scans for InfiniTime watches and reports them to the server.
4. Select your watch from the discovered list in the web UI.
5. A 6-digit passkey will appear on the PineTime screen — confirm it in the web UI.
6. After bonding, all future connections are automatic.

---

## InfiniSim (Desktop Testing)

InfiniSim can test UI and scheduling logic but does **not** simulate BLE.

```bash
cd InfiniSim
cmake -DInfiniTime_DIR=../InfiniTime -S . -B build
cmake --build build -j$(nproc)
./build/infinisim
```

Note: InfiniSim's `NimbleController` stub must match InfiniTime's constructor signature (including `ReminderController&`).

---

## Operational Notes

- **Bridge reboots** are safe — the bridge restores its paired watch address from NVS and reconnects automatically. If a sync was interrupted, it retries on the next connection.
- **Watch reboots** (including after DFU) clear reminder storage. The bridge detects the hash mismatch on the next poll and re-syncs all reminders.
- **Poll interval** is configurable at runtime via the bridge web UI (default 60 seconds, range 10-3600).
- **Watch battery** — the bridge uses connect-on-demand to minimize BLE radio usage. The watch's BLE radio is idle most of the time.
- **DFU timing** — the bridge checks for DFU requests every 10 seconds. After triggering, firmware download + transfer takes 2-5 minutes depending on file size and BLE conditions.
