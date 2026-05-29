# esp32-amoled-ha-panel

ESPHome firmware for a **handheld, battery-powered** Home Assistant remote
running on the **Waveshare ESP32-S3-Touch-AMOLED-2.16** (480×480 AMOLED +
capacitive touch + onboard LiPo + QMI8658 IMU). Lives on the nightstand or in
the palm of your hand: pick it up, screen wakes; set it down and don't touch
it, screen dims then sleeps. Swipe between Home Assistant **areas**
horizontally, vertically scroll **entities** in each area, tap to toggle the
supported ones.

Idle behaviour is the whole point of going to a small AMOLED:
- **No touch + no IMU motion for N seconds** → dim screen.
- **Still no activity** → blank screen (AMOLED black = pixels off — saves
  battery, prevents burn-in).
- **Touch _or_ IMU motion** → instant wake.

Designed so a second/third AMOLED board (1.75", 1.8", 2.41") can be added
later with only a new board package file — UI, HA logic, and idle/wake
behaviour live in shared packages.

> **Status:** scaffolding stage. See [plan.md](plan.md) for the phased build
> plan and what is/isn't working yet.

> ### ⚠️ Required HA setup after first flash
>
> When the panel registers in Home Assistant, HA defaults the per-device
> permission **"Allow the device to perform Home Assistant actions"** to
> **OFF** for security (since HA 2024.6). With it off, every tap on the
> panel will appear to work on-device (you'll see `tap … → homeassistant.…`
> in the firmware log) but **nothing will happen in HA**, and the HA log
> will show one rejection line per tap:
>
> ```
> AMOLED Panel: Service call homeassistant.turn_on: with data
> {'entity_id': 'light.foo'} rejected; If you trust this device …
> enable this functionality in the options flow
> ```
>
> To enable it: **HA → Settings → Devices & Services → ESPHome →**
> click the `ha-amoled-panel` device → **Configure** (gear icon) →
> toggle **"Allow the device to perform Home Assistant actions" ON** →
> Submit.
>
> Per-device, per-config-entry. Re-add or migrating to a new HA install
> resets it. Any other ESPHome device in this repo will need the same flip.

---

## Target hardware (first board)

| | |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-2.16 |
| MCU | ESP32-S3R8 (240 MHz, 8 MB octal PSRAM, 16 MB flash) |
| Display | 2.16" AMOLED 480×480, CO5300 driver, QSPI |
| Touch | CST9220, I²C |
| Extras | QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC |

See [docs/esp32-s3-amoled-ha-guide.md](docs/esp32-s3-amoled-ha-guide.md) for
the full hardware/driver reference, pin tables, and known-working community
configs.

---

## Repo layout (target)

```
.
├── README.md
├── plan.md
├── .gitignore
├── secrets.example.yaml        # template — committed
├── secrets.yaml                # real secrets — gitignored
├── docs/
│   └── esp32-s3-amoled-ha-guide.md
├── ha-amoled-panel.yaml        # top-level device YAML (board-agnostic shell)
├── boards/
│   └── waveshare-2.16.yaml     # pins, display, touch, RTC for this board
│   └── waveshare-1.75.yaml     # (future) other board variants
├── packages/
│   ├── base.yaml                  # wifi, api, ota, logger, time
│   ├── idle.yaml                  # dim/blank state machine + IMU wake
│   ├── lvgl-ui.yaml               # area carousel + entity scroller (board-agnostic)
│   ├── ha-entities.example.yaml   # area + entity template — committed
│   └── ha-entities.yaml           # area + entity real config — gitignored, user-edited
└── components/
    └── (custom ESPHome external_components, e.g. QMI8658)
```

Top-level YAML is a thin shell that `!include`s a board package + shared
packages. Switching boards = swapping one `!include` line.

---

## Quick start

```bash
# 1. Clone
git clone <this-repo>
cd esp32-amoled-ha-panel

# 2. Fill in secrets (NEVER commit this file)
cp secrets.example.yaml secrets.yaml
# edit secrets.yaml with your wifi + HA details

# 3. Define your areas + entities (NEVER commit this file — it lists your real entity_ids)
cp packages/ha-entities.example.yaml packages/ha-entities.yaml
# edit packages/ha-entities.yaml — drop in your areas, entity_ids, friendly names

# 4. Install ESPHome (Python 3.10+, pipx recommended)
pipx install esphome

# 5. Compile + flash over USB (first time)
esphome run ha-amoled-panel.yaml

# 6. In HA: Settings → Devices & Services → ESPHome → ha-amoled-panel
#    → Configure → enable "Allow the device to perform Home Assistant actions"
#    Without this every tap is rejected silently by HA. See the warning above.

# 7. Subsequent updates flash OTA automatically over Wi-Fi
```

---

## Feature scope — first pass

- ✅ Wi-Fi + HA native API (encrypted)
- ✅ OTA updates
- ✅ 480×480 AMOLED display + capacitive touch
- ✅ **QMI8658 IMU for motion-based wake** (pick-up detection)
- ✅ Idle dim → blank → wake-on-touch-or-motion state machine
- ✅ Horizontal swipe = cycle between HA **areas**
- ✅ Vertical scroll = entities within current area
- ✅ Tap to toggle for: `light`, `switch`, `fan`, `input_boolean`, `automation`, `script`, `cover`
- ✅ Read-only status display for unsupported types (`sensor`, `binary_sensor`, `climate`, `media_player`, etc.)
- ✅ Battery voltage readout via AXP2101 (best-effort — no native ESPHome component)
- 🔜 **Dynamic area/entity discovery** via a single HA-side template sensor — re-arrange your home in HA, panel updates without a firmware rebuild. MVP ships with static YAML; dynamic comes in Phase 9 (see [plan.md](plan.md)).

### Out of scope for v1

- Brightness/colour control for lights (toggle only)
- Climate / thermostat control
- Media player transport controls
- Microphone / voice assistant
- SD card asset loading
- Full battery % / charge-curve modelling (raw voltage only)

See [plan.md](plan.md) for the phased delivery plan.

---

## Secrets

`secrets.yaml` must define:

| Key | What |
|---|---|
| `wifi_ssid` / `wifi_password` | Wi-Fi network |
| `ha_url` | HA base URL — reference only, not consumed by firmware in v1 |
| `api_encryption_key` | ESPHome ↔ HA native API encryption key (base64, 32 bytes). HA generates its half automatically; no long-lived token needed |
| `ota_password` | OTA update password |
| `ap_password` | Fallback AP password if Wi-Fi fails |

`secrets.yaml` is in `.gitignore`. Use `secrets.example.yaml` as a template.

---

## References

Primary derivation target: [SentientCustard/esphome-waveshare-amoled2.41](https://github.com/SentientCustard/esphome-waveshare-amoled2.41)
(same MCU + driver family, includes the QMI8658 custom component we'll reuse).

Closest published CO5300 ESPHome config: [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://devices.esphome.io/devices/waveshare-esp32-s3-touch-amoled-175/).

Full reference list: [docs/esp32-s3-amoled-ha-guide.md](docs/esp32-s3-amoled-ha-guide.md).
