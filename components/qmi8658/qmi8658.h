// qmi8658.h — minimal polling driver for QencheTech QMI8658 6-axis IMU.
// We only need: WHO_AM_I check, enable accel, periodic read, and a software
// "any-motion" binary sensor. Hardware INT line on the Waveshare 2.16 board
// isn't routed to a GPIO (verified against Waveshare pin_config.h), so we
// poll the accel registers and compute frame-to-frame magnitude delta.
//
// Register map: lewisxhe/SensorLib SensorQMI8658.hpp / QMI8658Constants.h.
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

namespace esphome {
namespace qmi8658 {

static const uint8_t QMI8658_REG_WHOAMI = 0x00;
static const uint8_t QMI8658_REG_CTRL1 = 0x02;   // serial-IF / auto-increment
static const uint8_t QMI8658_REG_CTRL2 = 0x03;   // accel scale + ODR
static const uint8_t QMI8658_REG_CTRL7 = 0x08;   // sensor enable
static const uint8_t QMI8658_REG_RESET = 0x60;
static const uint8_t QMI8658_REG_STATUS0 = 0x2E;
static const uint8_t QMI8658_REG_AX_L = 0x35;

static const uint8_t QMI8658_WHOAMI_EXPECTED = 0x05;

// CTRL2: bits[6:4]=aScale (001=±4g), bits[3:0]=aODR.
// Pick low-power 31.25Hz (0x0E) — plenty for pickup detection, minimal current.
static const uint8_t QMI8658_CTRL2_4G_31HZ = (0x01 << 4) | 0x0E;

// 4g full-scale → 8g span over int16 → 4.0/32768 g per LSB.
static constexpr float QMI8658_ACC_LSB_PER_G = 32768.0f / 4.0f;

class QMI8658Component : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_motion_threshold(float t) { this->motion_threshold_ = t; }
  void set_motion_hold_ms(uint32_t ms) { this->motion_hold_ms_ = ms; }
  void set_motion_binary_sensor(binary_sensor::BinarySensor *s) { this->motion_ = s; }

 protected:
  bool read_accel_g_(float &ax, float &ay, float &az);

  binary_sensor::BinarySensor *motion_{nullptr};
  float motion_threshold_{0.10f};
  uint32_t motion_hold_ms_{500};

  bool ok_{false};
  bool have_baseline_{false};
  float last_ax_{0}, last_ay_{0}, last_az_{0};
  uint32_t motion_off_at_{0};
  bool motion_state_{false};
};

}  // namespace qmi8658
}  // namespace esphome
