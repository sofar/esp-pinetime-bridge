#include "dfu_client.h"
#include "esphome/core/log.h"

#include <cstring>
#include <esp_http_client.h>
#include <esp_heap_caps.h>

namespace esphome {
namespace pinetime_bridge {

static const char *const TAG = "dfu_client";

// Nordic DFU opcodes
static constexpr uint8_t OP_START_DFU = 0x01;
static constexpr uint8_t OP_INIT_DFU_PARAMS = 0x02;
static constexpr uint8_t OP_RECEIVE_FW = 0x03;
static constexpr uint8_t OP_VALIDATE_FW = 0x04;
static constexpr uint8_t OP_ACTIVATE_RESET = 0x05;
static constexpr uint8_t OP_PKT_RECEIPT_REQ = 0x08;
static constexpr uint8_t OP_RESPONSE = 0x10;
static constexpr uint8_t OP_PKT_RECEIPT_NOTIF = 0x11;

static constexpr uint8_t IMAGE_TYPE_APP = 0x04;
static constexpr uint8_t RESULT_SUCCESS = 0x01;

const char *DfuClient::state_str() const {
  switch (state_) {
    case DfuState::IDLE: return "Idle";
    case DfuState::DOWNLOADING: return "Downloading firmware";
    case DfuState::CONNECTING: return "Connecting";
    case DfuState::DISCOVERING: return "Discovering services";
    case DfuState::STARTING: return "Starting DFU";
    case DfuState::SENDING_INIT: return "Sending init packet";
    case DfuState::SENDING_FIRMWARE: return "Uploading firmware";
    case DfuState::VALIDATING: return "Validating";
    case DfuState::ACTIVATING: return "Activating";
    case DfuState::COMPLETE: return "Complete";
    case DfuState::FAILED: return "Failed";
    default: return "Unknown";
  }
}

float DfuClient::progress() const {
  if (state_ != DfuState::SENDING_FIRMWARE || bin_size_ == 0) return 0.0f;
  return (float)bytes_sent_ / (float)bin_size_;
}

// HTTP download into PSRAM buffer
struct PsramBuf {
  uint8_t *data;
  size_t len;
  size_t capacity;
};

static esp_err_t http_dl_handler(esp_http_client_event_t *evt) {
  auto *buf = static_cast<PsramBuf *>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_DATA && buf && buf->data) {
    if (buf->len + evt->data_len <= buf->capacity) {
      memcpy(buf->data + buf->len, evt->data, evt->data_len);
      buf->len += evt->data_len;
    }
  }
  return ESP_OK;
}

static bool download_to_psram(const std::string &url, uint8_t *&out, size_t &out_size, size_t max_size) {
  // Pre-allocate in PSRAM
  out = (uint8_t *)heap_caps_malloc(max_size, MALLOC_CAP_SPIRAM);
  if (!out) {
    ESP_LOGE(TAG, "Failed to allocate %u bytes in PSRAM", max_size);
    return false;
  }

  PsramBuf buf = {out, 0, max_size};

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.event_handler = http_dl_handler;
  config.user_data = &buf;
  config.timeout_ms = 30000;
  config.buffer_size = 4096;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK || status != 200 || buf.len == 0) {
    ESP_LOGW(TAG, "Download %s failed: err=%d status=%d len=%u", url.c_str(), err, status, buf.len);
    heap_caps_free(out);
    out = nullptr;
    out_size = 0;
    return false;
  }

  out_size = buf.len;
  return true;
}

bool DfuClient::download_firmware_(const std::string &api_base_url) {
  // Free any previous data
  if (bin_data_) { heap_caps_free(bin_data_); bin_data_ = nullptr; bin_size_ = 0; }
  if (dat_data_) { heap_caps_free(dat_data_); dat_data_ = nullptr; dat_size_ = 0; }

  ESP_LOGI(TAG, "[DFU] Downloading firmware bin into PSRAM...");
  if (!download_to_psram(api_base_url + "/api/firmware/bin", bin_data_, bin_size_, 500000)) {
    fail_("Failed to download firmware.bin");
    return false;
  }
  ESP_LOGI(TAG, "[DFU] Downloaded bin: %u bytes (PSRAM)", bin_size_);

  ESP_LOGI(TAG, "[DFU] Downloading firmware dat...");
  if (!download_to_psram(api_base_url + "/api/firmware/dat", dat_data_, dat_size_, 1024)) {
    fail_("Failed to download firmware.dat");
    return false;
  }
  ESP_LOGI(TAG, "[DFU] Downloaded dat: %u bytes", dat_size_);

  return true;
}

