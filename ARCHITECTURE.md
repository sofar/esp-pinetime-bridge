# Architecture

A system for managing reminders, appointments, and events on a PineTime smartwatch via an ESP32 bridge and a central API server. Reminders are stored on the watch and fire independently — no BLE connection required at alarm time.

```
┌─────────────────┐       HTTP/REST       ┌────────────────────────┐
│  Go API Server  │◄────────────────────►│   ESP32 Bridge          │
│  (SQLite)       │  poll / ack / status  │   (ESPHome + ESP-IDF)   │
│  :8080          │                       │   WiFi + BLE + Display  │
│                 │                       │   + Speaker (ES8311)    │
└─────────────────┘                       └──────────┬─────────────┘
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
- The bridge has a display + speaker for local event notifications (no phone needed).

---

## Project Layout

```
esp-pinetime-bridge/
├── InfiniTime/                       # Git submodule — custom firmware (branch: sofar/reminders)
├── InfiniSim/                        # InfiniTime simulator (for desktop testing)
├── docs/
│   ├── hardware-pinout.md            # Waveshare board GPIO reference
│   └── ESP32-S3-Touch-AMOLED-1.8-schematic.pdf
├── esphome/
│   ├── pinetime-bridge.yaml          # ESPHome configuration
│   ├── secrets.yaml                  # WiFi, watch MAC, API URL (git-ignored)
│   ├── fonts/
│   │   ├── materialdesignicons-webfont.ttf  # MDI icons (Apache 2.0)
│   │   └── README.md                 # Font license and glyph table
│   └── components/
│       └── pinetime_bridge/
│           ├── __init__.py
│           ├── pinetime_bridge.h / .cpp  # Main bridge component
│           ├── dfu_client.h / .cpp       # Nordic DFU protocol
│           ├── bridge_speaker.h          # Raw I2S speaker driver
│           └── tones/                    # Generated PCM audio
│               ├── event_tones.h         # Tone-to-event mapping
│               ├── tone_positive.h       # Ascending chime
│               ├── tone_warning.h        # Descending warning
│               ├── tone_error.h          # Triple beep
│               ├── tone_info.h           # Single ding
│               └── tone_welcome.h        # Welcome jingle
├── server/
│   ├── main.go                       # Entry point, embedded web UI
│   ├── go.mod / go.sum
│   ├── handlers/handlers.go          # All HTTP handlers
│   ├── models/models.go              # Data structures
│   ├── store/sqlite.go               # SQLite data access + embedded schema
│   ├── web/index.html                # Web management UI
│   └── firmware/                     # DFU firmware files (git-ignored)
└── tools/
    └── rtttl2pcm.py                  # RTTTL → PCM tone generator
