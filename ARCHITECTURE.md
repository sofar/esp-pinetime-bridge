# Architecture

A system for managing reminders, appointments, and events on a PineTime smartwatch via an ESP32 bridge and a central API server. Reminders are stored on the watch and fire independently — no BLE connection required at alarm time.

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

## Project Layout

```
esp-pinetime-bridge/
├── InfiniTime/               # Git submodule — custom firmware (branch: reminder-service)
├── esphome/
│   ├── pinetime-bridge.yaml  # ESPHome configuration
│   ├── secrets.yaml          # WiFi, watch MAC, API URL (git-ignored)
│   └── components/
│       └── pinetime_bridge/
│           ├── __init__.py
│           ├── pinetime_bridge.h / .cpp
│           └── dfu_client.h / .cpp
└── server/
    ├── main.go               # Entry point, embedded web UI
    ├── go.mod / go.sum
    ├── handlers/handlers.go  # All HTTP handlers
    ├── models/models.go      # Data structures
    ├── store/sqlite.go       # SQLite data access + embedded schema
    ├── web/index.html        # Web management UI
    └── firmware/             # DFU firmware files (git-ignored)
```

---

## InfiniTime Firmware

**Submodule:** `InfiniTime/` (branch `reminder-service`)

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
| `src/components/ble/NimbleController.h/.cpp` | Added `ReminderService` member, `ReminderController&` param |
| `src/systemtask/SystemTask.h/.cpp` | Handle `SetOffReminder`, init controller, reschedule on half-hour |
| `src/displayapp/DisplayApp.h/.cpp` | Pass `ReminderController` through to screens |
| `src/displayapp/screens/SystemInfo.h/.cpp` | 6th page showing reminder count, enabled count, last sync |
| `src/main.cpp` | Instantiate `ReminderController`, pass to `SystemTask` and `DisplayApp` |
| `src/CMakeLists.txt` | Added new `.cpp` files to main and recovery builds |

### Reminder struct (72 bytes)