void DfuClient::start(const std::string &api_base_url) {
  state_ = DfuState::DOWNLOADING;
  error_msg_ = "";
  bytes_sent_ = 0;
  packets_since_receipt_ = 0;
  waiting_for_notification_ = false;

  if (!download_firmware_(api_base_url)) {
    return;  // fail_ already called
  }

  state_ = DfuState::CONNECTING;
  ESP_LOGI(TAG, "[DFU] Firmware downloaded, ready for BLE connection");
}

void DfuClient::on_services_discovered(uint16_t ctrl_handle, uint16_t pkt_handle) {
  ctrl_handle_ = ctrl_handle;
  pkt_handle_ = pkt_handle;

  if (ctrl_handle_ == 0 || pkt_handle_ == 0) {
    fail_("DFU service not found on watch");
    return;
  }

  ESP_LOGI(TAG, "[DFU] DFU service found: ctrl=0x%04x pkt=0x%04x", ctrl_handle_, pkt_handle_);

  // Register for notifications on control point
  if (register_notify) {
    register_notify(ctrl_handle_);
  }

  state_ = DfuState::STARTING;
  send_start_dfu_();
}

void DfuClient::send_start_dfu_() {
  ESP_LOGI(TAG, "[DFU] Sending Start DFU (image type=app, size=%u)", bin_size_);

  // Step 1: Write Start DFU command to control point
  uint8_t cmd[2] = {OP_START_DFU, IMAGE_TYPE_APP};
  write_char(ctrl_handle_, cmd, sizeof(cmd), true);

  // Step 2: Write firmware sizes to packet characteristic
  // Format: [softdevice_size(4), bootloader_size(4), app_size(4)]
  uint32_t app_size = bin_size_;
  uint8_t sizes[12] = {0};
  memcpy(&sizes[8], &app_size, 4);  // little-endian app size at offset 8
  write_char(pkt_handle_, sizes, sizeof(sizes), false);

  waiting_for_notification_ = true;
}

void DfuClient::send_init_packet_() {
  ESP_LOGI(TAG, "[DFU] Sending init packet (%u bytes)", dat_size_);

  // Init DFU parameters - start
  uint8_t cmd_start[2] = {OP_INIT_DFU_PARAMS, 0x00};
  write_char(ctrl_handle_, cmd_start, sizeof(cmd_start), true);

  // Write dat contents to packet
  write_char(pkt_handle_, dat_data_, dat_size_, false);

  // Init DFU parameters - complete
  uint8_t cmd_complete[2] = {OP_INIT_DFU_PARAMS, 0x01};
  write_char(ctrl_handle_, cmd_complete, sizeof(cmd_complete), true);

  waiting_for_notification_ = true;
}

void DfuClient::send_firmware_chunks_() {
  if (waiting_for_notification_) return;

  // Send up to RECEIPT_INTERVAL chunks before waiting for receipt
  while (bytes_sent_ < bin_size_ && packets_since_receipt_ < RECEIPT_INTERVAL) {
    size_t remaining = bin_size_ - bytes_sent_;
    size_t chunk_len = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;

    write_char(pkt_handle_, bin_data_ + bytes_sent_, chunk_len, false);
    bytes_sent_ += chunk_len;
    packets_since_receipt_++;
  }

  if (bytes_sent_ >= bin_size_) {
    // All data sent, wait for final notification
    ESP_LOGI(TAG, "[DFU] All firmware data sent (%u bytes)", bytes_sent_);
    waiting_for_notification_ = true;
  } else if (packets_since_receipt_ >= RECEIPT_INTERVAL) {
    // Wait for packet receipt notification
    waiting_for_notification_ = true;
  }
}

void DfuClient::send_validate_() {
  ESP_LOGI(TAG, "[DFU] Requesting firmware validation");
  uint8_t cmd[1] = {OP_VALIDATE_FW};
  write_char(ctrl_handle_, cmd, sizeof(cmd), true);
  waiting_for_notification_ = true;
}

