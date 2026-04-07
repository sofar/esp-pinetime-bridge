#include "pinetime_bridge.h"
#include "esphome/core/log.h"
#include "esphome/core/time.h"
#include "esphome/core/application.h"
#include "esphome/components/esp32_ble_client/ble_client_base.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#include <cstring>
#include <cJSON.h>
#include <esp_http_client.h>
#include <esp_netif.h>

namespace espbt = esphome::esp32_ble_tracker;

namespace esphome {
namespace pinetime_bridge {

static const char *const TAG = "pinetime_bridge";

// Reminder Service UUID: 00060000-78fc-48fe-8e23-433b3a1942d0
static const uint8_t REMINDER_SERVICE_UUID[16] = {
    0xd0, 0x42, 0x19, 0x3a, 0x3b, 0x43, 0x23, 0x8e,
    0xfe, 0x48, 0xfc, 0x78, 0x00, 0x00, 0x06, 0x00};

// Nordic DFU Service UUID: 00001530-1212-efde-1523-785feabcd123
static const uint8_t DFU_SERVICE_UUID[16] = {
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
    0xde, 0xef, 0x12, 0x12, 0x30, 0x15, 0x00, 0x00};
// Control Point: 00001531-...
static const uint8_t DFU_CTRL_UUID[16] = {
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
    0xde, 0xef, 0x12, 0x12, 0x31, 0x15, 0x00, 0x00};
// Packet: 00001532-...
static const uint8_t DFU_PKT_UUID[16] = {
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
    0xde, 0xef, 0x12, 0x12, 0x32, 0x15, 0x00, 0x00};

// ANS UUID: 0x1811
static const uint16_t ANS_SERVICE_UUID = 0x1811;
static const uint16_t ANS_CHAR_UUID = 0x2A46;

// CTS UUID: 0x1805
static const uint16_t CTS_SERVICE_UUID = 0x1805;
static const uint16_t CTS_CHAR_UUID = 0x2A2B;


WatchReminder ApiReminder::to_watch_reminder() const {
  WatchReminder wr;
  wr.version = 2;
  wr.id = reminder_id;
  wr.hours = hours;
  wr.minutes = minutes;
  wr.recurrence = recurrence;
  wr.flags = (enabled ? 0x01 : 0x00) | ((priority & 0x03) << 1);
  wr.month = month;
  wr.day = day;
  memset(wr.message, 0, sizeof(wr.message));
  size_t len = std::min(message.size(), sizeof(wr.message) - 1);
  memcpy(wr.message, message.c_str(), len);
  return wr;
}

// Global pointer for GAP callback (ESP-IDF requires C-style callback)
static PineTimeBridge *g_bridge = nullptr;

void bridge_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (!g_bridge) return;

  // IMPORTANT: GAP callbacks run on the BLE stack task.
  // Never make blocking calls (HTTP, heavy logging) here.
  // Set flags and let loop() handle the HTTP work.
  switch (event) {
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
      ESP_LOGI(TAG, "[PAIR] Watch requesting passkey");
      g_bridge->passkey_pending_ = true;
      g_bridge->gap_event_pending_ = 1;  // passkey needed
      break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      if (param->ble_security.auth_cmpl.success) {
        ESP_LOGI(TAG, "[PAIR] Authentication complete");
        g_bridge->pairing_in_progress_ = false;
        g_bridge->gap_event_pending_ = 2;  // paired
      } else {
        ESP_LOGW(TAG, "[PAIR] Authentication failed: reason=%d", param->ble_security.auth_cmpl.fail_reason);
        g_bridge->pairing_in_progress_ = false;
        g_bridge->gap_auth_fail_reason_ = param->ble_security.auth_cmpl.fail_reason;
        g_bridge->gap_event_pending_ = 3;  // failed
      }
      break;

    default:
      break;
  }
}

void PineTimeBridge::setup() {
  ESP_LOGI(TAG, "PineTime Bridge setup");
  ESP_LOGI(TAG, "  API: %s", api_base_url_.c_str());
  ESP_LOGI(TAG, "  User ID: %s", user_id_.c_str());
  ESP_LOGI(TAG, "  Bridge ID: %s", bridge_id_.c_str());
  ESP_LOGI(TAG, "  Poll interval: %u ms", poll_interval_ms_);
  // Delay first poll to let WiFi stabilize, but not too long
  last_poll_ms_ = millis() - poll_interval_ms_ + 10000;  // first poll 10s after boot
  last_heartbeat_ms_ = millis() - 280000;  // first heartbeat ~20s after boot

  // Restore paired watch address from server user record after WiFi is up
  // (done in loop once server is reachable)

  // Register GAP callback for passkey handling
  g_bridge = this;
  esp_ble_gap_register_callback(bridge_gap_event_handler);

  // Set security parameters for passkey pairing
  esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
  esp_ble_io_cap_t io_cap = ESP_IO_CAP_KBDISP;  // keyboard + display
  uint8_t key_size = 16;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, sizeof(io_cap));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
}