```cpp
struct Reminder {
  uint8_t version;      // format version (currently 2)
  uint8_t id;           // 0-55, assigned by bridge
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
- Max 56 reminder slots.

### Alarm scheduling

- A single FreeRTOS one-shot timer targets the soonest enabled reminder.
- On fire: creates a `NotificationManager::Notification` with the reminder message and pushes `Messages::SetOffReminder` to SystemTask, which wakes the display and vibrates.
- High-priority reminders bypass DND. One-shot reminders auto-disable after firing. Recurring reminders reschedule.
- `OnNewHalfHour` tick reschedules as a safety net (handles long timer overflow, time changes).

### BLE Reminder Service

**Service UUID:** `00060000-78fc-48fe-8e23-433b3a1942d0`

| Characteristic | UUID suffix | Access | Payload |
|---------------|-------------|--------|---------|
| Upload Reminder | `0001` | Write | 72-byte `Reminder` struct |
| Delete Reminder | `0002` | Write | 1 byte: reminder ID (0xFF = delete all) |
| List Reminders | `0003` | Read | 1 byte count + N × 72-byte structs |
| Ack Notify | `0004` | Notify | 5 bytes: 1 byte reminder ID + 4 bytes unix timestamp (LE) |
| Sync All | `0005` | Write | 1 byte count + N × 72-byte structs (clears existing, replaces) |

---

## ESP32 Bridge (ESPHome)

**Location:** `esphome/`

**Hardware:** Waveshare ESP32-S3-Touch-AMOLED-1.8 (ESP32-S3, 8MB PSRAM, AMOLED display, touch)

### Component behavior

The `PineTimeBridge` class inherits from `esphome::Component` and `esphome::ble_client::BLEClientNode`.

**Connect-on-demand (saves watch battery):**

1. **Poll API** every 60 seconds over WiFi (configurable, no BLE needed).
2. **Hash comparison** — computes a hash of the reminder data. If unchanged from last sync, no BLE connection is made.
3. **Connect only when needed** — if data changed or there are pending notifications, the bridge connects.
4. **On connect:** discovers services, syncs time via CTS, writes delete-all + individual reminders, delivers pending notifications, collects acks.
5. **Disconnect after sync** — once the command queue is drained, the bridge disconnects.
6. **Safety timeout** — if still connected after 30 seconds, force-disconnects.
7. **Disconnect recovery** — if disconnected mid-sync, the hash is reset and the queue is cleared so the next connection retries the full sync.
8. **Heartbeat** (`POST /api/bridges/{id}/status`) — reports status every 5 minutes over WiFi.
9. **Watch polling** — every 30 minutes, connects to read battery, steps, and sync time.

### Reminder sync strategy

The bridge uses individual writes (not bulk sync) to avoid MTU issues:
1. Write `0xFF` to the delete characteristic (clear all reminders on watch)
2. Write each reminder individually (72 bytes each) to the upload characteristic
3. Verify by reading the list characteristic after all writes complete

### Pairing

InfiniTime uses **passkey display** pairing:
1. Bridge initiates connection — PineTime displays a 6-digit code.
2. One-time manual entry via the web UI during initial setup.
3. After bonding, keys are stored in ESP32 NVS — all subsequent connections are automatic.

### DFU (firmware updates)

The bridge can push firmware to PineTime using **Nordic Legacy DFU** protocol:
1. Server stores firmware `.bin` + `.dat` files
2. Bridge downloads firmware into PSRAM
3. Streams to watch via DFU service (`00001530-1212-efde-1523-785feabcd123`)
4. Watch reboots with new firmware
5. Triggered by setting bridge pairing state to `"dfu"` on the server

### AMOLED display

The bridge has an LVGL-based status display showing:
- Next upcoming reminder (time + message)
- Watch status (battery, firmware version, BLE connection)
- Bridge status (WiFi signal, server reachability, reminder count)
- Clock with auto-dim after 30 seconds of no touch

---

## Go API Server

**Location:** `server/`

### Dependencies

- **Go 1.22+** (uses `http.ServeMux` method routing)
- **`modernc.org/sqlite`** — pure Go SQLite, no CGO required

### Data models

| Model | Description |
|-------|-------------|
| **User** | Person with a watch and bridge assignment (name, watch MAC, bridge ID) |
| **Reminder** | Scheduled event: time, recurrence, priority, message, enabled flag |
| **ReminderAck** | Acknowledgment log: which reminder, when acked on watch, when received |
| **BridgeStatus** | Heartbeat: connection state, watch battery, IP address, firmware versions |
| **Notification** | One-off push message (not stored on watch, displayed once via ANS) |

### REST API

| Method | Path | Description |
|--------|------|-------------|
| `GET/POST` | `/api/users` | List / create users |
| `GET/PUT/DELETE` | `/api/users/{id}` | User CRUD |
| `GET/POST` | `/api/users/{id}/reminders` | List / create reminders |
| `PUT/DELETE` | `/api/users/{id}/reminders/{rid}` | Reminder CRUD |
| `POST/GET` | `/api/users/{id}/acks` | Log / list acknowledgments |
| `POST` | `/api/users/{id}/notifications` | Queue one-off notification |
| `GET` | `/api/users/{id}/notifications/pending` | Bridge polls for undelivered |
| `PUT` | `/api/notifications/{id}/delivered` | Mark as delivered |
| `POST/GET` | `/api/bridges/{id}/status` | Heartbeat / get status |
| `POST/GET` | `/api/bridges/{id}/pairing` | Pairing state management |
| `POST` | `/api/bridges/{id}/discovered` | Post discovered watch list |
| `GET/POST` | `/api/users/{id}/logs` | Bridge and system logs |
| `POST/GET` | `/api/firmware` | Upload / list firmware files |

### Web UI

Embedded at `/` — provides reminder management, calendar view, equipment status, bridge logs, pairing, and DFU trigger controls.

---

## Data Flows

### Reminder sync (API → watch)

```
1. User creates/updates reminder via web UI or API
2. Bridge polls GET /api/users/{id}/reminders (every 60s)
3. Hash comparison detects change → bridge connects to watch
4. Bridge writes delete-all (0xFF) then each reminder individually
5. Watch stores reminders to flash, schedules FreeRTOS timer
6. Bridge disconnects
```

### Reminder firing (watch standalone)

```
1. FreeRTOS timer fires at scheduled time
2. ReminderController pushes SetOffReminder to SystemTask
3. SystemTask creates notification with reminder message
4. Watch wakes display, shows notification, vibrates
5. One-shot reminders disable themselves; recurring ones reschedule
```

### Acknowledgment (watch → API)

```
1. User dismisses notification on watch
2. ReminderService sends 5-byte notify on characteristic 0004
3. Bridge receives notification during next BLE connection
4. Bridge posts to POST /api/users/{id}/acks
```

### One-off notifications (API → watch)

```
1. Notification created via POST /api/users/{id}/notifications
2. Bridge polls GET /api/users/{id}/notifications/pending
3. Bridge writes to ANS characteristic (0x2A46) on watch
4. Watch displays notification and vibrates
5. Bridge marks delivered via PUT /api/notifications/{id}/delivered
```

### DFU (firmware update via bridge)

```
1. Upload firmware files to server via POST /api/firmware
2. Set bridge pairing state to "dfu" via POST /api/bridges/{id}/pairing
3. Bridge downloads firmware into PSRAM, clears DFU state on server
4. Bridge connects to watch, streams firmware via Nordic Legacy DFU
5. Watch validates and reboots with new firmware
6. Bridge reconnects, syncs time and reminders to freshly booted watch
```
