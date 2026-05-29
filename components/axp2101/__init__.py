# ESPHome external_component: AXP2101 PMIC battery voltage (read-only).
#
# Minimal port of lewisxhe/XPowersLib's VBAT read path. No charge curve, no USB
# detect, no PMU power control — P7b only needs the averaged battery voltage
# to drive the header battery icon.
#
# Datasheet: AXP2101 register 0x30 = ADC channel control (bit 0 = VBAT ADC
# enable). Registers 0x34/0x35 = BAT_AVER_VOL (14-bit, mV per LSB).

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, sensor
from esphome.const import (
    CONF_BATTERY_VOLTAGE,
    CONF_ID,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_VOLT,
)

DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["sensor"]

axp2101_ns = cg.esphome_ns.namespace("axp2101")
AXP2101Component = axp2101_ns.class_(
    "AXP2101Component", cg.PollingComponent, i2c.I2CDevice
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AXP2101Component),
            cv.Optional(CONF_BATTERY_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement=UNIT_VOLT,
                accuracy_decimals=3,
                device_class=DEVICE_CLASS_VOLTAGE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
        }
    )
    .extend(cv.polling_component_schema("30s"))
    .extend(i2c.i2c_device_schema(0x34))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if vbat_conf := config.get(CONF_BATTERY_VOLTAGE):
        sens = await sensor.new_sensor(vbat_conf)
        cg.add(var.set_battery_voltage_sensor(sens))
