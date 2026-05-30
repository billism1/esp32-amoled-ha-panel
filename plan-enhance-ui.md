# Build Plan — UI enhancements (post-MVP)

Three scoped UI improvements on top of the shipped MVP panel: a persistent
bottom navigation bar (with settings moved into an overlay sheet), richer
Wi-Fi / Home Assistant connection-status indicators, and a fix for the area
title overrunning its dropdown chevron.

Sibling plans: [plan-mvp.md](plan-mvp.md) (shipped baseline) ·
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
- [ ] Per-entity component size (small / medium / large) → **E5**
- [ ] Boot splash: show current connection stage → **E6**

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

**Status:** ⬜ not started · target tag: `e4-cover-state`

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
- [ ] State-line helper that maps cover `Entity::state` → label text + colour.
- [ ] Add it to the top of `build_detail_cover_`; drop the bare "Position not
      reported" branch; keep the slider gated on `current_position`.
- [ ] Mirror the state line under the cover confirm-sheet title.

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

### Phase E5 — Per-entity component size (small / medium / large)

**Status:** ⬜ not started · target tag: `e5-entity-size`

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
- [ ] `size:` enum in `__init__.py` (strict `one_of`, default small); thread
      through `to_code` → `add_entity`.
- [ ] `EntitySize` enum + `Entity::size` field; extend `add_entity` signature.
- [ ] Per-size dimension lookup; rework `make_entity_row` to read row height,
      name font, insets, and widget size from it (small = current values).
- [ ] Enable `montserrat_24`/`montserrat_32` linkage in `lvgl-ui.yaml`.
- [ ] Re-bake MDI glyphs at 36/48 px via `build-mdi-glyphs.py`; add the font
      objects + size-aware MDI font selection in `ha_panel`.
- [ ] Document `size:` + flash note in `ha-entities.example.yaml`.

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
  for E5 — `size:` only affects the area-row rendering, not the modals.

---

### Phase E6 — Boot splash: show current connection stage

**Status:** ⬜ not started · target tag: `e6-splash-stage`

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
- [ ] Add `splash_status_` member + `update_splash_status_()` declaration.
- [ ] Store the status label in `splash_status_`; set initial text via the
      helper instead of the hard-coded string.
- [ ] Implement `update_splash_status_()` (2-stage Wi-Fi → HA text, nullptr
      guard).
- [ ] Call it from `set_wifi_connected`.

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

---

## Open decisions

- None outstanding. (Resolved: arrows wrap around; connecting state blinks
  amber with a solid-amber fallback. E5: full scale — height + name font + icon
  + widget; sizes 60/84/108 px with montserrat 18/24/32; strict-enum `size:`.)

---

## Out-of-scope

- No new HA entity subscriptions or attribute fetches (keeps the connect-time
  TX budget clean — see the P7e/P7d TX-saturation lesson in plan-mvp.md).
- No power-state / sleep-timer changes — E2 only re-reads connection state, it
  doesn't alter when the device sleeps or wakes.
- No board / pin / driver changes; all three phases are LVGL + YAML wiring.
- No redesign of the settings *content* (brightness/timeouts/power/about) — E1
  only relocates it from a tile to a sheet.
- Battery-icon behaviour is unchanged.
