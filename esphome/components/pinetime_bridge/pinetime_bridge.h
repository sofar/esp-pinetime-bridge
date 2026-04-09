#pragma once

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include <esp_gap_ble_api.h>
#include <string>
#include <vector>
#include <queue>
#include "dfu_client.h"

namespace esphome {
namespace pinetime_bridge {

// Must match the InfiniTime Reminder struct exactly (72 bytes)
struct __attribute__((packed)) WatchReminder {
  uint8_t version = 3;
  uint8_t id = 0;           // 0-19
  uint8_t hours = 0;        // 0-23
  uint8_t minutes = 0;      // 0-59
  uint8_t recurrence = 0;   // 0x00=once, 0x80=daily, bits 0-6=weekday bitmask
  uint8_t flags = 0;        // bit0=enabled, bits 1-2=priority (0-2)
  uint8_t month = 0;        // 1-12 for specific date, 0=ignore
  uint8_t day = 0;          // 1-31 for specific date, 0=ignore
  char message[64] = {0};
};

// Reminder from API (parsed from JSON)
struct ApiReminder {
  int64_t db_id = 0;
  uint8_t reminder_id = 0;
  uint8_t hours = 0;
  uint8_t minutes = 0;
  uint8_t recurrence = 0;
  uint8_t priority = 1;
  uint8_t month = 0;
  uint8_t day = 0;
  std::string message;
  bool enabled = true;

  WatchReminder to_watch_reminder() const;
};

enum class BleCommandType { WRITE_REMINDER, DELETE_REMINDER, SYNC_ALL };

struct BleCommand {
  BleCommandType type;
  std::vector<uint8_t> data;
};

class PineTimeBridge : public Component, public ble_client::BLEClientNode {
  friend void bridge_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void set_api_base_url(const std::string &url) { api_base_url_ = url; }
  void set_user_id(const std::string &id) { user_id_ = id; }
  void set_bridge_id(const std::string &id) { bridge_id_ = id; }
  void set_poll_interval(uint32_t ms) { poll_interval_ms_ = ms; }

  // Watch info (populated on BLE connect)
  uint8_t watch_battery_ = 0;
  uint32_t watch_steps_ = 0;
  uint32_t watch_uptime_ = 0;
  std::string watch_firmware_;
  std::string watch_manufacturer_;
  std::string watch_software_;
  std::string watch_model_;
  std::string watch_hw_rev_;
  std::string watch_serial_;

  bool is_connected() const { return ble_connected_; }
  bool is_server_reachable() const { return server_reachable_; }
  void check_pairing_request_();
  std::string get_last_sync_time() const { return last_sync_time_.empty() ? "Never" : last_sync_time_; }
  size_t get_reminder_count() const { return api_reminders_.size(); }
  uint8_t get_watch_battery() const { return watch_battery_; }
  uint32_t get_watch_steps() const { return watch_steps_; }
  uint32_t get_watch_uptime() const { return watch_uptime_; }
  const std::string &get_watch_firmware() const { return watch_firmware_; }
  const std::string &get_watch_manufacturer() const { return watch_manufacturer_; }
  const std::string &get_watch_software() const { return watch_software_; }

  bool get_next_reminder(uint8_t &hours, uint8_t &minutes, std::string &message) const;
  bool watch_just_returned() { bool r = watch_returned_event_; watch_returned_event_ = false; return r; }
  bool has_pending_ack() { bool r = ack_event_; ack_event_ = false; return r; }
  std::string last_ack_message() const { return last_ack_message_; }

  void on_ble_advertise(uint64_t address, const std::string &name, int rssi);
  void post_discovered_watches_();

  // DFU
  void start_dfu();
  DfuClient &dfu() { return dfu_client_; };
  void set_paired_address(uint64_t addr);
  uint64_t get_paired_address() const { return paired_address_; }

 protected:
  // BLE characteristic handles (discovered on connect)
  uint16_t upload_handle_ = 0;   // Char 0001 - write reminder
  uint16_t delete_handle_ = 0;   // Char 0002 - delete reminder
  uint16_t list_handle_ = 0;     // Char 0003 - read all reminders
  uint16_t ack_handle_ = 0;      // Char 0004 - notify ack
  uint16_t sync_handle_ = 0;     // Char 0005 - sync all
  uint16_t status_handle_ = 0;   // Char 0006 - read uptime/status

