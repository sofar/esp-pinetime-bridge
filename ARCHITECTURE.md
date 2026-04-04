# PineTime Reminder Bridge

A system for managing reminders, appointments, and events on a PineTime smartwatch via an ESP32 bridge and a central API server. Reminders are stored on the watch and fire independently — no BLE connection required at alarm time.

## Architecture

```
┌─────────────────┐       HTTP/REST       ┌────────────────────┐
│  Go API Server  │◄────────────────────►│   ESP32 Bridge      │
│  (SQLite)       │  poll / ack / status  │   (ESPHome + IDF)   │
│  :8080          │                       │   WiFi + BLE        │
└─────────────────┘                       └─────────┬──────────┘
                                                    │ BLE GATT Client
                                                    │
                                          ┌─────────▼──────────┐
                                          │  PineTime Watch     │
                                          │  (Custom InfiniTime)│
                                          │  Stores & fires     │
                                          │  reminders locally  │
                                          └─────────────────────┘
```

- **1 bridge per watch** — each ESP32 is paired to one PineTime and one user account.
- The bridge periodically fetches the user's reminder schedule from the API and syncs it to the watch over BLE.
- The watch persists reminders to flash and fires them via FreeRTOS timers, even when the bridge is out of range.
- When the user acknowledges a reminder on the watch, the ack flows back through the bridge to the API.

---

## Component 1: Custom InfiniTime Firmware

**Location:** `~/git/InfiniTime/` (branch `reminder-service`)

### New files

| File | Purpose |
|------|---------|
| `src/components/reminder/ReminderController.h` | Reminder struct, storage, scheduling |
| `src/components/reminder/ReminderController.cpp` | Implementation |
| `src/components/ble/ReminderService.h` | BLE GATT service definition |
| `src/components/ble/ReminderService.cpp` | BLE characteristic handlers |

### Modified files

| File | Change |
|------|--------|
| `src/systemtask/Messages.h` | Added `SetOffReminder` message |
| `src/components/ble/NimbleController.h` | Added `ReminderService` member, `ReminderController&` constructor param, `reminder()` accessor |
| `src/components/ble/NimbleController.cpp` | Init service, pass `ReminderController` through constructor |
| `src/systemtask/SystemTask.h` | Added `ReminderController&` member |
| `src/systemtask/SystemTask.cpp` | Handle `SetOffReminder` (push notification + vibrate), init controller, reschedule on half-hour tick |
| `src/main.cpp` | Instantiate `ReminderController`, pass to `SystemTask` |
| `src/CMakeLists.txt` | Added new `.cpp` files to main and recovery builds |

### Reminder struct (72 bytes)

```cpp
struct Reminder {
  uint8_t version;      // format version (currently 2)
  uint8_t id;           // 0-19, assigned by bridge
  uint8_t hours;        // 0-23
  uint8_t minutes;      // 0-59
  uint8_t recurrence;   // see Recurrence below
  uint8_t flags;        // bit 0: enabled, bits 1-2: priority (0=low, 1=medium, 2=high)
  uint8_t month;        // 1-12 for specific date events, 0 = not used
  uint8_t day;          // 1-31 for specific date events, 0 = not used
  char message[64];     // null-terminated UTF-8
};
```

### Recurrence encoding

| Value | Meaning |
|-------|---------|
| `0x00` | Fire once at the next matching time, then disable |
| `0x80` | Daily |
| Bits 0-6 | Day-of-week bitmask: bit 0 = Sunday, bit 1 = Monday, ..., bit 6 = Saturday |

When `month` and `day` are non-zero with `recurrence = 0x00`, the reminder fires once on that specific calendar date.

**Examples:**

| recurrence | month | day | Meaning |
|-----------|-------|-----|---------|
| `0x00` | `0` | `0` | Once, next matching time |
| `0x00` | `6` | `15` | Once on June 15 |
| `0x80` | `0` | `0` | Every day |
| `0x02` | `0` | `0` | Every Monday |
| `0x3E` | `0` | `0` | Weekdays (Mon-Fri) |
| `0x41` | `0` | `0` | Weekends (Sun + Sat) |
| `0x04` | `0` | `0` | Every Tuesday (weekly) |

### Flags encoding

```
bit 0     = enabled (1 = active, 0 = disabled)
bits 1-2  = priority (0 = low, 1 = medium, 2 = high)
bits 3-7  = reserved
```

### Persistence

