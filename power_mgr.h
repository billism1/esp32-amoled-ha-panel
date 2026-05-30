// power_mgr.h — P8 light-sleep helper.
//
// ESPHome has no first-class light-sleep component, so the light-sleep path is
// a tiny IDF wrapper called from a lambda in packages/idle.yaml. Deep sleep is
// the ESPHome `deep_sleep:` component (boards/waveshare-2.16.yaml); this file
// is only the light-sleep ("default") branch.
//
// Included into main.cpp via `esphome: includes:` in ha-amoled-panel.yaml so
// the YAML lambda can call ha_power::enter_light_sleep().
//
// Wake source: CST9220 touch INT on GPIO11 (RTC-capable, active-low). The IMU
// INT is not routed on this board (P4), so motion can NOT wake sleep — only a
// touch (or the optional GPIO18 button / RTC alarm, not wired here). See the
// P8 wake-source table in plan.md.
//
// Wi-Fi is dropped by the caller (`wifi.disable`) BEFORE this runs so the radio
// is fully off — on the `power_save_mode: NONE` Google Wifi mesh that is what
// actually saves current (an *associated* radio stays hot even in light sleep).
//
// VERIFY ON HARDWARE (plan.md §P8 risks):
//  - Touch IC still pulses INT on GPIO11 while we are in polling mode so the
//    wake edge actually fires.
//  - ESPHome's wifi component cooperates with wifi.disable/enable around the
//    blocking sleep (vs. fighting a raw esp_wifi_stop()).
#pragma once

#include "esp_sleep.h"
#include "driver/gpio.h"

namespace ha_power {

// CST9220 INT — RTC-capable GPIO, idles high, pulses low on touch.
static constexpr gpio_num_t WAKE_TOUCH_INT = GPIO_NUM_11;

// Block in light sleep until the touch INT pin goes low, then return. RAM is
// retained across this call, so the caller resumes mid-function and the LVGL
// UI is exactly as it was left (no rebuild).
inline void enter_light_sleep() {
  // Configure the INT pin as a pulled-up input so it reads high while idle and
  // the low-level wake doesn't fire immediately on a floating line.
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = 1ULL << WAKE_TOUCH_INT;
  cfg.mode = GPIO_MODE_INPUT;
  cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);

  gpio_wakeup_enable(WAKE_TOUCH_INT, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  // CPU halts here until the wake edge. We sleep explicitly rather than via
  // automatic tickless idle, so CONFIG_PM_ENABLE isn't required.
  esp_light_sleep_start();

  // Back from sleep — drop the wake config so GPIO11 is free for normal use.
  gpio_wakeup_disable(WAKE_TOUCH_INT);
}

}  // namespace ha_power