void DfuClient::send_activate_() {
  ESP_LOGI(TAG, "[DFU] Activating firmware and resetting watch");
  uint8_t cmd[1] = {OP_ACTIVATE_RESET};
  write_char(ctrl_handle_, cmd, sizeof(cmd), true);
  state_ = DfuState::COMPLETE;
}

void DfuClient::process() {
  if (state_ == DfuState::SENDING_FIRMWARE && !waiting_for_notification_) {
    send_firmware_chunks_();
  }
}

void DfuClient::on_gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
  if (state_ == DfuState::IDLE || state_ == DfuState::COMPLETE || state_ == DfuState::FAILED) return;

  switch (event) {
    case ESP_GATTC_NOTIFY_EVT:
      if (param->notify.handle == ctrl_handle_) {
        handle_notification_(param->notify.value, param->notify.value_len);
      }
      break;

    case ESP_GATTC_WRITE_CHAR_EVT:
      if (param->write.status != ESP_GATT_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "BLE write failed: handle=0x%04x status=%d", param->write.handle, param->write.status);
        fail_(msg);
      }
      break;

    case ESP_GATTC_DISCONNECT_EVT:
      if (state_ != DfuState::COMPLETE) {
        fail_("Watch disconnected during DFU");
      }
      break;

    default:
      break;
  }
}

void DfuClient::handle_notification_(const uint8_t *data, size_t len) {
  if (len < 1) return;
  waiting_for_notification_ = false;

  if (data[0] == OP_RESPONSE && len >= 3) {
    uint8_t req_op = data[1];
    uint8_t result = data[2];

    ESP_LOGI(TAG, "[DFU] Response: op=0x%02x result=0x%02x", req_op, result);

    if (result != RESULT_SUCCESS) {
      char msg[64];
      snprintf(msg, sizeof(msg), "DFU error: op=0x%02x result=0x%02x", req_op, result);
      fail_(msg);
      return;
    }

    switch (req_op) {
      case OP_START_DFU:
        // Start DFU acknowledged — send init packet
        state_ = DfuState::SENDING_INIT;
        send_init_packet_();
        break;

      case OP_INIT_DFU_PARAMS:
        // Init packet received — prepare to send firmware
        ESP_LOGI(TAG, "[DFU] Init accepted, starting firmware transfer");
        state_ = DfuState::SENDING_FIRMWARE;
        bytes_sent_ = 0;
        packets_since_receipt_ = 0;
        {
          // Request packet receipt notification every N packets
          uint8_t cmd[3] = {OP_PKT_RECEIPT_REQ, (uint8_t)(RECEIPT_INTERVAL & 0xFF), (uint8_t)(RECEIPT_INTERVAL >> 8)};
          write_char(ctrl_handle_, cmd, sizeof(cmd), true);
        }
        // Write receive firmware command
        {
          uint8_t cmd[1] = {OP_RECEIVE_FW};
          write_char(ctrl_handle_, cmd, sizeof(cmd), true);
        }
        // Start sending chunks
        send_firmware_chunks_();
        break;

      case OP_RECEIVE_FW:
        // All firmware received and CRC OK — validate
        state_ = DfuState::VALIDATING;
        send_validate_();
        break;

      case OP_VALIDATE_FW:
        // Firmware validated — activate
        state_ = DfuState::ACTIVATING;
        send_activate_();
        break;

      default:
        break;
    }
  } else if (data[0] == OP_PKT_RECEIPT_NOTIF && len >= 5) {
    // Packet receipt notification — contains bytes received so far
    uint32_t received = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
    float pct = bin_size_ > 0 ? (float)received / (float)bin_size_ * 100.0f : 0.0f;
    ESP_LOGI(TAG, "[DFU] Progress: %u / %u bytes (%.1f%%)", received, bin_size_, pct);
    packets_since_receipt_ = 0;
    // Continue sending
    send_firmware_chunks_();
  }
}

void DfuClient::fail_(const char *msg) {
  ESP_LOGE(TAG, "[DFU] FAILED: %s", msg);
  state_ = DfuState::FAILED;
  error_msg_ = msg;
}

}  // namespace pinetime_bridge
}  // namespace esphome
