# esp32-amoled-ha-panel — Development Log

Consolidated build history: goals, design decisions, trade-offs, technical
notes, on-device gotchas, and the dated session log for the shipped panel
(MVP phases 0–8 + UI enhancement phases E1–E10). Originally two separate
phased build plans (`plan-mvp.md` and `plan-enhance-ui.md`); merged here once
the work shipped so the reasoning stays in one place.

Still-active forward plans live in their own files and are linked from the
[README](README.md): [plan-multi-board-support.md](plan-multi-board-support.md)
and [plan-dynamic-discovery.md](plan-dynamic-discovery.md).

---

# Build Plan — esp32-amoled-ha-panel (MVP)

Phased plan from empty repo → working **handheld, battery-powered** HA remote
on the Waveshare ESP32-S3-Touch-AMOLED-2.16, with a structure that lets us
add other AMOLED boards later by dropping in a new board package.

**Follow-on plans** (split out of this one):
- [plan-multi-board-support.md](plan-multi-board-support.md) — Phase 9, portability across AMOLED boards.
- [plan-dynamic-discovery.md](plan-dynamic-discovery.md) — Phase 10, HA-driven dynamic area/entity discovery.
- UI enhancements (post-MVP) — see the "Build Plan — UI enhancements" section below in this document.

Device runs on a LiPo cell in a hand-held enclosure. Idle screen behaviour
(dim → blank → wake on touch or IMU motion) is a **first-class feature**,
not polish — without it the battery dies in hours.

Background reference: [docs/esp32-s3-amoled-ha-guide.md](docs/esp32-s3-amoled-ha-guide.md).

---

## ⚠️ HA permission flag — flip this or nothing works

**Every time** the panel is added to a Home Assistant install (first flash,
HA migration, device re-add), HA defaults the per-device permission
**"Allow the device to perform Home Assistant actions"** to **OFF**
(default since HA 2024.6). With it off, the panel's firmware log shows
taps dispatching correctly (`tap … → homeassistant.turn_on`) but
**nothing happens in HA**. HA log shows one rejection per tap:

```
AMOLED Panel: Service call homeassistant.turn_on: with data
{'entity_id': 'light.foo'} rejected; If you trust this device and want
to allow access for it to make Home Assistant service calls, you can
enable this functionality in the options flow
```

Fix: **HA → Settings → Devices & Services → ESPHome →** click the
`ha-amoled-panel` device → **Configure** → toggle the permission **ON** →
Submit. Per-device, per-config-entry — has to be done for each ESPHome
device on the install.

Discovered after P7c verify (2026-05-28): P6's earlier on-device tap
verification stopped working without any code change. Permission had been
flipped on once and silently reset, probably by a HA update or re-add.
Capturing here so future-me doesn't waste another half-hour chasing the
firmware when HA is the one silently rejecting calls.

---

## Status overview

> **Legend:** ⬜ not started · 🟡 in progress · 🔴 blocked · ✅ done
>
> Update the status emoji here AND on the phase's own status line when you start/finish work. Tag the commit that lands phase exit criteria with `git tag pN-<short-name>` so the audit trail lives in git too.

| # | Phase | Status | Notes |
|---|---|---|---|
| 0 | Repo bootstrap | ✅ | secrets, gitignore, README, plan |
| 1 | Skeleton (Wi-Fi + HA API, no display) | ✅ | flashed, Wi-Fi up, HA API connected |
| 2 | Display bring-up (CO5300) | ✅ | Hello label rendering, no edge artifacts |
| 3 | Touch bring-up (CST9220) | ✅ | press events fire after vendored driver rewrite + power-cycle |
| 4 | Idle state machine + IMU wake | ✅ | verified on-device 2026-05-28: dim @ 16s, blank @ 45s, touch wakes from blank, motion wakes from dim |
| 5 | Static HA entity model (MVP) | ✅ | verified 2026-05-28: 88 entity subscriptions arrived within ~1.5 s of HA connect, states match HA |
| 6 | LVGL UI: area carousel + entity scroller | ✅ | verified 2026-05-28: tileview swipe, vertical scroll, tap-toggle, area picker modal all live; touch transform reset to no-swap/no-mirror |
| 7a | Polish round 1 (clock, settings tile, splash, tap feedback) | ✅ | verified 2026-05-28; battery + RTC deferred |
| 7b | Polish round 2 (header layout, battery + Wi-Fi icons, Apply/Cancel in settings) | ✅ | verified on-device 2026-05-29 |
| 7c | Entity control: explicit on/off + per-domain update operations | ✅ | verified on-device 2026-05-29 (tap-toggle live after the P7d attr-sub burst was disabled) |
| 7d | Per-entity detail/popup view (light dim/colour, climate, media, number, select) | ✅ | verified on-device 2026-05-29; live-attr modal preload parked → P10 (see Post-P7 TODO) |
| 7e | Per-entity icons (left of friendly name) | ✅ | v1: YAML override → domain default → fallback (zero new subs); baked MDI subset. HA `icon` attr deferred to P10 batched sensor. On-device verified |
| 7f | Per-entity tap-confirmation guard | ✅ | verified on-device 2026-05-29. `confirm: true` → short-tap opens confirm sheet (action/lock/cover/switch) or detail modal (light/climate/…). switch/input_boolean use action sheet (no P7d modal exists for binary domains) |
| 8 | Power management (sleep/wake for nightstand) | ✅ | verified on-device: light+deep sleep, settings toggle/mode, idle-tier transition, touch GPIO11 wake, Wi-Fi drop/restore |
| 9 | Multi-board support | ⬜ | |
| 10 | Dynamic discovery via HA template sensor | ⬜ | replaces P5 static YAML |

**Last updated:** 2026-05-30 (P8 verified on-device: light+deep sleep, settings toggle/mode, touch GPIO11 wake, Wi-Fi drop/restore. Next: P9 multi-board).

---

## Guiding principles

1. **Ship in vertical slices.** Each phase ends with something flashable that
   does *more* than the previous phase. No big-bang merges.
2. **Board package isolates hardware.** All pins, display init, touch driver
   live in `boards/<name>.yaml`. UI and HA logic never reference pins.
3. **HA areas + entities start declarative, become dynamic.** MVP (P5) reads a static YAML list; P10 swaps to a single HA template sensor that pushes JSON. Each phase ships a usable panel.
4. **Battery-first.** Idle dim+blank and IMU wake are in Phase 4 — before any
   real UI. Every later phase is tested with the idle state machine running,
   so we catch power regressions early.
5. **External components pinned by commit.** Touch + IMU drivers come from
   community forks; pin to a SHA so a remote rebase can't break our build.

---

## Phase 0 — Repo bootstrap

**Status:** ✅ done · tag: _untagged_

**Goal:** structure + secrets in place; nothing flashable yet.

- [x] `.gitignore` excludes `secrets.yaml`, ESPHome build dirs
- [x] `secrets.example.yaml` template committed
- [x] `secrets.yaml` placeholder created (gitignored)
- [x] `README.md` overview
- [x] `plan.md` (this file)
- [x] `docs/esp32-s3-amoled-ha-guide.md` reference guide already present

**Exit criteria:** `git status` clean; secrets not staged; reader can
understand intent from `README.md` alone.

---

## Phase 1 — Skeleton ESPHome project, Wi-Fi + HA API only

**Status:** ✅ done · target tag: `p1-skeleton`

**Goal:** board boots, connects to Wi-Fi, appears in HA, accepts OTA. No display.

Files added:
- [x] `ha-amoled-panel.yaml` — top-level device YAML. `packages:` merges board + base.
- [x] `boards/waveshare-2.16.yaml` — board-specific substitutions and `esp32:` block (PSRAM `mode: octal`, 16 MB flash, ESP-IDF framework). **No display/touch yet.**
- [x] `packages/base.yaml` — `wifi:`, `api:`, `ota:`, `logger:`, `captive_portal:`, fallback AP, `improv_serial`, HA time source. All values pulled from `!secret`.

Tasks:
- [x] Pick a friendly_name + node name. Made substitutions in board package (overridable in top YAML).
- [x] Add `improv_serial:` for first-flash Wi-Fi onboarding without rebuilding.
- [x] Confirm `psram: mode: octal, speed: 80MHz` is in the board package (required by ESP32-S3R8 with 8 MB stacked PSRAM — guide §1).
- [x] Set `api.encryption.key: !secret api_encryption_key`.
- [x] `esphome config ha-amoled-panel.yaml` → "Configuration is valid!" against ESPHome 2026.5.1.
- [x] `esphome run ha-amoled-panel.yaml` over USB → boot OK, Wi-Fi connected, HA API client connected, device live in HA ESPHome integration.

**Exit criteria:** Flash over USB, device shows up in HA with no entities, OTA from `esphome run` works wirelessly. Log shows `[psram] heap initialized` with ~8 MB free.

**Risks / unknowns:**
- ESP-IDF vs Arduino framework choice — go ESP-IDF (LVGL + mipi_spi need it, per guide §4).

---

## Phase 2 — Bring up display

**Status:** ✅ done · target tag: `p2-display`

**Goal:** AMOLED lights up with a solid colour or test pattern.

Files added to `boards/waveshare-2.16.yaml`:
- [x] `spi:` block (QSPI, `type: quad`, clk 38, data pins [4,5,6,7])
- [x] `display:` block (`platform: mipi_spi`, `model: CO5300`, dimensions 480×480, `data_rate: 40MHz`, `invert_colors: true`, `brightness: 0xD0`, `auto_clear_enabled: false`)

Tasks:
- [x] Pins verified against working sibling project `esphome-lvgl-dashboard` (same board): CS=12, SCLK=38, D0..D3=4,5,6,7, RST=2. Guide §4 had 1.75 board pins which differ.
- [x] ESPHome 2026.5.1 has the #15765 fix. No `offset_width` needed when over-drawing to 480.
- [x] `homeassistant.event: esphome.display_first_frame` fires from top-level `on_boot` after 2s delay. Verified in HA Developer Tools → Events listener.
- [x] `packages/lvgl-ui.yaml` ships single page with white "Hello" label on black bg.

**Edge-artifact resolution:** Tried (a) native 466x466 → pink strip on right + bottom; (b) 466x466 + offset_width/height: 6 → thin pink ring on all four edges; (c) over-draw 480x480, no offset → clean. Driver appears to apply its own internal offset, so stacking ours shifts the image. Over-draw + AMOLED black bg = invisible bezel rim.

**Exit criteria:** Panel shows "Hello" centred. No green edge line. No crash log on boot.

**Risks / unknowns:**
- CO5300 init sequence quirks — the guide notes runtime chip-ID detection logic landed upstream; if the display stays black, log the bus mode and CS/RESET pin levels first before tweaking the init sequence.
- AMOLED brightness control: native `brightness:` on `mipi_spi` is the preferred path; the lambda `set_brightness()` approach in the guide is **unverified** (guide §4 explicit warning). Use the YAML key, not the lambda, until confirmed.

---

## Phase 3 — Bring up touch

**Status:** ✅ done · target tag: `p3-touch`

**Goal:** Touches are logged with correct (x, y) coordinates.

Files added:
- [x] `components/cst9220/` — vendored from sibling `esphome-lvgl-dashboard`, then rewritten against lewisxhe `SensorLib TouchDrvCST92xx` reference (working Arduino driver in `esp32-cheap-yellow-display-examples/projects/sand-multi-task-waveshare-esp32-s3-2_16inch`).
- [x] `boards/waveshare-2.16.yaml` — `i2c:` bus on SDA=15/SCL=14, `external_components:` local path, `touchscreen:` block with shared `reset_pin: 2` (`allow_other_uses` on both display + touch), polling at 50ms, `transform: swap_xy: true, mirror_x: true`.

Tasks:
- [x] Driver port — vendored sibling driver, then fixed 3 bugs against SensorLib: (1) raw 2-byte write for cmd-mode entry instead of `write_register16` 4-byte form; (2) added 8-byte FW-version read at 0xD208 (load-bearing for chip to enter scan mode); (3) write ACK `D0 00 AB` after every DATA_REG read so chip releases buffer for next scan; (4) dropped the bogus exit-cmd-mode write (chip transitions automatically on first DATA_REG access).
- [x] Raw touch logging at INFO via board `on_touch` lambda.
- [x] Single-finger press events fire (status 0x06). Release events (0x00 / 0x0B) intentionally ignored; chip emits them after every press.
- [~] Multi-touch/gesture: P6 confirmed single-touch is enough — LVGL tileview swipe, vertical scroll, and tap-toggle all worked with `CST9220_MAX_TOUCH_POINTS = 1`. Deferred to backlog; no pinch-zoom / two-finger use case in v1.

**Operator quirk:** After every flash, touch is dead until the device is power-cycled (USB unplug or PWR button). Same behavior on the working Arduino reference and sibling ESPHome project. Cause unknown — chip enters bad state during ESP32 partial reset. Document and live with it.

**Exit criteria:** A tap in each corner logs coordinates close to (0,0), (479,0), (0,479), (479,479) after transforms.

**Risks / unknowns:**
- CST9220 may need a register tweak the cst9217 driver doesn't make. Budget a half-day spike here; if blocked, fall back to polling-style touch using the existing driver and revisit later.

---

## Phase 4 — Idle state machine + IMU wake (battery-critical)

**Status:** ✅ done · target tag: `p4-idle`

**Goal:** Device is usable on a LiPo for more than a few hours. Screen dims, then blanks, then wakes on touch *or* IMU motion. Wired up before any real UI so we catch power regressions in every later phase.

Files added:
- [x] `packages/idle.yaml` — global state (`active`/`dim`/`blank`), restart-mode scripts driving transitions, runtime brightness via `mipi_spi::set_brightness(uint8_t)`.
- [x] `components/qmi8658/` — minimal polling driver (WHO_AM_I check, accel @ 31.25 Hz LP, frame-to-frame |Δa| magnitude). Hardware INT pin not exposed on this board, so software polling is the only path. Verified compile against ESPHome 2026.5.1.
- [x] Board package gains the `qmi8658:` block. **No interrupt pin** — Waveshare's own pin_config.h does not route IMU INT; their official `04_LVGL_QMI8658_ui` example polls.

### State machine

```
        any touch / IMU motion above threshold
   ┌────────────────────────────────────────────┐
   ▼                                            │
[active]  --no-input for ${dim_timeout}-->  [dim]  --no-input for ${blank_timeout}-->  [blank]
   ▲                                                                                    │
   └────────────────────────────────────────────────────────────────────────────────────┘
                              touch / motion wakes from any state
```

Default substitutions (tunable in `secrets.yaml` or top-level overrides):
- `dim_timeout: 15s` → drop backlight from 80% to 15%.
- `blank_timeout: 30s` → black LVGL page, backlight off (AMOLED pixels off = ~0 mA from panel).
- Motion threshold: small enough to catch picking the device up; large enough to ignore vibration on the nightstand.

### IMU integration

