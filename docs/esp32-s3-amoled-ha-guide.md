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

> **Panel geometry trap.** Marketing says 480×480 but the visible AMOLED is **466×466**,
> centred inside a 480×480 controller addressable region with ~7-pixel inset on each side.
> The ESPHome `mipi_spi` CO5300 driver already applies an internal offset, so **do not**
> stack your own `offset_width: 6` on top — it ends up *under-drawing* and the unwritten
> bezel rim shows pink/garbage. Practical fix: declare `width: 480, height: 480` and let
> the LVGL black background over-draw the bezel rim (AMOLED black = pixels off = invisible).
> See §3 below.

> **GPIO map** — verified from `waveshareteam/ESP32-S3-Touch-AMOLED-2.16` `pin_config.h`
> and confirmed working in ESPHome 2026.5.1:
>
> | Bus | Function | GPIO |
> |---|---|---|
> | QSPI LCD | CS | 12 |
> | QSPI LCD | SCLK | 38 |
> | QSPI LCD | D0..D3 | 4, 5, 6, 7 |
> | QSPI LCD | RST | **2** (shared with TP_RST) |
> | I²C | SDA | 15 |
> | I²C | SCL | 14 |
> | Touch | INT | 11 |
> | Touch | RST | **2** (shared with LCD_RST) |
>
> Display and touch share RST on GPIO 2. In ESPHome that needs
> `allow_other_uses: true` on **both** the `display.reset_pin` and `touchscreen.reset_pin`
> blocks — set it on only one side and the config validator errors out.

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
is the closest reference for ESPHome's CO5300 bring-up — but several specifics are
**wrong for our 2.16" board on ESPHome 2026.5.1+**, so do not copy blindly. Key fixes:

- **Pins differ** on every board variant — use the verified 2.16" map in §1, not the 1.75" pins.
- **`dimensions:` should be `480×480` on ESPHome 2026.5.1+**, not `466×466`. The driver
  now applies its own 6-pixel offset; stacking your own underdraws and produces pink
  edges. See §3.
- **`platform: cst9217` doesn't work** on the 2.16" board — it identifies the chip but
  produces no touch events. Use the vendored `cst9220` driver instead (§3a).
- `invert_colors: false` is correct for both boards (verified on the 2.16").

> Read 1.75" / 2.41" / 2.06" configs for the *structure* (`spi:`, `display:`, `touchscreen:`
> blocks) and the gestalt — but cross-check every pin, dimension, and driver name against
> §1 and §4 of this guide before flashing.

---

### 2e. Working Arduino touch reference (use this to debug CST9220)

