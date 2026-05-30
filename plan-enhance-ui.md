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
- [x] Boot splash: show current connection stage → **E5**
- [x] Rename areas → pages + bottom-bar Home button → **E6**
- [x] Light detail modal: real brightness, no fake 100% → **E7**
- [x] Per-entity component size (small / medium / large) → **E8**
- [~] Read-only entity history chart sheet → **E9** (ring-buffer shipped; REST backfill deferred)

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

**Status:** 🟡 ring-buffer mode implemented (REST backfill deferred) · target
tag: `e9-history-chart`

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
- [ ] **DEFERRED** — `http_request` component + `ha_history_token` secret wiring
      and HA base-URL config. Not wired yet; build compiles without it.
- [x] Per-entity ring buffer in `struct Entity` (`HistorySample` + `history`,
      cap `HISTORY_CAP` = 240); append in `on_state_` via `record_history_`
      with numeric parse / binary mapping (`state_to_value_`). Only chartable
      entities (`is_chartable_`) allocate one.
- [x] `build_history_sheet_` overlay (title, current value, `lv_chart`, min/max
      labels, 1h/6h/24h window chips, `✕`); `open_history_` / `close_history_` /
      `redraw_history_`. Added `-DLV_USE_CHART=1` to the board build flags.
- [ ] **DEFERRED** — REST fetch + JSON parse → chart points keyed on window.
      Sheet currently reads only the ring buffer.
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

## Open decisions

- None outstanding. (Resolved: arrows wrap around; connecting state blinks
  amber with a solid-amber fallback. E8: full scale — height + name font + icon
  + widget; sizes 60/84/108 px with montserrat 18/24/32; strict-enum `size:`.)

---

## Out-of-scope

- No new HA entity subscriptions or attribute fetches (keeps the connect-time
  TX budget clean — see the P7e/P7d TX-saturation lesson in plan-mvp.md).
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