- Stored at `/.system/reminders.dat` on the watch's LittleFS flash.
- Binary format: 1 byte count + N × 72 bytes.
- Same pattern as InfiniTime's existing `AlarmController`.
- Max 20 reminders (1440 bytes).

### Alarm scheduling

- A single FreeRTOS one-shot timer targets the soonest enabled reminder.
- On fire: creates a `NotificationManager::Notification` with the reminder message and pushes `Messages::SetOffReminder` to SystemTask, which wakes the display and vibrates.
- One-shot reminders auto-disable after firing. Recurring reminders reschedule.
- `OnNewHalfHour` tick reschedules as a safety net (handles long timer overflow, time changes).

### BLE Reminder Service

**Service UUID:** `00060000-78fc-48fe-8e23-433b3a1942d0`

Follows InfiniTime's custom UUID scheme (`0006` prefix, base `xxxx-78fc-48fe-8e23-433b3a1942d0`).

| Characteristic | UUID suffix | Access | Payload |
|---------------|-------------|--------|---------|
| Upload Reminder | `0001` | Write | 72-byte `Reminder` struct |
| Delete Reminder | `0002` | Write | 1 byte: reminder ID (0-19) |
| List Reminders | `0003` | Read | 1 byte count + N × 72-byte structs |
| Ack Notify | `0004` | Notify | 5 bytes: 1 byte reminder ID + 4 bytes unix timestamp (LE) |
| Sync All | `0005` | Write | 1 byte count + N × 72-byte structs (clears existing, replaces) |

**Full UUIDs:**
- Upload: `00060001-78fc-48fe-8e23-433b3a1942d0`
- Delete: `00060002-78fc-48fe-8e23-433b3a1942d0`
- List: `00060003-78fc-48fe-8e23-433b3a1942d0`
- Ack: `00060004-78fc-48fe-8e23-433b3a1942d0`
- Sync: `00060005-78fc-48fe-8e23-433b3a1942d0`

---

## Component 2: ESP32 Bridge (ESPHome)

**Location:** `esphome/`

```
esphome/
├── pinetime-bridge.yaml          # Main ESPHome configuration
├── secrets.yaml                  # WiFi, watch MAC, API URL (git-ignored)
└── components/
    └── pinetime_bridge/
        ├── __init__.py           # ESPHome component registration
        ├── pinetime_bridge.h     # C++ component header
        └── pinetime_bridge.cpp   # C++ implementation
```

### Configuration

`pinetime-bridge.yaml` uses:
- **Framework:** ESP-IDF (not Arduino)
- **Built-in components:** `wifi`, `logger`, `ota`, `time` (SNTP), `esp32_ble_tracker`, `ble_client`
- **Custom component:** `pinetime_bridge`

### secrets.yaml

```yaml
wifi_ssid: "YourSSID"
wifi_password: "YourPassword"
watch_mac: "AA:BB:CC:DD:EE:FF"
api_base_url: "http://192.168.1.100:8080"
user_id: "1"
bridge_id: "bridge-01"
```

### Custom component behavior

The `PineTimeBridge` class inherits from `esphome::Component` and `esphome::ble_client::BLEClientNode`.

**Connect-on-demand (saves watch battery):**

The bridge does NOT maintain a persistent BLE connection. Instead:

1. **Poll API** every 60 seconds over WiFi (no BLE needed).
2. **Hash comparison** — computes a hash of the reminder data. If unchanged from last sync, no BLE connection is made.
3. **Connect only when needed** — if data changed or there are pending notifications, the bridge connects to the watch.
4. **On connect:** discovers services, syncs time via CTS, writes reminders, delivers pending notifications, collects acks.
5. **Disconnect after sync** — once the command queue is drained, the bridge disconnects. Typically connected for only a few seconds.
6. **Safety timeout** — if still connected after 30 seconds, force-disconnects (something stalled).
7. **Heartbeat** (`POST /api/bridges/{id}/status`) — reports status every 5 minutes over WiFi.

This means the watch's BLE radio is idle most of the time, preserving battery.

**On ack notification from watch:**
- Receives 5-byte packet (reminder ID + timestamp) from char `0004` during the brief connection window.
- Forwards to API via `POST /api/users/{id}/acks`.

**BLE command queue:**
- GATT writes are asynchronous. Commands are queued and sent one at a time, advancing on write confirmation.

### Pairing

InfiniTime uses **passkey display** pairing:
1. Bridge initiates connection → PineTime displays a 6-digit code.
2. One-time manual entry required during initial setup.
3. After bonding, keys are stored in ESP32 NVS — all subsequent connections are automatic.
4. InfiniTime supports re-pairing without manual bond deletion.

