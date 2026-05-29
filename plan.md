# Build Plan — esp32-amoled-ha-panel

Phased plan from empty repo → working **handheld, battery-powered** HA remote
on the Waveshare ESP32-S3-Touch-AMOLED-2.16, with a structure that lets us
add other AMOLED boards later by dropping in a new board package.

Device runs on a LiPo cell in a hand-held enclosure. Idle screen behaviour
(dim → blank → wake on touch or IMU motion) is a **first-class feature**,
not polish — without it the battery dies in hours.

Background reference: [docs/esp32-s3-amoled-ha-guide.md](docs/esp32-s3-amoled-ha-guide.md).

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
| 7b | Polish round 2 (header layout, battery + Wi-Fi icons, Apply/Cancel in settings) | 🟡 | code complete 2026-05-28; awaiting on-device verification |
| 7c | Entity control: explicit on/off + per-domain update operations | ⬜ | toggle already in P5/P6; round out actions for other domains where cheap, defer the rest to P7d |
| 7d | Per-entity detail/popup view (light dim/colour, climate, media, number, select) | ⬜ | long-press → shared modal with per-domain widgets |
| 8 | Multi-board support | ⬜ | |
| 9 | Dynamic discovery via HA template sensor | ⬜ | replaces P5 static YAML |

**Last updated:** 2026-05-28 (P7b code complete).

---

## Guiding principles

1. **Ship in vertical slices.** Each phase ends with something flashable that
   does *more* than the previous phase. No big-bang merges.
2. **Board package isolates hardware.** All pins, display init, touch driver
   live in `boards/<name>.yaml`. UI and HA logic never reference pins.
3. **HA areas + entities start declarative, become dynamic.** MVP (P5) reads a static YAML list; P9 swaps to a single HA template sensor that pushes JSON. Each phase ships a usable panel.
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

**Goal:** Define how the user describes their home to the panel — **for the MVP only**. Phase 9 replaces this with HA-side dynamic config; Phase 5's job is to get something on screen fast so we can validate the UI, touch, and idle/wake stack against a real home.

Files added:
- [x] `packages/ha-entities.example.yaml` — committed template (sanitized placeholder areas/entities) so cloners see the schema.
- [x] `packages/ha-entities.yaml` — **gitignored**, user-edited per-install copy of the example. Contains real entity_ids and friendly names, so it never gets pushed. Will be replaced by dynamic discovery in Phase 9.

### Why static first, dynamic later

ESPHome's native HA API has no area-registry or entity-registry calls — those live behind HA's websocket API. Dynamic discovery is therefore a real feature, not a one-liner. Splitting into two phases lets us:

1. Ship a working panel within hours, against a real home, on real hardware.
2. Lock in UI / domain-behaviour decisions before adding the runtime-LVGL + JSON-parsing complexity that dynamic discovery requires.

Option matrix considered for static-vs-dynamic in Phase 5:

| Option | Verdict |
|---|---|
| Hard-code areas + entities in YAML | ✅ MVP. Simple, compiles fast, no runtime surprises. |
| HTTP fetch `/api/states` + filter | ❌ Areas not in state objects; would need websocket. Skip. |
| HA template sensor pushes JSON via native API attribute | ✅ Chosen for Phase 9 — see below. |
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

**Status:** 🟡 code complete 2026-05-28 · target tag: `p7b-polish`

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

**Status:** ⬜ not started · target tag: `p7c-controls`

**Goal:** Two things in one phase, both row-scoped:
1. **Dispatch:** tighten the tap action per domain — explicit on/off where state is known, real services for action domains (scene/script/automation/button) and lock.
2. **Rendering:** replace the generic right-aligned text badge with a per-domain widget that matches the domain's affordance (toggle switch for binaries, play icon for action-domains, text+colour for multi-state). Modals stay in P7d.