```

---

## InfiniTime Firmware

**Submodule:** `InfiniTime/` (branch `sofar/reminders`)
**Base:** Stock InfiniTime 1.16.0

### Changes from stock

| Change | Purpose |
|--------|---------|
| Remove MusicService | RAM savings (~1424 bytes BSS) |
| Remove NavigationService | RAM savings (~224 bytes BSS) |
| Remove SimpleWeatherService + weather UI | RAM savings (~280 bytes BSS) |
| Add ReminderController + ReminderService | Core reminder system |
| Add Reminders app in launcher | View sync status, count, next reminder |
| Cherry-pick quiet hours (PR #2420) | Scheduled notification silencing |
| High-priority reminders exit quiet hours | Wake-up alarms end the night |
| Git short ref in version string | Identify builds on watch |
| Fix LFS debug macro | Build with GCC 15+ |

### Reminder struct (72 bytes)

```cpp
struct Reminder {
  uint8_t version;      // format version (currently 3)
  uint8_t id;           // 0-9, assigned by bridge
  uint8_t hours;        // 0-23
  uint8_t minutes;      // 0-59
  uint8_t recurrence;   // see Recurrence below
  uint8_t flags;        // bit 0: enabled, bits 1-2: priority (0-2)
  uint8_t month;        // 1-12 for specific date, 0 = not used
  uint8_t day;          // 1-31 for specific date, 0 = not used
  char message[64];     // null-terminated UTF-8
};
// Max 10 reminders, 64-char messages, heap-allocated (std::vector<Reminder>)
```

### Recurrence encoding

| Value | Meaning |
|-------|---------|
| `0x00` | Fire once, then disable |
| `0x80` | Daily |
| Bits 0-6 | Day-of-week bitmask: bit 0 = Sunday ... bit 6 = Saturday |

### RAM budget

Stock InfiniTime 1.16.0 BSS: ~24,284 bytes. After changes: ~22,900 bytes.
FreeRTOS heap (64KB - BSS): ~41KB available.
Reminders use heap allocation (`std::vector<Reminder>`) — only active reminders consume RAM.

Key constraints:
- **SystemTask stack: 350 words (1400 bytes)** — too small for LittleFS write operations
- **DisplayApp stack: 800 words (3200 bytes)** — sufficient for LFS writes
- **Solution:** Reminder file saves are forwarded from SystemTask to DisplayApp via message

### Stack overflow mitigations

| Problem | Solution |
|---------|----------|
| `SaveSettingsToFile` overflows SystemTask stack | Route save through DisplayApp (3200B stack) |
| `SetOffReminder` handler inflates Work() frame | Extracted to separate `HandleSetOffReminder()` function |
| `lfs_file_t` / `lfs_dir` on stack | Moved to ReminderController member variables (BSS) |

### BLE Reminder Service

**Service UUID:** `00060000-78fc-48fe-8e23-433b3a1942d0`

| Characteristic | UUID suffix | Access | Payload |
|---------------|-------------|--------|---------|
| Upload Reminder | `0001` | Write | 72-byte `Reminder` struct |
| Delete Reminder | `0002` | Write | 1 byte: reminder ID (0xFF = delete all) |
| List Reminders | `0003` | Read | 1 byte count + N × 72-byte structs |
| Ack Notify | `0004` | Notify | 5 bytes: 1 byte ID + 4 bytes timestamp (LE) |
| Sync All | `0005` | Write | 1 byte count + N × 72-byte structs |
| Status | `0006` | Read | 4 bytes: uptime in seconds (LE) |

### Quiet hours interaction

- Low/medium priority reminders are silenced during quiet hours
- High priority (2) reminders call `OverrideQuietHours()` which:
  1. Exits quiet hours (restores previous notification state)
  2. Sets `quietHoursOverridden` flag blocking re-entry
  3. Flag clears at the configured quiet hours end time
- Prevents race condition with `OnNewTime` / `OnNewHalfHour` re-entering quiet hours

---

## ESP32 Bridge (ESPHome)

**Location:** `esphome/`

**Hardware:** Waveshare ESP32-S3-Touch-AMOLED-1.8

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3R8, 240MHz, 8MB PSRAM |
| Display | 1.8" AMOLED 368×448 (SH8601 QSPI) |
| Touch | FT3168 (I2C) |
| Audio | ES8311 codec + speaker + PA (GPIO46) |
| Power | AXP2101 PMIC |

### Display pages

| Page | Content | Navigation |
|------|---------|------------|
| Clock (default) | Large time (72px) + date | Swipe right → Debug |
| Debug | Reminders, watch status, bridge info | Swipe left → Clock |
| Event | Full-screen icon + text + sound (30s auto-dismiss) | Auto → Clock |

**Volume:** Swipe up/down on any page. Persists across reboots via NVS.

### Event notification system

11 events with visual display (MDI icons at 120px) and audio chimes:

| # | Event | Trigger | Icon | Tone | Color |
|---|-------|---------|------|------|-------|
| 1 | Reminder fired | Watch ack received | bell-ring | Warning | Amber |
| 2 | Battery <60% | Threshold cross | battery-low | Warning | Orange |
| 3 | Reminders syncing | Count changed | sync | Info | Blue |
| 4 | User returns | Watch seen after absence | home | Welcome | Green |
| 9 | Server unreachable | Transition | server-off | Error | Red |
| 10 | Server restored | Transition | server-network | Positive | Green |
| 11 | DFU started | State change | download | Info | Blue |
| 12 | DFU complete | State change | check-circle | Positive | Green |
| 13 | DFU failed | State change | alert-circle | Error | Red |
| 14 | Watch paired | BLE connected | bluetooth-connect | Positive | Green |
| 15 | Reminder ack'd | Ack notification | bell-check | Info | Green |

Battery warning repeats hourly between 8am-8pm while below 60%.

### Audio pipeline

```
RTTTL strings (tools/rtttl2pcm.py)
  → 16kHz 16-bit mono PCM .h C arrays
  → bridge_speaker.h: mono→stereo conversion + software volume scaling
  → Raw ESP-IDF I2S driver (MCLK×384, Philips, stereo)
  → ES8311 codec → PA (GPIO46) → Speaker