- [x] Bring up QMI8658 on the I²C bus. Component reads WHO_AM_I at setup; mark_failed if != 0x05.
- [~] Hardware "any-motion" interrupt **not used** — Waveshare board does not route the IMU INT pin to a free GPIO (confirmed against waveshareteam pin_config.h, and Waveshare's own `04_LVGL_QMI8658_ui` example polls). Software path is the only path on this board.
- [x] Software motion detection: poll accel at 100 ms (10 Hz), compute frame-to-frame `|Δa|` magnitude, fire `imu_motion` binary_sensor when delta > `motion_threshold` (default 0.10 g). 500 ms hold-off keeps the sensor latched briefly so the idle script wakes cleanly.
- [x] `imu_motion` is `internal: true` and its `on_press` calls `script.execute: notify_input` — same entry point the touchscreen on_touch uses.

### Display blanking strategy

- "Blank" = set display brightness to `0x00` via `mipi_spi::set_brightness(uint8_t)`. AMOLED at brightness 0 = pixels emit no light = ~0 mA from the panel. No LVGL page swap needed; the current rendered frame just goes dark.
- "Dim" = `set_brightness(0x20)` (~12%). Same mechanism — no page swap, no widget teardown.
- Verified that `mipi_spi.h:103` exposes `set_brightness(uint8_t)` and re-runs `reset_params_()` so the BRIGHTNESS command (0x51) is re-issued. The plan-level "lambda set_brightness is unverified" warning is resolved: it works in ESPHome 2026.5.1.
- Do **not** put the ESP32 itself into deep sleep in v1 — losing the HA API connection on every wake would make the UX terrible. We rely on the AMOLED panel sleeping and the MCU staying in modem-sleep / light-sleep automatically courtesy of ESP-IDF.

### Battery readout (best-effort)

- AXP2101 PMIC sits on the I²C bus. No native ESPHome component, but we can read battery voltage register via a `sensor` template + I²C lambda (see guide §9).
- **Deferred to P7** — P4 exit criteria don't require a battery readout, and rendering the value needs the header strip we add in polish. Keep P4 scope to dim/blank/wake.

**Exit criteria:**
- Untouched, motionless device dims after 15s, blanks after 45s total.
- Picking up the device wakes it before you've finished lifting it.
- Tap on a blank screen wakes it (the touch IC must remain powered).
- No noticeable lag re-entering `active` (LVGL page swap < ~100 ms).

**Risks / unknowns:**
- Touch IC must stay powered during blank to register wake-tap. CST9220 polling at 50 ms continues regardless of idle state. If idle current proves too high, the fallback is "wake only on motion" — drop the touchscreen poll rate (or stop the touchscreen update from board package) on entering blank.
- ~~QMI8658 interrupt wiring~~ — resolved by polling. Waveshare board does not expose INT.
- Motion threshold 0.10 g is a guess. Will need on-device calibration. Tune via top-level YAML override of `motion_threshold_g`.

---

## Phase 5 — HA entity model (static YAML for MVP)

**Status:** ✅ done · target tag: `p5-static-entities`

**Goal:** Define how the user describes their home to the panel — **for the MVP only**. Phase 10 replaces this with HA-side dynamic config; Phase 5's job is to get something on screen fast so we can validate the UI, touch, and idle/wake stack against a real home.

Files added:
- [x] `packages/ha-entities.example.yaml` — committed template (sanitized placeholder areas/entities) so cloners see the schema.
- [x] `packages/ha-entities.yaml` — **gitignored**, user-edited per-install copy of the example. Contains real entity_ids and friendly names, so it never gets pushed. Will be replaced by dynamic discovery in Phase 10.

### Why static first, dynamic later

ESPHome's native HA API has no area-registry or entity-registry calls — those live behind HA's websocket API. Dynamic discovery is therefore a real feature, not a one-liner. Splitting into two phases lets us:

1. Ship a working panel within hours, against a real home, on real hardware.
2. Lock in UI / domain-behaviour decisions before adding the runtime-LVGL + JSON-parsing complexity that dynamic discovery requires.

Option matrix considered for static-vs-dynamic in Phase 5:

| Option | Verdict |
|---|---|
| Hard-code areas + entities in YAML | ✅ MVP. Simple, compiles fast, no runtime surprises. |
| HTTP fetch `/api/states` + filter | ❌ Areas not in state objects; would need websocket. Skip. |
| HA template sensor pushes JSON via native API attribute | ✅ Chosen for Phase 10 — see below. |
| Custom websocket external_component | ❌ Multi-weekend C++ build; not worth it. |

### Schema (static, MVP)

```yaml
# packages/ha-entities.yaml (gitignored; copy from ha-entities.example.yaml)
ha_panel:
  areas:
    - name: "Living Room"
      entities:
        - entity_id: light.couch_lamp
          friendly_name: "Couch lamp"        # optional; falls back to entity_id
        - entity_id: switch.tv_power
```

The schema is consumed by a custom `ha_panel` external_component (see
[components/ha_panel/](components/ha_panel/)). It is **not** a list of
per-entity `text_sensor` / button declarations — that would be hundreds of
ESPHome objects, none of which we'd actually render. Instead:

- `ha_panel` is a single `Component` + `api::CustomAPIDevice` that owns a flat
  `std::vector<Entity>` and a `std::vector<Area>` (areas hold indices into the
  entity vector).
- At codegen, Python `to_code` iterates `areas:` and emits one
  `add_area(name)` per area + one `add_entity(entity_id, friendly_name)` per
  entity. No per-entity ESPHome objects are created.
- At runtime, `setup()` calls `subscribe_homeassistant_state(&HAPanel::on_state_, entity_id)`
  once per entity. The 2-arg callback form passes `entity_id` back so a single
  dispatcher updates the model.
- `tap(area_idx, entity_idx)` parses the domain from `entity_id` and calls
  `call_homeassistant_service` with the right action (toggle / script.turn_on /
  automation.trigger). Read-only domains (sensor, binary_sensor, climate,
  media_player, …) log and return false. Domain → action map mirrors P6 table.
- Requires `homeassistant_states: true` and `homeassistant_services: true` on
  the `api:` block. Both flags added in `packages/base.yaml`.

**File layout:** `packages/ha-entities.example.yaml` (tracked, sanitized) +
`packages/ha-entities.yaml` (gitignored, real). User copies example → real.

**Exit criteria:** User can add a new entity by appending three lines to `ha-entities.yaml`, recompile, see `[ha_panel] <entity_id> = <state>  (first state)` in the firmware log within ~1 s of HA connect, and see `[ha_panel] tap <entity_id> → homeassistant.toggle` when `tap()` is called.

---

## Phase 6 — LVGL UI: area carousel + entity scroller

**Status:** ✅ done · target tag: `p6-ui`

**Goal:** The actual feature — horizontal swipe between areas, vertical scroll for entities within an area, tap to toggle.

Files added:
- [x] `packages/lvgl-ui.yaml` — board-agnostic LVGL config (minimal: just `lvgl:` block + empty `main_page`; widget tree built at runtime by `ha_panel`).

### UI structure

- LVGL `tileview` widget with one column per area, scroll direction horizontal.
- Each area tile is a vertical `list` (or `tileview` nested vertically — TBD) of entity rows.
- Each entity row = icon + name + state badge. Tap → fires the toggle service (if supported) or does nothing visible (if read-only).

### Entity-type → behaviour table

| HA domain | First-pass behaviour |
|---|---|
| `light` | Tap = `homeassistant.toggle`. State badge shows on/off. |
| `switch` | Tap = `homeassistant.toggle`. |
| `fan` | Tap = `homeassistant.toggle`. |
| `input_boolean` | Tap = `homeassistant.toggle`. |
| `automation` | Tap = `automation.trigger`. |
| `script` | Tap = `script.turn_on`. |
| `cover` | Tap = `cover.toggle`. |
| `sensor`, `binary_sensor` | Read-only; show current value. |
| `climate`, `media_player`, anything else | Read-only state; tap = no-op (v1). |

The domain is parsed from the entity_id prefix at config-time using YAML
substitutions / Jinja, not at runtime.

### Gestures

- Horizontal swipe = `tileview` built-in.
- Vertical scroll = LVGL `list` built-in.
- Tap detection vs. swipe: `lv_btn`'s `on_short_click` fires only on a real
  click, not a drag, so we get tap-vs-swipe disambiguation for free.

### Styling

- Tile width = 480px, full screen per area.
- Header at top with area name; entities below.
- Large rows (~80–100px) for touch comfort (guide §6 — 60px minimum).
- Avoid drawing on the panel's rounded corners (last ~16px on each side).

**Exit criteria:** Live on the panel, swipe between 2+ areas, scroll entities, tap toggles a real HA light.

**Risks / unknowns:**
- LVGL memory budget on 8 MB PSRAM should be fine for 480×480 double-buffered, but if we hit OOM, drop to single-buffer.
- `tileview` swipe sensitivity on a small panel may need tweaking.

---

## Phase 7a — Polish round 1

**Status:** ✅ done · target tag: `p7a-polish`

**Goal:** Make it pleasant to live with.

- [x] Header clock — 12-hour HH:MM AM/PM, refreshed every 15 s from HA time source via top-level YAML `interval`.
- [x] Visual feedback on tap — explicit `LV_STATE_PRESSED` bg color (0x3A4A6A) on entity + picker rows. The built-in `lv_button` press-dim was already visible in P6; the override makes it more obvious.
- [x] Boot splash — full-screen modal showing node name + "Connecting to Home Assistant…" until `api.on_client_connected` fires. Hidden once API reaches CONNECTED. Status dot in header turns red→green on the same event.
- [x] Settings tile at the end of the area carousel — appended after the last area in the tileview, also reachable via a `⚙ Settings` row in the area picker. Contains: brightness slider (16–255, persisted via `active_brightness_g` global with `restore_value: yes`), idle timeout summary, About block (node name, ESPHome version, build time).
- [~] RTC integration (`pcf85063`) — **deferred**. HA time source already covers the use case while online; PCF85063 is for offline survival, which is not yet a stated goal. Move into a follow-up if/when the panel is expected to run untethered.
- [~] AXP2101 battery readout — **deferred**. No native ESPHome component; would need a custom `axp2101` external_component with ADC init + voltage register read. Worth its own ticket; not in P7a exit criteria.

---

## Phase 7b — Polish round 2 (header reflow + status icons + settings buttons)

**Status:** ✅ done · verified on-device 2026-05-29 · target tag: `p7b-polish`

**Goal:** Rework the header strip into a useful status bar and give the settings tile real commit semantics.

### Header layout reflow

Current (P7a):
```
[ ●(status) ────── Area Name ▼ ──────── HH:MM ]
```
Target (P7b):
```
[ HH:MM ────── Area Name ▼ ──────  📶 🔋 ● ]
```
- [x] Move the **clock** to the top-left corner (replace the status-dot position).
- [x] Move the **API-connected status indicator** to the top-right corner (replace the clock position).
- [x] Add a **battery level indicator icon** immediately to the left of the status indicator. Use the AXP2101 read for raw voltage; render as a 4-bar / 3-bar / 2-bar / 1-bar / empty glyph (or LV_SYMBOL_BATTERY_3 etc) based on bucketed voltage. **Battery is now in scope** because the icon needs a real source — the deferral from P7a is reversed for P7b.
- [x] Add a **Wi-Fi signal strength icon** immediately to the left of the battery icon. Use the ESPHome `wifi.signal_strength` (RSSI) sensor; bucket into 4 / 3 / 2 / 1 / 0 bars (LV_SYMBOL_WIFI variants, or a custom 4-bar arc). Single LVGL glyph available, so tint colour communicates the bucket instead of swapping variants.
- [x] All four header items must clear the panel's rounded corners — current empirical inset is **44 px** on each side. Keep the same.

### AXP2101 battery readout

- [x] Add `components/axp2101/` as a minimal external_component (read-only). Init ADC for VBAT channel; expose a `sensor` that publishes battery voltage in V.
- [x] No native ESPHome component exists for AXP2101 (verified again at P7b start). XPowersLib (lewisxhe) is the canonical Arduino driver — port the VBAT-read path only; skip charge-curve mapping, USB-detect, and PMU power control (P7b scope: read voltage, render icon).
- [x] Bucket thresholds (LiPo): full ≥ 4.0 V, high ≥ 3.85 V, mid ≥ 3.70 V, low ≥ 3.55 V, empty < 3.55 V. Document so we can tune.

### Settings tile commit semantics

- [x] Add **Apply** and **Cancel** buttons at the bottom of the settings tile.
- [x] Slider changes become *staged* — they don't write the active brightness global until Apply is tapped. The current behaviour (live preview on every drag) stays; Cancel reverts to the saved value.
- [x] Pad the tile content area so the buttons don't run into the bottom rounded corner — content area shrunk to 372 px, button row 60 px at y=380.
- [x] If the user navigates away (swipes off the tile, opens picker) with un-applied changes, revert silently — wired via `on_tileview_changed_` + `on_header_clicked_`. Idle-blank does not currently trigger a revert (no tile change), but the next Apply/Cancel still clears the dirty flag and any wake puts the user back on the same tile. Tracked as a follow-up if it becomes confusing.

**Exit criteria:**
- Header shows: time top-left, area + chevron centered, Wi-Fi → battery → status (left to right) top-right.
- Battery icon updates within ~30 s of unplugging USB.
- Wi-Fi icon updates when RSSI bucket changes.
- Settings: dragging the slider previews; Apply commits to `active_brightness_g`; Cancel restores; navigating away discards.

---

## Phase 7c — Entity control: explicit on/off + per-domain rendering + per-domain dispatch

**Status:** ✅ done · verified on-device 2026-05-29 · target tag: `p7c-controls`

**Goal:** Two things in one phase, both row-scoped:
1. **Dispatch:** tighten the tap action per domain — explicit on/off where state is known, real services for action domains (scene/script/automation/button) and lock.
2. **Rendering:** replace the generic right-aligned text badge with a per-domain widget that matches the domain's affordance (toggle switch for binaries, play icon for action-domains, text+colour for multi-state). Modals stay in P7d.

### Already in place (P5/P6/P7a)
- Tap on a row in `light` / `switch` / `fan` / `input_boolean` / `cover` → `homeassistant.toggle`.
- Tap on `script` → `script.turn_on`. Tap on `automation` → `automation.trigger`.
- All rows render the same way: friendly_name left, state text right (colour-tinted by [`rebuild_entity_row_text_`](components/ha_panel/ha_panel.cpp#L87-L103)).

### Dispatch — in scope for P7c

- [x] **Explicit on / off when state is known.** For binary domains (`light` / `switch` / `fan` / `input_boolean`): if `state == "on"` send `homeassistant.turn_off`; if `state == "off"` send `homeassistant.turn_on`. **Fall through to `homeassistant.toggle` for any other state** (`unavailable`, `unknown`, transient values like a light's mid-transition `transitioning`, or anything else we don't recognise). Comment the toggle fallback so the "why" is captured — it covers more than just unknown/unavailable.
- [x] **`cover`** stays on `homeassistant.toggle`. Cover state alphabet is `open` / `closed` / `opening` / `closing` — the binary on/off mapping doesn't fit, and toggle does the right thing (HA forwards to `cover.open_cover` / `cover.close_cover` based on current position). Explicit open/close lives in the P7d position slider.
- [x] **`lock`** → `lock.unlock` if `state == "locked"`, `lock.lock` if `state == "unlocked"`. Fall through to no-op log for any other state (`locking` / `unlocking` / `jammed` / `unavailable`) — better to do nothing than commit the wrong action mid-transition. Lock is security-sensitive; only fires on `LV_EVENT_CLICKED` (already drag-disambiguated by lv_button), never on long-press or scroll.
- [x] **`scene`** → `scene.turn_on` on tap. Currently falls through to read-only.
- [x] **`button`** → `button.press` on tap. Currently falls through to read-only. (User still has to list a `button.*` entity_id in `ha-entities.yaml` to surface it — codegen isn't auto-discovering.)
- [x] **State callback** keeps populating `Entity::state` for every subscribed entity — no change. Rendering layer (next section) reads the latest `state` whenever it redraws.

### Rendering — in scope for P7c

Replace the single text-badge slot on each entity row with a domain-specific widget. Pick the widget at row-build time using `Entity::domain` (already populated at codegen). One method per render class, dispatched off a small table.

| Domain(s) | Row widget | Reason |
|---|---|---|
| `light` / `switch` / `fan` / `input_boolean` | `lv_switch` indicator, non-interactive. Updated programmatically from `state`. | Stock LVGL widget, communicates on/off at a glance, matches HA's own UI convention. Non-interactive because the row itself is already the tap target — making the switch clickable too would double-fire (switch event + row event bubble) or force us to suppress one of them. Simpler: switch is read-only visual, row tap drives dispatch + state mirroring on next subscription update. |
| `scene` / `script` / `automation` / `button` | `LV_SYMBOL_PLAY` icon in the badge slot, tinted accent. | Action-only domains have no on/off state to show — the badge becomes an affordance ("this fires when tapped") instead of a status. Single glyph keeps row height unchanged. |
| `lock` | Text badge: `LV_SYMBOL_CLOSE` + "Locked" (red-tinted) / `LV_SYMBOL_OK` + "Unlocked" (amber-tinted). | Lock has >2 logical states (`locking` / `unlocking` / `jammed`) so an `lv_switch` would lie during transitions. Text + glyph stays honest and security-sensitive. Amber on unlocked (not green) because "open lock" is a warning state for most users. |
| `cover` | Text badge with chevron: `LV_SYMBOL_UP` + "Open" (green-tint) / `LV_SYMBOL_DOWN` + "Closed" (grey-tint) / italic "…" for `opening` / `closing`. | Cover has the same multi-state problem as lock. Chevron hints at the toggle direction the next tap will produce. |
| `sensor` / `binary_sensor` / `weather` / any other read-only | Text badge, same as today. Numeric values render verbatim; well-known string states (`home` / `away`, `clear-night` / `cloudy`) keep their P6 colour cue. | These don't have an action — text *is* the right widget. No regression. |
| `climate` / `media_player` / `number` / `select` | Compact text summary only. See "Row summary for P7d-bound domains" below. | These get the full control surface in P7d; row stays informative but not interactive. |

#### Row summary for P7d-bound domains (P7c renders only)

These render a richer text summary so the row is useful before P7d ships, but the tap still falls through to the existing "read-only — no action" log. P7d's long-press handler is what activates the modal; short-tap stays inert.

- **`climate`**: badge shows `"<mode> <current_temp>°"` if attributes available, else just `state`. Needs attribute readout (deferred — render bare `state` in P7c if attribute path isn't ready, document the placeholder).
- **`media_player`**: badge shows `state` (`playing` / `paused` / `idle` / `off`). Bare state is fine for P7c; media_title in the row is too long.
- **`number`**: badge shows the numeric `state` verbatim (already works via the read-only path).
- **`select`**: badge shows the currently-selected option (`state` is the selected option for `select` entities — verify on first one).

### Punted to P7d (proper detail/popup view per entity)

Anything that needs a value picker, slider, dropdown, or multi-button transport stays in P7d. This is unchanged from the prior plan; included here as a checklist of what the row widget *doesn't* do.

- **`light` brightness / colour temperature / RGB.** Row `lv_switch` handles on/off fast path. Long-press → P7d modal with brightness slider (`light.turn_on { brightness_pct }`), optional CT slider, optional HSV picker. Colour wheel still flagged as non-trivial; may slip to P7e.
- **`climate` set-point + HVAC mode.** Row shows summary text. Long-press → P7d modal with temperature spinbox/arc + mode dropdown.
- **`media_player` transport + volume.** Long-press → P7d modal with prev / play-pause / next / vol± / mute buttons + volume slider.
- **`number` set value, `select` set option, `fan` speed, `cover` position.** Long-press → P7d modal with the appropriate spinbox / roller / slider.

### Implementation notes (decisions captured for the build)

- **Row-build dispatch.** Add a `RenderClass` enum to `ha_panel.h` (`BINARY_SWITCH` / `ACTION_ICON` / `LOCK_TEXT` / `COVER_TEXT` / `READ_ONLY_TEXT` / `SUMMARY_TEXT`) + a `render_class_for_(const std::string &domain)` helper. `make_entity_row` switches on the class and creates the right child widget. Keeps the row builder linear and easy to extend in P7e.
- **Switch double-fire avoidance.** When `lv_switch` is the indicator: `lv_obj_clear_flag(sw, LV_OBJ_FLAG_CLICKABLE)` so the parent button gets the tap. Comment it — without context the cleared flag looks accidental.
- **Switch state mirroring.** `rebuild_entity_row_text_` becomes `rebuild_entity_row_(entity_idx)` and switches on `RenderClass`. For `BINARY_SWITCH`: `state == "on"` → `lv_obj_add_state(sw, LV_STATE_CHECKED)`; else clear. For `ACTION_ICON`: nothing to mirror. For text classes: update label text + colour as today.
- **Storage.** `badges_by_entity_` already stores one `lv_obj_t *` per entity; rename to `widgets_by_entity_` since it's no longer always a label. No behavioural change.
- **LVGL widget enables.** Add `LV_USE_SWITCH=1` to `boards/waveshare-2.16.yaml` `build_flags` — runtime widget construction won't auto-pull it in. Confirm switch compiles against LVGL v9.5 (`lv_switch_create`, `LV_STATE_CHECKED`).
- **Fall-through dispatch logging.** Keep the existing `"tap … (domain '%s') is read-only — no action"` log for any domain not in the dispatch table. Don't silently swallow taps — the log is how we notice missing domains.
- **`button` codegen filter.** Re-read `ha_panel/__init__.py` before P7c starts — current schema doesn't filter by domain, so `button.*` entries already work today. Verify on a real `button.*` entity_id before claiming the dispatch line ships the feature.
- **Cover dispatch is still `homeassistant.toggle`.** Comment in `tap_entity_` documenting why we don't use `cover.toggle` directly: the generic service dispatches to the right per-domain handler and keeps the dispatch table flat.

### Risks / unknowns

- **Switch widget width on a 60 px row.** `lv_switch` defaults to ~50×25 px. Should fit the existing badge slot (right-aligned, ~80 px wide). If layout fights us, shrink via `lv_obj_set_size(sw, 44, 22)`.
- **Action-icon visual weight.** Single `LV_SYMBOL_PLAY` glyph on the right may look like a static label rather than an affordance. If on-device feedback says it's not obvious, tint accent (e.g. cyan 0x44CCDD) and/or add a subtle border. Hold the call until on-device check.
- **`climate` summary needs attributes.** Attribute subscriptions are a P7d concern (HVAC modes, target temp, etc). If P7c ships before that path lands, climate rows render bare `state` and a one-line comment notes the gap.
- **State alphabet drift across HA versions.** Map of known states (`on` / `off` / `open` / `closed` / `locked` / `unlocked` / `home` / `away` / `unavailable` / `unknown`) is built into the renderer. Any new HA state value falls into the "render as-is, default colour" bucket — safe default, not a crash.

**Exit criteria:**
- Tapping an `on` light/switch/fan/input_boolean sends `turn_off`; tapping an `off` one sends `turn_on`. Unknown/transient state falls back to toggle with a log entry.
- `scene`, `script`, `automation`, `button` taps fire their respective HA services.
- `lock` taps lock/unlock based on current state; mid-transition taps log + no-op.
- Binary domains render with `lv_switch` indicator that mirrors state within ~1s of the HA round-trip.
- Action domains render with `LV_SYMBOL_PLAY` icon badge.
- `lock` / `cover` render with state text + glyph + colour cue.
- `climate` / `media_player` / `number` / `select` rows render a summary text and still log "read-only — no action" on short-tap (the modal in P7d will be the action path).
- All other domains keep today's text badge unchanged.

---

## Phase 7d — Per-entity detail / popup view

**Status:** ✅ done · verified on-device 2026-05-29 (live-attr preload parked → P10) · target tag: `p7d-detail`

**Goal:** Give domains that need more than a binary tap their own control surface. Long-press an entity row → a shared modal opens, populated by a per-domain builder. Short-tap stays as the P7c row-level action (dispatch for binaries + action domains, no-op-with-log for the P7d-bound domains); the modal is strictly opt-in via long-press.

P7c already shipped: row-level `lv_switch` for binary domains, action-icon for scene/script/automation/button, text+glyph for lock/cover, summary text for climate/media_player/number/select. P7d's job is the *modal* surface on top of that — sliders, dropdowns, transport buttons — for domains where a single tap can't communicate the user's intent.

### What P7d can lift from earlier phases

- **Apply/Cancel staging pattern** — P7b's settings tile already ships the dirty-flag + revert-on-navigate-away + commit-on-Apply recipe. Lift it for the modal.
- **Full-screen modal infrastructure** — P5/P6's area picker (`picker_` lv_obj in [ha_panel.cpp](components/ha_panel/ha_panel.cpp)) is a working show/hide/bg-click-to-dismiss overlay. `detail_modal_` is the same shape.
- **Tap visual feedback** — `LV_STATE_PRESSED` bg override already on rows from P7a; new modal buttons should match.
- **Slider widget** — `LV_USE_SLIDER` + `LV_USE_BAR` already on from P7a; no new flag needed for brightness/volume/position sliders.
- **Switch widget** — `LV_USE_SWITCH` on from P7c; the modal's per-light on/off toggle can reuse it.

### Interaction model

- Long-press threshold: ~600 ms (LVGL `LV_EVENT_LONG_PRESSED`). Avoids accidental detail-view opens during scroll.
- Detail modal covers the screen (same shape as the existing area picker overlay) — easier than fitting controls into the row.
- Close on: tap Apply, tap Cancel, tap an empty area, or swipe down. Apply commits; Cancel + dismiss revert to the entity's last known state.
- One open modal at a time. If a state update arrives for the entity while the modal is open, refresh the displayed value but don't override the in-progress edit (sticky local value until Apply / Cancel).

### Architecture

- `HAPanel` gains:
  - `lv_obj_t *detail_modal_` (built once at setup, hidden by default).
  - `size_t detail_entity_idx_` (which entity the modal is currently configured for).
  - `void open_detail_(size_t entity_idx)` — clears prior widgets, dispatches on domain.
  - One builder method per supported domain. Each populates `detail_modal_` with the right widgets + binds events.
- Long-press handler on every entity row (registered alongside the existing `LV_EVENT_CLICKED`). Reads `entity_idx` from `user_data` and calls `open_detail_`.
- Apply handler builds the `homeassistant.<service>` call with the right data map and dispatches via `call_homeassistant_service`. Cancel just hides the modal.

### Per-domain widget plan

- **`light` (full control).**
  - Brightness slider (0–100 %): `light.turn_on { brightness_pct: N }`.
  - On / off toggle button at top: `light.turn_on` / `light.turn_off`.
  - If the entity reports `supported_color_modes` includes `color_temp`: CT slider in Kelvin (3000–6500 K) → `light.turn_on { color_temp_kelvin: N }`.
  - If `rgb` is supported: leave colour-wheel as a stretch item — LVGL has no built-in colour picker, would need a custom `LV_USE_CANVAS` widget. Hold out-of-scope or punt to a dedicated colour-wheel mini-phase (P7f or parking lot) if it grows large.
  - Need to consume entity attributes (`brightness`, `color_mode`, `supported_color_modes`) — current `ha_panel` only subscribes to state, not attributes. **Add attribute subscription path** (`subscribe_homeassistant_state(cb, entity_id, "brightness")` etc) and a flat `std::map<std::string, std::string>` per entity for attribute storage. **Shared with P7e** (HA `icon` attribute subscription) — whichever phase ships first builds it; the other reuses.
- **`climate` (set-point + mode).**
  - Current temperature read-only at top.
  - Target temperature spinbox or arc widget (range derived from `min_temp` / `max_temp` attrs) → `climate.set_temperature { temperature: N }`.
  - HVAC mode dropdown (`heat` / `cool` / `auto` / `off` etc from `hvac_modes` attr) → `climate.set_hvac_mode { hvac_mode: <m> }`.
- **`media_player` (transport + volume).**
  - Six buttons: prev / play-pause / next / vol- / vol+ / mute.
  - Optional volume slider 0–100 → `media_player.volume_set { volume_level: N/100 }`.
  - Currently-playing track read-only at top (`media_title` attr if present).
- **`number` (set value).**
  - Spinbox or slider scoped to `min` / `max` / `step` attrs → `number.set_value { value: N }`.
- **`select` (set option).**
  - Roller widget populated from `options` attr → `select.select_option { option: <s> }`.
- **`fan` (speed).**
  - Speed slider 0–100 % → `fan.set_percentage { percentage: N }`.
  - Off button → `fan.turn_off`.
- **`cover` (position).**
  - Open / Stop / Close buttons → `cover.open_cover` / `cover.stop_cover` / `cover.close_cover`.
  - If `current_position` attr present: position slider 0–100 % → `cover.set_cover_position { position: N }`.

### New LVGL widget enables needed

Add to `boards/waveshare-2.16.yaml` `build_flags` for any not already on:
- `LV_USE_DROPDOWN=1` (climate HVAC mode, generic option pickers).
- `LV_USE_ROLLER=1` (select option picker).
- `LV_USE_SPINBOX=1` (number set value, climate set-point).
- `LV_USE_ARC=1` (optional alternative to slider for temperature).
- `LV_USE_CANVAS=1` (only if RGB colour wheel ends up in scope).

Already on, no flag change needed:
- `LV_USE_FLEX=1`, `LV_USE_LABEL=1`, `LV_USE_BUTTON=1`, `LV_USE_TILEVIEW=1` (P6).
- `LV_USE_BAR=1`, `LV_USE_SLIDER=1` (P7a settings tile).
- `LV_USE_SWITCH=1` (P7c binary-row indicator).

### Risks / unknowns

- **Attribute subscriptions multiply the API traffic.** A light with brightness + colour_temp + color_mode + supported_color_modes = 4 extra subscriptions per light. For ~30 lights that is ~120 extra subscriptions. Should be fine within HA native API limits but flag for memory if it grows.
- **Sticky local edit vs. live state updates.** Defining "user has started editing" is fuzzy — first slider movement is a reasonable start; close of modal is the end. Don't try to merge live updates back into the slider mid-drag.
- **Service call data types.** Native API service-call data is `map<string, string>`. Some HA services expect numeric / boolean — values are coerced from string on HA side, but verify each per-domain service signature before going live.
- **Modal vs. tile.** Detail view is a modal because tileview tiles are reserved for areas + settings. If modal swipe-down conflicts with vertical row scroll under it, switch to a slide-up sheet that covers ~80 % of the screen instead. Decide after the first widget lands.

**Exit criteria:**
- Long-press on a `light` row opens a modal with a working brightness slider that drives `light.turn_on { brightness_pct }`. Slider snapped to current value on open, sticky during edit.
- Long-press on a `climate` row opens a modal with set-point + HVAC-mode controls that fire the right services.
- Long-press on `media_player`, `number`, `select`, `fan`, `cover` opens the relevant modal and dispatches the right service.
- Tap (short-press) on entity rows still does the P7c short-tap action — no regression.

---

## Phase 7e — Per-entity icons (left of friendly name)

**Status:** ✅ done · on-device verified · target tag: `p7e-icons`

**Goal:** Show the entity's chosen icon to the left of `friendly_name` on every row. Sourced from a YAML override or a compile-time domain default. **No live HA `icon` attribute subscription in v1** — see the connect-time TX-saturation lesson below.

> **⚠️ Dependency — do NOT subscribe to the HA `icon` attribute at connect.** The 2026-05-29 P7d post-mortem (§Session notes) proved that bursting ~100+ per-entity attribute subscriptions at connect saturates the native-API TX path, drops HA's `SubscribeHomeassistantServicesRequest`, and makes **every firmware-initiated service call silently fail** (taps log but nothing happens; HA eventually disconnects). P7e's original "subscribe to `icon` on every entity" would re-trigger exactly that. v1 therefore resolves icons with **zero new subscriptions**. The live HA-`icon` source is deferred to the P10 HA-side batched template sensor (one sub, JSON payload) — same path that unparks the P7d live-attrs modal.

### Icon resolution chain (per entity, evaluated each render) — v1

1. **YAML override** in `ha-entities.yaml` — `icon: mdi:foo` on the entity entry. Highest priority. For entities where you want a panel-specific glyph.
2. **Domain default** — compile-time map (`light` → `mdi:lightbulb`, `switch` → `mdi:toggle-switch`, `cover` → `mdi:window-shutter`, `lock` → `mdi:lock`, `fan` → `mdi:fan`, `climate` → `mdi:thermostat`, `media_player` → `mdi:speaker`, `scene` → `mdi:palette`, `script` → `mdi:script-text-play`, `automation` → `mdi:robot`, `button` → `mdi:gesture-tap-button`, `sensor` → `mdi:gauge`, `binary_sensor` → `mdi:checkbox-marked-circle-outline`).
3. **Generic fallback** — `LV_SYMBOL_REFRESH` (already in default LVGL font) when the resolved MDI name isn't in the baked subset. Logs once per missing icon name so we know what to bake next.

**Deferred to P10 (HA `icon` attribute, batched):** slots between YAML override and domain default once the P10 template sensor lands. Single startup subscription to one HA entity whose attribute payload carries every entity's icon as JSON — no per-entity sub multiplier. Until then, domain default covers the common case and YAML override handles the rest.

### YAML schema change

```yaml
ha_panel:
  areas:
    - name: "Office"
      entities:
        - entity_id: light.office_color_lamp
          friendly_name: "Office lamp"
        - entity_id: light.desk_lamp
          friendly_name: "Desk lamp"
          icon: mdi:desk-lamp      # optional override
```

`components/ha_panel/__init__.py` adds `cv.Optional("icon", default="")`. Codegen passes it into `add_entity(entity_id, friendly_name, icon_override)`. Stored as `Entity::icon_override`. Empty string = "fall through to domain default → fallback" (HA-attr tier inserted at P10).

### Attribute subscription path — NOT used in P7e v1

P7e v1 adds **no** attribute subscriptions. Icons resolve from YAML override + compile-time domain map only, so there is no connect-time sub burst. This is the deliberate fix for the 2026-05-29 TX-saturation failure (see warning above and §Session notes).

The live HA-`icon` source folds into P10's batched template-sensor work: one startup subscription to a single HA entity whose JSON attribute payload carries all icons. At that point `on_attr_` parses the payload and re-resolves affected rows — re-using the `ensure_attrs_subscribed_` / `on_attr_` scaffolding left in source. No per-entity icon subscription, ever.

### Baked MDI font subset

LVGL needs a bitmap font compiled in. ESPHome's `font:` block accepts a TTF file + glyph list + size:

```yaml
# packages/lvgl-ui.yaml (or board package — TBD which is cleaner)
font:
  - file:
      type: web
      url: https://github.com/Templarian/MaterialDesign-Webfont/raw/master/fonts/materialdesignicons-webfont.ttf
      refresh: never
    id: mdi_24
    size: 24
    glyphs:
      - "\U000F0335"   # mdi-lightbulb
      - "\U000F1255"   # mdi-lightbulb-multiple
      - "\U000F0425"   # mdi-toggle-switch
      # … initial subset (see below)
```

Initial baked subset (~60–80 icons, ~50–100 KB flash). Maintained as a top-of-file comment in the board package so adding new ones is a one-file edit:

- **Lights**: `lightbulb`, `lightbulb-multiple`, `lightbulb-on`, `lamp`, `desk-lamp`, `ceiling-light`, `wall-sconce`, `floor-lamp`, `string-lights`, `led-strip-variant`.
- **Switches / power**: `toggle-switch`, `toggle-switch-off`, `power`, `power-plug`, `power-socket`, `power-socket-us`.
- **Fans**: `fan`, `fan-off`, `ceiling-fan`, `ceiling-fan-light`.
- **Locks**: `lock`, `lock-open`, `lock-alert`.
- **Covers**: `window-shutter`, `window-shutter-open`, `blinds`, `blinds-open`, `curtains`, `garage`, `garage-open`, `gate`, `gate-open`.
- **Climate**: `thermostat`, `air-conditioner`, `radiator`, `fireplace`, `heat-pump`, `weather-sunny`, `weather-cloudy`, `weather-rainy`.
- **Media**: `television`, `television-classic`, `speaker`, `speaker-multiple`, `music`, `volume-high`, `volume-off`.
- **Sensors**: `thermometer`, `water-percent`, `gauge`, `motion-sensor`, `door`, `door-open`, `window-closed`, `window-open`, `leak`, `smoke-detector`, `battery`.
- **Buttons / scenes / scripts**: `gesture-tap-button`, `play`, `palette`, `script-text-play`, `robot`, `cog`.
- **Generic**: `home`, `alert`, `alert-circle`, `refresh`, `checkbox-marked-circle-outline`.

Codepoints come from the MDI webfont CSS — script the list, don't type by hand. A `tools/build-mdi-glyphs.py` helper that reads a YAML list of `mdi-*` names + outputs the `\U…` codepoints for the `glyphs:` block is the cleanest maintenance path. Same script can be reused when adding new icons.

### Row layout reflow

Current row: `[friendly_name (LEFT_MID +12) ─── widget (RIGHT_MID -12)]`, name width 280 px.

P7e row:
```
[ icon (LEFT_MID +12, 28×28) │ friendly_name (LEFT_MID +48) ─── widget (RIGHT_MID) ]
```

- Icon: `lv_label` with the resolved codepoint, font `mdi_24`. Position `LV_ALIGN_LEFT_MID, +12, 0`. 24 px glyph + ~4 px breathing room.
- Friendly name shifts right by ~36 px (`LV_ALIGN_LEFT_MID, +48, 0`). Width drops from 280 → ~240 to keep ellipsis behaviour.
- Icon tint: white (0xFFFFFF) default; could later tint by state (lit lights green, off lights grey) — flag as nice-to-have, not required for P7e exit.

### Architecture additions

- `Entity` struct gains `std::string icon_override;`. (`icon_attr` deferred — populated by P10 batched payload, not a per-entity sub.)
- `HAPanel::resolve_icon_(const Entity &e) -> const char *` returns the codepoint string for the row. Implements the v1 chain (override → domain default → fallback). Caches the resolution (`mutable std::string icon_resolved_` per Entity) so we're not re-walking the chain every redraw. P10 inserts the HA-`icon` tier into this same function.
- `icons_by_entity_` parallel vector (matches `widgets_by_entity_`) for the icon label widgets so the rebuild path can update them in place (needed when P10 pushes live icon updates).

### Risks / unknowns

- **MDI font flash cost.** 60 glyphs × ~1 KB each = ~60 KB. Acceptable on 16 MB flash. If subset grows past ~200 icons, revisit.
- **Codepoint maintenance.** Hand-typing `\U000F0335` is error-prone. Build the helper script before the first icon lands.
- **Missing-icon log spam.** If a user has an unusual `mdi:foo-bar` set in HA, we'll log it once and fall back to the domain default. Throttle to once-per-name to avoid log floods.
- **~~Attribute subscription overhead.~~ RESOLVED by dropping HA-`icon` subs from v1.** Original plan (~100 icon subs on top of ~100 state subs) would have re-triggered the 2026-05-29 TX-saturation failure that silently kills service calls. v1 adds zero subs; live HA icons deferred to P10 batched sensor. See warning at top of phase.
- **Icon != state widget.** Don't try to encode state in the icon (e.g. swap `lightbulb` → `lightbulb-off` on state change). State is the *right-side* widget's job. Keep the icon static per entity to avoid jumpy visual.

**Exit criteria:**
- Every entity row shows an icon to the left of the friendly_name.
- Icon resolves from YAML override if set, else from HA `icon` attribute if set, else from the domain default, else from the generic fallback.
- Changing an entity's icon in HA's entity editor reflects on the panel within one state callback (no reflash).
- Adding a new icon to the baked subset = one line in the font glyph list + one line in the domain-default map (if a new default), recompile.
- Missing-icon name (resolved MDI name not in the baked subset) logs once and renders the generic fallback — no crash, no repeated noise.

---

## Phase 7f — Per-entity tap-confirmation guard

**Status:** ✅ verified on-device 2026-05-29 · target tag: `p7f-confirm`

**Goal:** Stop me from accidentally opening the garage door (or unlocking the front door, or running a "panic" script) by brushing the screen. A per-entity opt-in flag in `ha-entities.yaml` turns the short-tap into "open a confirm sheet" instead of "fire the action immediately". The actual action only fires after an explicit second tap on a labelled button inside the sheet.

### YAML schema change

Add an optional `confirm: true` flag to the entity entry:

```yaml
ha_panel:
  areas:
    - name: "Garage"
      entities:
        - entity_id: cover.ratgdov25_dc1381_door
          friendly_name: "Garage door"
          confirm: true              # short-tap → confirm sheet, not toggle
        - entity_id: light.ratgdov25_dc1381_light
          friendly_name: "Garage light"
          # no confirm: short-tap toggles immediately as today
```

`components/ha_panel/__init__.py` adds `cv.Optional("confirm", default=False)`. Codegen passes through to `add_entity(entity_id, friendly_name, icon_override, confirm)` — same call shape P7e will extend with `icon_override`, so the two phases stack cleanly.

### Behaviour

| Entity domain | `confirm: false` (default) | `confirm: true` |
|---|---|---|
| `light` / `switch` / `fan` / `input_boolean` | Short-tap fires turn_on/turn_off (P7c). Long-press opens the detail modal (P7d, where applicable). | Short-tap opens the detail modal (same modal P7d builds, with the on/off switch + brightness etc. pre-loaded). Apply commits, Cancel does nothing. Long-press also opens the detail modal — same target either way. |
| `scene` / `script` / `automation` / `button` | Short-tap fires the action service (P7c). No detail modal. | Short-tap opens a new "confirm action" sheet: title = friendly name, big centered button labelled with the action verb ("Run script", "Trigger automation", "Press button", "Activate scene"), Cancel button. Apply fires the service; Cancel closes. |
| `lock` | Short-tap toggles lock state for known `locked`/`unlocked` (P7c). | Short-tap opens a confirm sheet with two large buttons ("Lock"/"Unlock") based on current state, plus Cancel. Same security justification as P7c's no-op on transient states — extra friction before committing. |
| `cover` | Short-tap fires `homeassistant.toggle` (P7c). Long-press opens position modal (P7d). | Short-tap opens a confirm sheet: title + "Open" / "Stop" / "Close" buttons (mirrors the P7d cover modal's transport row but **without** the persistent slider — keeps the sheet visually minimal and the action commit explicit). |
| `climate` / `media_player` / `number` / `select` | Short-tap is read-only / summary (P7c); long-press opens detail modal (P7d). | Short-tap opens the same P7d detail modal directly. Functionally similar to long-press; the difference is "tap also works" so the user doesn't need to know the long-press gesture for a confirm-flagged entity. |
| `sensor` / `binary_sensor` / `weather` / other read-only | Short-tap logs no-op (P7c). | `confirm: true` is meaningless on read-only domains — log a config-validation warning at codegen and ignore the flag at runtime. |

### Architecture

- **`Entity::confirm` bool field**, populated at codegen-time alongside `friendly_name` and (P7e's) `icon_override`.
- **Short-tap dispatch (`on_entity_row_clicked_` → `tap_entity_`) gains a pre-flight check.** If `confirm == true` AND the domain has a meaningful confirm path (everything except read-only), route the tap to a new `open_confirm_or_detail_(entity_idx)` that picks between:
  - The existing P7d detail modal for domains that have one (`light`, `climate`, `media_player`, `number`, `select`, `fan`, `cover`).
  - A new **action confirm sheet** for action-only / lock-only domains (`scene`, `script`, `automation`, `button`, `lock`).
- **Action confirm sheet** is a third overlay alongside `picker_` and `detail_modal_`. Built once at setup, hidden by default, repopulated on each open:
  - Title bar: friendly name.
  - Centered body: 1-2 large buttons (200×70 px, 12 px radius) coloured by criticality:
    - "Run script" / "Trigger automation" / "Press button" / "Activate scene" — accent blue (`0x44CCDD` background).
    - "Lock" — green (`0x2A553A`).
    - "Unlock" — amber (`0x553A2A`).
    - Cover "Open" — green; "Close" — amber; "Stop" — neutral grey.
  - Cancel button at the bottom (same shape as P7d/settings Cancel).
- **Long-press behaviour unchanged.** Long-press still opens the detail modal for domains that have one; confirm-flagged entities have a redundant long-press path but no regression.
- **Confirm flag is per-entity, not per-area or global.** No mass opt-in or opt-out — every entity decides individually in YAML. Aligns with the static-config model in P5.

### Picker / overlay z-order

LVGL z-order today is:
1. Tileview (entity rows).
2. Header.
3. Picker modal (when open) — `move_foreground` on open.
4. Detail modal (when open) — `move_foreground` on open.
5. Splash (top until API connects).

Action confirm sheet slots between 3 and 4 — same `move_foreground` pattern. Only one of {picker, detail, confirm} is ever open at a time; bg-tap on any of them dismisses.

### Edge cases

- **Detail modal already open + user taps another row.** Detail modal eats touches outside its content area (Cancel on bg-tap), so the underlying row won't fire. No interaction needed.
- **`confirm: true` on a read-only entity.** Codegen logs `WARN: confirm: true ignored for read-only domain '<d>' (<entity_id>)` and leaves the flag at its default false. Avoids silent surprise.
- **Confirm-flagged entity goes `unavailable`.** Sheet still opens; Apply button is disabled (grey + non-clickable) and a small text under the title reads "Currently unavailable". Cancel stays active so the user can dismiss.
- **Long-press on a confirm-flagged action-only entity** (e.g. `script.panic` with `confirm: true`). No detail modal exists for the domain, so long-press should match short-tap and open the confirm sheet — same target either way. Trivially supported by registering both LV_EVENT_SHORT_CLICKED and LV_EVENT_LONG_PRESSED to the same dispatcher for confirm-flagged rows.

### Implementation notes (decisions captured for the build)

- **Single dispatcher method.** `open_confirm_or_detail_(entity_idx)` parses domain and either calls existing `open_detail_(idx)` (for domains with a detail modal — pre-load handled by P7d's path, which today means "use defaults" per the parked attr-fetch TODO) OR a new `open_confirm_action_(idx)` for action-only / lock-only domains.
- **Cover special case.** Cover has both a detail modal (P7d position slider) AND wants a "confirm before tap" sheet that the user expects to be the *fast* path for the garage door (no slider, just Open / Stop / Close). Decide at runtime: if `confirm: true`, short-tap opens the confirm sheet (no slider), long-press opens the full position modal. Two different entries to the cover surface, each appropriate to the gesture.
- **No "are you sure?" dialog vs. modal proper.** Keep it as a real modal sheet (same look as detail / picker), not a small dialog box. The screen is 480 px and the user is at arm's length — a quick large-button "Open garage door" / "Cancel" sheet is faster to read than a centered dialog with smaller text.
- **No tap-timeout / "press and hold to confirm" gesture.** Considered but rejected — adds friction and is harder to discover. The two-step tap (open sheet → tap "Run") is the minimum that prevents pocket-tap mistakes without making the panel feel slow.
- **Schema migration.** Existing `ha-entities.yaml` entries without `confirm:` keep working unchanged (default false). Roll-out for sensitive entities is a one-line YAML edit per entity.

### Risks / unknowns

- **User muscle memory.** If the same entity is sometimes tappable (light) and sometimes not (lock with confirm), the user might expect a confirm sheet on every tap. Live with it; the YAML edit is opt-in and the user controls the trade-off.
- **Confirm sheet vs. P7d detail modal for `media_player`.** Adding `confirm: true` to a media player with a long-form modal could feel weird (opens a big modal to play a track). But the same modal already opens via long-press, so functionally no surprise. Document in the table above as "uses the detail modal as the confirm path".
- **Action confirm sheet width clashing with rounded corners.** Same 44 px corner inset rule as P7a header — primary buttons must stay within `[44, 436]` px horizontally to avoid corner clipping. Apply this in the builder.

**Exit criteria:**
- `confirm: true` on a `cover` entity → short-tap on the row opens a confirm sheet with Open / Stop / Close / Cancel buttons. Tap on Open / Stop / Close fires the corresponding `cover.*` service.
- `confirm: true` on a `script` / `automation` / `button` / `scene` entity → short-tap opens a confirm sheet with a single labelled action button + Cancel. Action button fires; Cancel closes.
- `confirm: true` on a `lock` entity → short-tap opens a confirm sheet with Lock / Unlock buttons (the one matching the *opposite* of current state is the action), plus Cancel.
- `confirm: true` on `light` / `switch` / `fan` / `input_boolean` / `climate` / `media_player` / `number` / `select` → short-tap opens the existing P7d detail modal. Apply commits the action; Cancel closes.
- `confirm: true` on a read-only domain (`sensor`, `binary_sensor`, …) → codegen warns and the runtime treats the entity as if `confirm` weren't set.
- Entities without `confirm:` behave exactly as today (short-tap fires per P7c).

---

## Phase 8 — Power management (sleep / wake for nightstand use)

**Status:** ✅ done · verified on-device 2026-05-30 · target tag: `p8-power`

**What landed (2026-05-29):**
- `power_mgr.h` — light-sleep IDF wrapper (`ha_power::enter_light_sleep`), touch
  GPIO11 low-level wake. Included via `esphome: includes:`.
- `packages/idle.yaml` — `sleep_enabled_g` / `power_saver_mode_g` (persisted) +
  `sleep_armed` / `idle_api_connected` globals; `sleep_timeout_s` (60 s) +
  `sleep_arm_uptime_s` (30 s) substitutions; `enter_sleep` script (deep →
  `deep_sleep.enter`, light → `wifi.disable` + delay + light-sleep + `wifi.enable`
  + resume); interval tick gains the `blank → sleep` transition gated on
  enabled + armed.
- `boards/waveshare-2.16.yaml` — `deep_sleep:` (`deep_sleep_ctl`) with ext0
  wake on GPIO11 (`inverted: true`), the safe single-pin fallback.
- `packages/base.yaml` — API callbacks set `idle_api_connected`.
- `ha-amoled-panel.yaml` — `includes: [power_mgr.h]`; on_boot wires
  `set_sleep_committer` (writes globals) + `set_sleep_settings` (seeds controls).
- `components/ha_panel` — settings tile "Power saving" section: master
  `lv_switch` (default ON) + Light/Deep `lv_dropdown` (greyed when toggle off),
  staged with the same dirty-flag + Apply/Cancel + revert-on-navigate-away
  recipe as brightness.

**Verified on-device 2026-05-30:** sleep current in the hundreds-of-µA band;
touch GPIO11 fires the wake edge in polling mode; `wifi.disable`/`enable` cleanly
drops + re-raises the link around light sleep; touch IC alive on first wake
without a power-cycle; deep-sleep ext0 wake + OTA-window survival.

**Deferred (not blocking P8):** GPIO18 button + ext1 any-low and RTC-alarm wake
(touch-wake-only for v1); cold-boot splash suppression + AXP rail gating
(optimizations).

**Goal:** Survive an overnight (and ideally multi-day) idle on the LiPo without a
charger. Use case is **nightstand**: set it down at night, leave it,
pick it up / tap it in the morning and have it usable within a couple of
seconds. The existing P4 idle machine (active → dim → blank) only turns the
*panel* off — the ESP32 + Wi-Fi stay fully awake keeping the HA API link, which
is tens of mA and drains a small cell in a night. P8 adds a real low-power state
below `blank` and the wake plumbing to leave it.

> **Reverses a parking-lot decision.** The original plan put low-power sleep out
> of scope because losing the HA API link on every wake makes an *interactive*
> panel feel terrible. That logic still holds for a hand-held remote you poke
> constantly. It does **not** hold for a nightstand device that sits untouched
> for hours — there, a 2–4 s reconnect on the few times a day you pick it up is a
> fine trade for not killing the battery overnight. P8 is the nightstand answer;
> the setting (below) lets the hand-held use case keep the old always-connected
> behaviour.
>
> **Chosen approach: light sleep with Wi-Fi dropped** (default), deep sleep as an
> option. Both turn the radio fully off while idle — which is what actually saves
> the battery on this `power_save_mode: NONE` mesh — but light sleep retains RAM
> so the screen resumes instantly where you left it, while deep sleep cold-boots
> for a bit more battery. Full reasoning in the section below.

### Hardware wake-source reality on this board (verified 2026-05-29)

Researched against `waveshareteam/ESP32-S3-Touch-AMOLED-2.16` `pin_config.h`,
the official schematic, and the board GPIO table. ESP32-S3 RTC-capable GPIOs are
**0–21**, so any INT/button on those can wake deep sleep via `ext0`/`ext1`.

| Wake source | Routing | Deep-sleep wake? | Light-sleep / awake wake? |
|---|---|---|---|
| **Touch (CST9220 INT)** | **GPIO11**, RTC-capable, active-low | ✅ `ext0`/`ext1` on GPIO11 — *if touch rail kept powered* | ✅ (already polled today) |
| **Custom button** | **GPIO18**, RTC-capable, active-low (pull-up) | ✅ **best button** — `ext0` low on GPIO18, hold internal pull-up | ✅ |
| **BOOT button** | **GPIO0**, RTC-capable | ⚠️ works but GPIO0 is a strapping/download pin — compromised for runtime wake | ✅ |
| **RTC alarm (PCF85063 INT)** | **GPIO13** (`RTC_INT`), RTC-capable; RTC domain kept alive by AXP (VBAT2 = `CHG_RTC`) | ✅ scheduled alarm via `ext0`/`ext1` on GPIO13 | ✅ |
| **PWR button** | **AXP2101 PWRON** → PMU `PWROK` → ESP `CHIP_PU`. **IRQ pin unrouted** (pin 38 → net `AXP_IRQ`, 10 K pull-up to VCC-RTC, terminates there — not on any GPIO) | ⚠️ **cold boot only** (long-press = PMU off → next press powers rails + releases `CHIP_PU` = fresh boot, *not* `esp_deep_sleep` resume) | ⚠️ short-press only sets an AXP IRQ flag; readable solely by I²C poll (GPIO14/15), never as a wake edge |
| **Motion (QMI8658 INT)** | **NOT routed to any GPIO** | ❌ **impossible** — no pin to wake on | ✅ only via software accel polling (CPU must run) |

**Load-bearing constraint #1 — motion can't wake deep sleep.** The IMU interrupt
line is not broken out (confirmed at P4 — Waveshare's own example polls). So
**motion cannot wake the device from deep sleep on this hardware, full stop.**
Motion-wake (the P4 pickup-to-wake behaviour) only survives in light sleep /
awake, where the CPU still ticks to poll the accel.

**Load-bearing constraint #2 — the PWR button can't wake deep sleep either**
(researched 2026-05-29 against the schematic + GPIO table). The AXP2101 IRQ pin
(chip pin 38) goes to net `AXP_IRQ`, gets a 10 K pull-up to the always-on
VCC-RTC rail, and **terminates there — it is wired to no ESP32 GPIO** (there is
no AXP column in the board GPIO table). The AXP is reachable only over the shared
I²C bus (GPIO14/15), useless during deep sleep because the CPU is off. So the
`AXP_IRQ → RTC GPIO → ext0/ext1` path does **not physically exist** here. The PWR
button works the laptop way: PWRON drives the PMU, PMU `PWROK` gates the ESP
`CHIP_PU`. Long-press = PMU power-off (rails drop); a subsequent press powers
rails back and releases `CHIP_PU` = **cold boot**, not a deep-sleep resume. A
short press during deep sleep just sets an AXP flag the dead CPU never reads.

**What this leaves for deep-sleep wake:** tap the screen (**GPIO11**, touch rail
must stay powered), the **GPIO18 user button** (best — clean RTC GPIO, active-low
with internal pull-up: `esp_sleep_enable_ext0_wakeup(GPIO_NUM_18, 0)`), the BOOT
button (**GPIO0**, works but it's a strapping pin so not ideal), or a **scheduled
PCF85063 RTC alarm** (**GPIO13**, RTC domain kept alive by the AXP — genuinely
usable for timed wake-ups even though *button-via-PMU* wake is not). These two
"not routed" constraints are the main UX input to the default choice below.

### Why "light sleep with Wi-Fi OFF" is the chosen state (the reasoning that drives P8)

**Earlier draft was wrong about light sleep.** It argued light sleep saved
little because the P1 `power_save_mode: NONE` on the Google Wifi mesh disables
modem-sleep (DTIM beacon skipping), so an *associated* radio stays near
full-power during light sleep. That is true — **but only matters if we keep
Wi-Fi associated while sleeping.** We don't need to.

**Key decision: drop Wi-Fi entirely before sleeping.** A nightstand panel that's
been idle for a minute has no reason to hold the HA link — nothing is looking at
it. So on sleep-enter we `esp_wifi_stop()` (radio fully off, not modem-sleep) and
on wake we bring Wi-Fi + the HA API back up. With the radio *off*, the
`power_save_mode: NONE` problem **completely disappears** — there's no associated
radio to keep hot. Light-sleep current collapses to the **core RAM-retention
band (~240–800 µA)** plus board peripherals, instead of the tens-of-mA an
associated radio would cost.

**Why light sleep over deep sleep, given both now drop Wi-Fi:** because both pay
the *same* ~2–4 s HA-reconnect on wake (radio was off either way), the HA-link
delay is a wash. What light sleep keeps that deep sleep throws away is **RAM** —
so the UI resumes **exactly** where it was (open modal/sheet/sub-tile intact, no
splash, no LVGL rebuild, instant visual wake). Deep sleep cold-boots back to the
home view. Light sleep costs ~10–30× more sleep current (hundreds of µA vs tens),
which on a 100 mAh cell is roughly **~a week vs several weeks** — and a week of
nightstand idle between charges is plenty.

**Therefore P8 targets light sleep with Wi-Fi off as the default power-saver
state.** Deep sleep stays available for anyone who wants maximum multi-week
battery and tolerates a cold-boot wake.

> **Motion-wake still gone in light sleep too.** Don't expect pickup-wake back.
> The IMU INT isn't routed (constraint #1), so even light sleep can only wake on
> a GPIO edge (touch GPIO11 / button GPIO18) or a timer — waking on motion would
> require periodically waking the CPU to poll the accel, which burns the very
> current we're sleeping to save. Light sleep buys UI-state retention, **not**
> motion-wake on this board.

### Power-state tiers (extends P4, doesn't replace it)

```
[active] --dim_timeout--> [dim] --blank_timeout--> [blank] --sleep_timeout--> [sleep]
   ▲ panel 80%             panel ~12%               panel off                 chosen mode
   │ MCU+Wi-Fi awake       MCU+Wi-Fi awake          MCU+Wi-Fi awake           │ Wi-Fi OFF
   └──────────────────── touch / button / motion wake (P4) ──────────────────┘
                                                                              │
                          [sleep] wake: touch (GPIO11) / button (GPIO18) /    │
                          RTC alarm (GPIO13). NO motion, NO PWR button. ──────┘
                          On wake: restore UI, then reconnect Wi-Fi + HA.
```

- `active` / `dim` / `blank` are unchanged from P4. In all three the MCU + Wi-Fi
  stay awake and the HA link is live.
- New `sleep` tier entered after **`sleep_timeout` = 1 min** of no input
  (measured from last input; the device has already passed through `dim`/`blank`
  by then). This is aggressive on purpose — the panel sleeps fast to save battery,
  and because the default is *light* sleep the cost of over-eager sleeping is
  low (wake is an instant RAM resume, only the background HA reconnect lags). If
  Deep sleep is selected, 1 min is more noticeable (cold boot on every re-poke);
  a user who picks Deep may want to raise it. **Wi-Fi is dropped on entry to
  every sleep state** (see reasoning above).
- What `sleep` does depends on the **Power saver** setting:
  - **Off** — never enter `sleep`; behave exactly as today (P4). Wi-Fi stays up,
    HA link never drops. For the hand-held / always-connected use case, or when
    on a charger.
  - **Light sleep (Wi-Fi off)** — *the default.* `esp_wifi_stop()` then
    `esp_light_sleep_start()`. **Keeps RAM**, so the UI resumes exactly where it
    was (no splash, no rebuild); wake is an instant visual resume followed by a
    background Wi-Fi + HA reconnect (~2–4 s). Sleep current ~hundreds of µA. No
    motion-wake (see note above).
  - **Deep sleep** — ESP32 deep sleep (~tens of µA, biggest battery win). RAM
    lost → cold boot: splash + LVGL rebuild + Wi-Fi reassociate + HA reconnect on
    wake. Wakes on touch GPIO11 / button GPIO18 / RTC alarm GPIO13 via
    `ext0`/`ext1`. No motion-wake, no PWR-button wake.

### Default choice (mine, per the brief)

**Default = Light sleep (Wi-Fi off), `sleep_timeout: 1 min`.** Reasoning for
"what most people in *this* nightstand scenario want":
1. The stated goal is "don't drain the battery overnight." With Wi-Fi dropped,
   light sleep is ~hundreds of µA → roughly a **week** on a 100 mAh cell, which
   comfortably clears any overnight (and multi-night) idle. The original
   objection (`power_save_mode: NONE` keeping the radio hot) only applied to an
   *associated* radio; turning Wi-Fi off removes it entirely.
2. Light sleep keeps RAM, so picking the panel up after it slept resumes the
   **exact** screen — open modal, current sub-tile, no splash flash, no rebuild.
   That "it's just instantly back" feel is the nightstand win deep sleep can't
   give without extra cold-boot-suppression work.
3. Both light and deep drop Wi-Fi, so both pay the same ~2–4 s HA reconnect on
   wake — light sleep is strictly the nicer UX for an equal link delay, at the
   cost of ~10–30× more sleep current (still a week+, so fine).
4. The 1-minute window sleeps quickly after you set the panel down. With light
   sleep that's the point — fast battery savings with a near-free instant resume;
   a brief glance-and-poke that ends just re-wakes from RAM. (For Deep sleep the
   short window trades battery for more frequent cold-boots — raise it if that
   mode is chosen.)

**Deep sleep** is the pick for someone who wants **multi-week** battery and
accepts a cold-boot wake (splash + home view). **Off** is for hand-held /
always-connected use or on a charger.

### Settings UI (extends the P7a/P7b settings tile)

Sleep is **opt-out, not opt-in** — on by default, user can disable it.

- **Sleep master toggle (`lv_switch`), default ON.** Labelled e.g. "Sleep when
  idle". ON = the idle machine is allowed to enter the `sleep` tier; OFF = never
  sleep, behave exactly as P4 (Wi-Fi stays up, HA link never drops). This is the
  single control most users touch. Default **ON** per the brief.
- **Mode `lv_dropdown`, shown when the toggle is ON:** `Light sleep` (default) /
  `Deep sleep`. Light = Wi-Fi-off light sleep (instant UI resume); Deep = ESP
  deep sleep (max battery, cold-boot wake). When the master toggle is OFF the
  dropdown is greyed/disabled (its value is irrelevant).
- Reuse the existing P7b Apply/Cancel staging + dirty-flag recipe (toggle + mode
  stage together, commit on Apply, revert on navigate-away).
- Persist via two `restore_value: yes` globals, same pattern as
  `active_brightness_g`:
  - `sleep_enabled_g` (bool, **default `true`**) — the master toggle.
  - `power_saver_mode_g` (uint8, `0` = light, `1` = deep, default `0`) — the
    mode, only consulted when `sleep_enabled_g` is true.
  Read both at boot to decide whether/how the idle machine arms the `sleep`
  transition.
- Optional third control: **Sleep after** (the `sleep_timeout`) as a small set
  of presets (1 / 5 / 10 / 30 min) rather than a free slider — keeps the tile
  simple. Decide during build whether this is worth the tile space; default
  **1 min** is fine to ship without exposing it (more relevant to expose once
  Deep sleep is in play, where a longer window is friendlier). (The master toggle already covers "never
  sleep", so a `Never` preset is redundant.)
- `LV_USE_DROPDOWN` / `LV_USE_SWITCH` are already enabled (P7d / P7a), so no new
  build flag for these controls.

### Architecture / implementation notes

- **ESPHome `deep_sleep:` component** drives deep sleep. Wake sources are now
  pinned (the PWR-button fallback is dead — AXP IRQ unrouted, see constraint #2
  above): the wake set is **touch GPIO11** + **button GPIO18**, both active-low,
  optionally **+ RTC-alarm GPIO13** for timed wake. ESPHome exposes `wakeup_pin`
  (single, ext0) and `esp32_ext1_wakeup` (multiple pins + `ANY_HIGH` / `ALL_LOW`
  mode). All our sources are active-low, which wants an "any low" trigger —
  **verify ESP32-S3 supports `ANY_LOW` for ext1** (classic ESP32 only had
  `ANY_HIGH`/`ALL_LOW`; S3 added more). If S3 ext1 can't do any-low across
  multiple pins, fall back to `ext0` on touch GPIO11 as the single primary wake
  and accept GPIO18 not waking deep sleep (the user reaches for the screen
  anyway). **There is no PMIC-button safety net** — the PWR button only
  cold-boots from a full PMU power-off, not from `esp_deep_sleep`. Resolve the
  ext1 question on real silicon before committing the wake design.
- **Enter sleep from the idle machine, not at boot.** The P4 `interval: 1s` +
  millis state machine gains one more transition: when `blank` and idle past
  `sleep_timeout` **and `sleep_enabled_g` is true**, enter the configured mode —
  light-sleep entry or `deep_sleep.enter`. `notify_input` (touch / button)
  restamps. If `sleep_enabled_g` is false the machine stops at `blank` exactly
  like P4.
- **Drop Wi-Fi on sleep-enter, restore on wake (the core of this design).** This
  applies to **both** sleep modes — it's what makes light sleep cheap on the
  `power_save_mode: NONE` mesh.
  - *On sleep-enter:* `esp_wifi_stop()` (radio fully off, not modem-sleep). For
    deep sleep this is implicit in the reboot; for light sleep it's an explicit
    call before `esp_light_sleep_start()`.
  - *On wake (light sleep):* the CPU resumes mid-function after
    `esp_light_sleep_start()` returns. Re-enable the panel, **restore the UI
    instantly from retained RAM** (no rebuild), then kick Wi-Fi + the HA API
    reconnect in the background — the dashboard is visible immediately and live
    data repopulates a few seconds later as the link comes back. ESPHome's
    `wifi`/`api` components must be told to reconnect; verify whether
    `esp_wifi_stop()` + restart cooperates cleanly with ESPHome's wifi component
    or whether the cleaner lever is `wifi.disable` / `wifi.enable` actions (test
    both on hardware — ESPHome may fight a raw `esp_wifi_stop`).
  - *On wake (deep sleep):* full boot brings Wi-Fi + API up the normal way; no
    special restore code, but the UI cold-boots (see splash note below).
- **Light sleep is custom, not the ESPHome `deep_sleep:` component.** ESPHome has
  no first-class light-sleep component, so the light path is a lambda/custom
  component: gate the panel, `esp_wifi_stop()`, arm the GPIO11/GPIO18 (and
  optional RTC) wake sources via `gpio_wakeup` / `esp_sleep_enable_ext1_wakeup`,
  call `esp_light_sleep_start()`, then on return restore. Needs
  `CONFIG_PM_ENABLE` / `CONFIG_FREERTOS_USE_TICKLESS_IDLE` considered, but since
  we sleep explicitly (not automatic tickless) the manual `esp_light_sleep_start`
  path is the simpler, more predictable route. Build this as the default mode;
  the `deep_sleep:` component is the alternate mode.
- **Never sleep before the device is usable / updatable.** Call
  `deep_sleep.prevent` during boot until the HA API has connected at least once,
  and provide an escape hatch for OTA: ESPHome's `deep_sleep` already cooperates
  with OTA, but confirm the device stays awake long enough after boot to accept
  an OTA push (a `run_duration` floor or "prevent for first 30 s" guard). Without
  this, a deep-sleeping device can become very annoying to reflash.
- **AXP2101 rail gating for lower sleep current (optimization, both modes).** The
  AMOLED is dark via brightness 0, but its rail still draws. The AXP2101 can cut
  the display rail entirely on sleep-enter and restore on wake — a bigger saving
  and useful for *light* sleep too (where the RAM-retention current is otherwise
  the floor). **Touch rail must stay powered** for the GPIO11 wake-tap to fire,
  so gate display only, never touch. Follow-up optimization, not required for the
  first working version (get sleep + wake correct first, then chase µA via rail
  control).
- **Cold-boot UX on deep-sleep wake (deep mode only).** Light sleep resumes RAM
  so it doesn't apply there. For deep sleep: check `esp_sleep_get_wakeup_cause()`
  at boot — if it's an `ext0`/`ext1`/RTC wake (not a real power-on), **skip the
  splash** and jump straight to the dashboard; optionally stash the last
  tile/page index in `RTC_DATA_ATTR` before sleeping and restore it on boot
  (modals/sheets are transient — fine to drop, just land on the right tile).
  Worthwhile polish so deep-sleep wake doesn't feel like a full reboot.
- **Clock after deep-sleep wake (deep mode only).** Deep sleep loses system time;
  the HA time source re-syncs a few seconds after API reconnect, so the header
  clock is blank/stale briefly on wake. Light sleep keeps the clock running (CPU
  state retained), so this is a deep-sleep-only wart. If annoying, integrate the
  on-board **PCF85063 RTC** (deferred at P7a) to hold time across deep sleep —
  optional, and it pairs naturally with the RTC-alarm wake source.
- **Light-sleep entry/exit checklist** (the default mode): gate panel →
  `esp_wifi_stop()` → arm GPIO11/GPIO18 (+ optional GPIO13) wake →
  `esp_light_sleep_start()` → *(on wake)* re-enable panel, restore UI from RAM,
  restart Wi-Fi + HA reconnect in background. Because we sleep explicitly rather
  than via automatic tickless idle, `CONFIG_PM_ENABLE` /
  `CONFIG_FREERTOS_USE_TICKLESS_IDLE` aren't strictly required — keep the manual
  `esp_light_sleep_start` path for predictability.

### Exit criteria

- **Default (sleep ON, Light sleep):** an idle device enters Wi-Fi-off light
  sleep after `blank` + `sleep_timeout`. Wi-Fi is confirmed *off* during sleep
  (radio down, not modem-sleep). Measured current is in the hundreds-of-µA range
  (verify with a USB power meter / AXP reading).
- **Light-sleep wake:** tapping the screen (GPIO11) or pressing GPIO18 resumes
  the device; **the UI is restored instantly from RAM to the exact prior screen**
  (no splash, no rebuild), and Wi-Fi + HA reconnect in the background within a few
  seconds (live data repopulates).
- **Deep sleep (mode = Deep):** idle device enters deep sleep after `blank` +
  `sleep_timeout`; current drops to the tens-of-µA range. Tap (GPIO11) or GPIO18
  wakes it; it cold-boots (splash suppressed on wake-cause if that polish landed),
  reconnects to HA, and the dashboard is usable within a few seconds.
- **Master toggle OFF:** behaviour is identical to P4 (no regression) — device
  stops at `blank`, never sleeps, Wi-Fi stays up, HA link never drops.
- Motion (pickup) and the PWR button do **not** wake from either sleep mode —
  documented as expected, not bugs.
- Both settings (`sleep_enabled_g` default **true**, `power_saver_mode_g` default
  **light**) persist across reboot and are changeable from the settings tile with
  Apply/Cancel; the mode dropdown disables when the master toggle is off.
- A sleeping device can still be reflashed (OTA window honoured, or documented
  USB/BOOT recovery path).

### Risks / unknowns

- **ext1 any-low on ESP32-S3 for two active-low pins.** The single biggest
  unknown — if unsupported, combined touch+button wake collapses to **ext0 on
  touch GPIO11 alone** (no PMIC-button fallback exists; see constraint #2). Verify
  on hardware early.
- **GPIO13 touch-IC / RTC-INT pin sharing.** Before wiring RTC-alarm wake on
  GPIO13, confirm GPIO13 isn't multiplexed with another live function on this
  board (the GPIO table lists it as `RTC_INT`, but verify it's free at runtime).
- **ESPHome vs raw `esp_wifi_stop()` cooperation (light sleep, the default).**
  Biggest light-sleep unknown. ESPHome's `wifi` component owns the radio and may
  fight a raw `esp_wifi_stop()` / restart (auto-reconnect logic, watchdog,
  state-machine assumptions). Test whether `wifi.disable` / `wifi.enable` actions
  (or the API `reboot_timeout`/connection callbacks) are the cleaner lever before
  reaching for the IDF call directly. If ESPHome can't be made to cleanly drop
  and re-raise the link in-process, the light-sleep design may need to lean on
  ESPHome primitives rather than manual IDF Wi-Fi control.
- **Reconnect time on the Google Wifi mesh (both modes).** P1 needed static IP +
  `fast_connect` just to associate; reassociation after Wi-Fi-off (light) or a
  cold boot (deep) may be slower than the "few seconds" target. Measure; keep the
  Wi-Fi creds/BSSID pinned for faster reassoc. Light sleep masks this better
  because the UI is already up while the link returns.
- **OTA reachability while sleeping.** A sleeping device (Wi-Fi off, either mode)
  is unreachable for OTA until it wakes. Keep an awake/connected window after boot
  before the first sleep (`prevent`/`run_duration` floor or "no sleep for first
  30 s"), and document that pushing OTA means waking the device first (tap it,
  then push within the active window).
- **Touch-IC state after deep-sleep wake.** P3 documented the CST9220 needing a
  power-cycle after a *flash* to respond. A deep-sleep wake is a softer reset than
  a flash, but verify the touch IC comes back live on first wake without a manual
  power-cycle — if not, that breaks tap-to-wake and forces button-only wake.
- **AXP2101 + ESP32 deep sleep interaction.** Confirm the PMIC doesn't itself cut
  the ESP32 rail or reset state in a way that interferes with `ext1` wake. Read
  the XPowersLib sleep path before gating any rails.

---

## Phase 9 — Multi-board support

**Moved to its own plan:** [plan-multi-board-support.md](plan-multi-board-support.md).
Status, goal, tasks, exit criteria, and risks now live there. This MVP plan
targets the Waveshare ESP32-S3-Touch-AMOLED-2.16 only.

---

## Phase 10 — Dynamic area + entity discovery (replaces static YAML)

**Moved to its own plan:** [plan-dynamic-discovery.md](plan-dynamic-discovery.md).
Mechanism (HA template sensor → JSON attribute → runtime LVGL build), hard
parts, migration, exit criteria, and risks now live there. The MVP path stays
the static, gitignored `packages/ha-entities.yaml` from Phase 5.

---

## Out-of-scope for this plan (parking lot)

- ~~ESP32 deep sleep between interactions~~ — **moved into scope at P8** for the
  nightstand use case (sleep/wake, setting-toggled, default deep sleep). The
  "breaks the live HA API link" objection still applies to the *hand-held*
  always-connected use case, which P8's `Off` setting preserves.
- AXP2101 charge-curve battery % — raw voltage only in v1.
- Audio / dual microphone / wake-word — entire vertical not addressed.
- Light brightness / colour control — toggle only in v1.
- Climate / thermostat / media transport control — read-only in v1.
- SD card asset loading for icons — embed icons at compile time instead.

### Post-P7 TODO — live attrs in modal

The P7d detail modal currently opens with **default** slider values (brightness slider at 100 %, climate target at mid-range, etc.) instead of the entity's current HA attribute values. Apply still commits whatever the user dials in — usable, just not pre-populated.

Tried twice on 2026-05-29:

1. **One-shot `api::global_api_server->get_home_assistant_state(eid, attr, lambda)` at modal open.** Failed: post-connect entries land in `state_subs_` but ESPHome's per-client `state_subs_at_` cursor (`api_connection.cpp:2435`) is already at -1 by then, so the sub message never goes out. Every modal hit the 1500 ms safety timeout with all 6 attrs pending.
2. **Lazy persistent subscribe + cursor re-arm via `on_subscribe_home_assistant_states_request()`.** Failed: re-arm restarts the walk at 0 and re-sends the full subs vector (88 state + 6 new) at one message per loop tick (~30 ms), so the new entries don't actually go out until ~2.5 s after modal open — past the 1500 ms timeout, plus a lot of repeat traffic for HA to dedupe on every modal open.

Possible paths to retry later:

- **HA-side template sensor that batches per-entity attributes into one entity's attribute payload.** Sub once at startup → no per-modal fetch dance. Closest in spirit to P10's dynamic discovery sensor; would naturally fold into that work.
- **Upstream a public method on `APIConnection` that extends `state_subs_at_` to "start of new entries"** instead of restarting from 0. Avoids the re-walk cost. ESPHome PR.
- **Increase the safety timeout to 4 s and accept the cost** of re-walking all subs every modal open. Cheapest by code change but worst by network traffic.

Modal infrastructure (per-domain widget builders, Apply dispatch, sticky-edit semantics) is all in place — only the value-preload step is parked. See `ha_panel::ensure_attrs_subscribed_` and `ha_panel::request_detail_attrs_` for the scaffolding left in source.

---

## Open decisions (need user input before / during Phase 4–6)

1. **Idle timeouts:** proposed `dim_timeout: 15s`, `blank_timeout: 30s` (45s total to blank). Too aggressive? Too lazy?
2. **Motion sensitivity:** pick-up should wake, but a tap on the nightstand probably shouldn't. Calibrate empirically — any preference for false-wake vs missed-wake?
3. **Header content:** clock + battery, clock + battery + weather, or area name only?
4. **Tap-and-hold behaviour:** v1 ignores it. Could later expose a detail page (brightness slider for lights, set-point for climate). OK to defer?
5. **Touch driver fallback:** if no community CST9220/CST9217 fork works, are we willing to spend the time to write a small external component, or fall back to the Arduino-side touch lib via lambda?
6. **Phase 10 entity filter:** blacklist by domain (current sketch) vs. label-based opt-in (`label: show_on_panel`). Label-based is cleaner long-term but requires labelling every entity in HA.

---

## Session notes & decisions log

> Newest entry at top. Date in `YYYY-MM-DD`. One line per gotcha, decision, or surprise — anything future-you will want when picking the work back up after a few days away. Not a changelog — git log already does that. This is for *why* and *what bit me*.

### 2026-05-29 — P7e per-entity icons (on-device verified ✅)

- **Shipped v1 with ZERO new subscriptions** — deliberately dropped the HA `icon` attribute tier the original plan called for. Subscribing `icon` per entity at connect would re-trigger the exact TX-saturation failure logged below (P7d): ~100 icon subs on top of 88 state subs, silent service-call death. Icons resolve from YAML `icon: mdi:foo` override → compile-time domain default → fallback glyph, all client-side. Live HA-sourced icons deferred to the P10 batched template sensor (one sub, JSON payload).
- **Glyphs are generated, never hand-typed.** `tools/build-mdi-glyphs.py` fetches `@mdi/font@7.4.47` CSS, resolves names→PUA codepoints, and emits BOTH `components/ha_panel/mdi_icons.h` (C++ name→cp table + domain-default map + fallback) and `packages/mdi-font.yaml` (the `font:` block with the baked glyph list). Both from the same `ICON_NAMES`/`DOMAIN_ICON` source so the lookup table and the baked glyphs can't drift. Script aborts if any name doesn't exist in MDI — 72 glyphs resolved clean.
- **Font→C++ bridge gotcha.** An ESPHome `font::Font` only exposes `get_lv_font()` (and populates its `lv_font_`) when `USE_LVGL_FONT` is defined, which only happens if the font is *referenced in lvgl config*. Our UI is built in C++, so nothing referenced it → would not compile. Fix: a hidden 0-size anchor label in `packages/lvgl-ui.yaml` with `text_font: mdi_icons`. That forces the define + links the glyph data; ha_panel reads the font back via a `set_mdi_font()` setter (codegen `cv.use_id(font.Font)`), no symbol-name guessing.
- **Layout:** icon `lv_label` (MDI font, white) at `LEFT_MID +12`; friendly name shifts `+12→+48`, width `280→240`. If no `mdi_font` configured, `resolve_icon_` returns empty and the row keeps the pre-P7e flush-left layout — graceful degrade.
- **Flash cost:** firmware 14.7 % (1.20 MB / 8 MB) with 72 glyphs baked at size 24, bpp 4. Comfortable headroom.
- **Build flake (not P7e):** first link died on `libwpa_supplicant.a ... cannot read contents of section .xtensa.info` — a corrupt prebuilt IDF archive from a *concurrent build in another CLI*, not a code error (C++ had already compiled clean). Clean re-link succeeded. Watch for this any time two builds touch `.pioenvs` at once.
- **On-device: confirmed ✅** — glyphs render clean (no tofu), icon column reads well. Phase done. Live HA-`icon` source still the one parked item, intentionally deferred to P10.

### 2026-05-29 — P7d on-device: attribute-sub burst saturates TX path

- **First flash of P7d + P7c stacked** showed taps dispatching in firmware logs (`tap light.X → homeassistant.turn_off`) but the corresponding light never reacted, and HA log had no rejection message (permission already ON, verified). ~60 s after connect: `[W][api.connection]: Buffer full, ping queued`, then `Home Assistant 2026.5.4 (192.168.86.97): is unresponsive; disconnecting`. Header status dot flipped green → red and stayed red.
- **Root cause: attribute-subscription burst.** ~30 lights × 6 attrs + 2 climates × 6 attrs + a cover ≈ 193 extra `subscribe_homeassistant_state` calls sent at connect, on top of 88 state subs. TX path never settles. `api_connection::send_homeassistant_action` returns early when `flags_.service_call_subscription` is false; under sustained burst pressure HA's `SubscribeHomeassistantServicesRequest` (sent during HA-side init) appears to land in a dropped/late state, so every later firmware-initiated service call silently disappears. No firmware error; no HA-side log.
- **Diagnostic / fix:** disabled the entire P7d attr-sub loop in `setup()` (single block comment, no logic deleted). Reflashed — toggle taps started working immediately, no buffer-full warning, status dot stays green. Confirms the attribute-flood path is the culprit.
- **Follow-up attempt #1 (one-shot `get_home_assistant_state`) — DID NOT WORK.** Switched to one-shot fetch at modal open expecting it to bypass the connect-time flood. On-device logs showed every modal hitting the 1500 ms safety timeout with all 6 requests still pending. **Reason: ESPHome only transmits state subscriptions while the per-client `state_subs_at_` cursor in `api_connection.cpp:2435` is `>= 0`. HA arms it once via `SubscribeHomeAssistantStatesRequest` at connect; after the cursor walks past the end (`state_subs_at_ = -1`) any subs added later — including `get_home_assistant_state` requests, which under the hood are just sub entries with `once=true` — sit silently in `state_subs_` and never go out over the wire.** Same root cause is why the original P7d burst at connect didn't itself crash the link, it just over-saturated TX while the cursor *was* walking.
- **Follow-up attempt #2 (lazy persistent sub + cursor re-arm) — ALSO FAILED.** Reasoning held: `on_subscribe_home_assistant_states_request()` resets the cursor to 0, forcing a full re-walk that includes the new entries. But on-device confirmed the re-walk takes ~2.5 s for 88+6 entries at one message per loop tick (~30 ms each), and HA round-trips its state push on top of that. Every modal open still hit the 1500 ms timeout. Tested 4 modal opens in a row — all timed out, all built with defaults.
- **Parked the live-attr fetch entirely** per user direction (TODO logged in plan §"Post-P7 TODO: live attrs in modal"). Stripped the Loading placeholder and the `request_detail_attrs_` call from `open_detail_` — modal now builds immediately with whatever's already in `Entity::attrs` (typically empty on first open), so sliders show defaults. Apply still commits the user's chosen values; the only thing missing is "show me where this entity *currently* is." Scaffolding (`ensure_attrs_subscribed_`, `request_detail_attrs_`, `attrs_subscribed_`, `pending_attr_responses_`) left in source for the next attempt — likely will end up folded into a HA-side template-sensor batched-attrs payload when P10 dynamic discovery lands.
- **Ellipsis fix.** Replaced the single `…` glyph (U+2026) with three literal dots `...` in every user-visible label (`make_entity_row`'s "no state yet" placeholder, the LOCK_TEXT / COVER_TEXT / SUMMARY_TEXT / READ_ONLY_TEXT rebuild path, the detail modal "Loading..." line). LVGL's default Montserrat font doesn't include U+2026; the glyph rendered as the missing-character box.
- **Side-note (operator paranoia):** HA permission flag ("Allow the device to perform Home Assistant actions") is still the first thing to check on any "taps log but nothing happens" report, but in this case the user had already verified it ON twice — the silent failure mode is distinct from the permission-rejection mode (which would have produced an HA log line per call).

### 2026-05-29 — P7d per-entity detail modal (code complete, compile clean)

- **Long-press → modal** wired via `LV_EVENT_LONG_PRESSED` on every entity row whose domain has a detail modal (`light` / `climate` / `media_player` / `number` / `select` / `fan` / `cover`). Short-tap kept the P7c dispatch by switching row registration from `LV_EVENT_CLICKED` to `LV_EVENT_SHORT_CLICKED` — otherwise the click bubbling on release would double-fire after a long press. Verified at compile, on-device confirmation pending.
- **Attribute subscription path** uses `api::global_api_server->subscribe_home_assistant_state(eid, optional<string>(attr), lambda)` directly, not the `CustomAPIDevice` template wrapper. The wrapper only accepts a member function pointer, which would mean writing N near-identical setter methods (one per attribute name). Going through the api_server lets one lambda capture `(entity_idx, attr_name)` and dispatch into a single `on_attr_` sink. Shared scaffolding with P7e (HA `icon` attribute).
- **Subscription count is the main risk.** A `light` adds 6 attrs (brightness, color_temp_kelvin, min/max_color_temp_kelvin, supported_color_modes, color_mode); `climate` adds 6; `media_player` 3; etc. For an 88-entity config with ~30 controllable entities, that's roughly 80–120 extra subscriptions on top of the 88 state subs. Within HA native API limits but logged at `setup()` so a regression is visible.
- **Sticky modal during edit.** `on_attr_` updates `Entity::attrs` but deliberately does not re-render the open detail modal. Values are snapshotted at `open_detail_` time, so a mid-edit HA push doesn't yank the slider out from under the user. Apply / Cancel / bg-tap ends the edit; the next open picks up the latest attrs.
- **No spinbox/roller/arc.** All numeric controls are `lv_slider` and all option pickers are `lv_dropdown`. Single new LVGL widget enable (`LV_USE_DROPDOWN=1`). Sliders fake non-integer ranges by scaling: store as `int = value / step`, draw the scaled label, send `value * step` on Apply. Climate target temp / number value use this for 0.1 / 0.5 step values.
- **Service-call data is `map<string, string>`.** `volume_level` → `"0.45"`, `temperature` → `"21.5"`, etc. — formatted via `snprintf("%.2f"/"%.1f")`. HA service handlers coerce strings to numerics on their side; no client-side type juggling.
- **Apply pattern per domain.** Light: switch off → `light.turn_off`; switch on → `light.turn_on { brightness_pct, color_temp_kelvin }` with both keys only if their sliders are present. Climate: unconditionally fires both `climate.set_hvac_mode` and `climate.set_temperature` — HA tolerates redundant calls, and tracking "did the user touch this widget" added complexity for no real win. Media: volume_set on Apply. Number / select / fan / cover: one service each.
- **Immediate (non-Apply) buttons** for media transport (prev / play_pause / next / mute), cover transport (open / stop / close), fan off. These fire on `LV_EVENT_CLICKED` directly — no Apply commit needed. Mute reads `is_volume_muted` attribute and inverts so a single tap toggles.
- **Cover position slider is conditional** on `current_position` being non-null. If the cover only reports `open/closed`, the slider is replaced by a "Position not reported" label and only the three transport buttons are usable.
- **`-fno-exceptions` in ESP-IDF** broke an initial `try { std::stof } catch` pair in the attribute helpers. Replaced with `strtof` + `end == start` failure check. Worth remembering for any future helper that parses HA-supplied numerics.
- **Build flag added**: `-DLV_USE_DROPDOWN=1` in `boards/waveshare-2.16.yaml`. Runtime widget construction skips ESPHome's YAML-driven LV_USE_* detection (P6 pattern). Flash impact ~5 KB.
- **Compile clean** against ESPHome 2026.5.1; firmware 1173 KB / 14.4 % flash, 50 KB / 15.4 % RAM. On-device verification owed before flipping the P7d status to ✅ in the table.

### 2026-05-28 — P7c entity control + per-domain rendering (code complete, compile clean)

- **`RenderClass` enum** (in `ha_panel.h`) keyed off `Entity::domain` at codegen time. Six classes: `BINARY_SWITCH`, `ACTION_ICON`, `LOCK_TEXT`, `COVER_TEXT`, `SUMMARY_TEXT`, `READ_ONLY_TEXT`. `render_class_for_(domain)` is the single source of truth — adding a new domain means one line in that mapper + one branch in the renderer + one branch in the dispatcher. `READ_ONLY_TEXT` is the default so an unknown HA domain just renders as text instead of crashing.
- **Row widget per class.** `make_entity_row` switches on `render_class`:
  - BINARY_SWITCH → `lv_switch` (50×26, right-aligned at -16 px). `LV_OBJ_FLAG_CLICKABLE` cleared so the parent button captures the tap — without that the switch's own `LV_EVENT_VALUE_CHANGED` fires alongside the row's `LV_EVENT_CLICKED`, double-dispatching. Indicator tint forced to 0x66BB66 to match the rest of the panel's "on" green.
  - ACTION_ICON → single `LV_SYMBOL_PLAY` label tinted cyan (0x44CCDD). Cyan accent makes it read as an affordance, not a status.
  - LOCK_TEXT / COVER_TEXT / SUMMARY_TEXT / READ_ONLY_TEXT → text label at right edge, content + colour driven by `rebuild_entity_row_`.
- **Lock UX choice**: amber on unlocked (0xDDAA33), green on locked (0x66BB66). "Open lock" is a warning state for most users — green-on-unlocked would teach the wrong instinct.
- **Cover UX choice**: chevron-up + "Open" / chevron-down + "Closed", greyed "Opening" / "Closing" for transient states. Glyph hints at the direction the next tap will produce.
- **Lock dispatch is the only one that no-ops on transient states**. For binary domains we fall through to `homeassistant.toggle` on unknown/unavailable/transitioning — better to act than refuse. For lock, the security cost of acting on stale state is higher than the inconvenience of waiting one round-trip, so transient states log + return false.
- **Cover dispatch stays on `homeassistant.toggle`**, not `cover.toggle`. Generic `homeassistant.*` services dispatch per-domain on the HA side and keep the C++ dispatcher flat (one less domain-specific branch). Comment in `tap_entity_` captures the rationale.
- **Renamed** `badges_by_entity_` → `widgets_by_entity_` and `rebuild_entity_row_text_` → `rebuild_entity_row_` since the right-side widget is no longer always a label. Mechanical change; no behavioural drift.
- **Build flag**: added `-DLV_USE_SWITCH=1` to the board package. Runtime widget construction doesn't auto-pull it in; without the flag `lv_switch_create` links but generates a black-bar rendering. Flash +3 KB, RAM unchanged.
- **`button` codegen**: re-read `ha_panel/__init__.py` — schema does not filter by domain, so `button.*` entity_ids ship today without codegen changes. User just has to list them in `ha-entities.yaml`.

### 2026-05-28 — P7b polish round 2 (code complete, compile clean)

- **AXP2101 component** at `components/axp2101/` — read-only port of XPowersLib's VBAT path. Init = read `ADC_CHANNEL_CTRL` (0x30), OR in bit 0 (VBAT ADC enable), write back. Read = `BAT_AVER_VOL_H/L` (0x34/0x35), 14-bit averaged, 1 mV per LSB. Publishes a `sensor` in volts; the board YAML marks the sensor `internal: true` and pipes the value into `HAPanel::set_battery_voltage(x)` via `on_value`.
- **Wi-Fi RSSI** routed via stock ESPHome `wifi_signal` sensor in `packages/base.yaml`, `internal: true`, 30 s update, `on_value` lambda calls `HAPanel::set_wifi_rssi((int) x)`.
- **Header reflow**: clock left (44 px inset) → area + chevron centered → wifi → battery → status dot right (44 px inset, 12 px gap between right-cluster items). `lv_obj_align_to` chain keeps the right cluster anchored to the status dot, which keeps its absolute right-edge inset stable as the icons change.
- **LVGL has one `LV_SYMBOL_WIFI` glyph** — no 4-bar variants in the default symbol font. Tint by RSSI bucket instead. Battery has 5 variants (`_EMPTY`, `_1`, `_2`, `_3`, `_FULL`) so the icon itself communicates the level, with matching tint reinforcing it.
- **Apply/Cancel staging**: split the brightness path into `set_brightness_setter` (live preview, drives display only) + `set_brightness_committer` (writes `active_brightness_g` global on Apply). Slider drag → `staged_brightness_` + setter; `brightness_dirty_` flag set true when staged ≠ active. Apply runs committer + clears dirty. Cancel snaps slider back to `active_brightness_` + setter + clears dirty. Navigating away (tileview change off settings, or header tap from settings tile) silently reverts.
- **Settings layout split**: scrollable content area shrunk to 480x372 at y=0; absolute-positioned 60 px button row at y=380. Apply (green-tinted) right, Cancel (cool-tinted) left, 200 px each with 32 px side inset. Idle-blank does not currently trigger revert (no tile change fires) — flagged as a follow-up in the P7b checklist; doesn't seem common enough to bake a global-hook in for now.
- **Compile clean** against ESPHome 2026.5.1; on-device verification still owed before flipping the status to ✅ in the table.

### 2026-05-28 — P7a polish round 1 (verified on-device)

- **Renamed P7 → P7a** at the user's request so a P7b can collect follow-up polish items uncovered during P7a on-device testing, before P8 (multi-board) starts.
- **`id: ha_panel_id`** added to `packages/ha-entities.yaml` (real, gitignored) + `ha-entities.example.yaml`. The id is referenced by top-level YAML lambdas for clock updates, brightness setter, and api connect/disconnect triggers. Cleanest way to wire YAML → C++ in ESPHome.
- **`brightness_active` substitution → runtime global** (`active_brightness_g`, `restore_value: yes`). Required so the settings-tile slider's edits survive idle cycles (every `enter_active` resets brightness) and reboots. Substitutions are compile-time only and can't be edited at runtime.
- **`api.on_client_connected` / `on_client_disconnected`** triggers feed `HAPanel::set_api_connected(bool)` — drives the boot splash hide/show and the header status dot color. Discovered ESPHome supports these as standard `api:` automation triggers; no custom-API plumbing needed.
- **Rounded-corner inset**: panel's visible corner radius is *much* larger than the docs guide's "~16 px" estimate. Empirical: status dot clipped at 12, 28, 36 px insets; clean at **44 px**. Updated header status-dot and clock to that value; entity-list and picker-list bottom paddings (28 / 28 / 32 px) are looser since vertical scroll can bring a clipped row up. Anchor for future corner-inset decisions: 44 px works for top corners of this CO5300 panel.
- **`App.get_compilation_time()` deprecated** in ESPHome 2026.1, removed in 2026.7. Replaced with `App.get_build_time_string(buf)` which takes a `std::span<char, Application::BUILD_TIME_STR_SIZE>` (BUILD_TIME_STR_SIZE = 26, qualify via `Application::` from inside esphome namespace). Returns void; populates the buffer.
- **`LV_USE_SLIDER` transitive dep**: enabling slider also requires `LV_USE_BAR=1` (slider is implemented on top of bar). Compile error was clear (`#error "lv_slider: lv_bar is required."`), worth noting for any future widget enable.
- **Battery + RTC explicitly deferred.** Neither is in P7 exit criteria; battery needs a custom AXP2101 component, RTC is only useful when running offline. Tracked as deferred bullets, not unchecked items.
- **`Settings` accessible two ways**: scroll past the last area, or open the header picker and tap the `⚙ Settings` row (separate tinted bg `0x222A33` to set it apart from area rows).

### 2026-05-28 — P6 LVGL UI (verified on-device)

- **Runtime LVGL widget build, not YAML.** With 15 areas × ~6 entities each, declaring tiles + rows in YAML would be a mess. `HAPanel::build_ui_()` now constructs the tree via the LVGL C API on `lv_scr_act()` once setup runs. Plan §P10 was already going to need this approach for dynamic discovery, so starting it in P6 reduces P10 risk.
- **Widget flags via `build_flags`.** ESPHome's lvgl auto-enables only LVGL widgets referenced in YAML. Runtime builds skip that detection, so `LV_USE_FLEX`, `LV_USE_LABEL`, `LV_USE_BUTTON`, and `LV_USE_TILEVIEW` are forced on via `esphome.platformio_options.build_flags` in the board package. Without those, the runtime calls fail to link (errors like `'lv_label_create' was not declared in this scope`).
- **No `lv_list` wrapper in ESPHome.** Use `lv_obj_create` + `lv_obj_set_flex_flow(LV_FLEX_FLOW_COLUMN)` + scroll direction `LV_DIR_VER`. Functionally equivalent for vertical entity scrolling. Same pattern for the area-picker modal.
- **LVGL v9.5 API differences from v8 caught at compile**:
  - `lv_event_get_target(e)` returns `void *` — use `lv_event_get_target_obj(e)` for `lv_obj_t *`.
  - `lv_tileview_get_tile_act` → `lv_tileview_get_tile_active`.
  - `lv_obj_set_tile_id` → `lv_tileview_set_tile_by_index(uint32_t, uint32_t, anim)`.
  - `lv_btn_*` → `lv_button_*`.
- **Touch transform reset.** P3 used `swap_xy: true, mirror_x: true` based on a working Arduino reference, but that reference rendered the framebuffer at a different rotation than ESPHome's `mipi_spi` does by default. With the LVGL UI driving directional gestures, physical vertical swipes registered as horizontal in the tileview. Reset to `swap_xy: false, mirror_x: false, mirror_y: false` — directions match the display orientation.
- **Area picker** addresses a real ergonomic problem: cycling through 15 areas by swiping is tedious. Header tap opens a full-screen modal with all areas in a scrollable flex column; row tap calls `lv_tileview_set_tile_by_index` and closes the modal. Picker bg tap dismisses without jumping.
- **State badge colour cue**: green for on/open/home/active, grey for off/closed/away/idle, red for unavailable/unknown, white otherwise. Numeric / free-text states render in white as-is.
- **Long entity names**: friendly_name label uses `LV_LABEL_LONG_DOT` so anything past ~300 px ellipsises rather than overlapping the state badge.

### 2026-05-28 — P5 static HA entity model (code complete)

- **Architecture:** single `ha_panel` external_component, not per-entity text_sensors. Reasoning: declaring 100+ `homeassistant.text_sensor` blocks would balloon compile time, eat heap for HA Entity object overhead we don't need (no `name:`, no HA discovery), and force a separate path for tap-toggle. One C++ class with a flat entity vector + N `subscribe_homeassistant_state` calls is cheaper and unifies state + service dispatch.
- **API flags required:** `homeassistant_states: true` (for `subscribe_homeassistant_state`) and `homeassistant_services: true` (for `call_homeassistant_service`). Without them, `custom_api_device.h` static_asserts at compile time with a clear message. Both added to `packages/base.yaml`.
- **Callback form:** 2-arg `subscribe_homeassistant_state(&HAPanel::on_state_, entity_id)` — ESPHome's variant captures `entity_id` by value in the per-subscription lambda, so a single dispatcher knows which entity fired. Cleaner than declaring N methods or capturing index.
- **Domain table** (mirrors plan §P6): `light/switch/fan/input_boolean/cover` → `homeassistant.toggle`; `script` → `script.turn_on`; `automation` → `automation.trigger`; everything else (sensor, binary_sensor, climate, media_player, select, …) → read-only, log + return false.
- **Schema migration:** users' real `ha-entities.yaml` is now wrapped under a top-level `ha_panel:` key (was naked `areas:`). Same change applied to the committed `ha-entities.example.yaml`. Two-line refactor for anyone updating from the pre-P5 shape.
- **Open**: state subscription is live but no LVGL rendering yet — that's P6. Verified flash next; on-device check confirms first-state logs land in serial console.

### 2026-05-28 — P4 idle state machine + IMU wake (verified on-device)

- **First flash worked.** Boot → dim @ +16s (expected 15s, +1s interval granularity). Touch from `dim` wakes to active. Idle 30s at dim → `blank`. Tap on blank screen → instant wake. Pick up from `dim` → motion wake. All four transitions confirmed.
- **CST9220 still works through dim/blank** — touch IC stays powered, no current reduction tweaks needed.
- **`mipi_spi::set_brightness(uint8_t)` confirmed runtime-callable** on this board (CO5300). 0xD0 / 0x20 / 0x00 all behave as expected. AMOLED at 0x00 = truly off-looking, not just very dim.
- **Motion log defaulted to `ESP_LOGD`** → silent at logger INFO. Bumped to `ESP_LOGI` so the wake-source is visible in the log without changing global verbosity.
- **No QMI8658 setup banner in first boot log.** Either truncated by serial buffer or component log filter; component clearly initialised since motion-wake fires. Not chasing.
- **Open tuning**: motion threshold 0.10 g not yet calibrated against false-wakes (e.g. pocket / table-tap). Surface via `motion_threshold_g` substitution in `idle.yaml` for top-level override.

### 2026-05-28 — P4 idle state machine + IMU wake (code complete)

- **IMU INT pin not exposed** on Waveshare 2.16. Confirmed by reading both Waveshare's own pin_config.h (only TP_INT=11 is broken out — no IMU INT) and their `04_LVGL_QMI8658_ui` Arduino example, which polls with `qmi.getDataReady()`. Plan correctly flagged this as a risk; outcome is software polling. Document so future-you doesn't waste a day chasing the INT line on a schematic.
- **`mipi_spi::set_brightness(uint8_t)` works at runtime** (header at `mipi_spi.h:103`; calls `reset_params_()` which re-sends the BRIGHTNESS controller command 0x51). The plan §P2 warning that "lambda set_brightness is unverified" can be retired for ESPHome 2026.5.1. Brightness goes 0x00–0xFF, with 0x00 = panel dark.
- **State machine via `interval: 1s` + millis() timestamp**, not LVGL screensaver or per-script delays. Reasoning: only one input timestamp to keep coherent; transitions to dim/blank are idempotent; wake is just `notify_input` from any source, which restamps and runs `enter_active` if we were dimmed/blanked.
- **QMI8658 polling cadence**: ODR = 31.25 Hz low-power (CTRL2 = 0x1E). Component polls at 100 ms via PollingComponent base. Frame-to-frame `|Δa|` magnitude is the motion signal — robust against gravity (it cancels in the delta) but you need a *recent* baseline, which is why update_interval is set short enough that two consecutive reads always have a frame in between.
- **Software motion threshold default = 0.10 g.** Tuned by feel for "picking up the device should wake it" vs "tapping the table next to it should not." Not on-device validated yet. Surface via `motion_threshold_g` substitution so the top-level YAML can override.
- **`auto_clear_enabled: false` + brightness-only blank** = no LVGL teardown, no widget recreate on wake. Page state preserved through dim/blank/active transitions. Cheap.

### 2026-05-27 — P3 touch up (after 6 false starts)

- **Authoritative CST9220 reference**: `lewisxhe/SensorLib` `TouchDrvCST92xx.cpp` in `C:/Users/billi/Source/repos/esp32-cheap-yellow-display-examples/.pio/libdeps/sand-multi-task-waveshare-esp32-s3-2_16inch/SensorLib/src/touch/`. The Arduino working code at `projects/sand-multi-task-waveshare-esp32-s3-2_16inch/main.cpp` is the proof-positive driver call sequence.
- **THE bug** in the vendored ESPHome cst9220 driver: missing post-read ACK write. Chip's DATA_REG works once-then-locks until you write `D0 00 AB` back. We were never ACKing → buffer never refilled → all-zero reads forever. Without this insight, no amount of YAML tweaking would have fixed it.
- **Second bug**: original used `write_register16(0xD101, [0xD1, 0x01], 2)` which puts 4 bytes on the wire (`D1 01 D1 01`); SensorLib's `writeRegister(0xD1, 0x01)` puts 2 bytes (`D1 01`). The extra junk confused chip's mode state machine. Fix: use raw `this->write(buf, 2)` for cmd-mode entry.
- **Third bug**: original wrote `0xD109` (NORMAL_MODE_REG) explicitly to exit cmd mode; SensorLib does nothing here — chip auto-transitions when DATA_REG (`0xD000`) is read. Removing the explicit exit write fixed the all-zero stall.
- **Load-bearing read**: SensorLib's `getAttribute()` reads 8 bytes from `0xD208` (FW version + checksum). Skipping that left chip in a half-init state. Added it.
- **Pin 2 RST sharing** between display + touch needs `allow_other_uses: true` on *both* claimants — confirmed reading both blocks compile together.
- **Power-cycle required after every flash** for touch to start responding. Sibling project hit same wall. Likely chip enters bad state during ESP32 partial reset (USB reset doesn't fully cycle the AXP2101 → touch IC stays in zombie mode). Document.
- **Transforms** per working Arduino: `setSwapXY(true) + setMirrorXY(true, false)` → LVGL `swap_xy: true, mirror_x: true, mirror_y: false`.

### 2026-05-27 — P2 display up

- **Authoritative pin map** for Waveshare 2.16: CS=12 SCLK=38 D0..D3=4,5,6,7 LCD_RST=2 (shared with TP_RST). Source: `esphome-lvgl-dashboard/device.yaml` cross-referenced with waveshareteam pin_config.h. Guide §4 had 1.75 board pins which differ — do not trust the guide for GPIO numbers.
- **Panel geometry trap**: CO5300 controller addresses 480x480 but the visible AMOLED is 466x466 inset ~7px. ESPHome's CO5300 driver appears to apply its own offset, so adding `offset_width: 6` per the guide stacked the shift and produced a thin pink ring on all four edges. Fix: declare dimensions as 480x480 (over-draw), no `offset_width`/`offset_height`. AMOLED black bg renders the over-drawn bezel rim invisible.
- **Pin 2 RST sharing** between display and touch needs `allow_other_uses: true` on *both* claimants. Cannot set it on display alone — P3 will add the touch side and re-enable it.
- **Perf sdkconfig**: added 5 sdkconfig_options (CPU 240, data cache 64K/64B, SPIRAM fetch instructions + rodata) borrowed from working sibling project. Improves LVGL framerate on PSRAM-backed buffers.

### 2026-05-27 — P1 skeleton flashed

- Google Wifi mesh required three wifi tweaks to associate: `power_save_mode: NONE`, `fast_connect: true`, and `manual_ip:` static lease. DHCP alone hit `Association Leave` timeout every time. Carry this forward to any new board package on this network.
- Static IP `192.168.86.124` reserved for this device. `.123` is the other esp32-amoled (esphome-lvgl-dashboard).
- First boot logged `safe_mode: Last reset too quick; invoke in 3 restarts` — benign, expected after a fresh USB flash + immediate reboot.

### 2026-05-27 — bootstrap

- **ESPHome version in use: 2026.5.1.** Pin any version-sensitive checks against this baseline (CO5300 green-line fix, `mipi_spi` brightness, LVGL widget API).
- Decided: native API encryption key only; no HA long-lived access token. ESPHome ↔ HA native API doesn't need one.
- Decided: idle dim→blank state machine with IMU + touch wake goes in **P4**, before any real UI. Drives every later phase's power test.
- Decided: static YAML entity model in P5, dynamic HA-template-sensor in P10. Two-step ships fastest.
- Decided: ESP32 deep sleep **not** used — would break HA API. AMOLED panel blank is the sleep mechanism.
- Open: idle timeouts (15s dim / 30s further to blank), motion sensitivity, header content, tap-and-hold behaviour, touch driver fallback, P10 filter strategy.

<!--
### YYYY-MM-DD — phase N
- Worked on X. Hit issue Y. Workaround Z (see commit SHA).
- Decision: chose A over B because C.
- Blocker: D — waiting on E.
-->



---

# Build Plan — UI enhancements (post-MVP)

Three scoped UI improvements on top of the shipped MVP panel: a persistent
bottom navigation bar (with settings moved into an overlay sheet), richer
Wi-Fi / Home Assistant connection-status indicators, and a fix for the area
title overrunning its dropdown chevron.

Sibling plans: the MVP build plan (shipped baseline — see the first half of this document) ·
[plan-multi-board-support.md](plan-multi-board-support.md) ·
[plan-dynamic-discovery.md](plan-dynamic-discovery.md).

Background reference: [docs/esp32-s3-amoled-ha-guide.md](docs/esp32-s3-amoled-ha-guide.md).

---

## Goal

Make navigation explicit and discoverable instead of swipe-only, and make the
header status icons tell the truth during the connection transitions that
happen on every sleep/wake cycle. All three changes are UI-layer only — no new
HA subscriptions, no board/hardware changes, no power-state changes.

---

## Motivation / context

- **Nav is swipe-only.** Areas are `lv_tileview` columns; the only deliberate
  way to change area is a horizontal swipe or the top dropdown picker. There's
  no visible affordance that says "you can move between areas," and no
  one-tap step control. Settings lives at the far end of the carousel
  ([build_settings_tile_](components/ha_panel/ha_panel.cpp#L646)) plus a row in
  the picker, which mixes a config screen into the area carousel.
- **Status icons lie during reconnect.** P8 sleep drops Wi-Fi
  (`wifi.disable`) on sleep-enter and brings it back on wake. During that
  window the Wi-Fi icon holds its last RSSI tint and the HA status dot is just
  red — neither communicates "re-establishing." The user sees a red dot on
  every wake with no signal that the panel is actively reconnecting.
  Status dot is binary red/green ([update_status_dot_](components/ha_panel/ha_panel.cpp#L1081));
  Wi-Fi icon is RSSI-tinted with no connecting state ([update_wifi_icon_](components/ha_panel/ha_panel.cpp#L1228)).
- **Long area names overrun the chevron.** The dropdown chevron is positioned
  once at build via `lv_obj_align_to(... OUT_RIGHT_MID)` ([ha_panel.cpp:860](components/ha_panel/ha_panel.cpp#L860))
  and the area label has no width cap or ellipsis. When the active area changes
  to a long name the label grows but the chevron doesn't reposition, so the
  text runs over the arrow.
- **Cover detail modal hides the state you already saw.** The area-row cover
  badge shows `Entity::state` (open/closed/opening/closing) from the state
  subscription. The cover detail modal ([build_detail_cover_](components/ha_panel/ha_panel.cpp#L2028))
  shows only Open/Stop/Close + a position slider gated on the `current_position`
  *attribute*. A cover that reports open/closed but no position percentage
  (e.g. a ratgdo garage door) falls into the `cur_pos < 0` branch and renders a
  bare "Position not reported" ([ha_panel.cpp:2065](components/ha_panel/ha_panel.cpp#L2065))
  — technically true (no percentage) but confusing, because the open/closed
  state is already in the model and never shown in the modal.

---

## Candidate enhancements (parking lot)

All three below are promoted to phases. Future ideas land here first.

- [x] Bottom nav bar + settings overlay sheet → **E1**
- [x] Wi-Fi / HA connecting-state indicators → **E2**
- [x] Area-label chevron overlap fix → **E3**
- [x] Cover detail modal: show current state → **E4**
- [x] Boot splash: show current connection stage → **E5**
- [x] Rename areas → pages + bottom-bar Home button → **E6**
- [x] Light detail modal: real brightness, no fake 100% → **E7**
- [x] Per-entity component size (small / medium / large) → **E8**
- [x] Read-only entity history chart sheet → **E9** (ring-buffer + REST backfill)

---

## Phases

### Phase E1 — Bottom navigation bar + settings sheet

**Status:** ✅ done · target tag: `e1-bottom-nav`

**Goal:** A persistent bottom bar with left/right area-step arrows and a center
settings gear. Settings moves out of the area carousel into an overlay sheet.
The top area dropdown stays; the carousel swipe stays. Settings is removed from
both the carousel and the area picker.

#### Layout

- New bottom bar, full width, ~48 px tall, at **y = 432–480**. The tileview
  shrinks from 440 → **392 px** (y = 40–432). The bar sits over the bottom
  rounded-corner zone, so the entity list's bottom padding can drop from 28 px
  → ~8 px (the list now ends at 432, well above the corner curve).
- Three controls on the bar:
  - `◀` left arrow — `LV_ALIGN_LEFT_MID, +44, 0` (44 px corner inset, same
    rule as the header).
  - `⚙` gear — `LV_ALIGN_CENTER`.
  - `▶` right arrow — `LV_ALIGN_RIGHT_MID, -44, 0`.
- Arrow + gear are `lv_button`s (~56 px wide touch targets) with the same
  `LV_STATE_PRESSED` feedback used elsewhere.

#### Arrow behaviour

- Arrows step the active area by ±1 via programmatic
  `lv_obj_set_tile(tileview, tile, LV_ANIM_ON)` (programmatic tile-set is not
  bound by the per-tile `LV_DIR_*` swipe constraints, so this works for any
  jump). **Wrap-around:** left arrow on the first area jumps to the last; right
  arrow on the last jumps to the first. No dead taps, no greyed states.
- Horizontal swipe between areas is retained — arrows are additive, not a
  replacement.

#### Settings → overlay sheet

- Settings stops being a tileview tile. Rebuild it as a **full-screen overlay
  sheet** matching the existing `detail_modal_` / `confirm_sheet_` pattern:
  built once at setup, hidden by default, `lv_obj_move_foreground` on open.
- The gear button opens the sheet. The current settings content — brightness
  slider, idle-timeout summary, Power-saving section (P8 toggle + mode
  dropdown), About block, and the Apply/Cancel row — lifts over essentially
  unchanged from [build_settings_tile_](components/ha_panel/ha_panel.cpp#L646).
- **Apply** → commit (brightness + sleep settings) **then close** the sheet.
  **Cancel** and **background tap** → revert **then close**. (Today Apply/Cancel
  only commit/revert and leave you on the tile; E1 adds the close.)
- The staged/dirty + revert-on-navigate-away machinery
  (`apply_brightness_`/`revert_brightness_`/`apply_sleep_`/`revert_sleep_`)
  is preserved; the trigger to revert changes from "tileview navigated away
  from settings tile" to "sheet closed without Apply."

#### Removals

- Drop the settings tile from the tileview build loop →
  `total_cols` = `areas_.size()` (no `+1`). The "settings is last tile" wiring
  in [build_ui_](components/ha_panel/ha_panel.cpp#L955), `settings_tile_`, and
  `is_settings_active_()` retarget to the sheet's open/close state.
- Remove the `LV_SYMBOL_SETTINGS "  Settings"` row from the area picker
  ([ha_panel.cpp:1014](components/ha_panel/ha_panel.cpp#L1014)). The picker
  becomes areas-only.

Tasks:
- [x] Add bottom-bar container + three buttons in `build_ui_`; resize tileview
      to 392 px; reduce entity-list bottom padding.
- [x] `step_area_(int delta)` helper with wrap-around; wire to the two arrows.
- [x] Convert `build_settings_tile_` → `build_settings_sheet_` overlay; add
      `open_settings_`/`close_settings_`; gear button opens it.
- [x] Apply/Cancel/bg-tap close the sheet; rewire the revert trigger off the
      tileview-change path onto sheet close.
- [x] Remove the settings tile from the carousel and the Settings row from the
      picker; fix `total_cols`, `is_settings_active_`, `on_tileview_changed_`.

**Exit criteria:**
- Bottom bar visible on every area; `◀`/`▶` step areas with wrap-around;
  swipe still works.
- Gear opens the settings sheet; Apply commits and closes; Cancel and bg-tap
  revert and close.
- Area carousel contains only areas (no settings tile); area picker lists only
  areas (no Settings row); top dropdown still opens the picker.

**Risks / unknowns:**
- Bottom-bar arrow buttons sit near the bottom rounded corners; 44 px inset
  should clear them but verify on-device (corner radius bites less at the
  bottom per P7a notes).
- Moving settings out of the tileview touches the revert-on-navigate logic in
  `on_tileview_changed_`; confirm no orphaned dirty state after the refactor.

---

### Phase E2 — Connection status indicators (Wi-Fi + HA)

**Status:** ✅ done · target tag: `e2-status`

**Goal:** The Wi-Fi icon and HA status dot show a distinct "connecting /
re-establishing" state instead of going stale or showing a bare red dot during
the reconnect that happens on every sleep/wake cycle.

#### Three states derived from `(wifi_connected, api_connected)`

- Feed Wi-Fi link state into the panel: add `wifi:` `on_connect` /
  `on_disconnect` triggers in [base.yaml](packages/base.yaml) calling a new
  `HAPanel::set_wifi_connected(bool)`. ESPHome auto-reconnects after a drop, so
  "disconnected" is treated as "connecting."
- **Wi-Fi icon:**
  - connected → RSSI-bucket tint (today's behaviour, [update_wifi_icon_](components/ha_panel/ha_panel.cpp#L1228)).
  - connecting → **amber, blinking**.
  - down → red.
- **HA status dot:**
  - api connected → green (today).
  - wifi up but API not yet connected → **amber, blinking** ("link not yet
    re-established"). Note the device is the API *server* and HA is the client,
    so this is "waiting for the link to come back," which amber communicates
    honestly.
  - wifi down → red (can't even attempt the HA link yet).

#### Blink

- One shared LVGL `lv_timer` at ~500 ms toggles a `blink_on_` bool and
  refreshes any indicator currently in a pending (amber) state. Stable states
  (green / red / RSSI tint) don't animate.
- Fallback: if blink misbehaves on-device, drop to solid amber — same colour,
  no timer. Low risk either way.

#### Wake scenario this fixes

On light-sleep wake (P8): `wifi.disable` fires `on_disconnect` → both
indicators go amber-blink; `wifi.enable` + reassociate fires `on_connect` →
Wi-Fi icon returns to RSSI tint; HA client reconnects → dot goes green. The
user now sees an active "reconnecting" animation instead of a stale icon + red
dot.

Tasks:
- [x] `set_wifi_connected(bool)` + `wifi_connected_` member; wire
      `on_connect`/`on_disconnect` in `base.yaml`.
- [x] Rework `update_wifi_icon_` and `update_status_dot_` to the 3-state model.
- [x] Shared 500 ms blink `lv_timer` refreshing pending indicators.

**Exit criteria:**
- After sleep/wake, the Wi-Fi icon blinks amber while reassociating, then
  returns to an RSSI tint once connected.
- The HA dot blinks amber while the API link is re-establishing, goes green on
  connect, and shows red only when Wi-Fi itself is down.
- No blink (solid states) when connection is stable.

**Risks / unknowns:**
- An `lv_timer` running continuously is cheap, but confirm it doesn't keep the
  device out of any idle/sleep path (it shouldn't — sleep tears down LVGL
  rendering anyway).
- Verify `on_disconnect`/`on_connect` fire cleanly around `wifi.disable`/
  `wifi.enable` and not just on real RF drops.

---

### Phase E3 — Area-label chevron overlap fix

**Status:** ✅ done · target tag: `e3-header-chevron`

**Goal:** Long area names never run over the dropdown chevron.

**Approach:** Wrap the area-name label and the chevron in a centered horizontal
flex container. Cap the label width to the span between the clock (left) and
the right-hand icon cluster, and enable `LV_LABEL_LONG_DOTS` ellipsis. The
chevron is laid out by the flex container immediately to the right of the
(possibly truncated) label, so it repositions automatically on every area
change and is never overlapped. A long name renders as `Living Roo…  ▼`.

This replaces the one-shot `lv_obj_align_to` ([ha_panel.cpp:860](components/ha_panel/ha_panel.cpp#L860)),
which only positioned the chevron at build time and never re-ran when the label
text changed.

Tasks:
- [x] Centered flex row containing label + chevron in the header.
- [x] Label width cap + `LV_LABEL_LONG_DOT` (LVGL 8.4 spelling).
- [x] Remove the build-time `align_to` chevron positioning.

**Exit criteria:**
- With the longest configured area name selected, the chevron is fully visible
  and the name ellipsizes rather than overrunning it.
- Short names still center cleanly with the chevron tight to their right.

**Risks / unknowns:**
- The available center span depends on clock width (left) and the
  Wi-Fi/battery/dot cluster (right); pick the cap so the worst-case clock
  ("12:00 pm") and the icon cluster don't collide with the label box.

---

### Phase E4 — Cover detail modal: show current state

**Status:** ✅ done · target tag: `e4-cover-state`

**Goal:** The cover detail modal shows the cover's current open/closed state, so
a cover that doesn't report a position percentage no longer reads as a bare
"Position not reported."

**Root cause:** state ≠ position. `Entity::state` (open/closed/opening/closing)
arrives on the state subscription and is shown in the area row. The
`current_position` *attribute* (0–100 %) is separate and many covers — e.g. a
ratgdo garage door — never report it. The modal gates its position slider on
that attribute and, when absent, shows only "Position not reported," never the
state the user already saw.

**Approach (zero new subscriptions — `Entity::state` is already populated):**
- Add a current-state line at the top of [build_detail_cover_](components/ha_panel/ha_panel.cpp#L2028):
  `Currently: Open` / `Closed` / `Opening…` / `Closing…`, sourced from
  `Entity::state`, with the same colour cue as the area-row cover badge
  (green = open, grey = closed, italic/neutral for the transient states).
- Keep the position slider gated on `current_position`. Replace the standalone
  "Position not reported" label ([ha_panel.cpp:2065](components/ha_panel/ha_panel.cpp#L2065))
  with the state line — for an open/closed-only cover the transport buttons +
  state line are the whole interface, so the confusing note is gone. Render the
  "Position" section + slider only when `current_position` is present.
- Mirror the same state line under the title in the cover **confirm sheet**
  ([open_confirm_action_ cover branch](components/ha_panel/ha_panel.cpp#L2576))
  for consistency. (Optional within this phase; same data, no extra cost.)

Tasks:
- [x] State-line helper that maps cover `Entity::state` → label text + colour.
- [x] Add it to the top of `build_detail_cover_`; drop the bare "Position not
      reported" branch; keep the slider gated on `current_position`.
- [x] Mirror the state line under the cover confirm-sheet title.

**Exit criteria:**
- Opening the detail modal for a position-less cover shows its open/closed
  state at the top and no "Position not reported" text.
- A cover that *does* report `current_position` still shows the position slider,
  now under the state line.
- The cover confirm sheet shows the current state under the title.

**Risks / unknowns:**
- Transient states (`opening`/`closing`) update only on the next state
  callback; the line refreshes whenever the modal is (re)opened, which is
  enough — no need to live-update an open modal for v1.

---

### Phase E5 — Boot splash: show current connection stage

**Status:** ✅ done · target tag: `e5-splash-stage`

**Goal:** The boot splash tells the user *which* connection gate it's waiting on
instead of always reading "Connecting to Home Assistant...".

**Root cause:** The splash hides on exactly one trigger —
`set_api_connected(true)` ([ha_panel.cpp:1195](components/ha_panel/ha_panel.cpp#L1195)),
fired by `api.on_client_connected` ([base.yaml:45](packages/base.yaml#L45)).
There are two sequential gates before that fires: Wi-Fi must associate, then the
HA API client must connect + handshake. The splash status label is a hard-coded
local string ([ha_panel.cpp:1098](components/ha_panel/ha_panel.cpp#L1098)) that
reads "Connecting to Home Assistant..." the whole time — so a panel stuck on the
Wi-Fi gate (or stuck because HA is down / wrong key / restarting) shows the same
text either way.

**Approach (no new signals — both link states are already wired):**
- Promote the splash status label to a member `splash_status_{nullptr}`
  ([ha_panel.h:268](components/ha_panel/ha_panel.h#L268)) so it can be updated
  after build.
- Add `update_splash_status_()` that picks text from current state:

  | State | Text |
  |-------|------|
  | `!wifi_connected_` | `"Connecting to Wi-Fi…"` |
  | wifi up, `!api_connected_` | `"Connecting to Home Assistant…"` |

  No "done" text — the splash hides the moment the API connects.
- Set initial text via `update_splash_status_()` at build time (reads current
  `wifi_connected_`/`api_connected_`, so it's correct even if `on_connect` fired
  before `build_ui_` ran).
- Call `update_splash_status_()` from `set_wifi_connected`
  ([ha_panel.cpp:1201](components/ha_panel/ha_panel.cpp#L1201)) so the text flips
  to "Home Assistant" once Wi-Fi lands. `set_api_connected` already hides the
  splash — no text change needed there.
- All label writes guard on `splash_status_ != nullptr` (link-state setters can
  fire before the UI is built).

Tasks:
- [x] Add `splash_status_` member + `update_splash_status_()` declaration.
- [x] Store the status label in `splash_status_`; set initial text via the
      helper instead of the hard-coded string.
- [x] Implement `update_splash_status_()` (2-stage Wi-Fi → HA text, nullptr
      guard).
- [x] Call it from `set_wifi_connected`.
- [x] Per-stage "working" indicator: a `splash_dot_` amber circle that pulses
      opacity off the shared 500 ms blink timer. Two extra wires were needed:
      drive the dot from `blink_timer_cb_` (via `update_splash_status_()`), and
      kick `update_blink_timer_()` once at the end of `build_ui_` so the timer
      runs even when stuck on the first gate (no setter fires when the initial
      state already matches).

**Deviation:** kept the existing ASCII `...` (not the `…` glyph in the table) —
built-in `lv_font_montserrat_18` carries ASCII only, so U+2026 would render as a
missing-glyph box.

**Exit criteria:**
- On a cold boot with no Wi-Fi yet, the splash reads "Connecting to Wi-Fi…".
- Once Wi-Fi associates but HA hasn't connected, it reads "Connecting to Home
  Assistant…".
- The splash still hides on HA API connect, unchanged.

**Risks / unknowns:**
- Only two stages are honestly distinguishable from wired signals (Wi-Fi, HA
  API). There's no separate "subscribing to entities" stage to show — out of
  scope, and it adds no TX-budget cost to keep it that way.
- `wifi.on_connect` may fire before `build_ui_`; the build-time
  `update_splash_status_()` call covers that ordering.

**Follow-up — per-stage rows + green checkmarks:**
The single swapping status label + single blinking dot were replaced by one row
per init stage. Each row is a phrase label (`"Connecting to Wi-Fi..."`,
`"Connecting to Home Assistant..."`) with a status indicator to its **right,
after the `...`**: an amber dot that blinks off the shared 500 ms timer while the
stage is in progress, swapped for a green `LV_SYMBOL_OK` check once the stage
completes. Wi-Fi is the first gate, HA API the second.
- `splash_status_`/`splash_dot_` members were dropped in favour of a `SplashStage`
  struct (`{dot, check}`) with one instance per stage (`splash_wifi_stage_`,
  `splash_ha_stage_`).
- `build_splash_stage_(parent, text, y)` builds a row; `update_splash_stage_(st,
  done, active)` drives it — green check when done, amber dot blinking when
  active, dim-steady when not yet reached.
- `update_splash_status_()` now just maps link state to per-stage done/active:
  Wi-Fi done = `wifi_connected_`, HA done = `api_connected_`.
- The HA (last) stage flips to its green check in `set_api_connected` just before
  the splash hides — not on-screen long, but kept consistent so future extra
  stages behave uniformly.
- Adding a stage later = add a `SplashStage` member + a `build_splash_stage_`
  call + an `update_splash_stage_` line.

---

### Phase E6 — Rename areas → pages + bottom-bar Home button

**Status:** ✅ done · target tag: `e6-pages-home`

**Goal:** Rebrand the panel's top-level entity grouping from "areas" to "pages"
throughout the config schema, code, and example YAML, and add a Home button to
the bottom bar that jumps to the first page. Two independent parts — a
mechanical rename (no behaviour change) and one new bottom-bar control.

#### Motivation / context

- **"Areas" collides with Home Assistant's own concept.** The panel's
  left-to-right groupings are authored locally in `ha-entities.yaml`; they are
  not HA areas. Calling them "pages" makes the UI grouping clearly ours and
  removes the ambiguity.
- **No one-tap "go home."** The bottom bar steps ±1 with `◀`/`▶`
  ([ha_panel.cpp:1018-1020](components/ha_panel/ha_panel.cpp#L1018-L1020)) and
  the top picker jumps arbitrarily, but there's no single control to return to
  the first page. A Home button is the obvious affordance.

#### Part A — Rename areas → pages

Pure rename, zero behaviour change. Targets every "area" identifier and the
YAML key, leaving HA-side names (`entity_id`, `friendly_name`) untouched.

- **[\_\_init\_\_.py](components/ha_panel/__init__.py):** `CONF_AREAS = "areas"`
  → `CONF_PAGES = "pages"` (the YAML key), `AREA_SCHEMA` → `PAGE_SCHEMA`,
  `add_area(...)` call → `add_page(...)`, loop var `area` → `page`, header
  comment "area + entity" → "page + entity".
- **[ha_panel.h](components/ha_panel/ha_panel.h):** `struct Area` → `struct
  Page`; `areas_` → `pages_`; `add_area` → `add_page`; `num_areas` →
  `num_pages`; `step_area_` → `step_page_`; `tap(size_t area_idx, …)` →
  `tap(size_t page_idx, …)`; member/comment docs.
- **[ha_panel.cpp](components/ha_panel/ha_panel.cpp):** all ~48 "area" hits —
  renamed method impls, the tileview build loop, `tile_objs_`/header wiring,
  log strings (`"… across %u areas"` → `"… pages"`, dump_config lines), picker
  title `"Pick area"` → `"Pick page"`, and comment refs.
- **[ha-entities.example.yaml](packages/ha-entities.example.yaml):** `areas:`
  → `pages:`; header comments ("Areas render left-to-right…" → "Pages render…").
- **Not touched:** [packages/ha-entities.yaml](packages/ha-entities.yaml) — the
  user updates the real config manually.

#### Part B — Home button on bottom bar

Current bar: `◀ left-step (LEFT_MID +44) · ⚙ gear (CENTER) · ▶ right-step
(RIGHT_MID −44)` ([ha_panel.cpp:1012-1038](components/ha_panel/ha_panel.cpp#L1012-L1038)).

- Edge step-arrows stay put (`◀` LEFT_MID +44, `▶` RIGHT_MID −44).
- **Home** `LV_SYMBOL_HOME` at `LV_ALIGN_CENTER, x_ofs = −44`.
- **Gear** moves from dead-center to `LV_ALIGN_CENTER, x_ofs = +44`.
- 56 px buttons → Home spans ~x168–224, gear ~x256–312 → a **~32 px gap**
  between them to reduce accidental cross-taps; both clear the edge arrows.
- New `on_home_clicked_` trampoline navigates to the first page:
  `lv_tileview_set_tile_by_index(tileview_, 0, 0, LV_ANIM_ON)` + set
  `header_label_` to `pages_[0].name`. Factor the tile-set + header-update tail
  of `step_page_` into a small `go_to_page_(size_t)` helper shared by both.
- `LV_SYMBOL_HOME` ships in the built-in montserrat symbol set — no new
  font/glyph cost.

Tasks:
- [x] Rename areas → pages in `__init__.py` (`CONF_PAGES`, `PAGE_SCHEMA`,
      `add_page`, comments).
- [x] Rename in `ha_panel.h` (`Page`, `pages_`, `add_page`, `num_pages`,
      `step_page_`, `tap(page_idx)`, docs) and declare `on_home_clicked_` +
      `go_to_page_`.
- [x] Rename all "area" hits in `ha_panel.cpp` (incidental "content area" /
      "Private Use Area" left as-is).
- [x] `areas:` → `pages:` + comments in `ha-entities.example.yaml`; leave
      `ha-entities.yaml` alone (user updates the real config — still on `areas:`,
      so the firmware won't compile until that one key is renamed).
- [x] Add Home to the `nav_btns[]` table; move gear to CENTER +44, Home CENTER
      −44; `step_page_` tail factored into `go_to_page_`; `on_home_clicked_` →
      `go_to_page_(0)`.

**Exit criteria:**
- Config uses `pages:`; the build compiles and the panel renders identically to
  today (rename is behaviour-neutral).
- Bottom bar shows `◀ … [🏠 ⚙] … ▶` with Home + gear centered as a pair and a
  visible gap between them.
- Tapping Home jumps to the first page from anywhere; gear still opens settings;
  `◀`/`▶` still step ±1 with wrap-around.

**Risks / unknowns:**
- The rename is wide (~48 cpp hits); it must be complete or it won't compile —
  a single sweep plus a build catches any miss.
- Centered-pair offsets are hardcoded `±44`; verify the 32 px gap feels right
  on-device and tune if cross-taps still happen.

---

### Phase E7 — Light detail modal: real brightness, no fake 100%

**Status:** ✅ done · target tag: `e7-light-brightness`

**Goal:** The light detail modal shows the light's *actual* brightness instead
of seeding the slider at a misleading 100%. An on, dimmable light shows its true
level instantly; an off / non-dimmable light shows an honest placeholder, never
a number that reads as current state.

**Root cause:** [build_detail_light_](components/ha_panel/ha_panel.cpp#L1909-L1913)
defaults `cur_pct = 100` whenever the `brightness` attribute is absent:

```cpp
int cur_pct = 100;
int b_raw = this->get_attr_int_(entity_idx, "brightness", -1);
if (b_raw >= 0)
  cur_pct = (b_raw * 100 + 127) / 255;
```

Persistent attribute subscriptions are deferred at setup
([ha_panel.cpp:256-276](components/ha_panel/ha_panel.cpp#L256-L276)), so
`brightness` is absent for **every** light on open — off lights (HA omits
`brightness` when off) *and* on lights alike — and the slider always seeds 100%,
reading as "current = 100%." The fully-correct lazy fetch was tried and parked:
re-arming ESPHome's per-client `state_subs_at_` cursor re-walks all ~88 state
subs at one msg/tick ≈ 3 s per modal open
([ha_panel.cpp:1741-1820](components/ha_panel/ha_panel.cpp#L1741-L1820)) — the
"couple seconds" lag. Subscribing *all* attrs at connect was the original
failure: ~6 attrs × ~30 lights ≈ 180 extra subs bursted at connect → `Buffer
full` → HA disconnect → silent service-call loss.

**Approach (A + D): subscribe `brightness` only, at connect; honest placeholder for the rest.**

*A — brightness-only connect-time subscription (truth, no burst, no re-arm):*
- The flood was the **full** attr set bursted. Subscribing only `brightness` for
  lights ≈ 30 extra subs, folded into the existing connect-time state-sub loop
  ([ha_panel.cpp:253-254](components/ha_panel/ha_panel.cpp#L253-L254)) → ~88 →
  ~118 total, well under the ~278 that broke iter 1. Because it rides the
  initial cursor walk, there is **no re-arm and no per-open latency** — the value
  is cached before the user ever opens a modal.
- In `setup()`, after the state-sub loop, call `subscribe_attr_(idx,
  "brightness")` for each `light` entity. Leave `ensure_attrs_subscribed_` /
  `request_detail_attrs_` untouched for the other per-domain attrs
  (color_temp, ranges) — those stay lazy-or-default; they are not the misleading
  part.

*D — honest placeholder (zero extra cost, covers the gaps A can't):*
- In `build_detail_light_`, drop the `cur_pct = 100` default. When `brightness`
  is absent (`b_raw < 0`): render the slider **disabled** (`LV_STATE_DISABLED`,
  greyed) and set the value label to `"—"` / `"Off"`, never `"100 %"`.
- Power-toggle reveal: flipping the Power switch **on** enables the brightness
  slider, seeded from the last-known cached brightness (below) or a neutral
  default — at which point the user is clearly setting a *target*, not reading a
  current value.
- The A subscription means an **on, dimmable** light almost always has a cached
  brightness by first open → slider shows truth instantly. The disabled
  placeholder only appears for: an **off** light (HA omits `brightness` even with
  the sub), a **non-dimmable** light (no `brightness` attr ever), or the brief
  window before the first value lands.

*Last-known brightness cache (small, makes the off→on seed honest-ish):*
- Retain the last non-null `brightness` seen per light (in `Entity` or a parallel
  map). Seed the toggle-on case from it instead of a blind default. Optional
  within this phase, but cheap and removes the last guess.

**Live-update note:** v1 does not live-refresh an already-open modal; the value
lands by next open. With the connect-time sub it's cached before first open in
practice, so this is invisible.

**Step 0 — validation gate (HARD STOP for explicit user acceptance).** Before
any UI work, prototype *only* the A subscription (connect-time `brightness` sub
for lights), flash to the real device against the real entity list, and **stop**.
The user runs the test scenario and must **explicitly accept** the result before
any further E7 work begins. No D/UI tasks, no commit past the prototype, until
that sign-off.

Test scenario the user runs:
1. Cold-boot the panel; watch the connect log. **Pass:** no `Buffer full, ping
   queued` and no `Home Assistant … is unresponsive; disconnecting`.
2. Note the connect-time subscription count vs. the prior ~88 baseline.
3. Tap an entity to confirm service calls still fire (regression check — iter 1's
   real symptom was silent service-call loss, not just the log warnings).
4. Open the detail modal for an **on, dimmable** light and confirm a real
   brightness value arrived in `Entity::attrs` (logged), not a default.

Outcome → next move:
- **Accepted (headroom holds, calls work):** proceed to the D/UI tasks.
- **Rejected (buffer saturates or calls drop):** abandon A; switch to the HA-side
  batch-sensor fallback (see Risks) before building further.

Tasks:
- [x] **Step 0:** build/flash the brightness-sub prototype only, then **STOP** for
      user testing. **Accepted** on-device 2026-05-30: 33 light brightness subs
      (~88→~121 total), no `Buffer full`/disconnect, on dimmable lights cached
      real values (bill_s_lamp=255, office_*=222), off lights report `None`.
- [x] `setup()`: subscribe `brightness` for every `light` entity after the
      state-sub loop. Measured: 33 subs, no buffer saturation on the real list.
- [x] `build_detail_light_`: remove the `cur_pct = 100` default; when brightness
      absent, render the slider disabled + `"Off"`/`"--"` label (ASCII — no
      em-dash glyph in montserrat_18) instead of a fake percent.
- [x] Power-toggle-on enables the brightness slider, seeded from last-known cache
      or a neutral 50% default (`on_detail_light_switch_`); `apply_detail_` gates
      `brightness_pct` on the new `dw_brightness_known_` flag.
- [x] Retain last non-null brightness per light (`Entity::last_bri_pct`, set in
      `on_attr_`) to seed the off→on case.
- [x] Update Out-of-scope to carve out the brightness-only connect-time sub.

**Exit criteria:**
- Opening the modal for an **on, dimmable** light shows its actual brightness %,
  not 100%, with no perceptible delay.
- Opening it for an **off** light shows a disabled / neutral brightness (no
  misleading number); toggling Power on enables the slider.
- A **non-dimmable** light shows no editable fake-100 brightness.
- Connect-time logs show no `Buffer full` / unresponsive-disconnect — the sub
  count stays within buffer headroom.

**Risks / unknowns:**
- **Sub-count headroom.** ~118 vs ~278 is a wide margin, but verify with the
  real (not dev) entity list — a much larger install narrows it. Fallback if a
  big install pushes the connect burst back toward the ceiling: a HA-side
  template/batch sensor exposing all light brightnesses as attributes, subscribed
  as **one** entity at connect (the [ha_panel.cpp:1750-1752](components/ha_panel/ha_panel.cpp#L1750-L1752)
  comment's own suggestion).
- Off lights never report `brightness` — the toggle-on seed is last-known /
  neutral, a target not live truth. Acceptable.
- This phase intentionally reverses the "no new subscriptions" rule, narrowly
  (one attr, lights only, connect-time). See updated Out-of-scope.

---

### Phase E8 — Per-entity component size (small / medium / large)

**Status:** ✅ done — verified on-device 2026-05-30 (full scale — 3 MDI re-bakes
+ montserrat 24/32) · target tag: `e8-entity-size`

**Goal:** Let a user make individual entity rows bigger for easier reading. A
new optional `size:` node on each entity selects `small` (default, today's
look), `medium`, or `large`. Size scales the whole row together — height, name
font, icon glyph, and the right-side widget — not just the row height.

#### Motivation / context

Every entity row is rendered at a fixed 60 px height with `montserrat_18`
([make_entity_row](components/ha_panel/ha_panel.cpp#L536)), regardless of
importance or how far away the panel sits. A nightstand or hallway panel viewed
from across the room benefits from a few large, glanceable rows (e.g. the room's
main light) while keeping the rest compact. Today there's no way to express
"make this one bigger."

#### Schema (`ha-entities.yaml`)

- Add an optional `size:` node to the entity schema in
  [\_\_init\_\_.py](components/ha_panel/__init__.py#L48), validated as a **strict
  enum** `cv.one_of("small", "medium", "large", lower=True)`. Default `small`.
  An unrecognised value is a **compile-time error** (same strictness intent as
  `confirm`/`icon` validation).
- Example:
  ```yaml
  - name: "Foyer"
    entities:
      - entity_id: light.foyer_1
        friendly_name: "Foyer Lamp 1"
        size: "medium"
      - entity_id: light.foyer_2
        friendly_name: "Foyer Lamp 2"
        size: "large"
      - entity_id: light.foyer_3
        friendly_name: "Foyer Lamp 3"   # no size → defaults to "small"
  ```
- Update the header comment block in
  [ha-entities.example.yaml](packages/ha-entities.example.yaml) documenting the
  three values, the small default, and the flash-cost note (below).

#### Data model + codegen

- Add an `EntitySize { SMALL, MEDIUM, LARGE }` enum and a
  `size{EntitySize::SMALL}` field to `struct Entity`
  ([ha_panel.h:31](components/ha_panel/ha_panel.h#L31)).
- Extend `add_entity(...)` with a trailing `size` argument (defaulted to keep the
  programmatic `tap()`/test callers source-compatible); `to_code` maps the YAML
  string → enum and passes it through.

#### Dimensions (full scale)

`small` is exactly today's render — zero visual change for existing configs.

| size   | row height | name font        | icon glyph (MDI) | switch widget | left insets (icon / name) |
|--------|-----------:|------------------|------------------|---------------|---------------------------|
| small  | 60 px      | `montserrat_18`  | 24 px            | 50 × 26       | +12 / +48 (or +12 no-icon)|
| medium | 84 px      | `montserrat_24`  | 36 px            | ~66 × 34      | scaled ∝                  |
| large  | 108 px     | `montserrat_32`  | 48 px            | ~84 × 44      | scaled ∝                  |

The right-side state/chevron/lock/action labels reuse the row's name font so
they grow with it. The unavailable-overlay label likewise tracks the row font.
The per-size insets/widget dimensions are codified in a small lookup (struct or
`switch`) read by `make_entity_row`, replacing today's hard-coded `60`,
`&lv_font_montserrat_18`, `48/12`, `240/280`, `50×26`, `-16/-12` constants.

#### Fonts (the real cost — read before building)

Full scale needs glyphs that don't exist in the firmware today:

- **Name/text font:** enable `montserrat_24` and `montserrat_32` so
  `lv_font_montserrat_24/32` link, the same way `montserrat_18` is forced to
  link via `default_font` in [lvgl-ui.yaml](packages/lvgl-ui.yaml#L15). Built-in
  LVGL fonts, but each baked size costs flash.
- **MDI icon font:** the icon column draws from a single baked size-24 font
  ([mdi-font.yaml](packages/mdi-font.yaml#L17)). Crisp medium/large icons need
  the **same glyph subset re-baked at 36 px and 48 px** → two additional font
  objects (`mdi_icons_36`, `mdi_icons_48`). `ha_panel` selects the MDI font by
  row size. Teach [tools/build-mdi-glyphs.py](tools/build-mdi-glyphs.py) to emit
  all three sizes from one glyph list so they can't drift, and add a matching
  `set_mdi_font_*` setter (or a small font-by-size array passed in).
- Board has 16 MB flash ([waveshare-2.16.yaml](boards/waveshare-2.16.yaml#L46));
  five extra fonts (2 montserrat + 2 MDI re-bakes, glyph subset is small) is
  comfortably affordable, but it is the main reason this is its own phase.
- **Fallback if flash/effort is tight:** keep the icon glyph at a single baked
  size and only scale row height + name/text font (still readable; icon just
  looks relatively smaller in big rows). The enum/schema/layout work is
  identical — only the MDI re-bake step is dropped.

#### Layout impact

- Rows live in a vertical scrolling flex list inside each tile, so taller rows
  simply mean fewer visible at once + more scroll — no layout breakage, no
  column reflow.
- Mixed sizes within one area are fine; each row sizes independently.
- `rebuild_entity_row_` ([ha_panel.cpp:341](components/ha_panel/ha_panel.cpp#L341))
  already rebuilds a row in place — verify it carries the size through (it reads
  the `Entity`, so it should once the field exists).

Tasks:
- [x] `size:` enum in `__init__.py` (strict `one_of`, default small); thread
      through `to_code` → `add_entity`.
- [x] `EntitySize` enum + `Entity::size` field; extend `add_entity` signature.
- [x] Per-size dimension lookup (`RowMetrics` / `row_metrics_for`); rework
      `make_entity_row` to read row height, name font, insets, and widget size
      from it (small = current values).
- [x] Enable `montserrat_24`/`montserrat_32` linkage in `lvgl-ui.yaml` (hidden
      anchor labels).
- [x] Re-bake MDI glyphs at 36/48 px via `build-mdi-glyphs.py` (`mdi_icons_36`/
      `mdi_icons_48`); `mdi_font_medium`/`mdi_font_large` config keys +
      `set_mdi_font_medium/large` setters + size-aware selection in `ha_panel`.
- [x] Document `size:` + flash note in `ha-entities.example.yaml`.

**Exit criteria:**
- An entity with no `size:` renders byte-for-byte as it does today (small).
- `size: medium` / `size: large` produce visibly taller rows with proportionally
  larger name text, icon, and right-side widget.
- An invalid `size:` value fails the build with a clear validation error.
- Mixed sizes in one area render and scroll cleanly.

**Risks / unknowns:**
- Flash budget for the extra fonts — measure the build-size delta; fall back to
  text-only scaling (icon fixed at 24 px) if it's tighter than expected.
- Vertical centering of name vs. icon vs. widget must hold at every size; the
  `LV_ALIGN_*_MID` anchors should make this automatic but verify on-device.
- Detail modal / confirm sheet are full-screen overlays and are **out of scope**
  for E8 — `size:` only affects the area-row rendering, not the modals.

---

### Phase E9 — Read-only entity history chart sheet

**Status:** ✅ done — ring-buffer + REST backfill verified on-device 2026-05-30
· target tag: `e9-history-chart`

**REST hardening (on-device fixes, 2026-05-30):** body read into a PSRAM buffer
(internal heap fragmented → `bad_alloc` abort under repeated fetches); timeline
switched to seconds with per-mode "now" (REST=epoch, ring=uptime) so the
time-axis span is honest across windows; REST points decimated at ingestion to
`SAMPLE_CAP` (300) to bound the internal-heap sample vector (a 24 h per-minute
series is ~1440 pts).

**Scope decision (2026-05-30):** shipped the chart sheet on the in-device ring
buffer first — fully self-contained, always compiles/runs, no external deps. The
REST/`http_request` backfill (blocking TLS GET in the LVGL loop, on-device JSON
parse, needs a HA token to test) is a deliberate follow-up, not abandoned. The
no-token "since-boot samples" behaviour the plan documents is therefore the
*current* behaviour, not a degraded fallback. Docs (README + `secrets.example`)
reserve `ha_history_token` and describe it as not-yet-consumed.

**Goal:** Tapping a read-only entity (a sensor with no action) opens a
full-screen overlay sheet showing a chart of its recent values. Default window
1 h, switchable to 6 h / 24 h. Closes via an `✕` button. The chart updates in
near-real-time while open.

#### Motivation / context

Read-only entities (`sensor`, `binary_sensor`, anything that resolves to
`READ_ONLY_TEXT`) currently do nothing on tap — they only render a text badge
([ha_panel.h:28](components/ha_panel/ha_panel.h#L28)). Their one useful
interaction is "show me the trend," and there is no history view anywhere in the
panel today.

#### Data source — REST backfill with ring-buffer fallback

The ESPHome native API exposes **current state only** — there is no history
retrieval over the protocol the panel already uses (`subscribe_homeassistant_state`
pushes on change; `get_homeassistant_state` is one-shot current). So history needs
a deliberate source:

- **Primary — HA REST history (true backfill).** Add the `http_request`
  component. On sheet open, `GET /api/history/period/<start_iso>?filter_entity_id=<id>&minimal_response`
  against the HA base URL, authenticated with a **long-lived access token** from
  `!secret ha_history_token`. Parse the returned JSON array into chart points for
  the selected window.
- **Fallback — in-device ring buffer (degraded).** Every charted entity keeps a
  small fixed-cap RAM ring buffer fed from `on_state_`
  ([ha_panel.cpp:322](components/ha_panel/ha_panel.cpp#L322)). **If
  `ha_history_token` is not configured, the sheet silently uses only this
  buffer** — samples since boot, sparse (HA pushes only on change), and wiped on
  reboot and on every P8 light-sleep wake. Documented as the no-token mode, not a
  bug.
- **Live tail (both modes) — no new subscription.** The panel already holds a
  *permanent* state subscription for every entity
  ([ha_panel.cpp:254](components/ha_panel/ha_panel.cpp#L254)); live values already
  arrive in `on_state_`. While the sheet is open, each new sample for the charted
  entity is appended to the series and the chart redrawn. **No temporary
  subscription is added** — that would duplicate an existing sub and risk the
  P7d/P7e TX-saturation problem.

So with a token: REST fills history + the live tail extends it. Without a token:
ring buffer only + live tail.

#### Entities & chart types

- **Numeric `sensor`** (state parses as a number) → `lv_chart` line series.
  Y-axis auto-scaled to the window's min/max; show min/max value labels only — no
  dense grid (small screen).
- **`binary_sensor`** → an on/off **state-timeline strip** (filled bands), not a
  line.
- **Non-chartable read-only entities** (text sensors, non-numeric) → **no-op**:
  only numeric-sensor and binary_sensor rows become tappable into this sheet.
  Other read-only rows stay inert as today (no sheet, no "no history" message).
- **Routing:** in the row-tap path, a chartable read-only entity opens this sheet
  instead of the existing no-op.

#### Layout (480×480, reuses the overlay pattern)

```
┌──────────────────────────────┐
│ Living Room Temp          ✕   │  title + close (top-right)
│ 21.4 °C                       │  current value, large
│                               │
│      ╱╲      ╱╲___            │  lv_chart line (numeric)
│  ___╱  ╲___╱       ╲__        │  fills the mid-band
│                               │
│ 19.8                    23.1  │  min / max labels
│  [ 1h ]  [ 6h ]  [ 24h ]      │  window chips (segmented)
└──────────────────────────────┘
```

- Full-screen sheet built once at setup, hidden, `lv_obj_move_foreground` on
  open — same recipe as `detail_modal_` / `confirm_sheet_` / `settings_sheet_`.
- `✕` close button top-right; background tap also closes (matches the existing
  sheets).
- Window chips: a 3-button segmented row, active chip highlighted. Tapping
  re-fetches (REST) or re-windows (ring buffer) and redraws. Default 1 h.

#### Documentation

- **Root [README.md](README.md):** add a section noting that
  `ha_history_token` (a HA long-lived access token) enables true backfilled
  history in the sensor detail chart, and that without it the chart degrades to
  in-device samples since boot.
- **[secrets.example.yaml](secrets.example.yaml):** add the `ha_history_token`
  placeholder with a comment explaining what it is for and the with/without-token
  behavior (including the since-boot fallback). **Do not touch the real
  `secrets.yaml`** — the user fills that in themselves.

Tasks:
- [x] `http_request` component (`ha_http` in base.yaml) + optional `history:`
      block on `ha_panel` (`http_request_id` / `time_id` / `base_url` / `token`).
      No-history build still compiles (block is optional).
- [x] Per-entity ring buffer in `struct Entity` (`HistorySample` + `history`,
      cap `HISTORY_CAP` = 240); append in `on_state_` via `record_history_`
      with numeric parse / binary mapping (`state_to_value_`). Only chartable
      entities (`is_chartable_`) allocate one.
- [x] `build_history_sheet_` overlay (title, current value, `lv_chart`, min/max
      labels, 1h/6h/24h window chips, `✕`); `open_history_` / `close_history_` /
      `redraw_history_`. Added `-DLV_USE_CHART=1` to the board build flags.
- [x] REST fetch + JSON parse → `history_samples_` keyed on window
      (`fetch_history_` / `load_history_samples_`). Blocking GET with a
      "Loading..." paint; bounded 48 KB body read; HA timestamps mapped to the
      device millis() timeline; graceful fallback to the ring buffer on any
      failure (null/non-2xx/timeout/parse/empty). Window chip re-fetches.
- [x] Live append from `on_state_` while the sheet is open; `redraw_history_`.
- [x] Binary `state`-timeline render path (filled on/off bands in
      `history_strip_`, anchored to the last pre-window sample).
- [x] Route chartable read-only row-tap → `open_history_` in
      `on_entity_row_clicked_`; other read-only rows stay no-op.
- [x] README ("Entity history chart" section) + `secrets.example.yaml`
      `ha_history_token` placeholder (marked reserved / not-yet-consumed); left
      `secrets.yaml` alone.

**Exit criteria:**
- Tapping a numeric read-only sensor opens the sheet with a line chart, default
  1 h; chips switch to 6 h / 24 h and redraw.
- A `binary_sensor` shows an on/off timeline strip.
- With `ha_history_token` set: the chart shows backfilled history. Without it:
  the chart shows only since-boot samples — no crash, no error spam.
- The chart updates live while open as new states arrive.
- `✕` and background tap close the sheet.
- A non-chartable read-only row (text sensor) does nothing on tap, as today.

**Risks / unknowns:**
- **On-device JSON parse** of a history period can be large — use
  `minimal_response`, cap returned points, and decimate to the chart pixel width
  (~200 px). Verify memory headroom on-device.
- **TLS to HA** via `http_request` adds flash + handshake cost; confirm against
  the 16 MB board budget ([waveshare-2.16.yaml](boards/waveshare-2.16.yaml#L46)).
- REST endpoint / token failure must fall back cleanly to ring-buffer mode, not
  hang the sheet — show a brief error/empty state, keep the live tail.
- Ring-buffer mode after a P8 sleep wake is near-empty — expected and documented.
- Window re-fetch latency on 6 h / 24 h — show a brief "loading" state while the
  REST call is in flight.

---

### Phase E10 — Touch click sound (settings-gated, tap-not-swipe)

**Status:** ✅ done (verified on hardware) · target tag: `e10-touch-click-sound`

**Goal:** Optional low-volume "click" on a registered tap, off by default,
toggled in the settings sheet. Must NOT fire on a swipe/scroll.

**Hardware:** The Waveshare 2.16 has no GPIO buzzer — only an **ES8311** codec
driving the onboard speaker (mic side is ES7210, unused here). Pins verified
from waveshareteam `pin_config.h` (Mylibrary) + the `07_ES8311` example:
I2S **MCLK=42, BCLK=9, LRCLK=45, DOUT=8**; ES8311 on `bus_a` @ **0x18**;
speaker amp **PA enable = GPIO46** (held on via an `ALWAYS_ON` gpio switch).
ESPHome stack: `i2s_audio` bus + `audio_dac: es8311` + `speaker: i2s_audio`
(`dac_type: external`, `audio_dac: es8311_dac`) — the same shape as the
ESP32-S3-Box-3 reference config (same PA pin).

**Approach:**
- New persisted global `sound_on_press_g` (bool, `restore_value: yes`,
  default **false**) in [idle.yaml](packages/idle.yaml).
- **Tap vs swipe:** the board touchscreen feeds `touch_pressed` / `touch_moved`
  / `touch_released`. A press that stays within ~16 px until release is a tap →
  click on release. Any movement past the slop = swipe/scroll → silent. Decided
  on *release* (not press-down) because a swipe also begins with a press-down,
  so press-time can't yet tell them apart.
- Settings sheet gets a "Sound → Click on press" `lv_switch`, wired with the
  same stage/commit/revert + committer recipe as the sleep toggle
  (`apply_sound_`/`revert_sound_`, hooked into Apply/Cancel). `set_speaker` +
  `set_sound_committer` + `set_sound_on_press` wired in
  [ha-amoled-panel.yaml](ha-amoled-panel.yaml) `on_boot`.
- **Portability (Option C):** ha_panel is backend-agnostic. `play_click_` is the
  gate + dispatch: if a board sets `set_click_action(std::function<void()>)`,
  that owns the sound (buzzer/haptic/other codec); otherwise it falls to the
  built-in i2s-speaker path (`play_speaker_click_`, guarded by `USE_SPEAKER`).
  ha_panel only decides *when* (tap, not swipe). A future board with no speaker
  just doesn't call `set_speaker` and wires its own `set_click_action`.

**Sound design (the part that took iterating — see notes below):**
- The click is **synthesized PCM** precomputed once in `setup()` into
  `click_pcm_` (16-bit mono LE @ 16 kHz); a tap re-queues the same buffer.
- It is an **impulsive click, not a sustained tone**: ~12 ms, very fast decay
  (`tau` ≈ 1.8 ms), ≈900 Hz + ~35 % decaying noise for broadband "knock" body,
  short cosine attack/tail to start and end at exactly 0. Knobs (`freq`, `tau`,
  `noise_mix`, `peak`) are commented as ear-tunable. Final `peak` is very low
  (~0.005 full-scale) — the small speaker is quietest/cleanest there.
- The speaker runs **continuously** (`timeout: never`): underruns are filled
  with silence, so there is **no per-click start/stop** → no start/stop "pop"
  and no fast-tap unevenness; every press is the same buffer at steady gain.
  `apply_sound_` `stop()`s the bus only when the setting is turned off.
- `warm_up_speaker_()` plays ~64 ms of silence when the setting turns on (boot
  restore-on, or Apply-enable) so the ES8311 un-mute ramp is spent up front
  rather than on the user's first click (that ramp made the first press
  uniquely soft).

**Risks / unknowns:**
- PA held always-on draws a little quiescent current; acceptable on a USB/LiPo
  panel. Idle hiss is negligible (ES8311 fed digital silence).
- `USE_SPEAKER` guards keep the speaker code out of any future board that omits
  the `speaker:` block; that board's YAML must then not call `set_speaker`.

**Notes / lessons (why this isn't the obvious first design):**
- First attempts used a sustained sine + `play()`+`finish()` per tap. That
  produced an uneven "pop" (codec start/stop transient + buffer truncation) and
  faint rapid taps (re-`play()` racing the `STOPPING` state).
- `timeout: never` made it *consistent* but the bare full-gain tone buzzed the
  small speaker ("harsh"). Chasing the nicer-sounding first press was a dead end:
  that pleasant sound is the **codec start transient** (broadband/impulsive),
  reproducible per-press only by restarting (which reintroduces the unevenness).
- Resolution: stop chasing the codec ramp; make the *sample itself* an impulsive
  low-level click that the speaker reproduces cleanly. Heard on a continuously
  running stream, it is the same every press. Option B (embedded WAV via
  `media_player: speaker`) remains the fallback if a curated sample is wanted —
  same speaker, so its only edge is sample authoring, not fidelity.

---

### Phase UE1 — Build-config guard for the new widget types

**Outcome:** All five new widget types (`arc`, `scale`, `led`, `spinner`,
`roller`) now compile and link from the C++ runtime tree. Enabled via
`build_flags` in [boards/waveshare-2.16.yaml](boards/waveshare-2.16.yaml), the
same mechanism P7/E9 already use — **not** the YAML font-anchor trick the plan
floated. The anchor trick is for *fonts*; for widget *types* this project's
precedent is `-DLV_USE_*=1` build flags (ha_panel builds widgets at runtime via
the LVGL C API, so ESPHome's YAML-driven `LV_USE_*` detection strips out
anything not referenced in YAML).

**Why build_flags, not anchors:** ESPHome's `generate_lv_conf_h()` hard-defines
every unused `LV_USE_*` to `0` in the generated `lv_conf.h`, *except* macros it
sees in `build_flags` (`-DLV_USE_X=1`) — those are omitted from the header and
defined by the compiler `-D` flag instead. So a build flag is the reliable way
to force a runtime-only widget type to link. Confirmed before the change:
`LV_USE_ARC`, `LV_USE_SCALE`, `LV_USE_LED`, `LV_USE_SPINNER`, `LV_USE_ROLLER`
were all `= 0`.

**LVGL version note:** the bundled LVGL is **9.5.0**, where `lv_meter` was
removed and replaced by `lv_scale`. UE4's gauge is therefore `lv_scale`
(`LV_USE_SCALE`), not the old `lv_meter`. ESPHome has no `scale` widget type —
only a `meter` widget that emulates the old API on top of `lv_scale`.

**Dependency chain discovered the hard way (two failed links):** enabling
`LV_USE_SCALE` alone fails to compile `lv_scale.c`:
- `error: 'lv_line_class' undeclared` → the line-needle path needs
  `LV_USE_LINE=1`.
- `implicit declaration of 'lv_image_set_rotation'` → the image-needle rotation
  path needs `LV_USE_IMAGE=1`.

This matches ESPHome's own `MeterType.get_uses()` returning
`(scale, line, image, label)`. Net: `LV_USE_SCALE` pulls in `LV_USE_LINE` +
`LV_USE_IMAGE` (label was already on). Flags added:

```
-DLV_USE_ARC=1
-DLV_USE_LINE=1     # required by LV_USE_SCALE
-DLV_USE_IMAGE=1    # required by LV_USE_SCALE
-DLV_USE_SCALE=1
-DLV_USE_LED=1
-DLV_USE_SPINNER=1
-DLV_USE_ROLLER=1
```

**Verification:** clean `esphome compile` links `firmware.elf` (RAM 16.5%, Flash
19.4% — ~1.58 MB) with `lv_arc.c.o` / `lv_led.c.o` / `lv_roller.c.o` /
`lv_spinner.c.o` / `lv_scale.c.o` (+ `lv_line.c.o`, `lv_image.c.o`) all in the
object tree. No per-instance YAML declaration needed.

**Caveat (pre-existing, unrelated):** on this Windows host `esphome compile`
exits non-zero *after* a successful firmware build with
`ERROR Could not match idedata` / "Set the terminal codepage to utf-8". That is
ESPHome's post-build IDE-metadata step choking on the Windows codepage, not a
link failure — `firmware.factory.bin` and `firmware.ota.bin` are produced
regardless.

---

### Phase UE2 — Spinner for loading states

**Outcome:** Shipped loading indicators in two places: a hand-rotated arc over
the **history chart** during the REST backfill, and a per-stage `lv_spinner` on
the **boot splash**.

**History sheet — the blocking-loop puzzle, and the way through it.** First pass
concluded a history spinner was impossible: `LvglComponent::loop()` calls
`lv_timer_handler()` on the **single main loop**, and the E9 backfill
(`history_http_->get()` in [fetch_history_](components/ha_panel/ha_panel.cpp))
blocks that loop, so LVGL never ticks during the fetch — and `lv_spinner`'s
animation is driven by `lv_timer_handler`, which additionally can't be
re-entered (the fetch runs inside the open-history LVGL event). That conclusion
was too quick. Two facts make an animated indicator viable:
- ESPHome sets `lv_tick_set_cb([]{ return millis(); })`, so LVGL's clock keeps
  advancing during the block — it isn't tied to `loop()`.
- The body is read in a **chunked loop** (`container->read` + `yield()` per
  iteration), which is a place to do work mid-fetch.

So `history_spinner_` is a plain `lv_arc` (60° bright segment on a faint
track), **not** an `lv_spinner`. `spin_history_()` bumps its rotation from
`millis()` and calls `lv_refr_now(NULL)` — the same re-entrancy-safe repaint the
code already used to paint "Loading..." at
[ha_panel.cpp:3761](components/ha_panel/ha_panel.cpp#L3761). It's pumped from the
read loop on a ~30 fps gate. Raised in `load_history_samples_`, hidden at the top
of `redraw_history_` (covers success / fallback / "No data yet" / error). The
body transfer animates; only the TCP connect and the one-shot JSON parse are
brief frozen gaps. (The earlier "history spinner not viable" note was wrong and
is superseded by this.)

**Motion is stepped, not smooth — on purpose.** `spin_history_()` can only tick
when `container->read()` returns a chunk, and chunks arrive network-paced and
lumpy, so a continuous `millis()`-based angle judders (uneven jumps). It instead
advances a fixed 30° per call (12 clock-tick positions): uniform steps read as a
deliberate stepping loader at any update rate, and double as iterative-progress
feedback (one step ≈ one data burst). Smooth constant-rate motion isn't possible
while the fetch blocks the loop — the only path to that is moving the blocking
HTTP read onto a second FreeRTOS task so the main loop animates a real
`lv_spinner` at full framerate (LVGL is not thread-safe, so the worker would
touch only the socket + PSRAM buffer; parse + draw stay on the main loop).
Deferred as a complexity bump not worth it for a loading indicator; the stepped
arc was the chosen trade.

**Why not the detail modal (the plan's secondary target):** the async
attribute-fetch path (`request_detail_attrs_`, with a "Loading…" placeholder and
a 1500 ms safety timeout) is **parked dead code** — never called.
`open_detail_` builds the modal synchronously and instantly
([ha_panel.cpp:2141](components/ha_panel/ha_panel.cpp#L2141)), so there is no
live placeholder to replace.

**What shipped (boot splash):** each init stage row (`build_splash_stage_`) now
carries three indicators sharing the same right-of-text anchor:
- `spinner` (new) — 18 px `lv_spinner`, 1 s/rev, 60° arc, amber indicator
  (`0xDDAA33`) on a dim track (`0x333333`). Shown only while the stage is the
  **active** gate.
- `dot` — the amber dot, now demoted to a steady `LV_OPA_30` marker for a
  **queued** stage (a later gate not yet reached, e.g. HA before Wi-Fi is up).
- `check` — green `LV_SYMBOL_OK`, shown when the stage is **done** (unchanged).

`update_splash_stage_` switches between the three on `(done, active)`. The boot
wait progresses across loop ticks, so the spinner genuinely animates — and it is
self-driven by LVGL's anim timer, so it no longer depends on the `blink_on_`
phase the old pulsing dot used. The shared blink timer still runs for the header
status dot; re-applying splash state on each tick is idempotent.

**Verification:** clean `esphome compile` links (RAM 16.5%, Flash 19.4%). Two
on-device checks to eyeball on the panel: (1) the splash spinner during boot —
async by construction, so it ticks; (2) the history arc turning on a 1h/6h/24h
load — animates during the body read, freezes only over the TCP connect + JSON
parse. `LV_USE_SPINNER` and `LV_USE_ARC` came in with UE1.

---

### Phase UE3 — Arc dials (climate setpoint + media volume)

**Outcome:** The climate **target-temp** slider and the media-player **volume**
slider in the detail modal are now round `lv_arc` dials with the value label
centered inside the ring. Pure widget swap — no HA-side change.

**Why it's a drop-in.** `lv_arc` is value-compatible with `lv_slider`:
`lv_arc_set_range` / `lv_arc_set_value` / `lv_arc_get_value` mirror the slider
calls, and it fires the same `LV_EVENT_VALUE_CHANGED`. So the existing handlers
(`on_detail_temp_slider_`, `on_detail_volume_slider_`) and the `apply_detail_`
read path only needed `lv_slider_get_value` → `lv_arc_get_value`; the int-scaling
math for the non-integer climate step (`temp = value * step`) is reused verbatim.
Members keep their `dw_*_slider_` names (both are `lv_obj_t*`); only the comments
were retagged.

**Layout.** Each arc is a fixed 180×180 square. The detail content is a
**flex-column** whose section labels are left-aligned full-width, so a centered
arc can't just be dropped in — it would left-align too. Each dial is wrapped in a
transparent, non-scrollable holder (`LV_PCT(100)` × 190) and `lv_obj_center`-ed
inside it, isolating the centering from the labels. The value label is a **child
of the arc**, centered, so it sits in the ring's hole. The content area scrolls
(`LV_DIR_VER`) and the Apply/Cancel row is pinned separately at y=396, so the
taller dial coexists with both without collision.

**Touch + color.** Arc/indicator width is 14 px with the default draggable knob
(tinted to match) for a fingertip-sized hit target on 480×480 — flagged for
on-device tuning. The climate indicator is tinted from the existing `e.state`
HVAC-mode read: `off` = grey `0x888888`, contains `heat` = warm `0xFF7043`,
contains `cool` = blue `0x4FC3F7`, else (auto/heat_cool/dry/fan_only) = teal
`0x44CCDD`. (`heat_cool` matches `heat` first → warm; acceptable for v1.) The
volume dial uses the teal accent. `LV_USE_ARC` came in with UE1.

**Data fix found in validation — climate attrs were never loaded.** First
on-panel test showed the dial pinned at **21** and **"Current: --"** for a real
°F thermostat in `cool`. Root cause predates UE3 (the old slider had the same
bug): the panel subscribes to entity **state** only, so for climate it gets the
hvac-mode string (`"cool"` → Mode dropdown correct) but **none of the climate
attributes**. The modal built from an empty `Entity::attrs`, so `temperature`
(target) and `current_temperature` fell back — `(min_temp 7 + max_temp 35) / 2 =
21`, with the 7/35 defaults being **Celsius** while the device reports °F.

These attrs (`current_temperature`, `temperature`, `min_temp`, `max_temp`,
`target_temp_step`, `hvac_modes`) are **standard HA `ClimateEntity` schema**, not
integration custom props — fetchable by name via the native API. Fix reuses the
**E7 precedent**: subscribe them at **connect time** so they ride the initial
`state_subs` cursor walk (no re-arm) and are cached before the first modal open —
no dynamic query at open. Scoped to climate only (6 attrs × few entities),
deliberately far under the ~278-sub burst that saturated the P7d iter-1 attempt;
the P7d failure was a burst-**size** problem, not "attr subs are impossible." See
the `UE3:` log line + the connect-time block in `setup()`.

**Mode-aware setpoints — single vs dual dials.** HA splits climate setpoints by
mode: `heat`/`cool` carry one `temperature`; `auto`/`heat_cool` carry **two** —
`target_temp_low` (heat point) + `target_temp_high` (cool point), and sending the
single `temperature` param to a dual-mode entity is invalid. `lv_arc` has one
knob, so the builder constructs **two boxes** in the flex column: a single-dial
box ("Target", 180px) and a dual-dial box with two **side-by-side** 150px dials
("Heat to" warm = low, "Cool to" blue = high). Side-by-side, not stacked: stacking
pushed the cool dial below the fold and the arc traps vertical scroll, so it was
unreachable; two-across fits the 400px content width with the header visible.
Both boxes are built once; `climate_mode_is_dual_(mode)` picks which is shown and the
other gets `LV_OBJ_FLAG_HIDDEN` (a hidden flex child consumes no layout space).
The HVAC-mode dropdown's `VALUE_CHANGED` (`on_detail_hvac_mode_changed_`) re-runs
the toggle, so switching to `auto` reveals two dials live and re-tints the single
dial otherwise. The dual handlers clamp low ≤ high (drag past the other knob
pushes it). `apply_detail_` branches on the **selected** mode: dual sends
`target_temp_low`+`target_temp_high`, single sends `temperature` — same
`climate.set_temperature` service. `auto` is treated as dual per the "set a heat
point and a cool point" intent; dual dials seed from `target_temp_low/high` when
present, else a small spread around the current target. The shared 180px-dial
construction is factored into the file-static `add_setpoint_dial`.

**hvac_modes enum-repr quirk (found in validation).** Some integrations report
`hvac_modes` as Python enum reprs — the dropdown showed `<HVACMode.OFF: 'off'>`
etc. `parse_ha_list_` only strips a token's *outer* quotes, so the interior
`'off'` survived. That broke more than display: preselect (`== e.state`),
`climate_mode_is_dual_`, and the `set_hvac_mode` payload all compare against bare
`off`/`heat_cool`. `clean_hvac_mode_` pulls the value out of the first
single-quoted span (pass-through for already-bare tokens), applied to each mode
after parse.

**Setpoint "sticks ~half the time" is the cloud thermostat, not firmware
(ruled out).** `climate.thermostat_downstairs` is Honeywell **Lyric**
(`platform: lyric`), cloud-polled. HA shows the new setpoint optimistically, but
the authoritative value arrives on the next poll from Honeywell's cloud; when the
poll beats the write (or the write is throttled/dropped) the old value returns —
intermittent by timing. Verified via direct HA access: test writes returned
"state change could not be verified within timeout" and the log carried a
DNS/SSL timeout to `api.honeywellhome.com`. The panel sends a single integer
`set_temperature`, clamped to the reported 60–80 range, with no redundant
`set_hvac_mode` — i.e. correct every time; the loss is downstream. (The earlier
redundant-`set_hvac_mode` removal still stands as a correctness fix.) No
firmware change warranted; for reliability the fix is HA-side (space writes,
`homeassistant.update_entity` to force a poll, or a permanent hold in the
Honeywell app).

**Page row shows current temperature.** Climate is a `SUMMARY_TEXT` render class
that previously displayed only the hvac-mode word (`"cool"`). The row now renders
`"<mode>  <current_temperature>°"` (e.g. `cool  77°`), reading the same
connect-time `current_temperature` attr. `on_attr_` re-runs `rebuild_entity_row_`
for climate when that attr arrives/changes, so the row is correct on connect and
tracks the live reading. (`°` = U+00B0, present in LVGL's built-in Montserrat.)

**Tap opens the modal, not only long-press.** `SUMMARY_TEXT` domains (climate,
media_player, number, select) had no inline tap action — `tap_entity_` no-ops
for them. `on_entity_row_clicked_` now opens the detail modal directly for any
`SUMMARY_TEXT` entity with a detail builder, so a short tap brings up the control
surface (long-press still works; LVGL doesn't fire `SHORT_CLICKED` after a
`LONG_PRESSED`, so no double-open).

**Verification:** clean `esphome compile` links (RAM 16.5%, Flash 19.5%).
On-device checks: (1) climate modal shows a round setpoint dial colored by mode,
**reading the true target + current temp in the device's unit**, drag + Apply
calls `climate.set_temperature` unchanged; (2) switch Mode to `auto`/`heat_cool`
→ two dials (Heat to / Cool to) appear, drag + Apply sends
`target_temp_low`/`target_temp_high`; (3) climate page row reads `<mode> <temp>°`
and updates live; (4) a single **tap** (not just long-press) on a climate row
opens the modal; (5) media volume dial drag + Apply calls `media_player.volume_set`
unchanged; (6) arc-drag ergonomics on the round panel; (7) connect log shows no
`Buffer full` / unresponsive-disconnect (2 climates × 8 attrs added).

---

### Phase UE4 — Analog gauge on the sensor history sheet

**Outcome:** The numeric-sensor history sheet now carries a small round **analog
gauge** (`lv_scale` + `lv_line` needle) top-right, rendering the *current* value
as a needle angle to complement the trend chart (gauge = now, chart = history).
Zero new data — the needle reads the same value as `history_value_`. Compiles +
links clean (RAM 16.5%, Flash 19.5%, +0.1 % over UE1); **on-device validation
pending** (per the plan's no-commit-before-flash gate).

**Widget = `lv_scale`, not `lv_meter`.** LVGL 9.5 removed `lv_meter`; the v9
replacement is `lv_scale` with a separately-created needle line aimed via
`lv_scale_set_line_needle_value(scale, line, length, value)` (the line's point
array is owned/re-aimed by the scale each call — cheap to re-run on every
redraw). Round look: `LV_SCALE_MODE_ROUND_INNER`, `angle_range 270`,
`rotation 135` (low end ≈ 7-8 o'clock, the classic gauge sweep). Parts:
`LV_PART_MAIN` = arc track, `LV_PART_ITEMS` = minor ticks, `LV_PART_INDICATOR` =
major ticks. `LV_USE_SCALE` (+ its `LV_USE_LINE`/`LV_USE_IMAGE` deps) came in
with UE1.

**Placement — the chart had to give up height.** The sheet was already full
(title + value + 432×230 chart + time row + chips). The top band above the chart
(< y96) is only ~50 px tall and the close ✕ owns the top-right corner — too
small for a readable dial. So the **chart was shortened 230→176 px** (top
y96→150, bottom unchanged at 326) and the 96×96 gauge dropped into the freed
band at `TOP_RIGHT (-24, 48)` — just below the ✕ (4 px gap) and above the chart
(6 px gap), with the big value label balancing it on the left. The chart stays
the dominant element (432 wide). The UE2 loading arc and the binary on/off strip
were re-centered/resized to the new 176 px chart footprint (`strip_h` 230→176,
spinner align y 181→208).

**Range tracks the window, needle floats.** The chart already computes
`vmin`/`vmax` for its Y axis; the gauge reuses them, **padded ±15 %** so the
current value (which is itself one of the samples, often the extreme) doesn't pin
to a dial end. A flat/degenerate series gets a synthetic span. Values are scaled
**×10 internally** for one-decimal needle resolution (a 0.2→0.4 kW band would
collapse to integers otherwise); tick **labels are off**
(`lv_scale_set_label_show(false)`), so the scaled ints never surface — the
numbers already live in `history_value_` (current) and `history_range_label_`
(min–max), and rotated labels on a 96 px dial would only clutter it.

**Driven from the existing redraw path.** `update_history_gauge_(vmin, vmax,
current)` is called at the tail of `redraw_history_`'s numeric branch, which runs
on open *and* on every live-tail append — so the needle tracks live with no new
timer. `current` prefers the live state (`state_to_value_`, matching the big
label) and falls back to the freshest in-window sample. The gauge is
**numeric-only**: explicitly hidden in the `binary_sensor` strip branch and the
no-data branch, shown only once a numeric series is drawn (default-hidden at
build, so it never flashes during the blocking fetch).

**Colored zones — deferred.** The plan floated optional green→amber→red sections.
Skipped for v1: the color semantics aren't universal (high temp = bad, high
battery = good, high humidity = neutral), and v1 is config-free. Needle + ticks
on the panel's teal accent (`0x44CCDD`) only. Revisit if a per-entity
direction/threshold hint ever lands.

**Verification:** clean `esphome compile` links `firmware.elf` + factory/OTA
bins. On-device checks to eyeball on the 480×480 panel: (1) tap a numeric sensor
→ a round gauge appears top-right with the needle at the current reading,
alongside the (now shorter) trend chart; (2) the needle moves on live updates and
on 1h/6h/24h window changes; (3) the needle isn't pinned to an end for a
narrow-band sensor (padding works); (4) tap a `binary_sensor` → no gauge, just
the on/off strip; (5) the gauge + chart + time row + chips all fit without
overlap or corner-clip.

---

### Phase UE5 — Glowing LED on binary_sensor rows

**Outcome:** `binary_sensor` rows now carry a small **glowing `lv_led` dot** at
the far-right of the row, driven by the on/off state (green glow = on, dim grey
ember = off, red = unavailable). Pure on-device polish — same `e.state` the row
text already reads, no new HA data. Compiles + links clean (RAM 16.5%, Flash
19.5%, +448 B over UE4); **on-device validation pending** (no-commit-before-flash
gate).

**Render class.** binary_sensor maps to `RenderClass::READ_ONLY_TEXT` (the shared
lock/cover/summary/read-only arm of both `make_entity_row` and
`rebuild_entity_row_`). The LED is built once per row in `make_entity_row` (gated
on `e.domain == "binary_sensor"`) and **driven from state** in the
`rebuild_entity_row_` READ_ONLY_TEXT arm — same path that already recolours the
row text, so no new timer and live updates ride the existing rebuild.

**Layout — dot rightmost, word beside it.** The on/off word was kept (colour-blind
redundancy + it was already there) and the dot added at the far-right slot
(`LV_ALIGN_RIGHT_MID, label_x`). The word is re-anchored to
`label_x - led_sz - 8` so it sits *left* of the fixed dot and can never overlap
it regardless of "on" vs "off" width. LED size scales with the row:
`m.height / 4` → 13 / 16 / 20 px for small / medium / large. Other render classes
are untouched — `make_entity_row` sets `*out_led = nullptr` and only the binary
arm fills it, so `leds_by_entity_[ei]` is nullptr everywhere else and the rebuild
drive is a no-op for them.

**Colour + brightness.** Colour reuses the arm's existing `col` (on = green
`0x66BB66`, off = grey `0x888888`, unavailable/unknown = red `0xCC4444` — the
panel's standard convention), so the dot and the word always agree. Brightness
carries the "glow": `lv_led_set_brightness` = 255 when on (full), 60 when off (a
dim ember rather than fully dark, so the dot is still locatable), 160 when
unavailable. `LV_USE_LED` came in with UE1.

**Plumbing.** One new per-entity handle vector `leds_by_entity_` (mirrors
`widgets_/icons_/unavail_labels_by_entity_`): `.assign(n, nullptr)` alongside the
others in `setup()`, filled at the row-build call site via a new `out_led`
out-param on `make_entity_row`, read back in `rebuild_entity_row_`.

**Verification:** clean `esphome compile` links `firmware.elf` + factory/OTA bins.
On-device checks to eyeball: (1) door/motion/window binary sensors show a green
glowing dot when active, a dim grey dot when clear; (2) the dot tracks live state
changes; (3) an unavailable binary sensor shows a red dot; (4) the on/off word
and dot don't overlap at any row size; (5) non-binary rows (switches, sensors,
climate, etc.) are visually unchanged.

---

### Phase UE6 — History fetch on a core-pinned worker task

**Outcome:** moved the E9 history backfill (the blocking HTTP GET + JSON parse)
off the main loop onto a **persistent FreeRTOS worker task pinned to core 0**.
loopTask/LVGL stay on core 1 and never stall, which fixes the crash and lets the
loader become a real `lv_spinner`. **Validated on-device** — but only after
freeing internal RAM (the worker's task stack had nowhere to live); see the
"memory wall" addendum below.

**The crash this fixes.** `http_request->get()` blocks the main loop while HA
computes a `/api/history/period` query; on 6h/24h windows that ran several
seconds and the default ~5 s task WDT rebooted the panel (`task_wdt: loopTask
(CPU 1) did not reset`). A committed band-aid (raise `CONFIG_ESP_TASK_WDT_TIMEOUT_S`
to 15 s + a `feed_wdt()` before the parse) stopped the crash but left the UI
frozen for up to the 8 s http timeout, and the UE2 "spinner" was a hand-rotated
`lv_arc` precisely because the loop was blocked. UE6 is the proper fix.

**Why the split is safe.** LVGL is single-threaded — every `lv_*` call must stay
on loopTask. The blocking work touches *no* LVGL: only the socket, a PSRAM body
buffer, and a staging `vector<HistorySample>`. So the worker owns exactly those
three and nothing else (no `entities_`, no widgets); the main thread owns all
drawing. Core 0 for the worker because loopTask/LVGL render on core 1 — no
contention.

**Handshake.** `dispatch_history_fetch_()` (main) builds the URL, sets
`hist_req_url_`/`hist_req_is_binary_`/`hist_req_seq_`, stores `hist_fetch_state_ =
HIST_RUNNING` with **release**, and gives `hist_req_sem_`. The worker
(`history_task_trampoline_` → `run_history_fetch_`) wakes, fetches into
`hist_staging_`, and stores `HIST_DONE_OK`/`FAIL` with **release**.
`poll_history_fetch_()` (main, from the new `loop()` override) loads the flag with
**acquire** — that release/acquire pair is the only barrier needed — then
`swap()`s staging into `history_samples_` and redraws. No mutex: the main thread
reads staging only after `DONE`, by which point the worker is parked back on the
semaphore. The semaphore give/take is itself a full barrier, so the worker sees
the request fields written before it.

**Supersede + cancellation.** Each user action (open, window chip) bumps
`hist_seq_want_`. On completion, `hist_req_seq_ != hist_seq_want_` means a newer
request superseded this one → the result is dropped (no redraw) and the pending
request is re-dispatched. A request that arrives while the worker is busy isn't
lost: `dispatch_*` early-returns on `HIST_RUNNING` leaving `hist_want_pending_`,
and `poll_*` re-dispatches after consuming the stale result. Redraw also guards
on the sheet still being open on the same entity, and the live-tail in `on_state_`
skips its redraw while a fetch is `HIST_RUNNING` (so it can't hide the spinner or
paint the soon-to-be-replaced samples).

**Spinner.** `history_spinner_` is now a real `lv_spinner` (1 s/rev, 60° sweep),
self-animated by `lv_timer_handler` because the loop runs during the fetch.
Removed `spin_history_()`, `history_spin_step_`, and the read-loop
`lv_refr_now`/`feed_wdt` pumping that the blocked-loop workaround needed.

**WDT backstop lowered 15 s → 10 s.** With loopTask never blocked, the default
would even suffice; 10 s comfortably covers the worker briefly starving core-0's
idle task during the one-shot `parse_json` (the chunked read loop `yield()`s, so
the streaming phase doesn't).

**Gotchas handled / noted.** Task stack is 16 KB — fine for this LAN `http` HA URL
(no TLS); a **https** base URL pulls in mbedTLS and needs ~16–20 KB+ (noted in the
board YAML comment). Only history uses `ha_http`, so the client has a single
consumer — no locking. Risk to confirm on-device: that ESPHome's `http_request`
has no implicit main-task affinity when driven from the worker.

**The memory wall (found on-device).** First flash with the worker created
eagerly in `setup()` **bricked WiFi**: `abort()` in `wifi_process_event_` →
`std::vector<WiFiScanResult>::reserve` → `operator new` → `bad_alloc`. The 16 KB
task stack (internal RAM) had shrunk the free internal heap below what the WiFi
scan allocator needed. Two follow-ups:
- **Lazy creation + sync fallback.** The worker is now created on the *first
  history open* (`ensure_history_worker_`), not at boot — so the boot/WiFi-connect
  window keeps its heap. Stack cut 16 KB → 8 KB. If creation still fails (low
  heap), `run_history_fetch_sync_()` runs the fetch on the main loop (blocking,
  WDT-covered) so history degrades gracefully instead of breaking.
- **The real disease: internal SRAM exhaustion.** Even lazily, the 8 KB task
  wouldn't create, and `esp_http_client` failed with `HTTP -1`. A heap probe
  (`heap_caps_get_*`, kept as a diagnostic) showed **~13 KB free internal, largest
  block 7.7 KB** at runtime. Root cause: `lvgl: buffer_size: 25%` allocates a
  ~115 KB draw buffer in **internal DMA RAM** (PSRAM isn't used for it). Dropping
  it to **10%** (~46 KB) freed ~69 KB — after which the worker creates, the async
  path runs, `HTTP -1` is gone, and a 24 h / 287-point / 97 KB fetch loads clean.
  This is committed separately (it's a device-wide fix, not UE6-specific).

**Flat-sensor anchor (found on-device).** A steady sensor (pool temp pinned at
84°) intermittently showed "No data yet". HA's `significant_changes_only` returns
~1 boundary point whose timestamp sits at the window start; `redraw_history_`
recomputes `cutoff` from a slightly later `now` (the async fetch adds latency), so
that lone sample drifted just *outside* the window and was filtered out.
`redraw_history_`'s numeric branch now carries an **anchor** — the last value
before the cutoff is prepended as the line's starting level (mirroring the binary
strip), and a 1-point series is duplicated into a flat 2-point line. A flat sensor
now always renders a flat line, fixing both the intermittency and a pre-existing
"single value → no line at all" gap.

**Verification (on-device, passed):** worker logs `history worker task started on
core 0` and fetches run on the `[ha_hist]` task; 1h/6h/24h cycle repeatedly across
many entities with **no reboot**; the real `lv_spinner` turns during the fetch;
24 h / 287-point fetches load; flat sensors show a flat line every open; no
`HTTP -1`, no `bad_alloc`. The earlier band-aid WDT was lowered 15 s → 10 s.

---

### Phase UE10 — User-editable idle timeouts + reset sources + burn-in guard

**Status:** code complete; compile + on-device validation pending.

**Outcome:** the dim / blank / sleep idle timings, baked into compile-time
substitutions before, are now **runtime-editable + persisted** from the settings
sheet, the user can pick which inputs (touch / motion) reset the idle timer, and
an AMOLED-only **burn-in warning** fires on Apply when all screen protection is
turned off. Config/infra, like UE6 — no widget swap, no HA-side change.

**Part A — timeouts as globals.** Three `restore_value: yes` `uint16_t` globals
(`dim_timeout_g` / `blank_timeout_g` / `sleep_timeout_g`, seconds) replace the
`${dim/blank/sleep_timeout_s}` reads in the idle interval lambda. Each
`initial_value` is seeded from the old substitution so a fresh flash is
byte-identical to before; `blank_timeout_g` is seeded `${dim_timeout_s} +
${blank_timeout_s}` = 45 and now stores the **TOTAL** no-input time to blank (the
old additive `blank_at = dim_at + …` is gone), so editing dim no longer shifts
blank. `0` disables a tier (dim/blank). The tick evaluates tiers **deepest-first**
(`s==2`→sleep, else blank, else dim) so a disabled dim jumps active→blank cleanly.
Sleep still enters only from the blank tier (`s==2`), preserving P8 exactly.

**Part B — editable controls.** The old read-only "Dim after 15 s / Blank after
45 s total" label is replaced by three labeled **5 s-step sliders** (0–600 s) with
live seconds labels ("Never" at 0 for dim/blank; sleep min 5 s). Sliders work in
units of 5 s (`value * 5 = seconds`) — a coarse step that's usable on the round
panel. Staged like brightness/sleep (`staged_*` + `timeouts_dirty_` +
`apply_timeouts_`/`revert_timeouts_`), wired into the existing settings
Apply/Cancel. `apply_timeouts_` **clamps** `dim ≤ blank ≤ sleep` on commit
(skipping disabled tiers) and reflects the clamp back into the sliders so Cancel
reverts to the committed state. The "Sleep after" slider greys with the mode
dropdown when "Sleep when idle" is off (`update_sleep_mode_enabled_` extended).

**Part C — reset sources.** Two persisted bool globals (`reset_on_touch_g` /
`reset_on_motion_g`, default true) gate the two `notify_input` call sites (touch
`on_touch`, IMU `on_press`) by wrapping each `script.execute` in an `if:` lambda.
Two settings switches ("Touch resets screen" / "Motion resets screen") + a caption
edit them via `set_reset_sources_committer` / `set_reset_sources`. The switches are
grouped *before* the dim/blank sliders because they **gate** them (see below).
**Scope boundary:** this gates only the dim/blank idle timer, NOT sleep-wake — a
touch still wakes the panel from the sleep tier via the GPIO11 INT / deep-sleep
wake pin even with "Touch resets screen" off. Called out in the UI caption + the
YAML comments so it doesn't read as a bug. (Note: the touch click-sound gesture
tracking still runs unconditionally — only the idle reset is gated.)

**Reset sources gate dim/blank (stuck-screen fix).** A dimmed/blanked screen can
only recover via `notify_input` (or sleep-wake). With *both* reset sources off,
`notify_input` never fires — so a config that still dimmed/blanked would strand
the screen black until reboot. Fix: dim/blank now **only apply when ≥1 reset
source is on**. The idle interval early-returns when neither is set, forcing the
screen back to active (`enter_active`) if a prior state left it dimmed/blanked and
suppressing all tiers. In the UI, `update_timeouts_enabled_` greys the dim + blank
sliders whenever neither reset switch is staged on, and the burn-in trigger
(`staged_protection_all_off_`) now returns true for "both resets off" too — since
the screen then sits at full brightness forever (sleep is unreachable without the
blank tier). So the previously-possible stuck-black screen is gone, and the
always-on screen it's replaced with is caught by the AMOLED warning.

**Part D — burn-in guard.** New `is_amoled_` flag + `set_is_amoled`, fed from a
board `${is_amoled}` substitution (waveshare = `"true"`). On Apply, if the screen
would sit at full brightness forever — (dim==0 && blank==0 && sleep off) **or**
both reset sources off — **and** `is_amoled_`, `open_burnin_warning_` raises the
existing `confirm_sheet_` with a
warning + a "Proceed anyway" button (`on_burnin_proceed_` → `commit_settings_`);
the sheet's own Cancel / bg-tap returns to the still-open settings sheet,
uncommitted. Non-AMOLED boards skip it (LCD/IPS don't burn in). `commit_settings_`
centralizes the full apply sequence so both the direct-Apply and post-warning
paths commit identically.

**`is_amoled` precedence decision.** Declared **board-only** (no top-level
default). ESPHome main-file substitutions override package ones, so a top-level
`is_amoled: "false"` default would have *inverted* the board's `"true"` — the
plan's suggested top-level default was dropped. `${is_amoled}` thus resolves
unambiguously from the board; a future board that forgets it gets a clean codegen
error, and the C++ `is_amoled_{false}` default is the ultimate safe fallback.

**Known corner (documented, not fixed):** with sleep gated on the blank tier, a
config of dim=0 + blank=0 + sleep *enabled* never reaches blank, so it never
sleeps despite sleep being on. Not a regression (timeouts weren't editable
before) and not a burn-in case (the warning only fires when sleep is also off).
Keeping blank ≥ 5 s avoids it.

**Verification (pending on-device):** edit each slider and confirm the timing
takes effect live + survives reboot; toggle touch/motion reset independently and
confirm a dimmed screen un-dims only from the enabled source while a touch still
wakes it from sleep; turn **both** reset sources off and confirm the dim/blank
sliders grey out, the screen no longer dims (stays active), and the burn-in
warning pops on the AMOLED; disable all three tiers and confirm the warning pops,
Proceed commits, Cancel doesn't; confirm defaults match today's behavior with no
edits.

---

### Phase UE12 — Report rows (computed aggregates as a row type)

**Status:** code complete; compiles + links clean (RAM 16.6%, Flash 19.6%);
on-device validation pending.

**Outcome:** a page row can now be a `report:` block instead of an `entity_id:`,
rendering a **computed aggregate** over the panel's other entities — "N lights
on", totals, an offline count, or min/max/avg of a sensor group — with **no extra
HA data**. Refreshed live as states arrive; view-only (a tap fires nothing).

**Data universe = panel-known entities (plan decision A).** A report scans
`entities_` (everything listed under any page), not all of HA. Config-free, stays
inside the "no HA-side changes" rule. Counting entities you never added isn't
possible without A′ (track-only subs) / B (HA template sensor) — both deferred.

**Row model = synthetic entity (plan decision, Step 2).** A report is an `Entity`
with `domain == "report"` + a new `RenderClass::REPORT_TEXT`, carrying a
`ReportSpec` (type enum + `domains` / `match_state` / `device_class` / `unit` +
`show_total` / `show_source`). It rides the *existing* row pipeline unchanged:
`add_report` appends it like `add_entity`; `make_entity_row` builds the same
right-aligned label (REPORT_TEXT folded into the LOCK/COVER/SUMMARY/READ_ONLY
label arm); `size:` works for free. The synthetic entity is **skipped in the
state-subscription loop** (no entity_id) and **excluded from its own scan**.
Report rows get **no icon** (no "report" glyph; a fallback "?" would look broken)
and their **pressed-bg flash is disabled** so they don't read as tappable.

**Aggregation.** `recompute_reports_()` walks the report rows; `compute_report_()`
filters `entities_` by `{domains, device_class}` then computes per `type`:
- `count` / `bool` — matches over `{match_state}`; `bool` collapses to green ✓
  (0) / amber "N"; `count` optionally "matched / in-scope" via `show_total`.
- `offline` — matched entities that are `unavailable`/`unknown` (red when > 0).
- `sum` / `avg` / `min` / `max` — numeric via the existing `state_to_value_`
  (skips non-numeric/unavailable); empty match set renders an em-dash, no
  div-by-zero. `min`/`max` track the extreme entity for `show_source`. Integer
  results print without a decimal, else one place; `unit:` is suffixed.
Colour follows the UE4/UE7 trap rule: counts/numeric stay **neutral**; only
`bool`/`offline` colour (green/amber/red). Recompute runs once after the
`build_ui_` row loop (initial paint; states may read 0/"—" pre-connect) and again
from `on_state_` on **any** state change (bounded — tens of entities; per-domain
indexing is the lever if it ever bites).

**Tap = inert.** `REPORT_TEXT` is added to the `tap_entity_` switch returning
false; `on_entity_row_clicked_`'s confirm / chartable / summary branches don't
match `domain=="report"`, so a tap reaches the no-op. No long-press cb is
registered (report has no detail modal / confirm).

**Config plumbing.** `report:` is mutually exclusive with `entity_id:` per row,
enforced by `cv.has_exactly_one_key(CONF_ENTITY_ID, CONF_REPORT)`. `type` is a
strict `cv.one_of` (unknown value = compile error). `domains` / `match_state` are
lists passed to C++ **comma-joined** and re-split with `parse_ha_list_`, so
codegen emits no `std::vector` initializers — mirrors the flat `add_entity` call.

**YAML on/off gotcha (fixed).** YAML 1.1 parses bare `on`/`off` as booleans, so
`match_state: [on]` arrived as `True`. The `match_state` item validator maps
booleans back (`True`→"on", `False`→"off"), so both `[on]` and `["on"]` work; docs
recommend quoting other states.

**device_class is parsed but inert** until UE7 subscribes the attribute — a
`device_class:` filter matches nothing today (documented). v1 reports filter on
`domains` + `match_state`, which fully covers the counts ask; numeric reports over
`domains:[sensor]` mix units unless the matched set is homogeneous (call-out in
the example file).

**Scope (follow-up).** Reports defaulted to aggregating **all** entities across
every page, which surprised on a per-room page. Added `scope:` — `all` (default,
unchanged behavior) or `page` (only the report row's own page). `compute_report_`
gained a `scope_indices` param + a `for_each` candidate iterator that walks either
the passed page index list or all of `entities_`; `recompute_reports_` finds the
page holding each report row (each index lives in exactly one page) and passes its
`entity_indices` for `page` scope, `nullptr` for `all`. Under `page` scope
`show_total`'s "in-scope" denominator is naturally that page's matching entities.

**Icon (follow-up).** Added optional `icon: mdi:foo` on a report row, plumbed to
`Entity::icon_override`. `resolve_icon_` already short-circuits a report with an
empty override to "no icon"; a set override resolves through the normal
chain, so a report row now shows an icon exactly like an entity row.

**Verification (pending on-device):** add `count` (with/without `show_total`),
`bool`, `offline`, and `min`/`max` (with `show_source`/`unit`) rows; confirm each
shows the right value, updates live when an underlying entity changes, honours
`size:`, never fires a service on tap, and that pages with no report rows are
unchanged. Also: put the same `count` block with `scope: page` on two different
pages and confirm each shows its own page's total, and that `scope: all` (or
omitted) matches the whole-panel total; confirm a report `icon:` renders in the
icon column.

---

## Open decisions

- None outstanding. (Resolved: arrows wrap around; connecting state blinks
  amber with a solid-amber fallback. E8: full scale — height + name font + icon
  + widget; sizes 60/84/108 px with montserrat 18/24/32; strict-enum `size:`.)

---

## Out-of-scope

- No new HA entity subscriptions or attribute fetches (keeps the connect-time
  TX budget clean — see the P7e/P7d TX-saturation lesson in the MVP session
  notes above).
  **Exception (E7):** one connect-time `brightness` subscription per light is
  permitted — it rides the existing state-sub walk (~88 → ~118), well under the
  ~278-sub burst that saturated iter 1, with no cursor re-arm.
- No power-state / sleep-timer changes — E2 only re-reads connection state, it
  doesn't alter when the device sleeps or wakes.
- No board / pin / driver changes; all three phases are LVGL + YAML wiring.
  **Exception (E9):** adds the `http_request` component + a HA long-lived token
  for REST history backfill. This is a new outbound HTTP/TLS path, not a native-
  API subscription — it does not touch the connect-time TX budget, and absent a
  token the feature degrades to the in-device ring buffer (no HTTP at all).
- No redesign of the settings *content* (brightness/timeouts/power/about) — E1
  only relocates it from a tile to a sheet.
- Battery-icon behaviour is unchanged.