void PineTimeBridge::loop() {
  uint32_t now = millis();

  // DFU processing takes priority (with 5-minute safety timeout)
  {
    static uint32_t dfu_start_ms = 0;
    bool dfu_active = dfu_client_.state() != DfuState::IDLE && dfu_client_.state() != DfuState::COMPLETE && dfu_client_.state() != DfuState::FAILED;
    if (dfu_active) {
      if (dfu_start_ms == 0) dfu_start_ms = now;
      if (now - dfu_start_ms > 300000) {
        ESP_LOGE(TAG, "[DFU] Timeout after 5 minutes — resetting DFU client");
        dfu_client_.reset();
        dfu_start_ms = 0;
      } else {
        dfu_client_.process();
        return;
      }
    } else {
      dfu_start_ms = 0;  // always reset when DFU is not active
    }
  }

  // After DFU completes, force a reconnect for time sync only.
  // Skip reminder sync for 2 minutes — let the freshly booted firmware stabilize.
  if (dfu_client_.state() == DfuState::COMPLETE || dfu_client_.state() == DfuState::FAILED) {
    ESP_LOGI(TAG, "[DFU] %s — forcing reconnect for time sync (reminders deferred 2 min)",
             dfu_client_.state() == DfuState::COMPLETE ? "Complete" : "Failed");
    post_dfu_ = true;
    post_dfu_time_ms_ = now;
    needs_ble_sync_ = true;
    last_watch_poll_ms_ = 0;  // trigger immediate watch poll (time sync, battery, steps)
    dfu_client_.reset();
  }

  // End post-DFU grace period after 2 minutes
  if (post_dfu_ && post_dfu_time_ms_ > 0 && (now - post_dfu_time_ms_ > 120000)) {
    ESP_LOGI(TAG, "[DFU] Post-DFU grace period ended, resuming normal reminder sync");
    post_dfu_ = false;
    post_dfu_time_ms_ = 0;
    last_sync_hash_ = 0;  // force full reminder sync on next poll
  }

  // Process BLE command queue when connected and services discovered
  if (!ble_busy_ && !command_queue_.empty() && services_discovered_ && ble_connected_) {
    process_next_command_();
  }

  // Periodic API poll (runs regardless of BLE state)
  if (now - last_poll_ms_ > poll_interval_ms_) {
    last_poll_ms_ = now;
    poll_api_();
    // Sync reminders if connected and data changed (skip during DFU/post-DFU)
    if (dfu_client_.state() == DfuState::IDLE && !post_dfu_) {
      sync_reminders_to_watch_(); // queues BLE command only if data changed
    }
  }

  // Persistent BLE connection — connect if paired and not connected
  if (!ble_connected_ && paired_address_ != 0 && !pairing_in_progress_ && dfu_client_.state() == DfuState::IDLE) {
    // Reconnect with backoff: wait 5 seconds between attempts
    if (now - ble_connect_time_ms_ > 5000) {
      ESP_LOGI(TAG, "[BLE] Connecting to watch...");
      this->parent()->set_enabled(true);
      this->parent()->set_state(espbt::ClientState::CONNECTING);
      esp_bd_addr_t bda;
      bda[0] = (paired_address_ >> 40) & 0xFF;
      bda[1] = (paired_address_ >> 32) & 0xFF;
      bda[2] = (paired_address_ >> 24) & 0xFF;
      bda[3] = (paired_address_ >> 16) & 0xFF;
      bda[4] = (paired_address_ >> 8) & 0xFF;
      bda[5] = paired_address_ & 0xFF;
      esp_ble_gattc_open(this->parent()->get_gattc_if(), bda, BLE_ADDR_TYPE_RANDOM, true);
      ble_connect_time_ms_ = now;
    }
  }

  // Periodic watch data refresh — read battery/steps over existing connection
  if (ble_connected_ && services_discovered_ && command_queue_.empty() && !ble_busy_) {
    static constexpr uint32_t WATCH_READ_INTERVAL_MS = 5 * 60 * 1000UL;  // every 5 min
    if (now - last_watch_poll_ms_ > WATCH_READ_INTERVAL_MS) {
      last_watch_poll_ms_ = now;
      // Re-read battery, steps, uptime over existing connection
      if (battery_handle_) esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), battery_handle_, ESP_GATT_AUTH_REQ_NONE);
      if (steps_handle_) esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), steps_handle_, ESP_GATT_AUTH_REQ_NONE);
      if (status_handle_) esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), status_handle_, ESP_GATT_AUTH_REQ_NONE);
      // Re-sync time
      if (cts_handle_) sync_time_();
      ESP_LOGI(TAG, "[WATCH] Periodic refresh (battery/steps/uptime/time)");
    }
  }

  // Post discovered watches to server every 60 seconds
  if (now - last_discovery_post_ms_ > 60000) {
    last_discovery_post_ms_ = now;
    post_discovered_watches_();
  }

  // Pairing: poll server — fast (3s) when pairing active, slow (60s) otherwise
  {
    static uint32_t last_pair_poll = 0;
    uint32_t pair_interval = (passkey_pending_ || pairing_in_progress_) ? 3000 : 60000;
    if (now - last_pair_poll > pair_interval) {
      last_pair_poll = now;
      check_pairing_request_();
    }
  }

  // Process deferred GAP events (set in BLE callback, processed here on main task)
  if (gap_event_pending_ > 0) {
    uint8_t evt = gap_event_pending_;
    gap_event_pending_ = 0;
    std::string pair_path = "/api/bridges/" + bridge_id_ + "/pairing";
    if (evt == 1) {
      remote_log_("bridge", "info", "Watch requesting passkey");
      http_post_(pair_path, "{\"state\":\"passkey_needed\"}");
    } else if (evt == 2) {
      remote_log_("bridge", "info", "Watch paired successfully");
      http_post_(pair_path, "{\"state\":\"paired\"}");
    } else if (evt == 3) {
      char body[128];
      snprintf(body, sizeof(body), "{\"state\":\"failed\",\"passkey\":\"auth failed (reason %d)\"}", gap_auth_fail_reason_);
      http_post_(pair_path, body);
    }
  }

  // Periodic heartbeat (every 60 seconds)
  if (now - last_heartbeat_ms_ > 60000) {
    last_heartbeat_ms_ = now;
    send_heartbeat_();
  }
}

