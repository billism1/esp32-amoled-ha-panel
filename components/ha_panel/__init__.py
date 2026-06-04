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
# UE12: a "report" row is a computed aggregate over the OTHER entities (lights
# on, totals, min/max/avg, …) instead of one HA entity. `report:` is mutually
# exclusive with `entity_id:` on a row (validated below). The fields mirror the
# C++ ReportSpec.
CONF_REPORT = "report"
CONF_TYPE = "type"
CONF_TITLE = "title"
CONF_DOMAINS = "domains"
CONF_MATCH_STATE = "match_state"
CONF_DEVICE_CLASS = "device_class"
CONF_UNIT = "unit"
CONF_SHOW_TOTAL = "show_total"
CONF_SHOW_SOURCE = "show_source"
CONF_SCOPE = "scope"
# UE12: report aggregation scope. "all" (default) = every entity on the panel
# across all pages; "page" = only entities on the same page as the report row.
REPORT_SCOPES = ["all", "page"]
# UE13: per-row name/title text styles. Values map to the STYLE_* bitmask in
# ha_panel.h. bold/italic need baked font variants (wired via `style_fonts:`
# below); underline is a runtime LVGL decor (no font). Mutually combinable.
CONF_STYLE = "style"
TEXT_STYLES = {"bold": 1, "italic": 2, "underline": 4}
# UE13: the baked Montserrat bold/italic fonts at the three row sizes, supplied
# by packages/style-fonts.yaml and merged into this component's config. Each is
# a list of exactly 3 font ids [small(18), medium(24), large(32)].
CONF_STYLE_FONTS = "style_fonts"
CONF_BOLD = "bold"
CONF_ITALIC = "italic"
# UE11: per-page picker badge(s). A page declares one badge or a list of badges,
# shown as icon+value stacked horizontally to the right of the page name in the
# picker, computed fresh on open over THAT page's entities only (page-scoped).
CONF_PICKER_BADGE = "picker_badge"
CONF_AGG = "agg"
CONF_THRESHOLD = "threshold"
# UE11: selectable badge `type:` values — source of truth for this compile-time
# validation and the C++ BadgeType enum (badge_type_from_). Config-free group
# (domain + state only) ships in v1; the device_class / numeric group parses +
# evaluates but stays empty until UE7 subscribes device_class (gated).
#   config-free: lights_on, devices_on, unlocked, open_covers, media_playing,
#                climate_active, running, offline, entities
#   needs UE7  : open_doors, motion, low_battery, alarm, temperature, humidity,
#                power, co2, aqi, severity (alarm/door portions)
#   composite  : severity, idle
BADGE_TYPES = [
    "lights_on", "devices_on", "unlocked", "open_covers", "media_playing",
    "climate_active", "running", "offline", "entities",
    "open_doors", "motion", "low_battery", "alarm",
    "temperature", "humidity", "power", "co2", "aqi",
    "severity", "idle",
]
# UE11: numeric-aggregate selector for temperature / co2 / aqi badges.
BADGE_AGGS = ["avg", "min", "max", "sum"]


def _style_flags(values):
    flags = 0
    for s in values:
        flags |= TEXT_STYLES[s]
    return flags
# UE12: selectable report `type:` values. Source of truth for both the
# compile-time validation here and the C++ ReportType enum (report_type_from_).
#   count    — N entities matching {domains, match_state}; show_total → "N / M"
#   bool     — count collapsed to a flag: 0 → green check, else amber "N"
#   offline  — N matched entities that are unavailable / unknown
#   sum/avg  — total / mean of the matched numeric states (carries `unit:`)
#   min/max  — smallest / largest numeric state; show_source → prepend its name
REPORT_TYPES = ["count", "bool", "offline", "sum", "avg", "min", "max"]
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

def _match_state_value(value):
    # YAML 1.1 parses bare on/off/yes/no as booleans, so `match_state: [on]`
    # arrives as `True`. Map booleans back to the on/off HA states users meant
    # (so both `[on]` and `["on"]` work). Quote any other non-on/off state.
    if isinstance(value, bool):
        return "on" if value else "off"
    return cv.string(value)


# UE12: the `report:` block. `type` is a strict enum (unknown value = compile
# error). `domains` / `match_state` are optional lists (empty = any). The two
# lists are passed to C++ comma-joined and re-split there, so codegen needn't
# emit std::vector initializers.
REPORT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_TYPE): cv.one_of(*REPORT_TYPES, lower=True),
        cv.Required(CONF_TITLE): cv.string,
        cv.Optional(CONF_DOMAINS, default=[]): cv.ensure_list(cv.string),
        cv.Optional(CONF_MATCH_STATE, default=[]): cv.ensure_list(_match_state_value),
        # device_class filtering needs UE7's connect-time attr subscription; it
        # is parsed + matched today but inert until that lands (matches nothing).
        cv.Optional(CONF_DEVICE_CLASS, default=""): cv.string,
        cv.Optional(CONF_UNIT, default=""): cv.string,
        cv.Optional(CONF_SHOW_TOTAL, default=False): cv.boolean,
        cv.Optional(CONF_SHOW_SOURCE, default=False): cv.boolean,
        # all (default) = aggregate every entity on the panel; page = only the
        # entities on the same page as this report row.
        cv.Optional(CONF_SCOPE, default="all"): cv.one_of(*REPORT_SCOPES, lower=True),
        # optional MDI icon for the row ("mdi:foo"); omit = no icon column.
        cv.Optional(CONF_ICON, default=""): cv.string,
        # UE13: title text styles, any of bold / italic / underline.
        cv.Optional(CONF_STYLE, default=[]): cv.ensure_list(
            cv.one_of(*TEXT_STYLES, lower=True)
        ),
    }
)

