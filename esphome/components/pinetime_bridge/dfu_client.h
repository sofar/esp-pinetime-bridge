#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <esp_gattc_api.h>
#include <esp_gap_ble_api.h>

namespace esphome {
namespace pinetime_bridge {

enum class DfuState {
  IDLE,
  DOWNLOADING,        // Downloading firmware from server
  CONNECTING,         // Connecting to watch
  DISCOVERING,        // Discovering DFU service
  STARTING,           // Sending Start DFU command
  SENDING_INIT,       // Sending init packet (.dat)
  SENDING_FIRMWARE,   // Streaming firmware .bin in 20-byte chunks
  VALIDATING,         // Waiting for firmware validation
  ACTIVATING,         // Sending activate & reset
  COMPLETE,
  FAILED
};

class DfuClient {
 public:
  void start(const std::string &api_base_url);
  void on_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
  void on_services_discovered(uint16_t ctrl_handle, uint16_t pkt_handle);
  void process();  // Call from loop

  DfuState state() const { return state_; }
  float progress() const;  // 0.0 to 1.0
  const char *state_str() const;
  const char *error() const { return error_msg_; }

  // Set by parent for BLE writes
  std::function<esp_err_t(uint16_t handle, const uint8_t *data, size_t len, bool response)> write_char;
  std::function<esp_err_t(uint16_t handle)> register_notify;

 private:
  DfuState state_ = DfuState::IDLE;
  const char *error_msg_ = "";

  // Firmware data (allocated in PSRAM)
  uint8_t *bin_data_ = nullptr;
  size_t bin_size_ = 0;
  uint8_t *dat_data_ = nullptr;
  size_t dat_size_ = 0;

  // BLE handles
  uint16_t ctrl_handle_ = 0;  // Control Point
  uint16_t pkt_handle_ = 0;   // Packet

  // Transfer state
  size_t bytes_sent_ = 0;
  uint32_t packets_since_receipt_ = 0;
  static constexpr uint32_t RECEIPT_INTERVAL = 10;
  static constexpr size_t CHUNK_SIZE = 20;
  bool waiting_for_notification_ = false;

  // HTTP download
  bool download_firmware_(const std::string &api_base_url);

  // DFU protocol steps
  void send_start_dfu_();
  void send_init_packet_();
  void send_firmware_chunks_();
  void send_validate_();
  void send_activate_();

  void handle_notification_(const uint8_t *data, size_t len);
  void fail_(const char *msg);
};

}  // namespace pinetime_bridge
}  // namespace esphome