void PineTimeBridge::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Connected to PineTime");
        ble_connected_ = true;
        ble_connect_time_ms_ = millis();
        last_watch_poll_ms_ = millis();
        remote_log_("bridge", "info", "BLE connected to watch");
        if (pairing_in_progress_) {
          pairing_in_progress_ = false;
          http_post_("/api/bridges/" + bridge_id_ + "/pairing", "{\"state\":\"paired\"}");
        }
      }
      break;

    case ESP_GATTC_DISCONNECT_EVT:
      ESP_LOGW(TAG, "Disconnected from PineTime — will reconnect");
      remote_log_("bridge", "info", "BLE disconnected from watch");
      ble_connected_ = false;
      services_discovered_ = false;
      ble_connect_time_ms_ = millis();  // start reconnect backoff from now
      // If commands were still pending, reset hash so next connection retries the full sync
      if (!command_queue_.empty() || pending_writes_ > 0) {
        ESP_LOGW(TAG, "[SYNC] Disconnected with %u queued + %u pending writes — will retry sync",
                 command_queue_.size(), pending_writes_);
        last_sync_hash_ = 0;
        while (!command_queue_.empty()) command_queue_.pop();
      }
      pending_writes_ = 0;
      upload_handle_ = 0;
      delete_handle_ = 0;
      list_handle_ = 0;
      ack_handle_ = 0;
      sync_handle_ = 0;
      status_handle_ = 0;
      ans_handle_ = 0;
      cts_handle_ = 0;
      battery_handle_ = 0;
      fw_rev_handle_ = 0;
      mfr_handle_ = 0;
      sw_rev_handle_ = 0;
      model_handle_ = 0;
      hw_rev_handle_ = 0;
      serial_handle_ = 0;
      steps_handle_ = 0;
      dfu_rev_handle_ = 0;
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
      ESP_LOGI(TAG, "Service discovery complete");
      discover_services_();
      // Request encryption to trigger pairing/bonding
      if (pairing_in_progress_) {
        ESP_LOGI(TAG, "[PAIR] Requesting encryption to trigger pairing...");
        esp_ble_set_encryption(this->parent()->get_remote_bda(), ESP_BLE_SEC_ENCRYPT_MITM);
      }
      break;

    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.status == ESP_GATT_OK) {
        // Suppress per-write logs during DFU (progress is logged by DfuClient every 10 chunks)
        if (dfu_client_.state() == DfuState::IDLE) {
          ESP_LOGI(TAG, "[BLE] Write OK, handle=0x%04x, queue=%u remaining", param->write.handle, command_queue_.size());
        }
        if (param->write.handle == ans_handle_) {
          ESP_LOGI(TAG, "[NOTIFY] ANS write confirmed by watch");
        }
      } else {
        ESP_LOGW(TAG, "[BLE] Write FAILED, handle=0x%04x status=%d", param->write.handle, param->write.status);
      }
      ble_busy_ = false;
      if (pending_writes_ > 0) pending_writes_--;
      if (command_queue_.empty() && pending_writes_ == 0 && dfu_client_.state() == DfuState::IDLE) {
        // Verify sync by reading back reminder count from watch
        if (list_handle_ != 0 && param->write.handle == sync_handle_) {
          ESP_LOGI(TAG, "[SYNC] Write complete, reading back from watch to verify...");
          esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                   list_handle_, ESP_GATT_AUTH_REQ_NONE);
          break;  // Wait for read response before marking done
        }
        ESP_LOGI(TAG, "[BLE] All commands sent");
        // Record sync time
        auto t = ::time(nullptr);
        struct tm *tm = ::localtime(&t);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", tm);
        last_sync_time_ = buf;
      }
      break;

    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle == ack_handle_) {
        on_ack_notification_(param->notify.value, param->notify.value_len);
      }
      // Forward to DFU client
      dfu_client_.on_gattc_event(event, gattc_if, param);
      break;

    case ESP_GATTC_READ_CHAR_EVT:
      if (param->read.status == ESP_GATT_OK) {
        uint16_t h = param->read.handle;
        if (h == battery_handle_ && param->read.value_len >= 1) {
          watch_battery_ = param->read.value[0];
          ESP_LOGI(TAG, "[WATCH] Battery: %u%%", watch_battery_);
        } else if (h == steps_handle_ && param->read.value_len >= 4) {
          watch_steps_ = param->read.value[0] | (param->read.value[1] << 8) |
                         (param->read.value[2] << 16) | (param->read.value[3] << 24);
          ESP_LOGI(TAG, "[WATCH] Steps: %u", watch_steps_);
        } else if (h == fw_rev_handle_) {
          watch_firmware_ = std::string((char *)param->read.value, param->read.value_len);
          ESP_LOGI(TAG, "[WATCH] Firmware: %s", watch_firmware_.c_str());
        } else if (h == mfr_handle_) {
          watch_manufacturer_ = std::string((char *)param->read.value, param->read.value_len);
          ESP_LOGI(TAG, "[WATCH] Manufacturer: %s", watch_manufacturer_.c_str());
        } else if (h == sw_rev_handle_) {
          watch_software_ = std::string((char *)param->read.value, param->read.value_len);
          ESP_LOGI(TAG, "[WATCH] Software: %s", watch_software_.c_str());
        } else if (h == model_handle_) {
          watch_model_ = std::string((char *)param->read.value, param->read.value_len);
          ESP_LOGI(TAG, "[WATCH] Model: %s", watch_model_.c_str());
        } else if (h == hw_rev_handle_) {
          watch_hw_rev_ = std::string((char *)param->read.value, param->read.value_len);
          ESP_LOGI(TAG, "[WATCH] HW Rev: %s", watch_hw_rev_.c_str());
        } else if (h == serial_handle_) {
          watch_serial_ = std::string((char *)param->read.value, param->read.value_len);
          ESP_LOGI(TAG, "[WATCH] Serial: %s", watch_serial_.c_str());
        } else if (h == status_handle_ && param->read.value_len >= 4) {
          watch_uptime_ = param->read.value[0] | (param->read.value[1] << 8) |
                          (param->read.value[2] << 16) | (param->read.value[3] << 24);
          ESP_LOGI(TAG, "[WATCH] Uptime: %u s (%u h %u m)", watch_uptime_,
                   watch_uptime_ / 3600, (watch_uptime_ % 3600) / 60);
        } else if (h == list_handle_) {
          // Sync verification read-back
          uint8_t watch_count = param->read.value[0];
          size_t expected = api_reminders_.size();
          if (watch_count == expected) {
            ESP_LOGI(TAG, "[SYNC] Verified: watch has %u reminders (matches)", watch_count);
            remote_log_("bridge", "info", "Sync verified: watch confirmed reminders");
          } else {
            ESP_LOGW(TAG, "[SYNC] Mismatch: watch has %u, expected %u", watch_count, expected);
            remote_log_("bridge", "warn", "Sync mismatch: watch count differs");
          }
          // Record sync time
          auto t = ::time(nullptr);
          struct tm *tmm = ::localtime(&t);
          char tbuf[32];
          strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", tmm);
          last_sync_time_ = tbuf;
        } else if (h == dfu_rev_handle_) {
          if (param->read.value_len >= 2) {
            uint16_t rev = param->read.value[0] | (param->read.value[1] << 8);
            ESP_LOGI(TAG, "[WATCH] DFU Revision: 0x%04x", rev);
          } else {
            ESP_LOGI(TAG, "[WATCH] DFU Revision: %u bytes", param->read.value_len);
          }
        } else {
          ESP_LOGI(TAG, "[WATCH] Read handle 0x%04x: %u bytes", h, param->read.value_len);
        }
      } else {
        ESP_LOGW(TAG, "[WATCH] Read handle 0x%04x failed: %d", param->read.handle, param->read.status);
      }
      break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
      ESP_LOGI(TAG, "Registered for notify on handle 0x%04x", param->reg_for_notify.handle);
      break;

    case ESP_GATTC_SRVC_CHG_EVT:
      ESP_LOGI(TAG, "[BLE] Service changed event");
      break;

    default:
      break;
  }
}

// Helper to build 128-bit ESPBTUUID for our custom service characteristics
static espbt::ESPBTUUID make_reminder_uuid(uint8_t x, uint8_t y) {
  // Match InfiniTime's CharUuid(x, y) which puts y at byte 12, x at byte 13
  uint8_t bytes[16] = {0xd0, 0x42, 0x19, 0x3a, 0x3b, 0x43, 0x23, 0x8e,
                        0xfe, 0x48, 0xfc, 0x78, y, x, 0x06, 0x00};
  return espbt::ESPBTUUID::from_raw(bytes);
}