# A row is EITHER a normal entity (entity_id + per-entity options) OR a report
# (report block). `size:` applies to both — report rows honour it like any row.
ENTITY_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_ENTITY_ID): cv.entity_id,
            cv.Optional(CONF_FRIENDLY_NAME, default=""): cv.string,
            # P7e: optional per-entity icon override ("mdi:foo"). Empty = fall
            # through to the compile-time domain default → fallback glyph.
            cv.Optional(CONF_ICON, default=""): cv.string,
            # P7f: short-tap opens a confirm sheet / detail modal instead of
            # firing the action immediately. Meaningless on read-only domains.
            cv.Optional(CONF_CONFIRM, default=False): cv.boolean,
            # E8: strict enum — an unrecognised value is a compile-time error.
            cv.Optional(CONF_SIZE, default="small"): cv.one_of(
                *ENTITY_SIZES, lower=True
            ),
            # UE7: bypass REST backfill, plot live from the ring buffer.
            cv.Optional(CONF_REALTIME, default=False): cv.boolean,
            # UE13: name text styles, any of bold / italic / underline.
            cv.Optional(CONF_STYLE, default=[]): cv.ensure_list(
                cv.one_of(*TEXT_STYLES, lower=True)
            ),
            # UE12: computed-aggregate row (mutually exclusive with entity_id).
            cv.Optional(CONF_REPORT): REPORT_SCHEMA,
        }
    ),
    cv.has_exactly_one_key(CONF_ENTITY_ID, CONF_REPORT),
)

# UE11: one picker badge. `type` is a strict enum (unknown value = compile
# error). The block form carries optional params; a bare type name is sugar for
# `{type: <name>}` (normalised by _picker_badge).
PICKER_BADGE_BLOCK_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_TYPE): cv.one_of(*BADGE_TYPES, lower=True),
        # numeric badges (temperature / co2 / aqi): which aggregate to show.
        cv.Optional(CONF_AGG, default="avg"): cv.one_of(*BADGE_AGGS, lower=True),
        # low_battery: percent at/below which a battery counts as low.
        cv.Optional(CONF_THRESHOLD, default=20): cv.int_,
        # numeric badges: suffix override ("°", "%", "W", …); empty = built-in.
        cv.Optional(CONF_UNIT, default=""): cv.string,
        # icon override ("mdi:foo" or bare); empty = the type's default glyph.
        cv.Optional(CONF_ICON, default=""): cv.string,
    }
)


def _picker_badge(value):
    # Accept a bare type name ("lights_on") or a block ({type: …, agg: …}).
    if isinstance(value, str):
        return PICKER_BADGE_BLOCK_SCHEMA({CONF_TYPE: value})
    return PICKER_BADGE_BLOCK_SCHEMA(value)


# A page's `picker_badge:` accepts one badge or a list of them (ensure_list
# wraps a single into a list); each item is bare-name-or-block.
PICKER_BADGE_SCHEMA = cv.ensure_list(_picker_badge)

PAGE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string,
        cv.Required(CONF_ENTITIES): cv.ensure_list(ENTITY_SCHEMA),
        # UE11: zero or more page-picker badges (default omitted = none).
        cv.Optional(CONF_PICKER_BADGE, default=[]): PICKER_BADGE_SCHEMA,
    }
)

# UE13: baked bold / italic name-label fonts, one per row size (small 18,
# medium 24, large 32) — exactly 3 ids each. Supplied + merged by
# packages/style-fonts.yaml; users don't write this by hand.
STYLE_FONTS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_BOLD): cv.All(
            cv.ensure_list(cv.use_id(font.Font)), cv.Length(min=3, max=3)
        ),
        cv.Optional(CONF_ITALIC): cv.All(
            cv.ensure_list(cv.use_id(font.Font)), cv.Length(min=3, max=3)
        ),
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
        cv.Optional(CONF_STYLE_FONTS): STYLE_FONTS_SCHEMA,
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
    # UE13: wire the baked bold/italic name fonts. variant 0 = bold, 1 = italic;
    # each list is [small, medium, large].
    if CONF_STYLE_FONTS in config:
        sf = config[CONF_STYLE_FONTS]
        for variant_idx, key in ((0, CONF_BOLD), (1, CONF_ITALIC)):
            if key in sf:
                for size_idx, fid in enumerate(sf[key]):
                    cg.add(
                        var.set_style_font(
                            size_idx, variant_idx, await cg.get_variable(fid)
                        )
                    )
    for page in config[CONF_PAGES]:
        cg.add(var.add_page(page[CONF_NAME]))
        # UE11: append each declared picker badge to the page just added.
        for badge in page[CONF_PICKER_BADGE]:
            cg.add(
                var.add_page_badge(
                    badge[CONF_TYPE],
                    badge[CONF_AGG],
                    badge[CONF_THRESHOLD],
                    badge[CONF_UNIT],
                    badge[CONF_ICON],
                )
            )
        for ent in page[CONF_ENTITIES]:
            # UE12: a report row — emit add_report and skip the entity path.
            if CONF_REPORT in ent:
                r = ent[CONF_REPORT]
                cg.add(
                    var.add_report(
                        r[CONF_TITLE],
                        r[CONF_TYPE],
                        ",".join(r[CONF_DOMAINS]),
                        ",".join(r[CONF_MATCH_STATE]),
                        r[CONF_DEVICE_CLASS],
                        r[CONF_UNIT],
                        r[CONF_SHOW_TOTAL],
                        r[CONF_SHOW_SOURCE],
                        r[CONF_SCOPE],
                        r[CONF_ICON],
                        ENTITY_SIZES[ent[CONF_SIZE]],
                        _style_flags(r[CONF_STYLE]),
                    )
                )
                continue
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
                    _style_flags(ent[CONF_STYLE]),
                )
            )
