#include "axp2101.h"
#include "esphome/core/log.h"

namespace esphome {
namespace axp2101 {

static const char *const TAG = "axp2101";

void AXP2101Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up AXP2101...");

  // Read the ADC channel control register, OR in bit 0 (VBAT enable), write back.
  // Preserves whatever else the bootloader / Waveshare firmware left enabled
  // (TS pin, die-temp, etc).
  uint8_t adc_ctrl = 0;
  if (!this->read_byte(AXP2101_REG_ADC_CHANNEL_CTRL, &adc_ctrl)) {
    ESP_LOGE(TAG, "ADC_CHANNEL_CTRL read failed — check wiring (addr 0x34)");
    this->mark_failed();
    return;
  }
  if (!(adc_ctrl & 0x01)) {
    adc_ctrl |= 0x01;
    if (!this->write_byte(AXP2101_REG_ADC_CHANNEL_CTRL, adc_ctrl)) {
      ESP_LOGE(TAG, "ADC_CHANNEL_CTRL write failed");
      this->mark_failed();
      return;
    }
  }
  this->ok_ = true;
  ESP_LOGCONFIG(TAG, "AXP2101 ready (ADC_CHANNEL_CTRL = 0x%02X)", adc_ctrl);
}

void AXP2101Component::dump_config() {
  ESP_LOGCONFIG(TAG, "AXP2101:");
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "Battery Voltage", this->battery_voltage_);
}

void AXP2101Component::update() {
  if (!this->ok_)
    return;

  uint8_t buf[2];
  if (!this->read_bytes(AXP2101_REG_BAT_AVER_VOL_H, buf, 2)) {
    ESP_LOGW(TAG, "VBAT read failed");
    return;
  }
  const uint16_t raw = ((uint16_t)(buf[0] & 0x3F) << 8) | buf[1];
  const float volts = raw / 1000.0f;

  if (this->battery_voltage_ != nullptr)
    this->battery_voltage_->publish_state(volts);
  ESP_LOGD(TAG, "VBAT = %.3f V (raw %u)", volts, (unsigned) raw);
}

}  // namespace axp2101
}  // namespace esphome
