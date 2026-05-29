# ha_panel — Phase 5 static area + entity model.
#
# Reads an `areas: [{name, entities: [{entity_id, friendly_name}]}]` list at
# codegen time and emits initializer calls into a single C++ HAPanel
# component. At runtime the component subscribes to each entity's state via
# the native API and exposes a `tap(area_idx, entity_idx)` method that
# dispatches to the right HA service based on domain.
#
# Phase 9 will replace this static schema with a live HA template sensor.

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import font
from esphome.const import CONF_ENTITY_ID, CONF_ICON, CONF_ID, CONF_NAME

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["api", "lvgl"]

CONF_AREAS = "areas"
CONF_ENTITIES = "entities"
CONF_FRIENDLY_NAME = "friendly_name"
# P7e: font id holding the baked MDI glyph subset (packages/mdi-font.yaml). The
# component reads it back via get_lv_font() to draw the per-entity icon column.
CONF_MDI_FONT = "mdi_font"
# P7f: per-entity tap-confirmation opt-in. Short-tap on a confirm-flagged entity
# opens a confirm sheet (action domains) or the detail modal (light/climate/…)
# instead of firing the action immediately.
CONF_CONFIRM = "confirm"

# Domains where `confirm: true` has a meaningful surface (detail modal or
# action confirm sheet). Everything else is read-only — the flag is ignored and
# warned about at codegen. Mirrors render_class_for_ / has_detail_ in C++.
CONFIRM_MEANINGFUL_DOMAINS = frozenset(
    {
        "light", "switch", "fan", "input_boolean",
        "scene", "script", "automation", "button",
        "lock", "cover",
        "climate", "media_player", "number", "select",
    }
)

ha_panel_ns = cg.esphome_ns.namespace("ha_panel")
HAPanel = ha_panel_ns.class_("HAPanel", cg.Component)

ENTITY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ENTITY_ID): cv.entity_id,
        cv.Optional(CONF_FRIENDLY_NAME, default=""): cv.string,
        # P7e: optional per-entity icon override ("mdi:foo"). Empty = fall
        # through to the compile-time domain default → fallback glyph.
        cv.Optional(CONF_ICON, default=""): cv.string,
        # P7f: short-tap opens a confirm sheet / detail modal instead of firing
        # the action immediately. Meaningless on read-only domains (warned).
        cv.Optional(CONF_CONFIRM, default=False): cv.boolean,
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
        cv.Optional(CONF_MDI_FONT): cv.use_id(font.Font),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if CONF_MDI_FONT in config:
        cg.add(var.set_mdi_font(await cg.get_variable(config[CONF_MDI_FONT])))
    for area in config[CONF_AREAS]:
        cg.add(var.add_area(area[CONF_NAME]))
        for ent in area[CONF_ENTITIES]:
            confirm = ent[CONF_CONFIRM]
            if confirm:
                domain = ent[CONF_ENTITY_ID].split(".", 1)[0]
                if domain not in CONFIRM_MEANINGFUL_DOMAINS:
                    _LOGGER.warning(
                        "ha_panel: confirm: true ignored for read-only domain "
                        "'%s' (%s)",
                        domain,
                        ent[CONF_ENTITY_ID],
                    )
                    confirm = False
            cg.add(
                var.add_entity(
                    ent[CONF_ENTITY_ID],
                    ent[CONF_FRIENDLY_NAME],
                    ent[CONF_ICON],
                    confirm,
                )
            )