void PineTimeBridge::discover_services_() {
  auto *client = this->parent();
  if (client == nullptr) {
    return;
  }

  // Debug: try to find reminder service and log what we're looking for
  auto svc_uuid = make_reminder_uuid(0x00, 0x00);
  ESP_LOGI(TAG, "[SVC] Looking for reminder service: %s", svc_uuid.to_string().c_str());
  auto *reminder_svc = client->get_service(svc_uuid);
  ESP_LOGI(TAG, "[SVC] get_service result: %s", reminder_svc ? "FOUND" : "NOT FOUND");

  // Also try the raw 16-byte comparison
  auto *test_svc = client->get_service(espbt::ESPBTUUID::from_raw(REMINDER_SERVICE_UUID));
  ESP_LOGI(TAG, "[SVC] get_service(raw) result: %s", test_svc ? "FOUND" : "NOT FOUND");

  // Reminder Service characteristics
  auto *chr = client->get_characteristic(svc_uuid, make_reminder_uuid(0x01, 0x00));
  if (chr) { upload_handle_ = chr->handle; ESP_LOGI(TAG, "Found upload char: 0x%04x", upload_handle_); }

  chr = client->get_characteristic(svc_uuid, make_reminder_uuid(0x02, 0x00));
  if (chr) { delete_handle_ = chr->handle; ESP_LOGI(TAG, "Found delete char: 0x%04x", delete_handle_); }

  chr = client->get_characteristic(svc_uuid, make_reminder_uuid(0x03, 0x00));
  if (chr) { list_handle_ = chr->handle; ESP_LOGI(TAG, "Found list char: 0x%04x", list_handle_); }

  chr = client->get_characteristic(svc_uuid, make_reminder_uuid(0x04, 0x00));
  if (chr) {
    ack_handle_ = chr->handle;
    ESP_LOGI(TAG, "Found ack char: 0x%04x", ack_handle_);
    esp_ble_gattc_register_for_notify(client->get_gattc_if(), client->get_remote_bda(), ack_handle_);
  }

  chr = client->get_characteristic(svc_uuid, make_reminder_uuid(0x05, 0x00));
  if (chr) { sync_handle_ = chr->handle; ESP_LOGI(TAG, "Found sync char: 0x%04x", sync_handle_); }

  chr = client->get_characteristic(svc_uuid, make_reminder_uuid(0x06, 0x00));
  if (chr) {
    status_handle_ = chr->handle;
    ESP_LOGI(TAG, "Found status char: 0x%04x, reading...", chr->handle);
    esp_ble_gattc_read_char(client->get_gattc_if(), client->get_conn_id(), chr->handle, ESP_GATT_AUTH_REQ_NONE);
  }

  // ANS (0x1811) — for one-off notifications
  auto ans_svc = espbt::ESPBTUUID::from_uint16(ANS_SERVICE_UUID);
  auto ans_chr_uuid = espbt::ESPBTUUID::from_uint16(ANS_CHAR_UUID);
  chr = client->get_characteristic(ans_svc, ans_chr_uuid);
  if (chr) { ans_handle_ = chr->handle; ESP_LOGI(TAG, "Found ANS char: 0x%04x", ans_handle_); }

  // CTS (0x1805) — for time sync
  auto cts_svc = espbt::ESPBTUUID::from_uint16(CTS_SERVICE_UUID);
  auto cts_chr_uuid = espbt::ESPBTUUID::from_uint16(CTS_CHAR_UUID);
  chr = client->get_characteristic(cts_svc, cts_chr_uuid);
  if (chr) { cts_handle_ = chr->handle; ESP_LOGI(TAG, "Found CTS char: 0x%04x", cts_handle_); }

  // Battery Service (0x180F)
  auto bat_svc = espbt::ESPBTUUID::from_uint16(0x180F);
  auto bat_chr_uuid = espbt::ESPBTUUID::from_uint16(0x2A19);
  chr = client->get_characteristic(bat_svc, bat_chr_uuid);
  if (chr) {
    battery_handle_ = chr->handle;
    ESP_LOGI(TAG, "Found Battery char: 0x%04x, reading...", chr->handle);
    esp_ble_gattc_read_char(client->get_gattc_if(), client->get_conn_id(), chr->handle, ESP_GATT_AUTH_REQ_NONE);
  }

  // Device Info Service (0x180A)
  auto dev_svc = espbt::ESPBTUUID::from_uint16(0x180A);
  chr = client->get_characteristic(dev_svc, espbt::ESPBTUUID::from_uint16(0x2A26));
  if (chr) {
    fw_rev_handle_ = chr->handle;
    ESP_LOGI(TAG, "Found Firmware Rev char: 0x%04x, reading...", chr->handle);
    esp_ble_gattc_read_char(client->get_gattc_if(), client->get_conn_id(), chr->handle, ESP_GATT_AUTH_REQ_NONE);
  }
  chr = client->get_characteristic(dev_svc, espbt::ESPBTUUID::from_uint16(0x2A29));
  if (chr) {
    mfr_handle_ = chr->handle;
    ESP_LOGI(TAG, "Found Manufacturer char: 0x%04x, reading...", chr->handle);
    esp_ble_gattc_read_char(client->get_gattc_if(), client->get_conn_id(), chr->handle, ESP_GATT_AUTH_REQ_NONE);
  }
  chr = client->get_characteristic(dev_svc, espbt::ESPBTUUID::from_uint16(0x2A28));
  if (chr) {
    sw_rev_handle_ = chr->handle;
    ESP_LOGI(TAG, "Found Software Rev char: 0x%04x, reading...", chr->handle);
    esp_ble_gattc_read_char(client->get_gattc_if(), client->get_conn_id(), chr->handle, ESP_GATT_AUTH_REQ_NONE);
  }
  // Model Number (0x2A24)
  chr = client->get_characteristic(dev_svc, espbt::ESPBTUUID::from_uint16(0x2A24));
  if (chr) {
    model_handle_ = chr->handle;
    ESP_LOGI(TAG, "Found Model char: 0x%04x, reading...", chr->handle);
    esp_ble_gattc_read_char(client->get_gattc_if(), client->get_conn_id(), chr->handle, ESP_GATT_AUTH_REQ_NONE);
  }
  // Hardware Revision (0x2A27)
  chr = client->get_characteristic(dev_svc, espbt::ESPBTUUID::from_uint16(0x2A27));
  if (chr) {
    hw_rev_handle_ = chr->handle;
    ESP_LOGI(TAG, "Found HW Rev char: 0x%04x, reading...", chr->handle);
    esp_ble_gattc_read_char(client->get_gattc_if(), client->get_conn_id(), chr->handle, ESP_GATT_AUTH_REQ_NONE);
  }
  // Serial Number (0x2A25)
  chr = client->get_characteristic(dev_svc, espbt::ESPBTUUID::from_uint16(0x2A25));
  if (chr) {
    serial_handle_ = chr->handle;
    ESP_LOGI(TAG, "Found Serial char: 0x%04x, reading...", chr->handle);
    esp_ble_gattc_read_char(client->get_gattc_if(), client->get_conn_id(), chr->handle, ESP_GATT_AUTH_REQ_NONE);
  }

  // Motion Service — Step Count (00030001-78fc-48fe-8e23-433b3a1942d0)
  auto step_svc_uuid = espbt::ESPBTUUID::from_raw((uint8_t[]){
      0xd0, 0x42, 0x19, 0x3a, 0x3b, 0x43, 0x23, 0x8e,
      0xfe, 0x48, 0xfc, 0x78, 0x00, 0x00, 0x03, 0x00});
  auto step_chr_uuid = espbt::ESPBTUUID::from_raw((uint8_t[]){
      0xd0, 0x42, 0x19, 0x3a, 0x3b, 0x43, 0x23, 0x8e,
      0xfe, 0x48, 0xfc, 0x78, 0x01, 0x00, 0x03, 0x00});
  chr = client->get_characteristic(step_svc_uuid, step_chr_uuid);
  if (chr) {
    steps_handle_ = chr->handle;
    ESP_LOGI(TAG, "Found Step Count char: 0x%04x, reading...", chr->handle);
    esp_ble_gattc_read_char(client->get_gattc_if(), client->get_conn_id(), chr->handle, ESP_GATT_AUTH_REQ_NONE);
  }

  // DFU Service — only discover and interact when a DFU is actively in progress.
  // Any access to DFU characteristics triggers DfuService::OnServiceData on the
  // watch, which shows a "firmware update disabled" notification if DFU is off
  // in settings and can cause unexpected behavior on freshly-booted firmware.
  if (dfu_client_.state() == DfuState::CONNECTING) {
    uint16_t dfu_ctrl = 0, dfu_pkt = 0;
    auto dfu_svc_uuid = espbt::ESPBTUUID::from_raw(DFU_SERVICE_UUID);
    auto dfu_ctrl_uuid = espbt::ESPBTUUID::from_raw(DFU_CTRL_UUID);
    auto dfu_pkt_uuid = espbt::ESPBTUUID::from_raw(DFU_PKT_UUID);
    chr = client->get_characteristic(dfu_svc_uuid, dfu_ctrl_uuid);
    if (chr) { dfu_ctrl = chr->handle; ESP_LOGI(TAG, "Found DFU control: 0x%04x", dfu_ctrl); }
    chr = client->get_characteristic(dfu_svc_uuid, dfu_pkt_uuid);
    if (chr) { dfu_pkt = chr->handle; ESP_LOGI(TAG, "Found DFU packet: 0x%04x", dfu_pkt); }
    // DFU Revision (00001534-...)
    static const uint8_t DFU_REV_UUID[16] = {
        0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15,
        0xde, 0xef, 0x12, 0x12, 0x34, 0x15, 0x00, 0x00};
    auto dfu_rev_uuid = espbt::ESPBTUUID::from_raw(DFU_REV_UUID);
    chr = client->get_characteristic(dfu_svc_uuid, dfu_rev_uuid);
    if (chr) {
      dfu_rev_handle_ = chr->handle;
      ESP_LOGI(TAG, "Found DFU revision: 0x%04x, reading...", chr->handle);
      esp_ble_gattc_read_char(client->get_gattc_if(), client->get_conn_id(), chr->handle, ESP_GATT_AUTH_REQ_NONE);
    }
    if (dfu_ctrl && dfu_pkt) {
      ESP_LOGI(TAG, "DFU service found");
      dfu_client_.on_services_discovered(dfu_ctrl, dfu_pkt);
      return;  // DFU takes over from here
    }
  }

  // Always sync time when connected (CTS)
  if (cts_handle_ != 0) {
    sync_time_();
  }

  if (upload_handle_ != 0 && sync_handle_ != 0) {
    services_discovered_ = true;
    ESP_LOGI(TAG, "Reminder service discovered");
    // Queue pending reminders now that we have handles (skip if DFU active or post-DFU)
    if (dfu_client_.state() == DfuState::IDLE && !post_dfu_) {
      sync_reminders_to_watch_();
    } else if (post_dfu_) {
      ESP_LOGI(TAG, "[DFU] Skipping reminder sync on post-DFU connection, time sync only");
    }
    // Force a poll to send any pending notifications
    last_poll_ms_ = 0;
  } else {
    ESP_LOGW(TAG, "Reminder service NOT found on watch");
  }
}

