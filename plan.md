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
| 1 | Skeleton (Wi-Fi + HA API, no display) | ⬜ | next up |
| 2 | Display bring-up (CO5300) | ⬜ | |
| 3 | Touch bring-up (CST9220) | ⬜ | |
| 4 | Idle state machine + IMU wake | ⬜ | battery-critical, before any real UI |
| 5 | Static HA entity model (MVP) | ⬜ | |
| 6 | LVGL UI: area carousel + entity scroller | ⬜ | |
| 7 | Polish (clock, battery, settings tile) | ⬜ | |
| 8 | Multi-board support | ⬜ | |
| 9 | Dynamic discovery via HA template sensor | ⬜ | replaces P5 static YAML |

**Current focus:** Phase 1 — skeleton.
**Last updated:** 2026-05-27.

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

**Status:** ⬜ not started · target tag: `p1-skeleton`

**Goal:** board boots, connects to Wi-Fi, appears in HA, accepts OTA. No display.

Files added:
- [ ] `ha-amoled-panel.yaml` — top-level device YAML. `!include`s board + base packages.
- [ ] `boards/waveshare-2.16.yaml` — board-specific substitutions and `esp32:` block (PSRAM `mode: octal`, 16 MB flash, ESP-IDF framework). **No display/touch yet.**
- [ ] `packages/base.yaml` — `wifi:`, `api:`, `ota:`, `logger:`, `captive_portal:`, fallback AP. All values pulled from `!secret`.

Tasks:
- [ ] Pick a friendly_name + node name. Make them substitutions so they're easy to override per board.
- [ ] Add `improv_serial:` for first-flash Wi-Fi onboarding without rebuilding.
- [ ] Confirm `psram: mode: octal, speed: 80MHz` is in the board package (required by ESP32-S3R8 with 8 MB stacked PSRAM — guide §1).
- [ ] Set `api.encryption.key: !secret api_encryption_key`.

**Exit criteria:** Flash over USB, device shows up in HA with no entities, OTA from `esphome run` works wirelessly. Log shows `[psram] heap initialized` with ~8 MB free.

**Risks / unknowns:**
- ESP-IDF vs Arduino framework choice — go ESP-IDF (LVGL + mipi_spi need it, per guide §4).

---

## Phase 2 — Bring up display

**Status:** ⬜ not started · target tag: `p2-display`

**Goal:** AMOLED lights up with a solid colour or test pattern.

Files added to `boards/waveshare-2.16.yaml`:
- [ ] `spi:` block (QSPI, `type: quad`, clk + 4 data pins)
- [ ] `display:` block (`platform: mipi_spi`, `model: CO5300`, dimensions 480×480, `offset_width: 6` for the known green-edge bug, `auto_clear_enabled: false`)

