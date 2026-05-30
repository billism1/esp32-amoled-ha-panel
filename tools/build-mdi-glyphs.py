#!/usr/bin/env python3
"""Generate the baked MDI glyph subset for Phase 7e per-entity icons.

Single source of truth for the icon set. Resolves Material Design Icons names
to their Private-Use-Area codepoints from the official MDI webfont CSS, then
emits BOTH artifacts that must stay in lockstep:

  1. components/ha_panel/mdi_icons.h  — C++ name->codepoint table + the
     per-domain default map + the fallback codepoint. Included by ha_panel.cpp.
  2. packages/mdi-font.yaml           — the ESPHome `font:` block whose `glyphs:`
     list bakes exactly these codepoints into flash. Included by the device YAML.

Because both files are generated from ICON_NAMES / DOMAIN_ICON below, the baked
glyphs and the C++ lookup can never drift. To add an icon: add its mdi name to
ICON_NAMES (or wire a domain in DOMAIN_ICON), rerun this script, reflash.

    python tools/build-mdi-glyphs.py

Pin MDI_VERSION to match the ttf URL in the generated font block so the
codepoints baked here line up with the glyphs in the downloaded font.
"""

from __future__ import annotations

import re
import sys
import urllib.request
from pathlib import Path

# Pinned so codepoints (CSS) and baked glyphs (ttf) come from the same release.
MDI_VERSION = "7.4.47"
CSS_URL = f"https://cdn.jsdelivr.net/npm/@mdi/font@{MDI_VERSION}/css/materialdesignicons.css"
# `type: web` font URL used by ESPHome at compile time. Same release as the CSS.
TTF_URL = f"https://cdn.jsdelivr.net/npm/@mdi/font@{MDI_VERSION}/fonts/materialdesignicons-webfont.ttf"

# E8: the icon column scales with the per-entity `size:`. We bake the same glyph
# subset at three pixel sizes so medium/large rows draw crisp icons instead of
# upscaling the 24 px base. (id, size) — id "mdi_icons" stays the base so older
# config keeps working; the re-bakes get the suffixed ids.
FONT_VARIANTS = [
    ("mdi_icons", 24),
    ("mdi_icons_36", 36),
    ("mdi_icons_48", 48),
]

# Per-domain default icon (step 2 of the resolution chain). entity_id domain ->
# mdi name. Anything not listed falls straight to the fallback glyph.
DOMAIN_ICON = {
    "light": "lightbulb",
    "switch": "toggle-switch",
    "input_boolean": "toggle-switch",
    "fan": "fan",
    "cover": "window-shutter",
    "lock": "lock",
    "climate": "thermostat",
    "media_player": "speaker",
    "scene": "palette",
    "script": "script-text-play",
    "automation": "robot",
    "button": "gesture-tap-button",
    "number": "numeric",
    "select": "format-list-bulleted",
    "sensor": "gauge",
    "binary_sensor": "checkbox-marked-circle-outline",
}

# Shown when the resolved name isn't in the baked subset. Lives in the MDI font
# so the icon column stays single-font (no montserrat/MDI mixing per row).
FALLBACK_ICON = "help-circle"

# Extra names worth baking beyond the domain defaults: YAML `icon:` overrides in
# use plus a useful spread for future overrides. Keep curated — every name here
# costs ~1 KB flash.
EXTRA_ICONS = [
    # lights
    "lightbulb-multiple", "lightbulb-on", "lamp", "desk-lamp", "ceiling-light",
    "floor-lamp", "string-lights", "led-strip-variant", "wall-sconce",
    # switches / power
    "toggle-switch-off", "power", "power-plug", "power-socket-us",
    # fans
    "fan-off", "ceiling-fan", "ceiling-fan-light",
    # locks
    "lock-open", "lock-alert",
    # covers
    "window-shutter-open", "blinds", "blinds-open", "curtains", "garage",
    "garage-open", "gate", "gate-open",
    # climate / weather
    "air-conditioner", "radiator", "fireplace", "heat-pump", "weather-sunny",
    "weather-cloudy", "weather-rainy",
    # media
    "television", "speaker-multiple", "music", "volume-high", "volume-off",
    # sensors
    "thermometer", "water-percent", "motion-sensor", "door", "door-open",
    "window-closed", "window-open", "leak", "smoke-detector", "battery",
    # buttons / scenes / scripts
    "play", "cog", "robot-outline",
    # generic
    "home", "alert", "alert-circle", "refresh",
    "checkbox-blank-circle-outline",
]

# Final set = domain defaults + extras + fallback, de-duped, stable order.
ICON_NAMES = []
for _n in list(DOMAIN_ICON.values()) + EXTRA_ICONS + [FALLBACK_ICON]:
    if _n not in ICON_NAMES:
        ICON_NAMES.append(_n)

