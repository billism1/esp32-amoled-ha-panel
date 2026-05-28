#include "cst9220_touchscreen.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace cst9220 {

void CST9220Touchscreen::setup() {
  ESP_LOGCONFIG(TAG, "Setting up CST9220 Touchscreen...");

  uint8_t data[8] = {0};

  this->reset_device_();
  vTaskDelay(pdMS_TO_TICKS(30));   // SensorLib waits 30ms to exit boot mode

  // Enter command mode: write 2 raw bytes 0xD1, 0x01 (NOT write_register16,
  // which would send 4 bytes and confuse the chip).
  uint8_t cmd_mode[2] = { 0xD1, 0x01 };
  this->write(cmd_mode, 2);
  vTaskDelay(pdMS_TO_TICKS(10));

  // Read checkcode (4 bytes from 0xD1FC). read_register16 writes the 2-byte
  // addr then reads N — same wire pattern as SensorLib's writeThenRead.
  this->read_register16(0xD1FC, data, 4);
  ESP_LOGI(TAG, "Checkcode: %02X %02X %02X %02X", data[0], data[1], data[2], data[3]);

  // Read resolution (4 bytes from 0xD1F8).
  this->read_register16(0xD1F8, data, 4);
  this->touch_res_x_ = (data[1] << 8) | data[0];
  this->touch_res_y_ = (data[3] << 8) | data[2];
  this->x_raw_min_ = 0;
  this->y_raw_min_ = 0;
  this->x_raw_max_ = this->touch_res_x_;
  this->y_raw_max_ = this->touch_res_y_;
  ESP_LOGI(TAG, "Resolution X: %d, Y: %d", this->touch_res_x_, this->touch_res_y_);

  // Read project_id + chip type (4 bytes from 0xD204).
  this->read_register16(0xD204, data, 4);
  this->chip_id_ = (data[3] << 8) | data[2];
  this->touch_project_id_ = (data[1] << 8) | data[0];
  ESP_LOGI(TAG, "Chip Type: 0x%04X (expected 0x%04X), ProjectID: 0x%04X",
           this->chip_id_, CST9220_CHIP_ID, this->touch_project_id_);

  // Read firmware version (8 bytes from 0xD208) — SensorLib does this and
  // it appears to be load-bearing for the chip to enter scan mode cleanly.
  this->read_register16(0xD208, data, 8);
  ESP_LOGI(TAG, "FW version: %02X%02X%02X%02X  checksum: %02X%02X%02X%02X",
           data[3], data[2], data[1], data[0],
           data[7], data[6], data[5], data[4]);

  // No explicit exit-cmd-mode write — chip transitions to normal scan
  // automatically on first DATA_REG (0xD000) access.
  vTaskDelay(pdMS_TO_TICKS(10));

  if (this->interrupt_pin_ != nullptr) {
    this->interrupt_pin_->setup();
    this->attach_interrupt_(this->interrupt_pin_, gpio::INTERRUPT_FALLING_EDGE);
    ESP_LOGV(TAG, "Attached Interrupt Pin: %d", this->interrupt_pin_);
  }

  ESP_LOGCONFIG(TAG, "CST9220 Touchscreen initialized");
}

void CST9220Touchscreen::dump_config() {
  ESP_LOGCONFIG(TAG, "CST9220 Touchscreen:");
  LOG_I2C_DEVICE(this);
  LOG_PIN("  Interrupt Pin: ", this->interrupt_pin_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  ESP_LOGCONFIG(TAG, "  Resolution X: %d, Y: %d", this->touch_res_x_, this->touch_res_y_);
  ESP_LOGCONFIG(TAG, "  Chip Type: 0x%04X, ProjectID: 0x%04X", this->chip_id_, this->touch_project_id_);
}

void CST9220Touchscreen::update_touches() {
  uint8_t data[CST9220_DATA_LENGTH] = {0};

  i2c::ErrorCode rc = this->read_register16(ESP_LCD_TOUCH_CST9220_DATA_REG, data, sizeof(data));
  if (rc != i2c::ErrorCode::NO_ERROR) {
      ESP_LOGW(TAG, "I2C read failed: rc=%d", (int)rc);
      this->status_set_warning("CST9220 Touchscreen: Failed to read touch data");
      return;
  }

  // ACK back to chip so it releases the data buffer for the next scan.
  // Per lewisxhe SensorLib: write 0xD0, 0x00, 0xAB after every read.
  // Without this, chip refuses to refill DATA_REG and we see all zeros forever.
  uint8_t ack_cmd[3] = { 0xD0, 0x00, CST9220_ACK_VALUE };
  this->write(ack_cmd, sizeof(ack_cmd));

  if (data[6] != CST9220_ACK_VALUE) {
      ESP_LOGV(TAG, "Idle read: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
               data[0], data[1], data[2], data[3], data[4],
               data[5], data[6], data[7], data[8], data[9]);
      return;
  }

  this->status_clear_warning();

  uint8_t points = data[5] & 0x7F;
  points = (points > CST9220_MAX_TOUCH_POINTS) ? CST9220_MAX_TOUCH_POINTS : points;
  for (int i = 0; i < points; i++) {
      uint8_t *p = &data[i * 5 + (i ? 2 : 0)];
      uint8_t status = p[0] & 0x0F;

      int16_t x = ((p[1] << 4) | (p[3] >> 4));
      int16_t y = ((p[2] << 4) | (p[3] & 0x0F));
      if (status == 0x06) {
          ESP_LOGD(TAG, "Press: X=%d Y=%d", x, y);
          this->add_raw_touch_position_(i, x, y);
      } else {
          // 0x00 / 0x0B = release events; chip emits them after every press.
          ESP_LOGV(TAG, "Frame status=0x%02X X=%d Y=%d (release)", status, x, y);
      }
  }
}

void CST9220Touchscreen::reset_device_() {
  if (this->reset_pin_ != nullptr) {
    ESP_LOGD(TAG, "Resetting CST9220 Touchscreen...");
    this->reset_pin_->digital_write(false);
    vTaskDelay(pdMS_TO_TICKS(10));
    this->reset_pin_->digital_write(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGD(TAG, "CST9220 Touchscreen reset complete");
  } else {
    ESP_LOGD(TAG, "No reset pin configured, skipping reset");
  }
}

} // namespace cst9220
} // namespace esphome
