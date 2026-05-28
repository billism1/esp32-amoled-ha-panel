# ESPHome external_component: QMI8658 6-axis IMU (polling, software motion).
#
# Hardware INT pin isn't routed on the Waveshare 2.16 board, so this component
# polls accel at update_interval (default 100 ms) and exposes a `motion`
# binary_sensor that pulses when the frame-to-frame |Δa| magnitude exceeds
# `motion_threshold` g.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, i2c
from esphome.const import CONF_ID

DEPENDENCIES = ["i2c"]
AUTO_LOAD = ["binary_sensor"]

CONF_MOTION = "motion"
CONF_MOTION_THRESHOLD = "motion_threshold"
CONF_MOTION_HOLD = "motion_hold"

qmi8658_ns = cg.esphome_ns.namespace("qmi8658")
QMI8658Component = qmi8658_ns.class_(
    "QMI8658Component", cg.PollingComponent, i2c.I2CDevice
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(QMI8658Component),
            cv.Optional(CONF_MOTION_THRESHOLD, default=0.10): cv.positive_float,
            cv.Optional(
                CONF_MOTION_HOLD, default="500ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MOTION): binary_sensor.binary_sensor_schema(),
        }
    )
    .extend(cv.polling_component_schema("100ms"))
    .extend(i2c.i2c_device_schema(0x6B))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    cg.add(var.set_motion_threshold(config[CONF_MOTION_THRESHOLD]))
    cg.add(var.set_motion_hold_ms(config[CONF_MOTION_HOLD].total_milliseconds))

    if motion_conf := config.get(CONF_MOTION):
        sens = await binary_sensor.new_binary_sensor(motion_conf)
        cg.add(var.set_motion_binary_sensor(sens))