**[lewisxhe/SensorLib](https://github.com/lewisxhe/SensorLib)** — `TouchDrvCST92xx.cpp`
and `TouchDrvCST92xx.h` under `src/touch/`.

This is the **only** community CST9220 driver we've found that actually produces touch
events on this board. When debugging a port (e.g. an ESPHome external_component) keep
this open and diff your I²C transactions byte-for-byte against the SensorLib calls.
The post-read ACK (`D0 00 AB` after every DATA_REG read) is the single most commonly
missed step — see §3a.

A complete working Arduino consumer of this lib for the 2.16" board exists at
`sand-multi-task-waveshare-esp32-s3-2_16inch` in
[esp32-cheap-yellow-display-examples](https://github.com/billkrahmer/esp32-cheap-yellow-display-examples)
(check the call sequence: `touch.setPins(TP_RST, TP_INT)`, `touch.begin(Wire, 0x5A, ...)`,
`touch.setSwapXY(true)`, `touch.setMirrorXY(true, false)`).

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
| CST9220 touch | ⚠️ Vendored | No ESPHome platform for CST92xx. Public `cst9217` forks (shelson, fuzzybear62) read register info OK but **do not produce touch events** — see §3a below. Working driver in this repo at `components/cst9220/` is derived from `lewisxhe/SensorLib TouchDrvCST92xx`. `cst226` is a different register layout — not drop-in. |
| PCF85063 RTC | ✅ Native | `platform: pcf85063` |
| QMI8658 IMU | ⚠️ Custom component | No native driver; use the custom component from the 2.41" repo |
| AXP2101 PMIC | ⚠️ Limited | Can control via I2C lambda; no dedicated platform |
| Dual microphones | ⚠️ Complex | Possible via `i2s_audio`; requires custom config |
| PSRAM | ✅ Required | Must enable `psram: mode: octal, speed: 80MHz` |
| Wi-Fi / HA API | ✅ Native | Standard ESPHome |
| LVGL UI | ✅ Native | Full LVGL support in ESPHome 2024.x+ |

### Known Bug: CO5300 Green Line Artifact (Fixed Upstream)

ESPHome [issue #15765](https://github.com/esphome/esphome/issues/15765) (now **CLOSED**)
documented a single green pixel-wide line at the panel edge caused by a missing
`esp_lcd_panel_set_gap()` 6-pixel column offset. ESPHome 2026.5.1 and later already
include the fix.

> ⚠️ **Don't reintroduce it.** The historical workaround was to declare `dimensions:
> width: 466, height: 466` and add `offset_width: 6`. With the fix landed upstream the
> driver already offsets by 6. Adding your own `offset_width: 6` on top of that produces
> a thin pink/garbage ring on *all four* edges because the visible 466 region now starts
> 6 columns *past* where you're drawing. The reliable config in 2026.5.1+ is:
>
> ```yaml
> dimensions:
>   width: 480
>   height: 480
> # no offset_width, no offset_height
> ```
>
> 480×480 over-draws into the unwritten bezel rim, but the LVGL black background fills
> those pixels and AMOLED black = pixels off = invisible.

### Other display caveats discovered

- **`invert_colors`**: on this 2.16" panel (and ESPHome 2026.5.1) set to `false`. Setting
  `true` (as some community configs show for adjacent boards) produces an inverted
  palette — LVGL `bg_color: 0x000000` renders as *white* and white text renders as black.
- **`data_rate: 40MHz`** works reliably. The 80 MHz value in some configs has reportedly
  caused intermittent corruption on the QSPI bus.
- **`brightness: 0xD0`** (~208/255) is a good idle default. The native `brightness:` key
  on the `mipi_spi` display block works; do **not** use the lambda
  `id(display).set_brightness()` approach in older guide drafts — the method name varies
  by ESPHome version and there's no upstream guarantee.
- **AMOLED has no PWM backlight.** Brightness is driven via a DCS command sent by the
  display driver itself. Phase 4 (idle/wake) drives this through the native key, not via
  a `light: monochromatic` entity bound to a template output.

---

## 3a. CST9220 touch driver — what the community forks get wrong

The Hynitron CST9220 is documented well enough by `lewisxhe/SensorLib`
([TouchDrvCST92xx.cpp](https://github.com/lewisxhe/SensorLib/blob/master/src/touch/TouchDrvCST92xx.cpp))
but the public ESPHome ports (`shelson/esphome-cst9217`, `fuzzybear62/esphome-cst9217`,
and downstream copies) ship a version that **reads register info correctly but never
produces touch events**. After flashing you'll see the chip identify itself
(`Chip Type: 0x9220`, resolution 480x480, etc.) but every poll of the data register
returns either all zeros, a stuck-stale frame, or `FF FF FF FF 0A 00 0A 00 E0 01`.

The vendored driver in this repo (`components/cst9220/`) fixes that. The four substantive
bugs vs. SensorLib were:

1. **Missing post-read ACK.** After reading the 10-byte data block from `0xD000`, the
   chip locks its buffer until you write the three bytes `D0 00 AB` back. SensorLib does
   this in `getPoint()`. The community forks don't. Without the ACK the chip refills
   exactly once and then returns garbage forever.

2. **Wrong cmd-mode entry byte count.** `write_register16(0xD101, [0xD1, 0x01], 2)` puts
   *four* bytes on the wire (`D1 01 D1 01`). The chip's command-mode protocol expects
   *two* (`D1 01`). Use raw `this->write(buf, 2)` instead of the 16-bit register helper
   for this specific write.

3. **Spurious explicit exit-cmd-mode write.** Several forks write to `0xD109`
   (NORMAL_MODE_REG) at the end of setup. SensorLib doesn't — the chip auto-transitions
   to normal scan mode on first DATA_REG access. The explicit write actually puts it
   into a hung half-scan state where ACKs no longer work.

4. **Missing 8-byte FW-version read at `0xD208`.** SensorLib's `getAttribute()` reads
   this before returning. Skipping it leaves the chip in a half-init state where it
   acknowledges I²C but never schedules a touch scan.

The driver in this repo also keeps `transform: swap_xy: true, mirror_x: true,
mirror_y: false` per the working Arduino reference's `setSwapXY(true) +
setMirrorXY(true, false)`. Adjust per your board mounting.

### Power-cycle quirk after flash

After **every** firmware flash, the touch IC reports no touches until the device is fully
power-cycled (USB unplug or the AXP2101 PWR button, *not* the ESP32 reset button). Same
behaviour reproduces on the working `sand-multi-task` Arduino driver and the
`esphome-lvgl-dashboard` sibling project. The chip enters a bad state during the ESP32's
partial reset and only a full PMIC cycle recovers it. Document this in any operator
runbook — it will catch every new user once.

---

## 4. ESPHome Base Config Template (verified on ESPHome 2026.5.1)

GPIO numbers, dimensions, transforms, and data-rate below are the **working** values
from this repo (`boards/waveshare-2.16.yaml` + `packages/base.yaml`). Use this as-is
as a starting point — every value has been confirmed on hardware.

```yaml
esphome:
  name: ha-amoled-panel
  friendly_name: HA AMOLED Panel
  platformio_options:
    upload_speed: 921600
    board_build.flash_mode: dio
    board_build.f_flash: 80000000L
    board_build.f_cpu: 240000000L

esp32:
  board: esp32-s3-devkitc-1
  variant: esp32s3
  flash_size: 16MB
  framework:
    type: esp-idf
    sdkconfig_options:
      # PSRAM perf — required for smooth LVGL framebuffer flushes.
      CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240: y
      CONFIG_ESP32S3_DATA_CACHE_64KB: y
      CONFIG_ESP32S3_DATA_CACHE_LINE_64B: y
      CONFIG_SPIRAM_FETCH_INSTRUCTIONS: y
      CONFIG_SPIRAM_RODATA: y

psram:
  mode: octal      # Required for 8 MB stacked PSRAM on ESP32-S3R8
  speed: 80MHz

# --- Connectivity ---
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  # Google Wifi (and other consumer mesh) drops the ESP32 if either of these is wrong.
  # Skip DHCP via manual_ip if the AP refuses association after 3-4 retries.
  power_save_mode: NONE
  fast_connect: true
  # manual_ip:
  #   static_ip: 192.168.x.x
  #   gateway: 192.168.x.1
  #   subnet: 255.255.255.0
  #   dns1: 8.8.8.8

api:
  encryption:
    key: !secret api_encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

logger:
  level: INFO

improv_serial:   # first-flash Wi-Fi onboarding without rebuilding

time:
  - platform: homeassistant
    id: ha_time

# --- I2C Bus (touch, IMU, RTC, AXP2101 PMIC) ---
i2c:
  - id: bus_a
    sda: 15
    scl: 14
    frequency: 400kHz
    scan: true

# --- SPI Bus (display — QSPI) ---
spi:
  - id: lcd_qspi
    type: quad
    clk_pin: 38
    data_pins: [4, 5, 6, 7]

# --- Display ---
display:
  - id: amoled_display
    platform: mipi_spi
    model: CO5300
    spi_id: lcd_qspi
    bus_mode: quad
    cs_pin: 12
    reset_pin:
      number: 2
      allow_other_uses: true   # shared with CST9220 touch RST
    dimensions:
      width: 480     # over-draw past visible 466 — driver applies its own offset
      height: 480
    data_rate: 40MHz
    color_order: rgb
    invert_colors: false       # set true and your LVGL colours invert (see §3)
    brightness: 0xD0           # ~80%; P4 idle state machine drives this dynamically
    update_interval: never     # LVGL drives flushes
    auto_clear_enabled: false

# --- Touch ---
# Use the vendored driver at components/cst9220/ — derived from lewisxhe/SensorLib.
# The public shelson/fuzzybear62 forks do NOT produce touch events (see §3a).
external_components:
  - source:
      type: local
      path: components
    components: [cst9220]

touchscreen:
  - id: main_touchscreen
    platform: cst9220
    i2c_id: bus_a
    address: 0x5A
    reset_pin:
      number: 2
      allow_other_uses: true   # shared with display RST
    touch_timeout: 50ms
    update_interval: 50ms
    transform:
      swap_xy: true
      mirror_x: true
      mirror_y: false

# --- RTC (added in Phase 7) ---
# time:
#   - platform: pcf85063
#     id: rtc_time
#     i2c_id: bus_a
#     address: 0x51
#     on_time_sync:
#       then:
#         - pcf85063.write_time

# --- LVGL ---
lvgl:
  displays:
    - amoled_display
  buffer_size: 25%
  bg_color: 0x000000          # AMOLED black = pixels off
  pages:
    - id: hello_page
      bg_color: 0x000000
      widgets:
        - label:
            align: CENTER
            text: "Hello"
            text_color: 0xFFFFFF
            text_font: montserrat_48
```

> Scaling this into a real project: keep the top-level YAML thin and split the
> hardware-specific blocks (chip/PSRAM/SPI/I²C/display/touch) into a `boards/<name>.yaml`
> package, plus shared services (`wifi`/`api`/`ota`/`logger`) into `packages/base.yaml`,
> and LVGL config into `packages/lvgl-ui.yaml`. The top YAML then just merges them via
> ESPHome's `packages:` mechanism. See this repo's `ha-amoled-panel.yaml` for the layout.

---

## 5. Recommended Development Approach

The phased plan in [plan.md](../plan.md) is the authoritative ordering for *this* repo.
The summary below is the generic version for anyone starting from scratch on a 2.16"
board — read `plan.md` for the actual decisions and dependencies (idle/wake before UI,
static-then-dynamic HA entity model, etc.).

### Phase 0 — Validate hardware with the vendor sample

Before ESPHome: flash the official Waveshare sample from
`waveshareteam/ESP32-S3-Touch-AMOLED-2.16` and confirm display, touch, IMU, RTC,
and Wi-Fi all work. Note the exact GPIO numbers from `pin_config.h` — they are
ground truth, and any guide/community config that disagrees is wrong.

### Phase 1 — Minimal ESPHome boot

Wi-Fi + HA API only, no display. Confirms ESPHome can flash, OTA works, and the
device appears in the HA ESPHome integration. PSRAM `mode: octal, speed: 80MHz`
must already be set or later display init will silently fail.

### Phase 2 — Display bring-up

Add `spi:` + `display: mipi_spi model: CO5300` with the verified pins from §1. Render
a single LVGL `"Hello"` label on a black page. Watch for pink bezel edges (see §3 for
the geometry/offset trap) and inverted colours (toggle `invert_colors`).

### Phase 3 — Touch bring-up

Add the vendored CST9220 driver from this repo (or port your own using §3a as the
checklist of bugs to avoid). Log raw touch events; calibrate `transform: swap_xy /
mirror_x / mirror_y` against actual finger-down coordinates. **Expect to power-cycle
the device after every flash** until touch starts responding.

### Phase 4 — Idle state machine + IMU wake (battery-first)

Implement `active → dim → blank` with touch+IMU wake **before** any real UI. The
AMOLED panel itself drives nearly the whole standby budget; native `brightness: 0`
puts it to sleep without losing the HA API connection. QMI8658 has no native ESPHome
component — port the one from the SentientCustard 2.41" repo. ESP32 deep sleep is
**not** the right answer (it kills the HA API link on every wake).

### Phases 5–9 — HA entity model, LVGL UI, polish, multi-board, dynamic discovery

See `plan.md`.

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

## 7. Burn-in protection / idle blanking (important for AMOLED)

AMOLED displays are susceptible to burn-in from static images. They also draw nearly
zero panel current when every pixel is black, so the same code path serves both
burn-in prevention **and** battery life.

Don't drive blanking via a `light: monochromatic` entity bound to a template `output:`.
The CO5300 has no PWM backlight — there's no analog brightness pin to write to. Drive
the panel's native brightness directly. Sketch:

```yaml
script:
  - id: idle_timer
    mode: restart
    then:
      - delay: ${dim_timeout}        # e.g. 15s
      - lambda: id(amoled_display).set_brightness(0x30);    # dim
      - delay: ${blank_timeout}      # additional 30s
      - lambda: id(amoled_display).set_brightness(0x00);    # blank
      - lvgl.page.show: blank_page   # all-black LVGL page

touchscreen:
  - platform: cst9220
    on_touch:
      - lambda: id(amoled_display).set_brightness(0xD0);   # wake
      - lvgl.page.show: main_page
      - script.execute: idle_timer

# Plus an IMU motion binary_sensor wired the same way for pick-up wake.
```

Verify the exact lambda method (`set_brightness` vs. equivalent) against your installed
ESPHome version before relying on it — the public mipi_spi C++ API has evolved.
Alternative: write the CO5300 DCS 0x51 command directly via the display block's `data:`
hooks. See [plan.md Phase 4](../plan.md) for the full idle state machine spec.

---

## 8. Useful Links Summary

| Resource | URL |
|---|---|
| Official board sample code | https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16 |
| Waveshare ESP32 component drivers | https://github.com/waveshareteam/Waveshare-ESP32-components |
| **Authoritative CST92xx Arduino driver** | https://github.com/lewisxhe/SensorLib (`src/touch/TouchDrvCST92xx.cpp`) |
| Working Arduino consumer (sand-multi-task) | https://github.com/billkrahmer/esp32-cheap-yellow-display-examples |
| ESPHome derivation target (2.41" AMOLED — different touch IC) | https://github.com/SentientCustard/esphome-waveshare-amoled2.41 |
| ESPHome device config for 1.75" (CO5300; pin/dim differences — see §2c) | https://devices.esphome.io/devices/waveshare-esp32-s3-touch-amoled-175/ |
| ESPHome CO5300/SH8601 driver discussion | https://github.com/orgs/esphome/discussions/3229 |
| ESPHome CO5300 green line bug (fixed; do not stack workaround on 2026.5.1+) | https://github.com/esphome/esphome/issues/15765 |
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

## 10. Known device quirks (collected from bring-up)

These all bit us at least once and are worth knowing up front:

| Symptom | Cause | Fix |
|---|---|---|
| Pink strip on right + bottom edges | Drew 466×466 into a 480×480 controller region; uninitialised bezel rim shows through | Declare `dimensions: width: 480, height: 480`; let LVGL black bg over-draw. **Do not** add `offset_width: 6` on top of the driver's internal offset. |
| LVGL black bg renders white on panel | `invert_colors: true` flips the palette | Set `invert_colors: false`. |
| Wi-Fi `Association Leave` timeout on Google Wifi mesh | Mesh band-steers / refuses DHCP for ESP32 in power-save | `wifi: power_save_mode: NONE`, `fast_connect: true`, and/or `manual_ip:` static lease. |
| CST9220 reads chip info OK but no touch events fire | Driver fork missing post-read ACK + extra mode writes | Use the vendored driver at `components/cst9220/` — see §3a. |
| CST9220 dead after every flash, alive after USB unplug | Chip enters bad state during ESP32 partial reset | Power-cycle the device (USB unplug / AXP2101 PWR button) after every flash. |
| Pin 2 RST validation error in `esphome config` | Pin 2 is shared between display and touch | `allow_other_uses: true` on **both** `display.reset_pin` and `touchscreen.reset_pin`. Setting it on only one side errors. |
| `Touch Polling Stopped` warning after adding `interrupt_pin` | ESPHome switches to interrupt-driven; if INT isn't wired right, zero events | Drop `interrupt_pin` for polling-mode P3 bring-up. Add it back in P4 only if idle current with 50 ms polling is too high. |
| Logger validation error: per-tag level more verbose than global | ESPHome rejects per-component levels above global | Bump `logger.level` to match the most verbose tag, or drop the per-tag override. |

---

*Guide compiled May 2026, revised through Phase 3 bring-up on ESPHome 2026.5.1.
ESPHome evolves rapidly — check the ESPHome changelog and the HA community threads
above for updates, especially around CO5300 / CST9xxx driver fixes.*