void PineTimeBridge::process_next_command_() {
  if (command_queue_.empty()) return;

  auto &cmd = command_queue_.front();
  uint16_t handle = 0;

  switch (cmd.type) {
    case BleCommandType::WRITE_REMINDER:
      handle = upload_handle_;
      break;
    case BleCommandType::DELETE_REMINDER:
      handle = delete_handle_;
      break;
    case BleCommandType::SYNC_ALL:
      handle = sync_handle_;
      break;
  }

  if (handle == 0) {
    ESP_LOGW(TAG, "No handle for command type %d", (int)cmd.type);
    command_queue_.pop();
    return;
  }

  auto status = esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle,
      cmd.data.size(), cmd.data.data(), ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);

  if (status == ESP_OK) {
    ble_busy_ = true;
    ESP_LOGD(TAG, "Sent BLE command to handle 0x%04x, %u bytes", handle, cmd.data.size());
  } else {
    ESP_LOGW(TAG, "BLE write failed: %d", status);
  }

  command_queue_.pop();
}

// HTTP response buffer
struct HttpBuffer {
  char *data;
  size_t len;
  size_t capacity;
};

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  auto *buf = static_cast<HttpBuffer *>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf != nullptr) {
    if (buf->len + evt->data_len < buf->capacity) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
      buf->data[buf->len] = '\0';
    }
  }
  return ESP_OK;
}

std::string PineTimeBridge::http_get_(const std::string &path) {
  std::string url = api_base_url_ + path;
  char response_buf[4096] = {0};
  HttpBuffer buf = {response_buf, 0, sizeof(response_buf) - 1};

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.event_handler = http_event_handler;
  config.user_data = &buf;
  config.timeout_ms = 3000;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_perform(client);

  std::string result;
  int status = esp_http_client_get_status_code(client);
  if (err == ESP_OK && status == 200) {
    result = std::string(buf.data, buf.len);
    ESP_LOGI(TAG, "[HTTP] GET %s -> 200 (%u bytes)", path.c_str(), buf.len);
  } else {
    ESP_LOGW(TAG, "[HTTP] GET %s -> err=%d status=%d", path.c_str(), err, status);
  }

  esp_http_client_cleanup(client);
  return result;
}

bool PineTimeBridge::http_post_(const std::string &path, const std::string &body) {
  std::string url = api_base_url_ + path;

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 3000;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, body.c_str(), body.size());

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK || (status < 200 || status >= 300)) {
    ESP_LOGW(TAG, "[HTTP] POST %s -> err=%d status=%d", path.c_str(), err, status);
    return false;
  }
  ESP_LOGI(TAG, "[HTTP] POST %s -> %d", path.c_str(), status);
  return true;
}

bool PineTimeBridge::get_next_reminder(uint8_t &hours, uint8_t &minutes, std::string &message) const {
  if (api_reminders_.empty()) return false;

  time_t now_t = ::time(nullptr);
  int64_t best_seconds = -1;
  const ApiReminder *best = nullptr;

  for (const auto &r : api_reminders_) {
    if (!r.enabled) continue;

    // Check up to 7 days ahead to find next occurrence
    for (int day_offset = 0; day_offset < 8; day_offset++) {
      time_t candidate_t = now_t + day_offset * 86400;
      struct tm *ctm = ::localtime(&candidate_t);
      ctm->tm_hour = r.hours;
      ctm->tm_min = r.minutes;
      ctm->tm_sec = 0;
      ctm->tm_isdst = -1;
      time_t fire_t = ::mktime(ctm);

      if (fire_t <= now_t) continue;  // in the past

      // Check if this reminder fires on this day
      bool fires = false;
      if (r.recurrence == 0x80) {
        fires = true;  // daily
      } else if (r.month > 0 && r.day > 0) {
        // specific date
        fires = (r.month == ctm->tm_mon + 1) && (r.day == ctm->tm_mday);
      } else if (r.recurrence == 0x00) {
        fires = true;  // once, next matching time
      } else {
        // weekday bitmask
        fires = (r.recurrence & (1 << ctm->tm_wday)) != 0;
      }

      if (!fires) continue;

      int64_t delta = (int64_t)(fire_t - now_t);
      if (best_seconds < 0 || delta < best_seconds) {
        best_seconds = delta;
        best = &r;
      }
      break;  // found next occurrence for this reminder
    }
  }

  if (best == nullptr) return false;
  hours = best->hours;
  minutes = best->minutes;
  message = best->message;
  return true;
}

