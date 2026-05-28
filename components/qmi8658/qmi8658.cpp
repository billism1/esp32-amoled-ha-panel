#include "qmi8658.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include <cmath>

namespace esphome {
namespace qmi8658 {

static const char *const TAG = "qmi8658";

void QMI8658Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up QMI8658...");

  uint8_t who = 0;
  if (!this->read_byte(QMI8658_REG_WHOAMI, &who)) {
    ESP_LOGE(TAG, "WHO_AM_I read failed — check wiring");
    this->mark_failed();
    return;
  }
  if (who != QMI8658_WHOAMI_EXPECTED) {
    ESP_LOGE(TAG, "WHO_AM_I = 0x%02X (expected 0x%02X)", who, QMI8658_WHOAMI_EXPECTED);
    this->mark_failed();
    return;
  }

  // CTRL1 = 0x40 → SPI 4-wire (don't care on I2C) + ADDR_AI auto-increment for
  // burst reads. SensorLib uses 0x60; bit5 INT2 enable is irrelevant here.
  this->write_byte(QMI8658_REG_CTRL1, 0x40);
  // CTRL2 = ±4g, 31.25Hz low-power accel ODR.
  this->write_byte(QMI8658_REG_CTRL2, QMI8658_CTRL2_4G_31HZ);
  // CTRL7 = enable accel only. Gyro off → ~1/3 the current.
  this->write_byte(QMI8658_REG_CTRL7, 0x01);

  this->ok_ = true;
  ESP_LOGCONFIG(TAG, "QMI8658 ready (WHOAMI 0x%02X)", who);
}

void QMI8658Component::dump_config() {
  ESP_LOGCONFIG(TAG, "QMI8658:");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG, "  motion threshold: %.2f g", this->motion_threshold_);
  ESP_LOGCONFIG(TAG, "  motion hold:      %u ms", (unsigned) this->motion_hold_ms_);
  LOG_UPDATE_INTERVAL(this);
}

bool QMI8658Component::read_accel_g_(float &ax, float &ay, float &az) {
  uint8_t status = 0;
  if (!this->read_byte(QMI8658_REG_STATUS0, &status))
    return false;
  if (!(status & 0x01))
    return false;  // accel data not ready yet

  uint8_t buf[6];
  if (!this->read_bytes(QMI8658_REG_AX_L, buf, 6))
    return false;
  int16_t rx = (int16_t)((buf[1] << 8) | buf[0]);
  int16_t ry = (int16_t)((buf[3] << 8) | buf[2]);
  int16_t rz = (int16_t)((buf[5] << 8) | buf[4]);
  ax = rx / QMI8658_ACC_LSB_PER_G;
  ay = ry / QMI8658_ACC_LSB_PER_G;
  az = rz / QMI8658_ACC_LSB_PER_G;
  return true;
}

void QMI8658Component::update() {
  if (!this->ok_)
    return;

  float ax, ay, az;
  if (!this->read_accel_g_(ax, ay, az))
    return;

  const uint32_t now = millis();

  if (!this->have_baseline_) {
    this->last_ax_ = ax;
    this->last_ay_ = ay;
    this->last_az_ = az;
    this->have_baseline_ = true;
    return;
  }

  const float dx = ax - this->last_ax_;
  const float dy = ay - this->last_ay_;
  const float dz = az - this->last_az_;
  const float mag = std::sqrt(dx * dx + dy * dy + dz * dz);

  this->last_ax_ = ax;
  this->last_ay_ = ay;
  this->last_az_ = az;

  if (mag > this->motion_threshold_) {
    this->motion_off_at_ = now + this->motion_hold_ms_;
    if (!this->motion_state_) {
      this->motion_state_ = true;
      if (this->motion_ != nullptr)
        this->motion_->publish_state(true);
      ESP_LOGI(TAG, "motion: delta=%.3f g (threshold %.2f)", mag, this->motion_threshold_);
    }
  } else if (this->motion_state_ && (int32_t)(now - this->motion_off_at_) >= 0) {
    this->motion_state_ = false;
    if (this->motion_ != nullptr)
      this->motion_->publish_state(false);
  }
}

}  // namespace qmi8658
}  // namespace esphome