### Future: OTA firmware updates

The bridge can push firmware to the PineTime using the **Nordic Legacy DFU** protocol (service `00001530-1212-efde-1523-785feabcd123`). This streams `.bin` + `.dat` files in 20-byte BLE chunks. Not yet implemented.

---

## Component 3: Go API Server

**Location:** `server/`

```
server/
├── main.go                       # Entry point, starts HTTP server
├── go.mod / go.sum               # Go module (modernc.org/sqlite)
├── handlers/
│   └── handlers.go               # All HTTP handlers
├── models/
│   └── models.go                 # Data structures
├── store/
│   └── sqlite.go                 # SQLite data access + embedded schema
└── migrations/
    └── 001_initial.sql           # Reference schema (embedded in store)
```

### Running

```bash
cd server
go run . -addr :8080 -db pinetime-bridge.db
```

### Dependencies

- **Go 1.22+** (uses `http.ServeMux` method routing)
- **`modernc.org/sqlite`** — pure Go SQLite, no CGO required

### Data models

**User** — a person with a watch and bridge assignment.

| Field | Type | Description |
|-------|------|-------------|
| `id` | int64 | Auto-increment PK |
| `name` | string | Display name |
| `watch_mac` | string | PineTime BLE MAC address |
| `bridge_id` | string | Assigned ESP32 bridge identifier |

**Reminder** — a scheduled event on the watch.

| Field | Type | Description |
|-------|------|-------------|
| `id` | int64 | Database PK |
| `user_id` | int64 | FK to users |
| `reminder_id` | uint8 | Watch-side ID (0-19) |
| `hours` | uint8 | 0-23 |
| `minutes` | uint8 | 0-59 |
| `recurrence` | uint8 | `0x00`=once, `0x80`=daily, bits 0-6=weekday bitmask |
| `priority` | uint8 | 0=low, 1=medium, 2=high |
| `month` | uint8 | 1-12 for specific date, 0=ignore |
| `day` | uint8 | 1-31 for specific date, 0=ignore |
| `message` | string | Max 63 characters |
| `enabled` | bool | Active flag |

**ReminderAck** — acknowledgment log entry.

| Field | Type | Description |
|-------|------|-------------|
| `user_id` | int64 | FK to users |
| `reminder_id` | uint8 | Which reminder was acknowledged |
| `acked_at` | time | When the user tapped the watch |
| `received_at` | time | When the server received the ack |

**BridgeStatus** — heartbeat from the ESP32.

| Field | Type | Description |
|-------|------|-------------|
| `bridge_id` | string | Bridge identifier |
| `connected` | bool | Currently connected to watch |
| `watch_battery` | uint8 | Watch battery percentage |
| `last_heartbeat` | time | Last heartbeat timestamp |

**Notification** — one-off messages pushed to the watch (not stored on watch, displayed once via ANS).

| Field | Type | Description |
|-------|------|-------------|
| `user_id` | int64 | FK to users |
| `message` | string | Notification text |
| `priority` | uint8 | 0-2 |
| `pending` | bool | Bridge polls for pending=true, marks delivered |

### REST API

#### Users

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/users` | List all users |
| `POST` | `/api/users` | Create user |
| `GET` | `/api/users/{id}` | Get user |
| `PUT` | `/api/users/{id}` | Update user |
| `DELETE` | `/api/users/{id}` | Delete user |

#### Reminders

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/users/{id}/reminders` | List reminders (bridge polls this) |
| `POST` | `/api/users/{id}/reminders` | Create/upsert reminder |
| `PUT` | `/api/users/{id}/reminders/{rid}` | Update reminder |
| `DELETE` | `/api/users/{id}/reminders/{rid}` | Delete reminder |

#### Acknowledgments

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/users/{id}/acks` | Log acknowledgment (bridge posts) |
| `GET` | `/api/users/{id}/acks` | List ack history (last 100) |

#### Notifications

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/users/{id}/notifications` | Queue a one-off notification |
| `GET` | `/api/users/{id}/notifications/pending` | Bridge polls for undelivered |
| `PUT` | `/api/notifications/{id}/delivered` | Mark as delivered |

#### Bridge status

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/api/bridges/{id}/status` | Heartbeat (bridge posts) |
| `GET` | `/api/bridges/{id}/status` | Get bridge status |

### Example: create a user and a reminder

```bash
# Create a user
curl -X POST http://localhost:8080/api/users \
  -H 'Content-Type: application/json' \
  -d '{"name":"Alice","watch_mac":"AA:BB:CC:DD:EE:FF","bridge_id":"bridge-01"}'

