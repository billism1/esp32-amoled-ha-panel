# ha_panel — Phase 5 static page + entity model.
#
# Reads a `pages: [{name, entities: [{entity_id, friendly_name}]}]` list at
# codegen time and emits initializer calls into a single C++ HAPanel
# component. At runtime the component subscribes to each entity's state via
# the native API and exposes a `tap(page_idx, entity_idx)` method that
# dispatches to the right HA service based on domain.
#
# Phase 9 will replace this static schema with a live HA template sensor.

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import font, http_request, time
from esphome.const import CONF_ENTITY_ID, CONF_ICON, CONF_ID, CONF_NAME, CONF_TIME_ID

_LOGGER = logging.getLogger(__name__)

DEPENDENCIES = ["api", "lvgl"]

CONF_PAGES = "pages"
CONF_ENTITIES = "entities"
CONF_FRIENDLY_NAME = "friendly_name"
# P7e: font id holding the baked MDI glyph subset (packages/mdi-font.yaml). The
# component reads it back via get_lv_font() to draw the per-entity icon column.
CONF_MDI_FONT = "mdi_font"
# E8: optional re-baked MDI fonts at 36 px / 48 px for medium / large rows. When
# omitted, those rows fall back to the base 24 px font (icon looks relatively
# smaller, layout still works).
CONF_MDI_FONT_MEDIUM = "mdi_font_medium"
CONF_MDI_FONT_LARGE = "mdi_font_large"
# P7f: per-entity tap-confirmation opt-in. Short-tap on a confirm-flagged entity
# opens a confirm sheet (action domains) or the detail modal (light/climate/…)
# instead of firing the action immediately.
CONF_CONFIRM = "confirm"
# E8: per-entity render size. Scales the whole row (height, name font, icon,
# right-side widget). Strict enum — an unrecognised value is a compile error.
CONF_SIZE = "size"
# UE7: realtime opt-in. Skips the REST history backfill for this entity and plots
# from the in-device ring buffer + live-tail, defaulting to the short "Live"
# window. For high-rate (1 Hz) sensors HA does not record (e.g. MQTT test feeds).
CONF_REALTIME = "realtime"
# E9: optional REST history backfill. When this block is present the history
# chart sheet fetches GET /api/history/period/... over http_request; when it's
# absent the chart uses only the in-device ring buffer (since-boot samples).
CONF_HISTORY = "history"
CONF_HTTP_REQUEST_ID = "http_request_id"
CONF_BASE_URL = "base_url"
CONF_TOKEN = "token"

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

# E8: mirrors enum class EntitySize in ha_panel.h.
EntitySize = ha_panel_ns.enum("EntitySize", is_class=True)
ENTITY_SIZES = {
    "small": EntitySize.SMALL,
    "medium": EntitySize.MEDIUM,
    "large": EntitySize.LARGE,
}

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
        # E8: strict enum — an unrecognised value is a compile-time error.
        cv.Optional(CONF_SIZE, default="small"): cv.one_of(
            *ENTITY_SIZES, lower=True
        ),
        # UE7: bypass REST backfill, plot live from the ring buffer.
        cv.Optional(CONF_REALTIME, default=False): cv.boolean,
    }
)

PAGE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string,
        cv.Required(CONF_ENTITIES): cv.ensure_list(ENTITY_SCHEMA),
    }
)

# E9: all four keys required when the block is present — REST backfill needs an
# HTTP client, the HA base URL, a long-lived token, and a time source (to build
# the start timestamp + convert returned timestamps to the device clock).
HISTORY_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_HTTP_REQUEST_ID): cv.use_id(http_request.HttpRequestComponent),
        cv.Required(CONF_BASE_URL): cv.string,
        cv.Required(CONF_TOKEN): cv.string,
        cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HAPanel),
        cv.Required(CONF_PAGES): cv.ensure_list(PAGE_SCHEMA),
        cv.Optional(CONF_MDI_FONT): cv.use_id(font.Font),
        cv.Optional(CONF_MDI_FONT_MEDIUM): cv.use_id(font.Font),
        cv.Optional(CONF_MDI_FONT_LARGE): cv.use_id(font.Font),
        cv.Optional(CONF_HISTORY): HISTORY_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    if CONF_MDI_FONT in config:
        cg.add(var.set_mdi_font(await cg.get_variable(config[CONF_MDI_FONT])))
    if CONF_MDI_FONT_MEDIUM in config:
        cg.add(var.set_mdi_font_medium(await cg.get_variable(config[CONF_MDI_FONT_MEDIUM])))
    if CONF_MDI_FONT_LARGE in config:
        cg.add(var.set_mdi_font_large(await cg.get_variable(config[CONF_MDI_FONT_LARGE])))
    if CONF_HISTORY in config:
        h = config[CONF_HISTORY]
        cg.add(var.set_history_http(await cg.get_variable(h[CONF_HTTP_REQUEST_ID])))
        cg.add(var.set_history_time(await cg.get_variable(h[CONF_TIME_ID])))
        cg.add(var.set_history_base_url(h[CONF_BASE_URL]))
        cg.add(var.set_history_token(h[CONF_TOKEN]))
    for page in config[CONF_PAGES]:
        cg.add(var.add_page(page[CONF_NAME]))
        for ent in page[CONF_ENTITIES]:
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
                    ENTITY_SIZES[ent[CONF_SIZE]],
                    ent[CONF_REALTIME],
                )
            )