static std::string mac_to_string(uint64_t address) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           (uint8_t)(address >> 40), (uint8_t)(address >> 32),
           (uint8_t)(address >> 24), (uint8_t)(address >> 16),
           (uint8_t)(address >> 8), (uint8_t)(address));
  return buf;
}

void PineTimeBridge::on_ble_advertise(uint64_t address, const std::string &name, int rssi) {
  if (name != "InfiniTime") return;

  uint32_t now = millis();
  std::string mac = mac_to_string(address);

  // Track paired watch presence (for "welcome back" event detection)
  if (address == paired_address_ && paired_address_ != 0) {
    if (last_watch_seen_ms_ > 0 && (now - last_watch_seen_ms_ > 300000)) {
      ESP_LOGI(TAG, "[SCAN] Paired watch returned after absence");
      watch_returned_event_ = true;
    }
    last_watch_seen_ms_ = now;
  }

  // Update existing or add new
  for (auto &w : discovered_watches_) {
    if (w.address == address) {
      w.rssi = rssi;
      w.last_seen = now;
      return;
    }
  }

  DiscoveredWatch w;
  w.address = address;
  w.name = name;
  w.rssi = rssi;
  w.last_seen = now;
  w.mac_str = mac;
  discovered_watches_.push_back(w);
  ESP_LOGI(TAG, "[SCAN] Found InfiniTime watch: %s (RSSI %d)", mac.c_str(), rssi);
}

void PineTimeBridge::post_discovered_watches_() {
  if (discovered_watches_.empty()) return;

  // Remove stale entries (not seen in 60s)
  uint32_t now = millis();
  discovered_watches_.erase(
    std::remove_if(discovered_watches_.begin(), discovered_watches_.end(),
      [now](const DiscoveredWatch &w) { return (now - w.last_seen) > 60000; }),
    discovered_watches_.end());

  // Build JSON array
  std::string json = "[";
  for (size_t i = 0; i < discovered_watches_.size(); i++) {
    auto &w = discovered_watches_[i];
    char entry[128];
    snprintf(entry, sizeof(entry), "%s{\"mac\":\"%s\",\"name\":\"%s\",\"rssi\":%d}",
             i > 0 ? "," : "", w.mac_str.c_str(), w.name.c_str(), w.rssi);
    json += entry;
  }
  json += "]";

  std::string path = "/api/bridges/" + bridge_id_ + "/discovered";
  http_post_(path, json);
}

void PineTimeBridge::check_pairing_request_() {
  // Poll server for pairing state
  std::string path = "/api/bridges/" + bridge_id_ + "/pairing";
  std::string response = http_get_(path);
  if (response.empty()) return;

  cJSON *root = cJSON_Parse(response.c_str());
  if (!root) return;

  const char *state = cJSON_GetObjectItem(root, "state") ? cJSON_GetObjectItem(root, "state")->valuestring : "idle";
  const char *passkey_field = cJSON_GetObjectItem(root, "passkey") ? cJSON_GetObjectItem(root, "passkey")->valuestring : "";

  if (strcmp(state, "dfu") == 0 && dfu_client_.state() == DfuState::IDLE) {
    // Clear the DFU request on server FIRST to prevent retry loops
    http_post_("/api/bridges/" + bridge_id_ + "/pairing", "{\"state\":\"idle\"}");

    ESP_LOGI(TAG, "[DFU] DFU requested via server, starting...");
    start_dfu();
    // After download, connect to the watch
    if (dfu_client_.state() == DfuState::CONNECTING) {
      needs_ble_sync_ = false;  // DFU takes over BLE
      this->parent()->set_enabled(true);
      this->parent()->set_state(espbt::ClientState::CONNECTING);
      esp_bd_addr_t bda;
      auto addr = this->parent()->get_address();
      bda[0] = (addr >> 40) & 0xFF;
      bda[1] = (addr >> 32) & 0xFF;
      bda[2] = (addr >> 24) & 0xFF;
      bda[3] = (addr >> 16) & 0xFF;
      bda[4] = (addr >> 8) & 0xFF;
      bda[5] = addr & 0xFF;
      ESP_LOGI(TAG, "[DFU] Connecting to watch for DFU...");
      esp_ble_gattc_open(this->parent()->get_gattc_if(), bda, BLE_ADDR_TYPE_RANDOM, true);
      ble_connect_time_ms_ = millis();
    }
  } else if (strcmp(state, "connecting") == 0 && !pairing_in_progress_ && strlen(passkey_field) == 17) {
    // User selected a watch MAC from the web UI — connect to it
    ESP_LOGI(TAG, "[PAIR] User selected watch %s, connecting...", passkey_field);

    uint64_t addr = 0;
    unsigned int b[6];
    if (sscanf(passkey_field, "%02X:%02X:%02X:%02X:%02X:%02X", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
      addr = ((uint64_t)b[0] << 40) | ((uint64_t)b[1] << 32) | ((uint64_t)b[2] << 24) |
             ((uint64_t)b[3] << 16) | ((uint64_t)b[4] << 8) | (uint64_t)b[5];
      ESP_LOGI(TAG, "[PAIR] Setting BLE client address to %s (0x%012llX)", passkey_field, addr);
      paired_address_ = addr;
      this->parent()->set_address(addr);
      this->parent()->set_enabled(true);
      this->parent()->set_state(espbt::ClientState::CONNECTING);

      // Directly initiate GATT connection
      esp_bd_addr_t bda;
      bda[0] = (addr >> 40) & 0xFF;
      bda[1] = (addr >> 32) & 0xFF;
      bda[2] = (addr >> 24) & 0xFF;
      bda[3] = (addr >> 16) & 0xFF;
      bda[4] = (addr >> 8) & 0xFF;
      bda[5] = addr & 0xFF;
      auto ret = esp_ble_gattc_open(this->parent()->get_gattc_if(), bda, BLE_ADDR_TYPE_RANDOM, true);
      ESP_LOGI(TAG, "[PAIR] esp_ble_gattc_open result: %d", ret);

      pairing_in_progress_ = true;
      ble_connect_time_ms_ = millis();
      remote_log_("bridge", "info", (std::string("Connecting to watch ") + passkey_field).c_str());
    }
  } else if (strcmp(state, "passkey_entered") == 0 && passkey_pending_) {
    // Server has the passkey from the user
    if (strlen(passkey_field) == 6) {
      uint32_t passkey = atoi(passkey_field);
      ESP_LOGI(TAG, "[PAIR] Got passkey %06u from server, injecting...", passkey);
      esp_ble_passkey_reply(this->parent()->get_remote_bda(), true, passkey);
      passkey_pending_ = false;
    }
  }

  cJSON_Delete(root);
}

void PineTimeBridge::restore_paired_address_from_server_() {
  // Fetch user record to get watch MAC
  std::string path = "/api/users/" + user_id_;
  std::string response = http_get_(path);
  if (response.empty()) return;

  cJSON *root = cJSON_Parse(response.c_str());
  if (!root) return;

  const char *mac = cJSON_GetObjectItem(root, "watch_mac") ? cJSON_GetObjectItem(root, "watch_mac")->valuestring : "";
  if (strlen(mac) == 17) {
    uint64_t addr = 0;
    unsigned int b[6];
    if (sscanf(mac, "%02X:%02X:%02X:%02X:%02X:%02X", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
      addr = ((uint64_t)b[0] << 40) | ((uint64_t)b[1] << 32) | ((uint64_t)b[2] << 24) |
             ((uint64_t)b[3] << 16) | ((uint64_t)b[4] << 8) | (uint64_t)b[5];
      if (addr != 0 && paired_address_ == 0) {
        ESP_LOGI(TAG, "[PAIR] Restored watch MAC from server: %s", mac);
        set_paired_address(addr);
      }
    }
  }
  cJSON_Delete(root);
}

void PineTimeBridge::set_paired_address(uint64_t addr) {
  paired_address_ = addr;
  if (addr != 0) {
    ESP_LOGI(TAG, "[PAIR] Restoring paired watch address: 0x%012llX", addr);
    this->parent()->set_address(addr);
  }
}

void PineTimeBridge::start_dfu() {
  ESP_LOGI(TAG, "[DFU] Starting firmware update...");
  remote_log_("bridge", "info", "Starting firmware update");

  // Wire up the DFU client's BLE write callbacks
  dfu_client_.write_char = [this](uint16_t handle, const uint8_t *data, size_t len, bool response) -> esp_err_t {
    auto type = response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP;
    return esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                     handle, len, const_cast<uint8_t *>(data), type, ESP_GATT_AUTH_REQ_NONE);
  };
  dfu_client_.register_notify = [this](uint16_t handle) -> esp_err_t {
    return esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(),
                                              this->parent()->get_remote_bda(), handle);
  };

  // Download firmware from server into PSRAM
  dfu_client_.start(api_base_url_);
}

