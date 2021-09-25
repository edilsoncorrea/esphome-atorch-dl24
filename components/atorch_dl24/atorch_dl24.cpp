#include "atorch_dl24.h"
#include "esphome/core/log.h"

// @FIXME: This define should be used as soon as esphome 2021.10 is released
// #ifdef USE_ESP32
#ifdef ARDUINO_ARCH_ESP32  // NOLINT

namespace esphome {
namespace atorch_dl24 {

static const char *TAG = "atorch_dl24";

static const uint16_t DL24_SERVICE_UUID = 0xFFE0;
static const uint16_t DL24_CHARACTERISTIC_UUID = 0xFFE1;

void AtorchDL24::dump_config() {
  ESP_LOGCONFIG(TAG, "DL24");
  LOG_SENSOR(" ", "Battery", this->battery_);
}

void AtorchDL24::setup() { this->current_sensor_ = 0; }

void AtorchDL24::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                     esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
      this->node_state = espbt::ClientState::Idle;
      if (this->battery_ != nullptr)
        this->battery_->publish_state(NAN);
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto chr = this->parent_->get_characteristic(DL24_SERVICE_UUID, DL24_CHARACTERISTIC_UUID);
      if (chr == nullptr) {
        ESP_LOGE(TAG, "[%s] No control service found at device, not an DL24..?", this->parent_->address_str().c_str());
        break;
      }
      this->char_handle_ = chr->handle;

      auto status =
          esp_ble_gattc_register_for_notify(this->parent()->gattc_if, this->parent()->remote_bda, chr->handle);
      if (status) {
        ESP_LOGW(TAG, "esp_ble_gattc_register_for_notify failed, status=%d", status);
      }
      break;
    }
    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      this->node_state = espbt::ClientState::Established;
      break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->char_handle_)
        break;

      ESP_LOGVV(TAG, "Notification received: %s", hexencode(param->notify.value, param->notify.value_len + 0).c_str());
      this->decode(param->notify.value, param->notify.value_len);

      break;
    }
    default:
      break;
  }
}

void AtorchDL24::decode(const uint8_t *data, uint16_t length) {
  auto dl24_get_16bit = [&](size_t i) -> uint16_t {
    return (uint16_t(data[i + 0]) << 8) | (uint16_t(data[i + 1]) << 0);
  };
  auto dl24_get_24bit = [&](size_t i) -> uint32_t {
    return (uint32_t(data[i + 0]) << 16) | (uint32_t(data[i + 1]) << 8) | (uint32_t(data[i + 2]) << 0);
  };
  auto dl24_get_32bit = [&](size_t i) -> uint32_t {
    return (uint32_t(dl24_get_16bit(i + 0)) << 16) | (uint32_t(dl24_get_16bit(i + 2)) << 0);
  };

  if (length != 36) {
    ESP_LOGW(TAG, "Frame skipped because of invalid length");
    return;
  }

  if (data[0] != 0xFF && data[1] != 0x55) {
    ESP_LOGW(TAG, "Invalid header");
    return;
  }

  if (data[2] != 0x01) {
    ESP_LOGW(TAG, "Unsupported message type (%02X)", data[2]);
    return;
  }

  if (data[3] != 0x01 && data[3] != 0x02) {
    ESP_LOGW(TAG, "Unsupported device type (%02X)", data[3]);
    return;
  }

  ESP_LOGD(TAG, "New measurement:");

  // Example responses
  //
  // FF.55.01.02.00.00.20.00.4E.20.00.13.FA.00.00.00.11.00.00.00.00.00.00.00.00.25.00.02.21.1A.3C.00.00.00.00.09
  // FF.55.01.02.00.00.20.00.4E.1E.00.13.FA.00.00.00.11.00.00.00.00.00.00.00.00.25.00.02.21.1B.3C.00.00.00.00.08
  // FF.55.01.02.00.00.20.00.4E.21.00.13.FB.00.00.00.11.00.00.00.00.00.00.00.00.25.00.02.21.1C.3C.00.00.00.00.15
  // FF.55.01.02.00.00.20.00.4E.20.00.13.FC.00.00.00.11.00.00.00.00.00.00.00.00.25.00.02.21.1D.3C.00.00.00.00.16
  // FF.55.01.02.00.00.20.00.4E.1B.00.13.FC.00.00.00.11.00.00.00.00.00.00.00.00.25.00.02.21.1E.3C.00.00.00.00.0A
  // FF.55.01.02.00.00.20.00.4E.23.00.13.FD.00.00.00.11.00.00.00.00.00.00.00.00.25.00.02.21.1F.3C.00.00.00.00.1C
  //
  // 0xFF 0x55:            Magic header
  // 0x01:                 Message type           1: Report (32 byte), 2: Reply (4 byte), 11: Command (6 byte)
  // 0x02:                 Device type            1: AC meter, 2: DC meter, 3: USB meter
  // 0x00 0x00 0x20:       Voltage                32 * 0.1 = 3.2 V
  ESP_LOGD(TAG, "  Voltage: %f", dl24_get_24bit(4) * 0.1f);

  // 0x00 0x4E 0x23:       Current                20003 * 0.001 = 20.003 A
  ESP_LOGD(TAG, "  Current: %f", dl24_get_24bit(7) * 0.001f);

  // 0x00 0x13 0xFD:       Power                  5117 * 0.1 = 511.7 W
  ESP_LOGD(TAG, "  Power: %f", dl24_get_24bit(10) * 0.1f);

  // 0x00 0x00 0x00 0x11:  Energy in Wh           17 * 10.0 = 170.0
  ESP_LOGD(TAG, "  Energy: %f", dl24_get_32bit(13) * 10.0f);

  // 0x00 0x00 0x00:       Price per kWh
  // 0x00 0x00 0x00 0x00:  Unknown
  // 0x00 0x25:            Temperature            25 °C
  ESP_LOGD(TAG, "  Temperature: %f", (float) dl24_get_16bit(24));

  // 0x00 0x02:            Hour                   2 h
  ESP_LOGD(TAG, "  Hour: %f", (float) dl24_get_16bit(26));

  // 0x21:                 Minute                      33 min
  ESP_LOGD(TAG, "  Minute: %f", (float) data[28]);

  // 0x1F:                 Second                      31 sec
  ESP_LOGD(TAG, "  Second: %f", (float) data[29]);

  // 0x3C:                 Backlight                   63 %
  ESP_LOGD(TAG, "  Backlight: %f\n", (float) data[30]);

  // 0x00 0x00 0x00 0x00:  Unknown
  // 0x1C:                 Checksum

  //      if (this->battery_ != nullptr) {
  //        this->battery_->publish_state(1.0);
  //      }
}

void AtorchDL24::update() {}

}  // namespace atorch_dl24
}  // namespace esphome

#endif