Tasks:
- [ ] **Verify pins against the Waveshare 2.16" schematic.** The 1.75" pins in the guide are a starting point only — board revisions differ. Cross-check by flashing Waveshare's sample Arduino code first if any pin is ambiguous.
- [ ] Confirm ESPHome version >= the one that closed [#15765](https://github.com/esphome/esphome/issues/15765); leave `offset_width: 6` as belt-and-suspenders.
- [ ] Add a `homeassistant.event` log when the display draws its first frame, so we can confirm bring-up over the HA log without USB.
- [ ] Add a single LVGL page with a `lv_label` "Hello" so we know the framebuffer is wired up.

**Exit criteria:** Panel shows "Hello" centred. No green edge line. No crash log on boot.

**Risks / unknowns:**
- CO5300 init sequence quirks — the guide notes runtime chip-ID detection logic landed upstream; if the display stays black, log the bus mode and CS/RESET pin levels first before tweaking the init sequence.
- AMOLED brightness control: native `brightness:` on `mipi_spi` is the preferred path; the lambda `set_brightness()` approach in the guide is **unverified** (guide §4 explicit warning). Use the YAML key, not the lambda, until confirmed.

---

## Phase 3 — Bring up touch

**Status:** ⬜ not started · target tag: `p3-touch`

**Goal:** Touches are logged with correct (x, y) coordinates.

Files added to `boards/waveshare-2.16.yaml`:
- [ ] `external_components:` pulling a `cst9217` driver (community fork — pin to a specific commit SHA).
- [ ] `touchscreen:` block bound to the display, with `interrupt_pin` + `reset_pin`.

Tasks:
- [ ] Try the `shelson/esphome-cst9217` fork first. CST9220 register layout is close enough that a `cst9217` driver often works (guide §3). If not: try `fuzzybear62`'s fork next; last resort write a thin external component derived from lewisxhe `SensorLib` `TouchDrvCST92xx`.
- [ ] Log raw touch events for orientation calibration. Set `transform: mirror_x/mirror_y` based on what we see.
- [ ] Confirm multi-touch / gesture events fire — needed for swipe detection in Phase 6.

**Exit criteria:** A tap in each corner logs coordinates close to (0,0), (479,0), (0,479), (479,479) after transforms.

**Risks / unknowns:**
- CST9220 may need a register tweak the cst9217 driver doesn't make. Budget a half-day spike here; if blocked, fall back to polling-style touch using the existing driver and revisit later.

---

## Phase 4 — Idle state machine + IMU wake (battery-critical)

**Status:** ⬜ not started · target tag: `p4-idle`

**Goal:** Device is usable on a LiPo for more than a few hours. Screen dims, then blanks, then wakes on touch *or* IMU motion. Wired up before any real UI so we catch power regressions in every later phase.

Files added:
- [ ] `packages/idle.yaml` — global state (`active`/`dim`/`blank`), restart-mode scripts driving transitions, brightness ramp.
- [ ] `components/qmi8658/` — custom external_component for the QMI8658 IMU (port the one from the SentientCustard 2.41" repo — same chip).
- [ ] Board package gains the `qmi8658:` block with the I²C address + interrupt pin.

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

- [ ] Bring up QMI8658 on the I²C bus. Confirm WHO_AM_I register reads expected value.
- [ ] Configure low-power "any-motion" interrupt on the QMI8658 — chip pulls its INT pin high when accel delta exceeds threshold. Use ESPHome `binary_sensor: gpio` on that interrupt pin so motion wakes the firmware without polling.
- [ ] Optional fallback: periodic `interval:` lambda reads the accel magnitude as a software fallback if the hardware interrupt path is flaky.
- [ ] Expose IMU motion as an internal `binary_sensor` — idle state machine listens to both `touchscreen.on_touch` and this sensor.

### Display blanking strategy

- "Blank" = swap to a fully-black LVGL page **and** set display brightness to 0. AMOLED black pixels emit no light and draw essentially no current, so this is true sleep for the panel.
- Do **not** put the ESP32 itself into deep sleep in v1 — losing the HA API connection on every wake would make the UX terrible. We rely on the AMOLED panel sleeping and the MCU staying in modem-sleep / light-sleep automatically courtesy of ESP-IDF.

### Battery readout (best-effort)

- AXP2101 PMIC sits on the I²C bus. No native ESPHome component, but we can read battery voltage register via a `sensor` template + I²C lambda (see guide §9).
- Show battery in the header / settings tile in Phase 7. No charge-curve mapping in v1 — just raw voltage.

**Exit criteria:**
- Untouched, motionless device dims after 15s, blanks after 45s total.
- Picking up the device wakes it before you've finished lifting it.
- Tap on a blank screen wakes it (the touch IC must remain powered).
- No noticeable lag re-entering `active` (LVGL page swap < ~100 ms).

**Risks / unknowns:**
- Touch IC must stay powered during blank to register wake-tap. Confirm CST9220 idle current is acceptable; if not, accept "wake only on motion" and document it.
- QMI8658 interrupt wiring on the 2.16" board may differ from the 2.41" reference — verify the INT pin on the schematic.
- Some QMI8658 forks expose accel data but not the any-motion hardware interrupt. If we have to poll, do it at ~10 Hz with a magnitude threshold and accept slightly higher idle current.

---

## Phase 5 — HA entity model (static YAML for MVP)

**Status:** ⬜ not started · target tag: `p5-static-entities`

**Goal:** Define how the user describes their home to the panel — **for the MVP only**. Phase 9 replaces this with HA-side dynamic config; Phase 5's job is to get something on screen fast so we can validate the UI, touch, and idle/wake stack against a real home.

Files added:
- [ ] `packages/ha-entities.yaml` — list of areas, each with an ordered list of entity IDs. **User-edited for MVP. Will be replaced by dynamic discovery in Phase 9.**

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
# packages/ha-entities.yaml — USER EDITS THIS for MVP
substitutions:
  areas: "living_room,kitchen,bedroom,office"

# Per entity, one of:
#   - homeassistant.text_sensor (for state of read-only entities)
#   - homeassistant.service template button (for toggleable entities)
```

Each entity becomes one of:
- A `text_sensor` for read-only display, subscribed to the HA entity's state.
- A `homeassistant.service` template button that calls `homeassistant.toggle` for the entity_id when pressed.

Both are generated per-entity in `ha-entities.yaml`. The UI in Phase 6 reads the list and renders tiles.

**File layout:** one flat YAML file for MVP. Split into per-area `!include`s only if it grows unwieldy.

**Exit criteria:** User can add a new entity by appending two lines to `ha-entities.yaml`, recompile, see it in HA-side logs as a subscribed state.

---

## Phase 6 — LVGL UI: area carousel + entity scroller

**Status:** ⬜ not started · target tag: `p6-ui`

**Goal:** The actual feature — horizontal swipe between areas, vertical scroll for entities within an area, tap to toggle.

Files added:
- [ ] `packages/lvgl-ui.yaml` — board-agnostic LVGL config.

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

## Phase 7 — Polish

**Status:** ⬜ not started · target tag: `p7-polish`

**Goal:** Make it pleasant to live with.

- [ ] RTC integration: `pcf85063` + `homeassistant` time sources (guide §4 example).
- [ ] Header clock that shows current time when not interacting.
- [ ] Visual feedback on tap (LVGL `lv_btn` press style — brief colour change).
- [ ] Boot splash with device + HA connection status.
- [ ] Settings tile at the end of the area carousel: brightness slider (bound to the `mipi_spi` native brightness), screensaver timeout, version info.

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