### Already in place (P5/P6/P7a)
- Tap on a row in `light` / `switch` / `fan` / `input_boolean` / `cover` → `homeassistant.toggle`.
- Tap on `script` → `script.turn_on`. Tap on `automation` → `automation.trigger`.
- All rows render the same way: friendly_name left, state text right (colour-tinted by [`rebuild_entity_row_text_`](components/ha_panel/ha_panel.cpp#L87-L103)).

### Dispatch — in scope for P7c

- [ ] **Explicit on / off when state is known.** For binary domains (`light` / `switch` / `fan` / `input_boolean`): if `state == "on"` send `homeassistant.turn_off`; if `state == "off"` send `homeassistant.turn_on`. **Fall through to `homeassistant.toggle` for any other state** (`unavailable`, `unknown`, transient values like a light's mid-transition `transitioning`, or anything else we don't recognise). Comment the toggle fallback so the "why" is captured — it covers more than just unknown/unavailable.
- [ ] **`cover`** stays on `homeassistant.toggle`. Cover state alphabet is `open` / `closed` / `opening` / `closing` — the binary on/off mapping doesn't fit, and toggle does the right thing (HA forwards to `cover.open_cover` / `cover.close_cover` based on current position). Explicit open/close lives in the P7d position slider.
- [ ] **`lock`** → `lock.unlock` if `state == "locked"`, `lock.lock` if `state == "unlocked"`. Fall through to no-op log for any other state (`locking` / `unlocking` / `jammed` / `unavailable`) — better to do nothing than commit the wrong action mid-transition. Lock is security-sensitive; only fires on `LV_EVENT_CLICKED` (already drag-disambiguated by lv_button), never on long-press or scroll.
- [ ] **`scene`** → `scene.turn_on` on tap. Currently falls through to read-only.
- [ ] **`button`** → `button.press` on tap. Currently falls through to read-only. (User still has to list a `button.*` entity_id in `ha-entities.yaml` to surface it — codegen isn't auto-discovering.)
- [ ] **State callback** keeps populating `Entity::state` for every subscribed entity — no change. Rendering layer (next section) reads the latest `state` whenever it redraws.

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

**Status:** ⬜ not started · target tag: `p7d-detail`

**Goal:** Give domains that need more than a binary tap their own control surface. Long-press an entity row → a shared modal opens, populated by a per-domain builder. Short-tap stays as the P7c row-level action (dispatch for binaries + action domains, no-op-with-log for the P7d-bound domains); the modal is strictly opt-in via long-press.

P7c already shipped: row-level `lv_switch` for binary domains, action-icon for scene/script/automation/button, text+glyph for lock/cover, summary text for climate/media_player/number/select. P7d's job is the *modal* surface on top of that — sliders, dropdowns, transport buttons — for domains where a single tap can't communicate the user's intent.

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
  - If `rgb` is supported: leave colour-wheel as a stretch item — LVGL has no built-in colour picker, would need a custom `LV_USE_CANVAS` widget. Hold for P7e if it grows large.
  - Need to consume entity attributes (`brightness`, `color_mode`, `supported_color_modes`) — current `ha_panel` only subscribes to state, not attributes. **Add attribute subscription path** (`subscribe_homeassistant_state(cb, entity_id, "brightness")` etc) and a flat `std::map<std::string, std::string>` per entity for attribute storage.
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
- `LV_USE_FLEX=1` and `LV_USE_LABEL=1` already on from P6/P7a.

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

## Phase 8 — Multi-board support

**Status:** ⬜ not started · target tag: `p8-multiboard`

**Goal:** Adding a second AMOLED board = adding one board package, nothing else.

Tasks:
- [ ] Extract anything still board-specific from `ha-amoled-panel.yaml` into the board package.
- [ ] Add a second board: `boards/waveshare-1.75.yaml`. Same UI YAML, different pins + dimensions.
- [ ] Document the "add a new board" recipe in `README.md`.

**Exit criteria:** Switching boards by changing one `!include` line, no other edits, panel works.

**Risks / unknowns:**
- Different touch ICs across boards = different external_component for each. Make the touch component an include from the board package, not the top YAML.
- 480×480 vs 466×466 vs other sizes — LVGL layout should pull dimensions from substitutions defined in the board package.

---

## Phase 9 — Dynamic area + entity discovery (replaces static YAML)

**Status:** ⬜ not started · target tag: `p9-dynamic`

**Goal:** Move source-of-truth for areas + entities from `packages/ha-entities.yaml` into Home Assistant itself. Re-arranging a home no longer requires a firmware rebuild.

### Mechanism

Single HA template sensor exposes the full area→entity map as a JSON attribute. Device subscribes to that attribute, parses, and builds LVGL tiles at runtime.

**HA side** (lives in HA's `configuration.yaml`, not in this repo — but we'll ship a sample snippet in `docs/ha-template-sensor.yaml`):

```yaml
template:
  - sensor:
      - name: "AMOLED Panel Config"
        unique_id: amoled_panel_config
        state: "ok"
        attributes:
          # Areas in carousel order. Override by sorting via labels or a manual list.
          areas: >
            {{ areas() | map('area_name') | list | tojson }}
          # { "Living Room": ["light.lamp", "switch.fan", ...], ... }
          entities_by_area: >
            {%- set ns = namespace(out={}) -%}
            {%- for a in areas() -%}
              {%- set ents = area_entities(a)
                  | reject('match', '^(sun|zone|person|device_tracker|update)\\.')
                  | list -%}
              {%- set ns.out = dict(ns.out, **{area_name(a): ents}) -%}
            {%- endfor -%}
            {{ ns.out | tojson }}
```

User can refine the reject/include filter to taste. Optionally support a `label` ("show_on_panel") on entities and filter to only labelled ones — cleaner than blacklist.

**Device side:**

```yaml
text_sensor:
  - platform: homeassistant
    id: panel_config_json
    entity_id: sensor.amoled_panel_config
    attribute: entities_by_area
    on_value:
      - lambda: |-
          // 1. Parse x via ArduinoJson
          // 2. Diff against currently-rendered area/entity set
          // 3. Rebuild LVGL tiles via lv_obj_create / lv_label_create / lv_btn_create
          // 4. Subscribe to per-entity states for the new set
```

### Hard parts (call out so we don't kid ourselves)

1. **Runtime LVGL widget creation.** ESPHome's YAML LVGL is declarative; building tiles in a lambda means calling the underlying LVGL C API directly. Works, but examples are sparse — budget a real spike. Pre-build by Phase 6 a small lambda that creates one tile programmatically as a proof.
2. **Runtime per-entity state subscriptions.** `homeassistant.text_sensor` is declared at compile time. Workaround: declare a *pool* of N (say 64) generic subscriptions at compile time, bind each one to whichever entity_id we currently care about via the C++ `set_entity_id()` setter. Confirm that ESPHome's native API client supports re-subscribing on `set_entity_id()` change — if not, a Phase 9 blocker.
3. **JSON payload size.** Native API protobuf message limit isn't tiny but isn't infinite. A 50-entity home is fine; a 500-entity home may overflow. Filter on the HA side aggressively.
4. **Domain → behaviour map stays in firmware.** Even with dynamic entity lists, knowing that `light.*` toggles and `sensor.*` is read-only is still a compile-time table. Acceptable.

### Migration

- Keep `packages/ha-entities.yaml` schema working. Add a top-level `discovery_mode: static | dynamic` substitution. `dynamic` ignores the static file; `static` keeps the MVP path. Lets us flip per board / per install without deleting code.

**Exit criteria:**
- Adding a new HA area + light + flashing nothing → panel reflects the change within a few seconds.
- Removing an entity from HA → panel drops the tile.
- Reordering areas via the HA template → panel carousel order updates.

**Risks / unknowns:**
- Re-subscription via `set_entity_id()` at runtime is the single biggest unknown. If it doesn't work, fallback: at boot, read the JSON once, restart device with state cached, declare subscriptions on next boot via generated config — much worse UX, only as a backstop.
- LVGL teardown on reconfigure must not leak memory. Track widget pointers and delete cleanly when an entity disappears.

---

## Out-of-scope for this plan (parking lot)

- ESP32 deep sleep between interactions — would break the live HA API link; rely on AMOLED panel sleep + ESP-IDF auto modem/light sleep instead.
- AXP2101 charge-curve battery % — raw voltage only in v1.
- Audio / dual microphone / wake-word — entire vertical not addressed.
- Light brightness / colour control — toggle only in v1.
- Climate / thermostat / media transport control — read-only in v1.
- SD card asset loading for icons — embed icons at compile time instead.

---

## Open decisions (need user input before / during Phase 4–6)

1. **Idle timeouts:** proposed `dim_timeout: 15s`, `blank_timeout: 30s` (45s total to blank). Too aggressive? Too lazy?
2. **Motion sensitivity:** pick-up should wake, but a tap on the nightstand probably shouldn't. Calibrate empirically — any preference for false-wake vs missed-wake?
3. **Header content:** clock + battery, clock + battery + weather, or area name only?
4. **Tap-and-hold behaviour:** v1 ignores it. Could later expose a detail page (brightness slider for lights, set-point for climate). OK to defer?
5. **Touch driver fallback:** if no community CST9220/CST9217 fork works, are we willing to spend the time to write a small external component, or fall back to the Arduino-side touch lib via lambda?
6. **Phase 9 entity filter:** blacklist by domain (current sketch) vs. label-based opt-in (`label: show_on_panel`). Label-based is cleaner long-term but requires labelling every entity in HA.

---

## Session notes & decisions log

> Newest entry at top. Date in `YYYY-MM-DD`. One line per gotcha, decision, or surprise — anything future-you will want when picking the work back up after a few days away. Not a changelog — git log already does that. This is for *why* and *what bit me*.

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

- **Runtime LVGL widget build, not YAML.** With 15 areas × ~6 entities each, declaring tiles + rows in YAML would be a mess. `HAPanel::build_ui_()` now constructs the tree via the LVGL C API on `lv_scr_act()` once setup runs. Plan §P9 was already going to need this approach for dynamic discovery, so starting it in P6 reduces P9 risk.
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
- Decided: static YAML entity model in P5, dynamic HA-template-sensor in P9. Two-step ships fastest.
- Decided: ESP32 deep sleep **not** used — would break HA API. AMOLED panel blank is the sleep mechanism.
- Open: idle timeouts (15s dim / 30s further to blank), motion sensitivity, header content, tap-and-hold behaviour, touch driver fallback, P9 filter strategy.

<!--
### YYYY-MM-DD — phase N
- Worked on X. Hit issue Y. Workaround Z (see commit SHA).
- Decision: chose A over B because C.
- Blocker: D — waiting on E.
-->

