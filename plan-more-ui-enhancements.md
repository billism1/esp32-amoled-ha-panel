# Build Plan — More UI enhancements (drop-in LVGL widgets)

> Pure on-device polish. Every item here swaps a widget the UI already builds
> for a flashier LVGL v9 widget, reusing the **same HA state and the same
> service call**. No new domain handling, no new attributes, no new HA
> plumbing. See [DEVELOPMENT.md](DEVELOPMENT.md) for the design-decision log and
> the existing widget tree (built at runtime in C++ in
> [components/ha_panel/ha_panel.cpp](components/ha_panel/ha_panel.cpp), not in
> YAML).
>
> Items needing new HA functionality first (RGB color wheel, weather animimg,
> album art, floor-plan canvas, live camera) are intentionally **out of scope**
> here and tracked separately.

Background reference: [lvgl.io/docs widgets](https://lvgl.io/docs/open/widgets).

Current widget vocabulary in `ha_panel.cpp`: button, label, switch, slider,
dropdown, tileview, chart, plain `obj` containers. Everything below adds to that.

---

## Ordering rationale

Sequenced easy → hard so each lands as an independent, shippable commit:

1. **UE1** — build-config guard (prerequisite, no UI change).
2. **UE2** — spinner (trivial, two call sites).
3. **UE3** — arc dials (climate setpoint + media volume).
4. **UE4** — gauge on the sensor history sheet.
5. **UE5** — glowing LED on binary_sensor rows.
6. **UE6** — roller drum for select / HVAC mode / fan speed.

---

## UE1 — Build-config guard for the new widget types

**Status:** ✅ done · target tag: `ue1-lvgl-widgets`

**Why first:** This project builds its widget tree from C++ via the LVGL C API,
not from YAML. ESPHome's `lvgl` component only compiles a widget type's code
(`LV_USE_ARC`, `LV_USE_SCALE`, `LV_USE_LED`, `LV_USE_SPINNER`, `LV_USE_ROLLER`,
…) when it decides that type is in use. `slider` / `switch` / `dropdown` /
`chart` link today because they are referenced; the new types
(`arc`, `scale`, `led`, `spinner`, `roller`) may not be enabled until something
references them. Confirm before building UI on top of them.

Tasks:
- [x] Trial-compile confirmed all five `LV_USE_*` started at `0`. Enabled them
      and verified `firmware.elf` links with `lv_arc/led/roller/spinner/scale`
      object files present.
- [x] Enablement done via `-DLV_USE_*=1` `build_flags` in
      [boards/waveshare-2.16.yaml](boards/waveshare-2.16.yaml) — the project's
      existing precedent for runtime-built widgets (P7/E9), not the YAML
      font-anchor trick. `LV_USE_SCALE` additionally required `LV_USE_LINE` +
      `LV_USE_IMAGE` (needle paths).
- [x] Outcome logged in `DEVELOPMENT.md` (Phase UE1).

**Exit criteria:** All five widget types compile and link from C++ with no YAML
declaration required for each instance.

**Risks / unknowns:**
- ESPHome's LVGL build may stub an unused widget rather than error — watch for a
  link error vs. a silently no-op widget.

---

## UE2 — Spinner for loading states

**Status:** ✅ done (scope shifted to boot splash) · target tag: `ue2-spinner`

**Pairs with (existing):** the blocking "Loading..." text in the history sheet
([ha_panel.cpp:3759-3761](components/ha_panel/ha_panel.cpp#L3759-L3761)) and the
detail-modal "Loading..." placeholder
([ha_panel.cpp:2235](components/ha_panel/ha_panel.cpp#L2235),
[:2252](components/ha_panel/ha_panel.cpp#L2252)). Optionally the boot-splash
connection stages ([:1275](components/ha_panel/ha_panel.cpp#L1275)).

**Change:** Replace the static "Loading..." label with an `lv_spinner` shown
while the blocking REST backfill runs, hidden once data lands or the fetch
times out / fails.

Tasks:
- [x] **History sheet spinner (delivered):** the REST backfill blocks the single
      main loop, so an `lv_spinner` (anim driven by `lv_timer_handler`, which
      also can't be re-entered from inside the open event) won't turn. Instead a
      hand-rotated `lv_arc` (`history_spinner_`) is raised over the chart and
      ticked from the fetch read loop via `spin_history_()` → `lv_refr_now()`,
      ~30 fps. LVGL's clock is `millis()`-based (`lv_tick_set_cb`), so the spin
      rate stays wall-clock steady. Hidden on first redraw / "No data yet" /
      error. "Loading..." text kept alongside. The body transfer animates; only
      the TCP connect + JSON parse are brief frozen gaps.
- [x] ~~Detail-modal placeholder~~ — **N/A.** The async attr-fetch path
      (`request_detail_attrs_`, with the "Loading…" placeholder + 1500 ms
      timeout) is parked dead code; `open_detail_` builds synchronously and
      instantly ([:2141](components/ha_panel/ha_panel.cpp#L2141)). No live
      placeholder to replace.
- [x] **Boot-splash spinner (delivered):** per-stage `lv_spinner` beside each
      init stage. The boot wait runs across loop ticks, so it genuinely
      animates. State machine: **active** → animated spinner; **queued** → dim
      amber dot (kept); **done** → green check (kept). See `update_splash_stage_`.

**Exit criteria:** (1) Opening a 1h/6h/24h window shows a turning arc over the
chart that disappears the instant the chart paints. (2) Boot splash shows an
animated spinner on the stage being worked on (Wi-Fi, then HA), the next stage a
dim dot, each flipping to a green check on completion. Compiles + links clean.

**Risks / unknowns (resolved):**
- The REST backfill blocks LVGL, but `lv_refr_now()` (not `lv_timer_handler`) is
  re-entrancy-safe inside the open event, and the body is read in a chunked loop
  — so a hand-spun arc animates there after all. Detail-modal load is dead code.

---

## UE3 — Arc dials (climate setpoint + media volume)

**Status:** ✅ done — validated on-device (climate single + dual dials, integer
steps, row temp, tap-to-open). Setpoint "revert ~half the time" traced to the
Honeywell **Lyric cloud poll**, not firmware (see DEVELOPMENT.md). Media volume
dial + heat_cool dual-apply compile clean, on-device check optional (no
media_player entity yet). · target tag: `ue3-arc-dials`

**Pairs with (existing):**
- Climate target-temp slider in
  [build_detail_climate_](components/ha_panel/ha_panel.cpp#L2366) (the
  `dw_temp_slider_` at [:2418](components/ha_panel/ha_panel.cpp#L2418)).
- Media volume slider in
  [build_detail_media_player_](components/ha_panel/ha_panel.cpp#L2432) (the
  `dw_volume_slider_` at [:2477](components/ha_panel/ha_panel.cpp#L2477)).

**Change:** Swap each `lv_slider` for an `lv_arc`. Same integer range
(climate already maps temp→int via `scale`; volume is 0–100), same value label,
same event handlers (`on_detail_temp_slider_`, `on_detail_volume_slider_`) —
arc fires `LV_EVENT_VALUE_CHANGED` and exposes `lv_arc_get_value` /
`lv_arc_set_value` just like the slider, so `apply_detail_` is untouched.

Tasks:
- [x] Climate: replaced `dw_temp_slider_` with `lv_arc`; range/step math reused
      verbatim. `dw_temp_label_` is a child of the arc, centered in the ring.
      Indicator + knob tinted by HVAC mode (off = grey, heat = warm, cool = blue,
      else teal) from the existing `e.state` read.
- [x] Media: replaced `dw_volume_slider_` with `lv_arc`, 0–100, `%` label
      centered inside (teal accent).
- [x] Repointed the `VALUE_CHANGED` handlers at the arc; `lv_slider_get_value`
      → `lv_arc_get_value` in both handlers.
- [x] Apply path updated (`lv_slider_get_value` → `lv_arc_get_value` for temp +
      volume); same services, same scaling. Compiles + links clean.
- [x] Touch sizing: 180×180 dial, 14 px arc width, default draggable knob. Each
      arc wrapped in a transparent non-scrollable holder so it centers without
      shifting the section labels; content scrolls, Apply/Cancel pinned at y=396
      → no collision. Fingertip ergonomics flagged for on-device tuning.

Scope grew during validation (all compile clean, on-device pending):
- [x] **Climate attrs were never loaded** (pre-existing; slider had it too). Fixed
      by subscribing the standard `ClimateEntity` attrs at connect time (E7
      precedent, no re-arm) — incl. `target_temp_low`/`target_temp_high`. Killed
      the bogus `21` midpoint + `--` current + °F/°C range mismatch.
- [x] **Mode-aware setpoints.** heat/cool → one 180px `temperature` dial;
      auto/heat_cool → two **side-by-side** 150px dials ("Heat to" =
      `target_temp_low`, "Cool to" = `target_temp_high`) so both are visible
      without scroll (stacking buried the cool dial + arc traps scroll). Two
      boxes built, one hidden; HVAC-dropdown change swaps them live
      (`on_detail_hvac_mode_changed_`) + re-tints. Dual handlers clamp low ≤
      high. Apply branches: dual sends low+high, single sends temperature.
- [x] **Page row shows current temp**: climate row renders `"<mode>  <temp>°"`,
      refreshed via `on_attr_` → `rebuild_entity_row_` on `current_temperature`.
- [x] **Tap opens modal** for `SUMMARY_TEXT`+detail domains (climate/media/number/
      select), not only long-press.
- [x] **hvac_modes enum-repr cleanup**: HA sent `<HVACMode.OFF: 'off'>`;
      `clean_hvac_mode_` extracts the quoted value (fixed dropdown text +
      preselect + dual-detect + apply payload).
- [x] **Whole-degree steps**: default `target_temp_step` 0.5 → 1.0 when HA omits
      it; labels drop the decimal for integer steps (`fmt_setpoint_`).
- [x] **No bg-tap close**: removed the detail-modal background-click handler so a
      stray side/bottom tap can't dismiss the modal mid-edit — only Apply/Cancel.
- [x] **Setpoint stuck only ~half the time** → apply sent a redundant
      `set_hvac_mode` (current mode) right before `set_temperature`, racing the
      integration's target re-read. Now sends `set_hvac_mode` only when the mode
      actually changed.

**Exit criteria:** Climate modal shows a round setpoint dial colored by mode,
reading the true target + current temp; auto/heat_cool shows two dials (heat +
cool point); the page row shows the current temperature; a tap (not just a
long-press) opens the modal; media modal shows a round volume dial. Dragging any
dial sets the value and Apply calls the same service (single `temperature`, or
`target_temp_low`/`target_temp_high` for dual; `media_player.volume_set`).

**Risks / unknowns:**
- Arc drag ergonomics vs. slider on a small round display — may need a wide
  arc-width hit area. Tune on-device.
- Climate non-integer step is already handled by the int-scaling; reuse it
  verbatim, don't reinvent.

---

## UE4 — Analog gauge on the sensor history sheet

**Status:** ⬜ not started · target tag: `ue4-gauge`

**Pairs with (existing):** numeric `sensor` history sheet
([build_history_sheet_](components/ha_panel/ha_panel.cpp#L3398)). The current
value is already shown as a big label (`history_value_`,
[:3440](components/ha_panel/ha_panel.cpp#L3440)) above the line chart.

**Change:** Add an `lv_scale` (v9 replacement for the removed `lv_meter`) with a
needle to render the **current** value as an analog gauge, complementing the
existing chart (gauge = now, chart = history). Zero new data — same value the
label already shows.

Tasks:
- [ ] Add an `lv_scale` in arc/round mode plus a needle line to the history
      sheet, laid out so it coexists with title / value / chart (may share the
      value-label corner or sit above the chart; keep the chart the focus).
- [ ] Drive the needle from the same value feeding `history_value_`, on open and
      on each live-tail update.
- [ ] Derive the gauge range from the data window's min/max (the sheet already
      computes a value range for the chart axis — reuse it) so the needle isn't
      pinned. Fall back to a sane default when the range is degenerate.
- [ ] Optional: colored zones (green → amber → red) on the scale; keep simple
      and config-free for v1.
- [ ] Hide the gauge for `binary_sensor` (the strip view already replaces the
      chart there — [:3466](components/ha_panel/ha_panel.cpp#L3466)).

**Exit criteria:** Tapping a numeric sensor shows a live analog gauge of the
current reading alongside the trend chart; binary sensors are unaffected.

**Risks / unknowns:**
- Screen real estate at 480×480 with title + value + 432×230 chart + time row
  already placed — the gauge must fit without crowding. Decide placement first.
- `lv_scale` needle API differs from the old `lv_meter`; budget time to wire the
  needle indicator correctly.

---

## UE5 — Glowing LED on binary_sensor rows

**Status:** ⬜ not started · target tag: `ue5-led`

**Pairs with (existing):** read-only `binary_sensor` rows. State styling lives in
[rebuild_entity_row_](components/ha_panel/ha_panel.cpp#L441); binary/read-only
text is colored in the `SUMMARY_TEXT` / `READ_ONLY_TEXT` arm
([:523-537](components/ha_panel/ha_panel.cpp#L523-L537)).

**Change:** For binary sensors, add a small `lv_led` status dot driven by the
existing on/off state — green glow when "on"/active, dim/grey when "off", red on
unavailable — using `lv_led_set_color` + `lv_led_set_brightness`. No new state;
read the same `e.state` the text arm reads.

Tasks:
- [ ] Confirm which `render_class` binary sensors fall under and whether the LED
      should attach in `rebuild_entity_row_` or at row-build time
      ([rebuild logic at :441](components/ha_panel/ha_panel.cpp#L441); row
      construction elsewhere — locate `make_entity_row`).
- [ ] Add an `lv_led` to the icon column (or beside the value) for binary
      sensors only; leave other render classes untouched.
- [ ] Map state → color + brightness in the existing per-state switch; respect
      the unavailable (`0xCC4444`) convention already used.
- [ ] Keep it cheap: one LED per binary-sensor row, updated in the existing
      rebuild path, no extra timer.

**Exit criteria:** Door/motion/window binary sensors show a glowing dot that
tracks state, distinct from the flat text rows around them.

**Risks / unknowns:**
- Don't regress the existing unavailable-overlay handling that other render
  classes use.
- Per-entity `size` (small/medium/large) affects row height — LED size should
  scale or at least look right across all three.

---

## UE6 — Roller drum for option pickers

**Status:** ⬜ not started · target tag: `ue6-roller`

**Pairs with (existing):**
- `select` detail modal dropdown in
  [build_detail_select_](components/ha_panel/ha_panel.cpp#L2538).
- HVAC mode dropdown in
  [build_detail_climate_](components/ha_panel/ha_panel.cpp#L2384-L2398)
  (`dw_hvac_dropdown_`).
- Fan speed in [build_detail_fan_](components/ha_panel/ha_panel.cpp#L2564).

**Change:** Replace the `lv_dropdown` (and/or the fan-speed slider) with an
`lv_roller` drum. Same options list (already parsed via `parse_ha_list_`), same
selected-index logic, same apply path — `lv_roller_get_selected` mirrors
`lv_dropdown_get_selected`.

Tasks:
- [ ] `select`: swap dropdown → roller; reuse the newline-joined options string
      the dropdown already builds; preselect the current option.
- [ ] Climate HVAC mode: same swap for `dw_hvac_dropdown_`; preserve the
      fallback `{off,heat,cool,auto}` list and the current-state preselect at
      [:2393-2398](components/ha_panel/ha_panel.cpp#L2393-L2398).
- [ ] Fan: decide roller-of-named-speeds vs. keep the slider (slider may stay
      better for continuous %); only convert if the entity exposes discrete
      `preset_modes`. Document the choice.
- [ ] Update the apply handlers to read the selected index from the roller.
- [ ] Confirm modal layout still fits with a taller roller vs. a one-line
      dropdown (rollers need vertical room).

**Exit criteria:** Select and HVAC-mode pickers present a scrolling drum;
choosing a value and Apply calls the same service as the dropdown did.

**Risks / unknowns:**
- Rollers consume more vertical space than dropdowns — may force modal layout
  rework, especially when stacked with other controls (climate has mode +
  setpoint + current-temp label).
- Keep the dropdown for very long option lists if the roller gets unwieldy.

---

## Cross-cutting notes

- **🛑 NO git commit until on-device validation passes.** Compiling clean is not
  enough — flash the panel and confirm the feature works before any commit.
  (UE3 passed this gate — validated on-panel, cleared to commit.)
- **Validate acceptance criteria before commit**
- **One commit per UE item**, each independently shippable and revertible.
- **No HA-side changes** for any item here — same states, same service calls.
  If an item starts needing a new attribute or service, it has left this plan's
  scope; stop and reassess.
- **Verify on-device**, not just on compile: arc/roller touch ergonomics, gauge
  layout fit, and spinner animation-during-blocking-fetch can only be judged on
  the 480×480 panel.
- Update `README.md` Features + `DEVELOPMENT.md` log as each item ships.