void PineTimeBridge::remote_log_(const char *source, const char *level, const char *message) {
  // Escape message for JSON (replace " with ' to avoid breaking JSON)
  std::string escaped;
  for (const char *p = message; *p; p++) {
    if (*p == '"') escaped += '\'';
    else if (*p == '\\') escaped += '/';
    else if (*p == '\n') escaped += ' ';
    else escaped += *p;
  }
  char body[512];
  snprintf(body, sizeof(body), "{\"source\":\"%s\",\"level\":\"%s\",\"message\":\"%s\"}", source, level, escaped.c_str());
  std::string path = "/api/users/" + user_id_ + "/logs";
  http_post_(path, body);
}

void PineTimeBridge::poll_api_() {
  poll_count_++;
  ESP_LOGD(TAG, "[API] Poll #%u: fetching reminders for user %s...", poll_count_, user_id_.c_str());
  std::string path = "/api/users/" + user_id_ + "/reminders";
  std::string response = http_get_(path);

  if (response.empty()) {
    ESP_LOGW(TAG, "[API] Empty or failed response for reminders");
    server_reachable_ = false;
    remote_log_("bridge", "error", "Failed to fetch reminders from server");
    return;
  }

  cJSON *root = cJSON_Parse(response.c_str());
  if (root == nullptr) {
    ESP_LOGW(TAG, "Failed to parse API response");
    return;
  }

  api_reminders_.clear();
  int count = cJSON_GetArraySize(root);
  for (int i = 0; i < count; i++) {
    cJSON *item = cJSON_GetArrayItem(root, i);
    ApiReminder r;
    r.db_id = cJSON_GetObjectItem(item, "id") ? cJSON_GetObjectItem(item, "id")->valueint : 0;
    r.reminder_id = cJSON_GetObjectItem(item, "reminder_id") ? cJSON_GetObjectItem(item, "reminder_id")->valueint : 0;
    r.hours = cJSON_GetObjectItem(item, "hours") ? cJSON_GetObjectItem(item, "hours")->valueint : 0;
    r.minutes = cJSON_GetObjectItem(item, "minutes") ? cJSON_GetObjectItem(item, "minutes")->valueint : 0;
    r.recurrence = cJSON_GetObjectItem(item, "recurrence") ? cJSON_GetObjectItem(item, "recurrence")->valueint : 0;
    r.priority = cJSON_GetObjectItem(item, "priority") ? cJSON_GetObjectItem(item, "priority")->valueint : 1;
    r.month = cJSON_GetObjectItem(item, "month") ? cJSON_GetObjectItem(item, "month")->valueint : 0;
    r.day = cJSON_GetObjectItem(item, "day") ? cJSON_GetObjectItem(item, "day")->valueint : 0;
    r.message = cJSON_GetObjectItem(item, "message") ? cJSON_GetObjectItem(item, "message")->valuestring : "";
    r.enabled = cJSON_GetObjectItem(item, "enabled") ? cJSON_IsTrue(cJSON_GetObjectItem(item, "enabled")) : true;
    api_reminders_.push_back(r);
  }

  cJSON_Delete(root);
  server_reachable_ = true;
  ESP_LOGD(TAG, "[API] Fetched %u reminders from server", api_reminders_.size());

  // First successful poll after boot
  if (last_sync_hash_ == 0) {
    char logmsg[128];
    snprintf(logmsg, sizeof(logmsg), "Bridge online, fetched %u reminders", (unsigned)api_reminders_.size());
    remote_log_("bridge", "info", logmsg);

    // Restore paired watch address from server if NVS didn't have it
    if (paired_address_ == 0) {
      restore_paired_address_from_server_();
    }

    // TODO: auto-connect to read watch info on boot
    // Disabled for now — the competing state machines cause a connect/disconnect loop
  }

  // Also check for pending notifications
  path = "/api/users/" + user_id_ + "/notifications/pending";
  response = http_get_(path);
  if (!response.empty()) {
    root = cJSON_Parse(response.c_str());
    if (root != nullptr) {
      count = cJSON_GetArraySize(root);
      for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);
        const char *msg = cJSON_GetObjectItem(item, "message") ? cJSON_GetObjectItem(item, "message")->valuestring : "";
        int64_t notif_id = cJSON_GetObjectItem(item, "id") ? cJSON_GetObjectItem(item, "id")->valueint : 0;

        if (strlen(msg) > 0) {
          if (ans_handle_ == 0) {
            // Not connected — trigger connection to deliver notification
            ESP_LOGI(TAG, "[NOTIFY] Pending notification, need BLE connection");
            needs_ble_sync_ = true;
            cJSON_Delete(root);
            return;  // will send on next connection
          }
          send_notification_(msg);
          // Mark as delivered on server
          char id_str[32];
          snprintf(id_str, sizeof(id_str), "%" PRId64, notif_id);
          std::string mark_path = "/api/notifications/" + std::string(id_str) + "/delivered";
          http_post_(mark_path, "{}");
          // Log to server
          char logmsg[128];
          snprintf(logmsg, sizeof(logmsg), "Notification delivered to watch: \"%s\"", msg);
          remote_log_("bridge", "info", logmsg);
        }
      }
      cJSON_Delete(root);
    }
  }
}

