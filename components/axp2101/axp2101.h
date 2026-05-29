// axp2101.h — minimal AXP2101 PMIC driver (battery voltage only).
//
// Reference: lewisxhe/XPowersLib (XPowersAXP2101.tpp).
// We enable the VBAT ADC channel at setup, then poll the 14-bit averaged
// battery voltage register pair every update_interval. Voltage is published
// in volts on the battery_voltage sensor.
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace axp2101 {

// ADC channel control: bit 0 = VBAT voltage ADC enable.
static const uint8_t AXP2101_REG_ADC_CHANNEL_CTRL = 0x30;
// 14-bit averaged battery voltage, 1 mV per LSB.
// High 6 bits in 0x34 (mask 0x3F), low 8 bits in 0x35.
static const uint8_t AXP2101_REG_BAT_AVER_VOL_H = 0x34;

class AXP2101Component : public PollingComponent, public i2c::I2CDevice {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_battery_voltage_sensor(sensor::Sensor *s) { this->battery_voltage_ = s; }

 protected:
  sensor::Sensor *battery_voltage_{nullptr};
  bool ok_{false};
};

}  // namespace axp2101
}  // namespace esphome
