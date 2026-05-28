# ESP32-S3 2.16" AMOLED — Home Assistant via ESPHome: Developer Guide

A reference guide for building a Home Assistant control/monitoring panel using the
**Waveshare ESP32-S3-Touch-AMOLED-2.16** and ESPHome with LVGL.

---

## 1. Hardware Overview

| Property | Detail |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-2.16 |
| MCU | ESP32-S3R8 (Xtensa LX7 dual-core, up to 240 MHz) |
| RAM | 512 KB SRAM + 8 MB PSRAM (stacked) |
| Flash | 16 MB external |
| Display | 2.16" AMOLED, 480×480, 16.7M color |
| Display Driver | **CO5300** (QSPI interface) |
| Touch IC | **CST9220** (I2C) |
| IMU | QMI8658 (6-axis: 3-axis accel + 3-axis gyro) |
| RTC | PCF85063 |
| Audio | Low-power audio codec, dual digital microphones with echo cancellation |
| Power | AXP2101 PMIC, 3.7V MX1.25 LiPo header |
| Connectivity | 2.4 GHz Wi-Fi 802.11 b/g/n + Bluetooth 5 (LE) |
| USB | USB Type-C |

### Key chip summary for ESPHome config

```
Display:  CO5300  —  QSPI (bus_mode: quad), mipi_spi platform
Touch:    CST9220 —  I2C
IMU:      QMI8658 —  I2C (no native ESPHome driver; needs custom component)
RTC:      PCF85063 — I2C (supported natively in ESPHome)
PMIC:     AXP2101  — I2C
```

> **PSRAM is required** for the display framebuffer. Ensure `psram: mode: octal` is set in
> your ESPHome config or the screen will not initialise. Valid `mode` values are `quad`,
> `octal`, or `hex`; 8 MB stacked PSRAM on the ESP32-S3R8 needs `octal`.

---

## 2. Key Reference Repositories

### 2a. Official Waveshare Sample Code (start here)

**[waveshareteam/ESP32-S3-Touch-AMOLED-2.16](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16)**

Waveshare's official engineering sample programs for this exact board.
Use this to:
- Confirm correct GPIO pin assignments for your specific board revision
- Verify the CO5300 display initialisation sequence
- Test that all peripherals (touch, IMU, RTC, audio) are wired as expected before moving to ESPHome