ROOT = Path(__file__).resolve().parent.parent
HEADER_OUT = ROOT / "components" / "ha_panel" / "mdi_icons.h"
FONT_OUT = ROOT / "packages" / "mdi-font.yaml"


def fetch_codepoints() -> dict[str, int]:
    """name -> codepoint int, parsed from the MDI webfont CSS."""
    data = urllib.request.urlopen(CSS_URL, timeout=60).read().decode("utf-8")
    pat = re.compile(r"\.mdi-([a-z0-9-]+)::before\s*\{\s*content:\s*\"\\([0-9A-Fa-f]+)\"")
    table = {name: int(hexcp, 16) for name, hexcp in pat.findall(data)}
    if not table:
        sys.exit("ERROR: parsed 0 icons from CSS — selector format changed?")
    return table


def main() -> None:
    cps = fetch_codepoints()

    missing = [n for n in ICON_NAMES if n not in cps]
    if missing:
        sys.exit(f"ERROR: these mdi names don't exist in @mdi/font@{MDI_VERSION}: {missing}")

    resolved = [(n, cps[n]) for n in ICON_NAMES]

    write_header(resolved)
    write_font(resolved)
    print(f"Generated {len(resolved)} glyphs from @mdi/font@{MDI_VERSION}")
    print(f"  {HEADER_OUT.relative_to(ROOT)}")
    print(f"  {FONT_OUT.relative_to(ROOT)}")


def write_header(resolved: list[tuple[str, int]]) -> None:
    lines = [
        "// GENERATED by tools/build-mdi-glyphs.py — do not edit by hand.",
        f"// Material Design Icons @mdi/font@{MDI_VERSION}. Rerun the script to change the set.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace esphome {",
        "namespace ha_panel {",
        "",
        "struct MdiGlyph {",
        "  const char *name;   // mdi name without the 'mdi:' prefix",
        "  uint32_t codepoint; // Private Use Area codepoint baked into mdi_icons font",
        "};",
        "",
        "// Sorted-by-insertion name->codepoint table. Linear scan is fine — set is small",
        "// and resolution is cached per entity after first render.",
        "static const MdiGlyph MDI_GLYPHS[] = {",
    ]
    for name, cp in resolved:
        lines.append(f'    {{"{name}", 0x{cp:05X}}},')
    lines.append("};")
    lines.append(f"static const int MDI_GLYPH_COUNT = {len(resolved)};")
    lines.append("")
    lines.append('// Fallback glyph when a resolved name is not in the baked set above.')
    fb_cp = dict(resolved)[FALLBACK_ICON]
    lines.append(f'static const uint32_t MDI_FALLBACK_CP = 0x{fb_cp:05X};  // mdi:{FALLBACK_ICON}')
    lines.append("")
    lines.append("struct DomainIcon {")
    lines.append("  const char *domain;")
    lines.append("  const char *icon;  // mdi name, must appear in MDI_GLYPHS")
    lines.append("};")
    lines.append("// Step 2 of the resolution chain: domain -> default mdi name.")
    lines.append("static const DomainIcon MDI_DOMAIN_DEFAULTS[] = {")
    for dom, name in DOMAIN_ICON.items():
        lines.append(f'    {{"{dom}", "{name}"}},')
    lines.append("};")
    lines.append(f"static const int MDI_DOMAIN_DEFAULT_COUNT = {len(DOMAIN_ICON)};")
    lines.append("")
    lines.append("}  // namespace ha_panel")
    lines.append("}  // namespace esphome")
    lines.append("")
    HEADER_OUT.write_text("\n".join(lines), encoding="utf-8")


def write_font(resolved: list[tuple[str, int]]) -> None:
    lines = [
        "# GENERATED by tools/build-mdi-glyphs.py — do not edit by hand.",
        f"# Material Design Icons @mdi/font@{MDI_VERSION}. Rerun the script to change the set.",
        "#",
        "# Baked MDI glyph subset for Phase 7e per-entity icons. The glyph list here",
        "# must match components/ha_panel/mdi_icons.h — both come from the same script.",
        "#",
        "# E8: the same subset is baked at 24/36/48 px so medium/large rows draw",
        "# crisp icons. All three are anchored into LVGL by hidden labels in",
        "# packages/lvgl-ui.yaml so USE_LVGL_FONT is defined and the fonts link;",
        "# ha_panel reads them back via set_mdi_font[_medium|_large]() and picks the",
        "# size-matched font per row with get_lv_font().",
        "",
        "font:",
    ]
    for font_id, size in FONT_VARIANTS:
        lines += [
            "  - file:",
            "      type: web",
            f"      url: {TTF_URL}",
            "      refresh: never",
            f"    id: {font_id}",
            f"    size: {size}",
            "    bpp: 4",
            "    glyphs:",
        ]
        for name, cp in resolved:
            lines.append(f'      - "\\U{cp:08X}"  # mdi:{name}')
    lines.append("")
    FONT_OUT.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