void PineTimeBridge::sync_reminders_to_watch_() {
  // Build sync packet to compute hash (even if not connected yet)
  uint8_t count = static_cast<uint8_t>(std::min(api_reminders_.size(), (size_t)56));
  std::vector<uint8_t> data;
  data.push_back(count);

  for (uint8_t i = 0; i < count; i++) {
    WatchReminder wr = api_reminders_[i].to_watch_reminder();
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&wr);
    data.insert(data.end(), bytes, bytes + sizeof(WatchReminder));
  }

  // Hash check — skip if data identical to last sync
  uint32_t hash = 5381;
  for (auto b : data) {
    hash = ((hash << 5) + hash) + b;
  }
  if (hash == last_sync_hash_) {
    ESP_LOGD(TAG, "[SYNC] Reminders unchanged (hash=0x%08x), no sync needed", hash);
    return;
  }
  ESP_LOGI(TAG, "[SYNC] Reminders changed (hash 0x%08x -> 0x%08x), %u reminders:", last_sync_hash_, hash, count);
  for (const auto &r : api_reminders_) {
    ESP_LOGI(TAG, "[SYNC]   #%u %02u:%02u \"%s\" pri=%u recur=0x%02x en=%d",
             r.reminder_id, r.hours, r.minutes, r.message.c_str(), r.priority, r.recurrence, r.enabled);
  }

  // Only queue the BLE commands if we have handles (connected)
  if (upload_handle_ == 0) {
    ESP_LOGD(TAG, "[SYNC] Not connected yet, will sync after connection");
    return;  // Don't update hash — sync hasn't happened yet
  }

  last_sync_hash_ = hash;  // update only after commands are actually queued

  // Clear all reminders on watch first (write 0xFF to delete char = delete all)
  if (delete_handle_ != 0) {
    BleCommand del_cmd;
    del_cmd.type = BleCommandType::DELETE_REMINDER;
    del_cmd.data = {0xFF};  // 0xFF = delete all
    command_queue_.push(std::move(del_cmd));
  }

  // Upload each reminder individually (72 bytes each, fits in MTU)
  for (uint8_t i = 0; i < count; i++) {
    WatchReminder wr = api_reminders_[i].to_watch_reminder();
    BleCommand cmd;
    cmd.type = BleCommandType::WRITE_REMINDER;
    cmd.data.assign(reinterpret_cast<const uint8_t*>(&wr),
                    reinterpret_cast<const uint8_t*>(&wr) + sizeof(WatchReminder));
    command_queue_.push(std::move(cmd));
  }

  ESP_LOGI(TAG, "[SYNC] Queued delete-all + %u individual reminder uploads", count);
  char logmsg[128];
  snprintf(logmsg, sizeof(logmsg), "Syncing %u reminders to watch (individual writes)", count);
  remote_log_("bridge", "info", logmsg);
}

void PineTimeBridge::send_heartbeat_() {
  ESP_LOGI(TAG, "[HEARTBEAT] Sending status (connected=%d)", services_discovered_);
  // Get our IP address
  esp_netif_ip_info_t ip_info;
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  char ip_str[16] = "";
  if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
  }
  bool watch_seen = services_discovered_ || watch_battery_ > 0;
  char body[512];
  snprintf(body, sizeof(body),
           "{\"connected\":%s,\"watch_battery\":%u,\"last_sync\":\"%s\",\"bridge_ip\":\"%s\","
           "\"watch_firmware\":\"%s\",\"watch_manufacturer\":\"%s\",\"watch_software\":\"%s\",\"watch_steps\":%u,\"watch_uptime\":%u}",
           watch_seen ? "true" : "false",
           watch_battery_,
           last_sync_time_.c_str(),
           ip_str,
           watch_firmware_.c_str(),
           watch_manufacturer_.c_str(),
           watch_software_.c_str(),
           watch_steps_,
           watch_uptime_);

  std::string path = "/api/bridges/" + bridge_id_ + "/status";
  http_post_(path, body);
}

void PineTimeBridge::sync_time_() {
  if (cts_handle_ == 0) {
    ESP_LOGW(TAG, "CTS handle not found, skipping time sync");
    return;
  }

  auto now = esphome::ESPTime::from_epoch_local(::time(nullptr));
  if (!now.is_valid()) {
    ESP_LOGW(TAG, "Time not valid yet, skipping sync");
    return;
  }

  // InfiniTime CTS format: year(2) month(1) day(1) hour(1) min(1) sec(1) weekday(1) fractions(1) reason(1)
  uint8_t data[10];
  uint16_t year = now.year;
  data[0] = year & 0xFF;
  data[1] = (year >> 8) & 0xFF;
  data[2] = now.month;
  data[3] = now.day_of_month;
  data[4] = now.hour;
  data[5] = now.minute;
  data[6] = now.second;
  data[7] = now.day_of_week; // 0=Sunday in InfiniTime
  data[8] = 0;               // fractions256
  data[9] = 0;               // reason

  pending_writes_++;
  auto status = esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(), cts_handle_,
      sizeof(data), data, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);

  if (status == ESP_OK) {
    ESP_LOGI(TAG, "[TIME] Synced time to watch: %04d-%02d-%02d %02d:%02d:%02d",
             now.year, now.month, now.day_of_month, now.hour, now.minute, now.second);
    last_watch_poll_ms_ = millis();
  } else {
    pending_writes_--;
  }
}

void PineTimeBridge::send_notification_(const std::string &message, uint8_t category) {
  if (ans_handle_ == 0) return;

  // ANS format: category(1) count(1) reserved(1) message...
  std::vector<uint8_t> data;
  data.push_back(category); // Schedule category
  data.push_back(0x01);     // count
  data.push_back(0x00);     // reserved

  size_t msg_len = std::min(message.size(), (size_t)97); // 100 - 3 header bytes
  data.insert(data.end(), message.begin(), message.begin() + msg_len);

  pending_writes_++;

  ESP_LOGI(TAG, "[NOTIFY] Writing %u bytes to ANS handle 0x%04x: cat=%u msg=\"%s\"",
           data.size(), ans_handle_, category, message.c_str());
  auto status = esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(), this->parent()->get_conn_id(), ans_handle_,
      data.size(), data.data(), ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);

  if (status == ESP_OK) {
    ESP_LOGI(TAG, "[NOTIFY] Write queued OK");
  } else {
    ESP_LOGW(TAG, "[NOTIFY] Write failed: %d", status);
    pending_writes_--;
  }
}

void PineTimeBridge::on_ack_notification_(const uint8_t *data, size_t len) {
  if (len < 5) {
    ESP_LOGW(TAG, "Ack notification too short: %u", len);
    return;
  }

  uint8_t reminder_id = data[0];
  uint32_t timestamp = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);

  ESP_LOGI(TAG, "[BLE] Received ack from watch: reminder #%u, timestamp %u", reminder_id, timestamp);

  // Forward to API
  char body[128];
  snprintf(body, sizeof(body),
           "{\"reminder_id\":%u,\"acked_at\":\"%u\"}",
           reminder_id, timestamp);

  std::string path = "/api/users/" + user_id_ + "/acks";
  ESP_LOGI(TAG, "[ACK] Forwarding ack for reminder #%u to server", reminder_id);
  http_post_(path, body);

  // Set event flag for display notification
  ack_event_ = true;
  // Find the reminder message for display
  for (const auto &r : api_reminders_) {
    if (r.reminder_id == reminder_id) {
      last_ack_message_ = r.message;
      break;
    }
  }
}

}  // namespace pinetime_bridge
}  // namespace esphome
