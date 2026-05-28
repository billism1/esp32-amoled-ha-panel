// cst9220.h — vendored from shelson/esphome-cst9217, adapted for CST9220 (Waveshare AMOLED 2.16)
#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/touchscreen/touchscreen.h"

/* CST9220 registers (Hynitron 16-bit register protocol — same as CST9217) */
#define ESP_LCD_TOUCH_CST9220_DATA_REG 0xD000
#define ESP_LCD_TOUCH_CST9220_PROJECT_ID_REG 0xD204
#define ESP_LCD_TOUCH_CST9220_CMD_MODE_REG   0xD101
#define ESP_LCD_TOUCH_CST9220_CHECKCODE_REG  0xD1FC
#define ESP_LCD_TOUCH_CST9220_RESOLUTION_REG 0xD1F8
#define ESP_LCD_TOUCH_CST9220_NORMAL_MODE_REG 0xD109

/* CST9220 parameters */
#define CST9220_CHIP_ID 0x9220
#define CST9220_ACK_VALUE 0xAB
#define CST9220_MAX_TOUCH_POINTS 1
#define CST9220_DATA_LENGTH (CST9220_MAX_TOUCH_POINTS * 5 + 5)

namespace esphome {
namespace cst9220 {

static const char *const TAG = "cst9220.touchscreen";


class CST9220Touchscreen : public touchscreen::Touchscreen, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;

  void update_touches() override;

  void set_interrupt_pin(InternalGPIOPin *pin) { this->interrupt_pin_ = pin; }
  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }

 protected:
  void reset_device_();
  uint8_t touch_data_[CST9220_DATA_LENGTH];
  InternalGPIOPin *interrupt_pin_{};
  GPIOPin *reset_pin_{};
  uint16_t chip_id_;
  uint16_t touch_res_x_;
  uint16_t touch_res_y_;
  uint16_t touch_project_id_;
};

} // namespace cst9220
} // namespace esphome