# Create a daily 9:00 AM medication reminder
curl -X POST http://localhost:8080/api/users/1/reminders \
  -H 'Content-Type: application/json' \
  -d '{"reminder_id":0,"hours":9,"minutes":0,"recurrence":128,"priority":2,"message":"Take morning medication","enabled":true}'

# Create a specific-date appointment on March 15
curl -X POST http://localhost:8080/api/users/1/reminders \
  -H 'Content-Type: application/json' \
  -d '{"reminder_id":1,"hours":14,"minutes":30,"recurrence":0,"priority":1,"month":3,"day":15,"message":"Doctor appointment","enabled":true}'

# Create a weekday (Mon-Fri) 8:00 AM reminder
curl -X POST http://localhost:8080/api/users/1/reminders \
  -H 'Content-Type: application/json' \
  -d '{"reminder_id":2,"hours":8,"minutes":0,"recurrence":62,"priority":0,"message":"Morning check-in","enabled":true}'

# Send a one-off notification
curl -X POST http://localhost:8080/api/users/1/notifications \
  -H 'Content-Type: application/json' \
  -d '{"message":"Lunch is ready!","priority":1}'
```

---

## Data flow

### Reminder sync (API → watch)

```
1. Caregiver creates/updates reminder via API
2. Bridge polls GET /api/users/{id}/reminders (every 60s)
3. Bridge writes Sync All characteristic (0005) to watch
4. Watch clears existing reminders, stores new set to flash
5. Watch schedules FreeRTOS timer for soonest enabled reminder
```

### Reminder firing (watch standalone)

```
1. FreeRTOS timer fires at scheduled time
2. ReminderController pushes SetOffReminder message to SystemTask
3. SystemTask creates notification with reminder message
4. Watch wakes display, shows notification, vibrates
5. One-shot reminders disable themselves; recurring ones reschedule
```

### Acknowledgment (watch → API)

```
1. User dismisses notification on watch
2. ReminderController records pending ack (reminder ID + timestamp)
3. ReminderService sends 5-byte notify on characteristic 0004
4. Bridge receives BLE notification
5. Bridge posts to POST /api/users/{id}/acks
```

### One-off notifications (API → watch)

```
1. Notification created via POST /api/users/{id}/notifications
2. Bridge polls GET /api/users/{id}/notifications/pending
3. Bridge writes to ANS characteristic (0x2A46) on watch
4. Watch displays notification and vibrates
5. Bridge marks notification delivered via PUT /api/notifications/{id}/delivered
```

---

## Building

### InfiniTime firmware

Requires the nRF52 ARM toolchain. Build from `~/git/InfiniTime/`:

```bash
cmake -DARM_NONE_EABI_TOOLCHAIN_PATH=/path/to/toolchain \
      -DNRF5_SDK_PATH=nRF5_SDK_15.3.0_59ac345 \
      -S . -B build
cmake --build build -j$(nproc) --target pinetime-app
```

### InfiniSim (desktop testing)

The InfiniSim simulator at `~/git/InfiniSim/` can test UI and scheduling logic but **does not simulate BLE**. The sim's `NimbleController.cpp` needs updating to match the new constructor signature (add `ReminderController&` parameter).

```bash
cmake -DInfiniTime_DIR=~/git/InfiniTime -S ~/git/InfiniSim -B ~/git/InfiniSim/build
cmake --build ~/git/InfiniSim/build -j$(nproc)
./~/git/InfiniSim/build/infinisim
```

### Go API server

```bash
cd server
go build -o pinetime-bridge-server .
./pinetime-bridge-server -addr :8080 -db pinetime-bridge.db
```

### ESPHome bridge

```bash
cd esphome
# Edit secrets.yaml with your WiFi, watch MAC, and API URL
esphome run pinetime-bridge.yaml
```

---

## Testing

| Component | How to test |
|-----------|-------------|
| Firmware BLE service | Use nRF Connect app to write/read characteristics manually |
| Firmware scheduling | Build with InfiniSim, observe notification firing in simulator |
| API server | `curl` against endpoints (see examples above) |
| Bridge | Flash to ESP32, monitor serial logs for BLE connection + HTTP polling |
| End-to-end | Create reminder in API → bridge syncs → watch fires → ack appears in API |
