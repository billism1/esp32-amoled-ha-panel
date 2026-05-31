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

**Status:** ⬜ not started · target tag: `ue1-lvgl-widgets`

**Why first:** This project builds its widget tree from C++ via the LVGL C API,
not from YAML. ESPHome's `lvgl` component only compiles a widget type's code
(`LV_USE_ARC`, `LV_USE_SCALE`, `LV_USE_LED`, `LV_USE_SPINNER`, `LV_USE_ROLLER`,
…) when it decides that type is in use. `slider` / `switch` / `dropdown` /
`chart` link today because they are referenced; the new types
(`arc`, `scale`, `led`, `spinner`, `roller`) may not be enabled until something
references them. Confirm before building UI on top of them.

Tasks:
- [ ] Trial-compile a throwaway `lv_arc_create` / `lv_scale_create` /
      `lv_led_create` / `lv_spinner_create` / `lv_roller_create` and confirm the
      `LV_USE_*` macros are on in the generated `lv_conf.h` (link succeeds).
- [ ] If any type is missing, add a hidden 0-sized anchor widget for it in
      [packages/lvgl-ui.yaml](packages/lvgl-ui.yaml) — same force-link trick the
      file already uses for the MDI/Montserrat fonts (see its `*_anchor` labels).
- [ ] Note the outcome (which anchors were needed) in `DEVELOPMENT.md`.

**Exit criteria:** All five widget types compile and link from C++ with no YAML
declaration required for each instance.

**Risks / unknowns:**
- ESPHome's LVGL build may stub an unused widget rather than error — watch for a
  link error vs. a silently no-op widget.

---

## UE2 — Spinner for loading states

**Status:** ⬜ not started · target tag: `ue2-spinner`

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
- [ ] Add a reusable `lv_spinner` to the history sheet, hidden by default; show
      it where the code currently paints `history_value_` = "Loading...", hide
      it on first redraw / "No data yet"
      ([:3682](components/ha_panel/ha_panel.cpp#L3682)) / error.
- [ ] Same treatment for the detail-modal "Loading..." placeholder.
- [ ] (Optional) Add a small spinner beside the active boot-splash stage.
- [ ] Confirm the spinner animates during the **blocking** fetch — LVGL anim
      ticks must still run while `http_request` blocks; if they don't, keep the
      text fallback for that one path and note it.

**Exit criteria:** Opening a 6h/24h window shows a spinning indicator, not frozen
text, and it disappears the instant the chart paints.

**Risks / unknowns:**
- The REST backfill is blocking by design. If the LVGL task is starved during
  the fetch the spinner won't animate — verify on-device, not just in theory.

---

## UE3 — Arc dials (climate setpoint + media volume)

**Status:** ⬜ not started · target tag: `ue3-arc-dials`

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
- [ ] Climate: replace `dw_temp_slider_` with `lv_arc`; keep range/step math at
      [:2402-2422](components/ha_panel/ha_panel.cpp#L2402-L2422). Center the
      `dw_temp_label_` inside the arc. Tint the arc indicator by HVAC mode
      (heat = warm, cool = blue, off = grey) from the existing mode read.
- [ ] Media: replace `dw_volume_slider_` with `lv_arc`, 0–100, keep the
      `%` label centered.
- [ ] Repoint the existing `VALUE_CHANGED` event callbacks at the arc;
      replace `lv_slider_*` calls in those handlers with `lv_arc_*`.
- [ ] Verify the apply path still sends correct values for both
      ([apply_detail_](components/ha_panel/ha_panel.cpp#L2690)).
- [ ] Size for touch: arc thick enough to drag with a fingertip on 480×480;
      check it doesn't collide with the modal's Apply/Cancel row.

**Exit criteria:** Climate modal shows a round setpoint dial colored by mode;
media modal shows a round volume dial. Dragging either sets the value and Apply
calls the same service as before.

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

- **One commit per UE item**, each independently shippable and revertible.
- **No HA-side changes** for any item here — same states, same service calls.
  If an item starts needing a new attribute or service, it has left this plan's
  scope; stop and reassess.
- **Verify on-device**, not just on compile: arc/roller touch ergonomics, gauge
  layout fit, and spinner animation-during-blocking-fetch can only be judged on
  the 480×480 panel.
- Update `README.md` Features + `DEVELOPMENT.md` log as each item ships.
