# ha_panel — Phase 5 static area + entity model.
#
# Reads an `areas: [{name, entities: [{entity_id, friendly_name}]}]` list at
# codegen time and emits initializer calls into a single C++ HAPanel
# component. At runtime the component subscribes to each entity's state via
# the native API and exposes a `tap(area_idx, entity_idx)` method that
# dispatches to the right HA service based on domain.
#
# Phase 9 will replace this static schema with a live HA template sensor.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ENTITY_ID, CONF_ID, CONF_NAME

DEPENDENCIES = ["api"]

CONF_AREAS = "areas"
CONF_ENTITIES = "entities"
CONF_FRIENDLY_NAME = "friendly_name"

ha_panel_ns = cg.esphome_ns.namespace("ha_panel")
HAPanel = ha_panel_ns.class_("HAPanel", cg.Component)

ENTITY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ENTITY_ID): cv.entity_id,
        cv.Optional(CONF_FRIENDLY_NAME, default=""): cv.string,
    }
)

AREA_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string,
        cv.Required(CONF_ENTITIES): cv.ensure_list(ENTITY_SCHEMA),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HAPanel),
        cv.Required(CONF_AREAS): cv.ensure_list(AREA_SCHEMA),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    for area in config[CONF_AREAS]:
        cg.add(var.add_area(area[CONF_NAME]))
        for ent in area[CONF_ENTITIES]:
            cg.add(var.add_entity(ent[CONF_ENTITY_ID], ent[CONF_FRIENDLY_NAME]))