**[waveshareteam/Waveshare-ESP32-components](https://github.com/waveshareteam/Waveshare-ESP32-components)**

Official Waveshare ESP component registry — drivers and BSP support packages.
Explicitly lists support for `ESP32-S3-Touch-AMOLED-1.8` and `ESP32-S3-Touch-AMOLED-1.75`,
which share the CO5300 driver with your 2.16" board. Useful for low-level driver reference
and init sequence cross-checking.

---

### 2b. Closest ESPHome Reference — Same Driver Family

**[SentientCustard/esphome-waveshare-amoled2.41](https://github.com/SentientCustard/esphome-waveshare-amoled2.41)**

The most directly useful ESPHome project for your board. The 2.41" board shares:
- Same general ESP32-S3 + AMOLED architecture
- Same RTC chip (PCF85063)
- Same IMU (QMI8658) — includes a **custom ESPHome component** for it since no native driver exists
- Same I2C peripheral bus layout
- Touch IC is FT6336 on the 2.41" vs CST9220 on your 2.16" (only thing you'll need to swap)

Features implemented:
- PSRAM + QSPI screen (RM690B0 driver on 2.41"; you'll use CO5300)
- I2C bus for touch, RTC, IMU, extended IO
- PCF85063 RTC
- QMI8658 IMU (custom component — reusable for your board)
- LVGL-based UI with a hue/brightness/saturation colour picker
- Home Assistant API + Wi-Fi integration

**This repo is your primary ESPHome derivation target.**

---

### 2c. ESPHome Device Config — Same CO5300 Driver, Similar Board

**[ESPHome Devices: Waveshare ESP32-S3-Touch-AMOLED-1.75](https://devices.esphome.io/devices/waveshare-esp32-s3-touch-amoled-175/)**

The 1.75" board uses the identical CO5300 + QSPI display stack. The published device YAML
is the closest working ESPHome display config you can adapt directly. Key config:

```yaml
display:
  - platform: mipi_spi
    model: CO5300
    bus_mode: quad
    reset_pin: GPIO39
    cs_pin: GPIO12
    dimensions:
      height: 466
      width: 466
    color_order: rgb
    invert_colors: false

touchscreen:
  - platform: cst9217       # ← swap to cst9220 for your 2.16" board
    display: disp1
    interrupt_pin: GPIO11
    reset_pin: GPIO40
    transform:
      mirror_x: true
      mirror_y: true
```

> **Pin numbers will differ** on your 2.16" board — always cross-reference with the
> Waveshare wiki schematic for the exact board.

---

### 2d. HA Community Threads Worth Reading

- **[WaveShare ESP32-S3 AMOLED 2.41 with ESPHome](https://community.home-assistant.io/t/waveshare-esp32-s3-amoled-2-41-with-esphome/927907)**
  The original thread from the author of the 2.41" ESPHome repo above. Details what works,
  what doesn't (SD card not supported in ESPHome), and the custom QMI8658 driver approach.

- **[ESP32-S3 1.8" AMOLED Touch — ESPHome](https://community.home-assistant.io/t/esp32-s3-1-8inch-amoled-touch/956270)**
  Similar CO5300/SH8601 board getting working in ESPHome. Useful for driver troubleshooting tips.

- **[Waveshare ESP32-S3-Touch-AMOLED-2.06 Watch](https://community.home-assistant.io/t/waveshare-esp32-s3-touch-amoled-2-06-watch/914798)**
  Another closely related board (same chip family). Good for community workarounds.

- **[ESPHome Discussion #3229 — SH8601/CO5300 driver](https://github.com/orgs/esphome/discussions/3229)**
  The upstream ESPHome discussion covering how the CO5300 and SH8601 drivers were added,
  including init sequences and runtime chip-ID detection logic.

---

## 3. ESPHome Support Status

| Component | ESPHome Support | Notes |
|---|---|---|
| CO5300 display | ✅ Native (`mipi_spi` platform) | `model: CO5300` |
| QSPI / quad bus | ✅ Native | `bus_mode: quad` |
| CST9220 touch | ❌ No native driver | No ESPHome platform for CST92xx. Use community `cst9217` external_component (shelson or fuzzybear62 fork); CST9220 may need protocol tweaks (see lewisxhe `SensorLib` `TouchDrvCST92xx`). `cst226` is a different register layout — not drop-in. |
| PCF85063 RTC | ✅ Native | `platform: pcf85063` |
| QMI8658 IMU | ⚠️ Custom component | No native driver; use the custom component from the 2.41" repo |
| AXP2101 PMIC | ⚠️ Limited | Can control via I2C lambda; no dedicated platform |
| Dual microphones | ⚠️ Complex | Possible via `i2s_audio`; requires custom config |
| PSRAM | ✅ Required | Must enable `psram: mode: ocpi` |
| Wi-Fi / HA API | ✅ Native | Standard ESPHome |
| LVGL UI | ✅ Native | Full LVGL support in ESPHome 2024.x+ |

### Known Bug: CO5300 Green Line Artifact (Fixed Upstream)

ESPHome [issue #15765](https://github.com/esphome/esphome/issues/15765) (now **CLOSED**)
documented a single green pixel-wide line at the panel edge caused by a missing
`esp_lcd_panel_set_gap()` 6-pixel column offset. Confirm your ESPHome version includes the
fix. As a manual workaround, set `offset_width: 6` under the display `dimensions:` block —
`mipi_spi` exposes `offset_width` / `offset_height` in YAML.

---

## 4. ESPHome Base Config Template

Use this as a starting scaffold. **Verify all GPIO numbers** against the Waveshare 2.16" schematic
before flashing.

```yaml
esphome:
  name: ha-amoled-panel
  friendly_name: HA AMOLED Panel

esp32:
  board: esp32-s3-devkitc-1
  variant: esp32s3
  framework:
    type: esp-idf

psram:
  mode: octal      # Required for 8 MB stacked PSRAM on ESP32-S3R8
  speed: 80MHz

# --- Connectivity ---
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret api_key

ota:
  - platform: esphome
    password: !secret ota_password

logger:

# --- I2C Bus (Touch, RTC, IMU, PMIC) ---
i2c:
  - id: i2c_bus
    sda: GPIO6      # Verify against schematic
    scl: GPIO7      # Verify against schematic
    scan: true
    frequency: 400kHz

# --- SPI Bus (Display — QSPI) ---
spi:
  - id: qspi_bus
    type: quad
    clk_pin: GPIO47   # Verify against schematic
    data_pins:
      - GPIO18
      - GPIO19
      - GPIO20
      - GPIO21

# --- Display ---
display:
  - platform: mipi_spi
    id: amoled_display
    model: CO5300
    cs_pin: GPIO12      # Verify
    reset_pin: GPIO39   # Verify
    bus_mode: quad
    data_rate: 80MHz
    spi_mode: MODE0
    dimensions:
      width: 480
      height: 480
      offset_width: 6      # Workaround for green-edge artifact (ESPHome #15765)
    color_order: rgb
    invert_colors: false
    brightness: 200        # Native AMOLED brightness option on mipi_spi (0-255)
    update_interval: never
    auto_clear_enabled: false

# --- Touch ---
# No native CST92xx driver in ESPHome. Pull in community cst9217 external_component
# and try it against the CST9220 — register layout is closest match.
external_components:
  - source: github://shelson/esphome-cst9217
    components: [cst9217]

touchscreen:
  - platform: cst9217    # External component; CST9220 may need minor protocol tweaks
    display: amoled_display
    id: ts
    interrupt_pin: GPIO11   # Verify
    reset_pin: GPIO40       # Verify
    transform:
      mirror_x: true
      mirror_y: true

# --- RTC ---
time:
  - platform: pcf85063
    id: rtc_time
    i2c_id: i2c_bus
    address: 0x51
  - platform: homeassistant
    id: ha_time
    on_time_sync:
      then:
        - pcf85063.write_time

# --- Brightness control ---
# NOTE: mipi_spi exposes a native `brightness:` option on the display block (set above).
# The lambda-driven template-output approach below is unverified for mipi_spi — confirm
# the C++ method name against ESPHome source before relying on it. Prefer driving the
# display block's brightness directly, or use AXP2101 PMIC backlight if your board wires it.
output:
  - platform: template
    id: display_brightness
    type: float
    write_action:
      - lambda: |-
          // Verify method name in esphome/components/mipi_spi source before flashing
          id(amoled_display).set_brightness(state * 255);

light:
  - platform: monochromatic
    id: backlight
    name: "Display Brightness"
    output: display_brightness
    default_transition_length: 0ms
    restore_mode: ALWAYS_ON
    initial_state:
      brightness: 80%

# --- LVGL ---
lvgl:
  displays:
    - amoled_display
  touchscreens:
    - ts
  # Add your UI pages here
```

> This is a scaffold — not guaranteed to work as-is. Treat it as a starting checklist
> of components to configure, not a ready-to-flash file.

---

## 5. Recommended Development Approach

### Phase 1 — Validate Hardware with Arduino/ESP-IDF

1. Flash the official Waveshare sample code from `waveshareteam/ESP32-S3-Touch-AMOLED-2.16`
2. Confirm display, touch, IMU, RTC, and Wi-Fi all work
3. Note the exact GPIO pin numbers from the working sample — these are your ground truth

### Phase 2 — Minimal ESPHome Boot

1. Start with **just Wi-Fi + HA API**, no display, to confirm ESPHome can flash and connect
2. Add PSRAM config and verify it initialises
3. Add the CO5300 display with a simple `fill(Color::BLACK)` lambda — confirm no crash

### Phase 3 — Display + Touch

1. Bring up the display with a test pattern
2. Add the touchscreen and log touch events to confirm coordinates are correct
3. Apply `mirror_x`/`mirror_y` transforms as needed for correct orientation

### Phase 4 — LVGL UI

1. Add a simple LVGL label showing a Home Assistant sensor value
2. Add a button wired to a HA service call (e.g. toggle a light)
3. Expand the UI iteratively — pages, navigation, controls

### Phase 5 — Polish

1. Add screensaver / display sleep (critical for AMOLED burn-in prevention)
2. Add the PCF85063 RTC for accurate time display
3. Optionally add QMI8658 IMU (wake-on-motion, orientation detection)
4. Tune LVGL styles to suit the 480×480 round-ish form factor

---

## 6. LVGL Tips for 480×480 AMOLED

- **480×480 is square but the AMOLED panel may have rounded corners** — design your UI with
  content centred and away from edges
- Use large tap targets (minimum ~60px) — this is a small physical screen
- Avoid dense text; prioritise icons + single values per tile
- Implement a **screensaver that blanks the display** after inactivity to prevent burn-in.
  ESPHome supports this via an LVGL screensaver or a `display.turn_off` action on a timer
- LVGL's `lv_tileview` widget works well for swipeable pages on a small touch panel
- For HA entity control, use `homeassistant.service` actions on LVGL button press events

### Example: LVGL button that toggles a HA light

```yaml
lvgl:
  pages:
    - id: main_page
      widgets:
        - btn:
            id: light_btn
            x: 140
            y: 200
            width: 200
            height: 80
            widgets:
              - label:
                  text: "Toggle Light"
            on_click:
              - homeassistant.service:
                  service: light.toggle
                  data:
                    entity_id: light.living_room
```

---

## 7. Burn-in Protection (Important for AMOLED)

AMOLED displays are susceptible to burn-in from static images. Always implement:

```yaml
# Blank display after 60s of no touch input
display:
  - platform: mipi_spi
    # ... other config ...
    on_page_change:  # reset timer on interaction

script:
  - id: sleep_timer
    mode: restart
    then:
      - delay: 60s
      - light.turn_off: backlight

touchscreen:
  - platform: cst9217
    on_touch:
      - script.execute: sleep_timer
      - light.turn_on:
          id: backlight
          brightness: 80%
```

---

## 8. Useful Links Summary

| Resource | URL |
|---|---|
| Official board sample code | https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16 |
| Waveshare ESP32 component drivers | https://github.com/waveshareteam/Waveshare-ESP32-components |
| **Best ESPHome derivation target** (2.41" AMOLED) | https://github.com/SentientCustard/esphome-waveshare-amoled2.41 |
| ESPHome device config for 1.75" (CO5300, same driver) | https://devices.esphome.io/devices/waveshare-esp32-s3-touch-amoled-175/ |
| ESPHome CO5300/SH8601 driver discussion | https://github.com/orgs/esphome/discussions/3229 |
| ESPHome CO5300 green line bug | https://github.com/esphome/esphome/issues/15765 |
| HA community — 2.41" AMOLED ESPHome thread | https://community.home-assistant.io/t/waveshare-esp32-s3-amoled-2-41-with-esphome/927907 |
| HA community — 2.06" watch ESPHome thread | https://community.home-assistant.io/t/waveshare-esp32-s3-touch-amoled-2-06-watch/914798 |
| HA community — 1.8" AMOLED ESPHome thread | https://community.home-assistant.io/t/esp32-s3-1-8inch-amoled-touch/956270 |
| Waveshare 2.16" product page | https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm |
| Waveshare 2.16" wiki/docs | https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16 |
| ESPHome LVGL docs | https://esphome.io/components/lvgl/ |
| ESPHome mipi_spi display docs | https://esphome.io/components/display/mipi_spi |

---

## 9. Things That Won't Work (Yet)

- **SD Card** — Board has a TF slot; ESPHome added an `sd_mmc_card` component in 2024 so
  basic read/write works, but LVGL cannot load image assets directly from SD — they must
  be embedded at compile time or loaded from internal flash (LittleFS; SPIFFS is deprecated
  on current ESP-IDF)
- **Audio/microphones** — Possible with `i2s_audio` but complex; not recommended for a first build
- **AXP2101 battery fuel gauge** — No native ESPHome component; can read voltage via I2C lambda
  as a workaround
- **Full Lovelace dashboard streaming** — You'd need the RemoteWebView approach (separate Docker
  server + headless Chromium) rather than ESPHome; possible but heavyweight and not ESPHome-native

---

*Guide compiled May 2026. ESPHome evolves rapidly — check the ESPHome changelog and the
HA community threads above for updates, especially around CO5300 driver fixes.*
