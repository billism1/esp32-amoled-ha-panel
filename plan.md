# Build Plan — esp32-amoled-ha-panel

Phased plan from empty repo → working **handheld, battery-powered** HA remote
on the Waveshare ESP32-S3-Touch-AMOLED-2.16, with a structure that lets us
add other AMOLED boards later by dropping in a new board package.

Device runs on a LiPo cell in a hand-held enclosure. Idle screen behaviour
(dim → blank → wake on touch or IMU motion) is a **first-class feature**,
not polish — without it the battery dies in hours.

Background reference: [docs/esp32-s3-amoled-ha-guide.md](docs/esp32-s3-amoled-ha-guide.md).

---

## Guiding principles

1. **Ship in vertical slices.** Each phase ends with something flashable that
   does *more* than the previous phase. No big-bang merges.
2. **Board package isolates hardware.** All pins, display init, touch driver
   live in `boards/<name>.yaml`. UI and HA logic never reference pins.
3. **HA areas + entities are declarative.** User edits one YAML list to
   describe their home; firmware reads that list. No runtime HA discovery
   (ESPHome can't enumerate HA areas — see Phase 3 design notes).
4. **Battery-first.** Idle dim+blank and IMU wake are in Phase 4 — before any
   real UI. Every later phase is tested with the idle state machine running,
   so we catch power regressions early.
5. **External components pinned by commit.** Touch + IMU drivers come from
   community forks; pin to a SHA so a remote rebase can't break our build.

---

## Phase 0 — Repo bootstrap ✅ (this commit)

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

**Goal:** board boots, connects to Wi-Fi, appears in HA, accepts OTA. No display.

Files added:
- `ha-amoled-panel.yaml` — top-level device YAML. `!include`s board + base packages.
- `boards/waveshare-2.16.yaml` — board-specific substitutions and `esp32:` block (PSRAM `mode: octal`, 16 MB flash, ESP-IDF framework). **No display/touch yet.**
- `packages/base.yaml` — `wifi:`, `api:`, `ota:`, `logger:`, `captive_portal:`, fallback AP. All values pulled from `!secret`.

Tasks:
1. Pick a friendly_name + node name. Make them substitutions so they're easy to override per board.
2. Add `improv_serial:` for first-flash Wi-Fi onboarding without rebuilding.
3. Confirm `psram: mode: octal, speed: 80MHz` is in the board package (required by ESP32-S3R8 with 8 MB stacked PSRAM — guide §1).
4. Set `api.encryption.key: !secret api_encryption_key`.

**Exit criteria:** Flash over USB, device shows up in HA with no entities, OTA from `esphome run` works wirelessly. Log shows `[psram] heap initialized` with ~8 MB free.

**Risks / unknowns:**
- ESP-IDF vs Arduino framework choice — go ESP-IDF (LVGL + mipi_spi need it, per guide §4).

---

## Phase 2 — Bring up display

**Goal:** AMOLED lights up with a solid colour or test pattern.

Files added to `boards/waveshare-2.16.yaml`:
- `spi:` block (QSPI, `type: quad`, clk + 4 data pins)
- `display:` block (`platform: mipi_spi`, `model: CO5300`, dimensions 480×480, `offset_width: 6` for the known green-edge bug, `auto_clear_enabled: false`)

Tasks:
1. **Verify pins against the Waveshare 2.16" schematic.** The 1.75" pins in the guide are a starting point only — board revisions differ. Cross-check by flashing Waveshare's sample Arduino code first if any pin is ambiguous.
2. Confirm ESPHome version >= the one that closed [#15765](https://github.com/esphome/esphome/issues/15765); leave `offset_width: 6` as belt-and-suspenders.
3. Add a `homeassistant.event` log when the display draws its first frame, so we can confirm bring-up over the HA log without USB.
4. Add a single LVGL page with a `lv_label` "Hello" so we know the framebuffer is wired up.

**Exit criteria:** Panel shows "Hello" centred. No green edge line. No crash log on boot.

**Risks / unknowns:**
- CO5300 init sequence quirks — the guide notes runtime chip-ID detection logic landed upstream; if the display stays black, log the bus mode and CS/RESET pin levels first before tweaking the init sequence.
- AMOLED brightness control: native `brightness:` on `mipi_spi` is the preferred path; the lambda `set_brightness()` approach in the guide is **unverified** (guide §4 explicit warning). Use the YAML key, not the lambda, until confirmed.

---

## Phase 3 — Bring up touch

**Goal:** Touches are logged with correct (x, y) coordinates.

Files added to `boards/waveshare-2.16.yaml`:
- `external_components:` pulling a `cst9217` driver (community fork — pin to a specific commit SHA).
- `touchscreen:` block bound to the display, with `interrupt_pin` + `reset_pin`.

Tasks:
1. Try the `shelson/esphome-cst9217` fork first. CST9220 register layout is close enough that a `cst9217` driver often works (guide §3). If not:
   - Try `fuzzybear62`'s fork next.
   - Last resort: write a thin external component derived from lewisxhe `SensorLib` `TouchDrvCST92xx`.
2. Log raw touch events for orientation calibration. Set `transform: mirror_x/mirror_y` based on what we see.
3. Confirm multi-touch / gesture events fire — needed for swipe detection in Phase 5.

**Exit criteria:** A tap in each corner logs coordinates close to (0,0), (479,0), (0,479), (479,479) after transforms.

**Risks / unknowns:**
- CST9220 may need a register tweak the cst9217 driver doesn't make. Budget a half-day spike here; if blocked, fall back to polling-style touch using the existing driver and revisit later.

---

## Phase 4 — Idle state machine + IMU wake (battery-critical)

**Goal:** Device is usable on a LiPo for more than a few hours. Screen dims, then blanks, then wakes on touch *or* IMU motion. Wired up before any real UI so we catch power regressions in every later phase.

Files added:
- `packages/idle.yaml` — global state (`active`/`dim`/`blank`), restart-mode scripts driving transitions, brightness ramp.
- `components/qmi8658/` — custom external_component for the QMI8658 IMU (port the one from the SentientCustard 2.41" repo — same chip).
- Board package gains the `qmi8658:` block with the I²C address + interrupt pin.

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

1. Bring up QMI8658 on the I²C bus. Confirm WHO_AM_I register reads expected value.
2. Configure low-power "any-motion" interrupt on the QMI8658 — chip pulls its INT pin high when accel delta exceeds threshold. Use ESPHome `binary_sensor: gpio` on that interrupt pin so motion wakes the firmware without polling.
3. Optionally: have a periodic `interval:` lambda read the accel magnitude and use that as a software fallback if the hardware interrupt path is flaky.
4. Expose IMU motion as an internal `binary_sensor` — idle state machine listens to both `touchscreen.on_touch` and this sensor.

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

## Phase 5 — HA entity model (declarative, in YAML)

**Goal:** Define how the user describes their home to the panel.

Files added:
- `packages/ha-entities.yaml` — list of areas, each with an ordered list of entity IDs. **This is the file the end user edits.**

### Design: why declarative, not auto-discovery

ESPHome's HA API is bidirectional for *state subscriptions* but does not
expose HA's area registry or entity registry. There's no
`homeassistant.list_areas` call. Options considered:

| Option | Verdict |
|---|---|
| Hard-code areas + entities in YAML | ✅ Chosen. Simple, compiles fast, no runtime surprises. |
| HTTP fetch HA `/api/config` at boot | ❌ Adds an HTTP client + parser; HA areas don't actually live in `/api/config` — would need the websocket API; way too much for v1. |
| `text_sensor` per "area config" key | ❌ Round-trip per entity = startup latency; still need a static list to drive the iteration. |

### Schema (substitutions + `homeassistant.` text/binary sensors)

```yaml
# packages/ha-entities.yaml — USER EDITS THIS
substitutions:
  # Areas in carousel order. Comma-separated, parsed at compile time by Jinja.
  # (We'll likely actually express this as a YAML list via !include rather than
  #  substitutions — finalised in Phase 5 implementation.)
  areas: "living_room,kitchen,bedroom,office"

# Per entity, one of:
#   - homeassistant.text_sensor (for state of read-only entities)
#   - switch (for toggleable entities — uses homeassistant.service: homeassistant.toggle)
```

Each entity becomes one of:
- A `text_sensor` for read-only display, subscribed to the HA entity's state.
- A `homeassistant.service` template button that calls `homeassistant.toggle` for the entity_id when pressed.

Both are generated per-entity in `ha-entities.yaml`. The UI in Phase 6 reads the list and renders tiles.

**Open question:** Whether to use ESPHome's `!include` of per-area YAMLs vs.
one flat file. Recommend one flat file for v1, split later if it grows.

**Exit criteria:** User can add a new entity by appending two lines to
`ha-entities.yaml`, recompile, see it in HA-side logs as a subscribed
state.

---

## Phase 6 — LVGL UI: area carousel + entity scroller

**Goal:** The actual feature — horizontal swipe between areas, vertical scroll for entities within an area, tap to toggle.

Files added:
- `packages/lvgl-ui.yaml` — board-agnostic LVGL config.

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

**Goal:** Make it pleasant to live with.

- RTC integration: `pcf85063` + `homeassistant` time sources (guide §4 example).
- Header clock that shows current time when not interacting.
- Visual feedback on tap (LVGL `lv_btn` press style — brief colour change).
- Boot splash with device + HA connection status.
- A "settings" tile at the end of the area carousel: brightness slider (bound to the `mipi_spi` native brightness), screensaver timeout, version info.

---

## Phase 8 — Multi-board support

**Goal:** Adding a second AMOLED board = adding one board package, nothing else.

Tasks:
1. Extract anything still board-specific from `ha-amoled-panel.yaml` into the board package.
2. Add a second board: `boards/waveshare-1.75.yaml`. Same UI YAML, different pins + dimensions.
3. Document the "add a new board" recipe in `README.md`.

**Exit criteria:** Switching boards by changing one `!include` line, no other edits, panel works.

**Risks / unknowns:**
- Different touch ICs across boards = different external_component for each. Make the touch component an include from the board package, not the top YAML.
- 480×480 vs 466×466 vs other sizes — LVGL layout should pull dimensions from substitutions defined in the board package.

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
3. **Area + entity definition syntax:** flat substitutions list vs. structured YAML (`!include` of per-area files). Recommend structured YAML.
4. **Header content:** clock + battery, clock + battery + weather, or area name only?
5. **Tap-and-hold behaviour:** v1 ignores it. Could later expose a detail page (brightness slider for lights, set-point for climate). OK to defer?
6. **Touch driver fallback:** if no community CST9220/CST9217 fork works, are we willing to spend the time to write a small external component, or fall back to the Arduino-side touch lib via lambda?