```

ESPHome's built-in `i2s_audio` speaker component does not work correctly with the ES8311 on this board (wrong MCLK multiple). The raw driver (`bridge_speaker.h`) uses `I2S_MCLK_MULTIPLE_384` matching the Waveshare reference implementation.

### Persistent BLE connection

1. Bridge maintains a persistent BLE connection to the watch (auto-reconnect with 5s backoff)
2. Poll API every 60s over WiFi
3. Hash comparison — skip BLE sync if data unchanged
4. When data changes: delete-all → upload reminders → acks (over existing connection)
5. Periodic watch polling: battery, steps, uptime, time sync
6. Post-DFU: 2-minute grace period before syncing reminders
7. DFU characteristics only discovered when DFU is active
8. Safety timeout: 5-minute DFU timeout prevents stuck state

---

## Go API Server

**Location:** `server/`

### Dependencies

- **Go 1.22+** (uses `http.ServeMux` method routing, `http.Server` with timeouts)
- **`modernc.org/sqlite`** — pure Go SQLite, no CGO required

### Database tables

| Table | Purpose |
|-------|---------|
| `users` | User accounts with watch MAC and bridge ID |
| `reminders` | Scheduled events per user |
| `reminder_acks` | Acknowledgment log |
| `bridge_status` | Latest heartbeat (battery, firmware, connection) |
| `battery_history` | Battery % over time (for chart) |
| `notifications` | One-off push messages |
| `logs` | Bridge and system log entries |

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
| `GET` | `/api/bridges/{id}/battery-history` | Battery chart data (30 days) |
| `POST/GET` | `/api/bridges/{id}/pairing` | Pairing state management |
| `POST` | `/api/bridges/{id}/discovered` | Post discovered watch list |
| `GET/POST` | `/api/users/{id}/logs` | Bridge and system logs |
| `POST/GET` | `/api/firmware` | Upload / list firmware files |

### Web UI

Embedded at `/` with four tabs:

- **Reminders** — CRUD with recurrence picker, priority, enable/disable
- **Notify** — send one-off push notifications
- **Logs** — real-time bridge logs (auto-refreshes every 10s)
- **Equipment** — watch info, battery SVG chart (30 days), MAC config, discovered watches, pairing flow, firmware upload/DFU trigger (auto-refreshes every 30s)

---

## Data Flows

### Reminder sync (API → watch)

```
1. User creates/updates reminder via web UI or API
2. Bridge polls GET /api/users/{id}/reminders (every 60s)
3. Hash comparison detects change → bridge connects to watch
4. Bridge writes delete-all (0xFF) then each reminder individually
5. Watch defers save (2s debounce timer) → SystemTask schedules timer
6. SystemTask forwards save to DisplayApp (larger stack for LFS)
7. DisplayApp writes to /.system/reminders.dat
8. Bridge disconnects
```

### Reminder firing (watch standalone)

```
1. FreeRTOS timer fires at scheduled time
2. ReminderController::SetOffReminderNow() → pushes SetOffReminder to SystemTask
3. HandleSetOffReminder():
   a. If high priority + quiet hours active → OverrideQuietHours()
   b. If DND active + low/medium priority → silently skip
   c. Creates notification with reminder message
   d. Wakes display, shows notification, vibrates
4. One-shot reminders disable themselves; recurring ones reschedule
```

### Event notification (bridge local)

```
1. Bridge detects state change (server down, watch connected, battery low, etc.)
2. show_event script: updates event page labels, switches LVGL page, wakes display
3. bridge_speaker::play_event_tone(): spawns task, converts mono→stereo, writes I2S
4. Display auto-dismisses after 30 seconds back to clock page
```

### Acknowledgment (watch → API)

```
1. User dismisses notification on watch → StopAlerting()
2. ReminderController records pending ack (ID + timestamp)
3. On next BLE connection, ReminderService sends 5-byte notify on char 0004
4. Bridge receives ack, posts to POST /api/users/{id}/acks
5. Bridge display shows bell-check icon + reminder message
```

### DFU (firmware update via bridge)

```
1. Upload firmware zip to server via POST /api/firmware
2. Set bridge pairing state to "dfu"
3. Bridge downloads firmware into PSRAM
4. Bridge connects to watch, streams via Nordic Legacy DFU protocol
5. Watch validates CRC, activates image, reboots
6. Bridge waits 2-minute grace period, then reconnects to sync time + reminders
7. Bridge skips DFU characteristics on non-DFU connections
```