  // ANS handles for one-off notifications
  uint16_t ans_handle_ = 0;

  // CTS handle for time sync
  uint16_t cts_handle_ = 0;

  // Watch info handles
  uint16_t battery_handle_ = 0;
  uint16_t fw_rev_handle_ = 0;
  uint16_t mfr_handle_ = 0;
  uint16_t sw_rev_handle_ = 0;
  uint16_t model_handle_ = 0;
  uint16_t hw_rev_handle_ = 0;
  uint16_t serial_handle_ = 0;
  uint16_t steps_handle_ = 0;
  uint16_t dfu_rev_handle_ = 0;

  // State (volatile: written by BLE callback task, read by main loop task)
  volatile bool services_discovered_ = false;
  volatile bool ble_busy_ = false;
  bool needs_ble_sync_ = false;      // true when we have data to push to watch
  bool ble_work_done_ = false;       // true when sync/acks complete, ready to disconnect
  volatile bool ble_connected_ = false;       // our own tracking of BLE connection state
  volatile uint8_t gap_event_pending_ = 0;  // deferred GAP events: 1=passkey, 2=paired, 3=failed
  uint8_t gap_auth_fail_reason_ = 0;
  volatile uint8_t pending_writes_ = 0;      // count of non-queue writes in flight
  uint32_t last_poll_ms_ = 0;
  uint32_t last_heartbeat_ms_ = 0;
  uint32_t ble_connect_time_ms_ = 0; // when we last connected, for idle disconnect
  uint32_t last_watch_poll_ms_ = 0;  // when we last connected to read watch data
  uint32_t last_watch_seen_ms_ = 0; // when scanner last saw the watch
  bool watch_was_away_ = false;     // true if watch wasn't seen for >5 min
  bool watch_returned_event_ = false; // set when watch returns after absence
  bool ack_event_ = false;           // set when reminder ack received
  std::string last_ack_message_;     // message text of last acked reminder
  bool boot_logged_ = false;        // true after first "Bridge online" log sent
  bool post_dfu_ = false;           // true after DFU, skip reminder sync for grace period
  uint32_t post_dfu_time_ms_ = 0;  // when DFU completed, for grace period timing
  std::string last_dfu_time_;      // ISO timestamp of last successful DFU

  // Config
  std::string api_base_url_;
  std::string user_id_;
  std::string bridge_id_;
  uint32_t poll_interval_ms_ = 60000;

  // Command queue
  std::queue<BleCommand> command_queue_;

  // Discovered watches from BLE scan
  struct DiscoveredWatch {
    uint64_t address;
    std::string name;
    int rssi;
    uint32_t last_seen;
    std::string mac_str;
  };
  std::vector<DiscoveredWatch> discovered_watches_;
  uint32_t last_discovery_post_ms_ = 0;

  // Cached API state
  std::vector<ApiReminder> api_reminders_;
  uint32_t last_sync_hash_ = 0;  // hash of last synced reminder set, skip BLE write if unchanged
  bool server_reachable_ = false;
  bool pairing_in_progress_ = false;
  uint64_t paired_address_ = 0;
  bool passkey_pending_ = false;
  uint16_t pairing_conn_handle_ = 0;
  uint32_t poll_count_ = 0;
  std::string last_sync_time_;   // ISO timestamp of last successful watch sync

  // Methods
  void discover_services_();
  void poll_api_();
  void sync_reminders_to_watch_();
  void send_heartbeat_();
  void sync_time_();
  void process_next_command_();
  void send_notification_(const std::string &message, uint8_t category = 0x07);
  void on_ack_notification_(const uint8_t *data, size_t len);

  // HTTP helpers
  std::string http_get_(const std::string &path);
  bool http_post_(const std::string &path, const std::string &body);
  void remote_log_(const char *source, const char *level, const char *message);
  void restore_paired_address_from_server_();

  // DFU
  DfuClient dfu_client_;
};

}  // namespace pinetime_bridge
}  // namespace esphome
