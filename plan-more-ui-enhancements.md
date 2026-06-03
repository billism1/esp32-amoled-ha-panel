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
6. **UE6** — history fetch on a core-pinned worker task (crash fix: the blocking
   GET tripped the task WDT on 6h/24h windows; also unfreezes the UI + enables a
   real `lv_spinner`, retiring UE2's hand-rotated arc). Infra, not a widget swap.
7. **UE7** — device_class-aware severity for the binary_sensor LED (follow-up to
   UE5; leaves the "no new HA data" scope — needs the `device_class` attr).
8. **UE8** — inline trend sparkline behind sensor rows (`plot_preview: true`
   opt-in). Reuses the E9 ring buffer + UE6/E9 decimation; a second, slimmer
   render target — no new HA data.
9. **UE9** — `readonly: true` per-entity flag that suppresses every control
   surface (service taps, confirm sheet, detail modal) while keeping view +
   plots. Pure dispatch gate; no new HA data, no new widget.
10. **UE10** — user-editable idle timeouts (dim / blank / sleep) + reset-source
    (touch / motion) selection in Settings, with an AMOLED burn-in warning on
    Apply when all screen protection is disabled. Config/infra like UE6 — not a
    widget swap. Needs a per-board `is_amoled` flag (groundwork for more boards).
11. **UE11** — page-picker badges: each page declares one `picker_badge:` (lights
    on, devices on, open doors, unlocked, offline, motion, low battery, room
    temp/humidity/power, alarm/severity dot, …) shown as icon+value right of the
    page name in the picker. Computed fresh on `open_picker_()` (no live wiring).
    No new widget, no new HA data for the config-free types. Per-page config
    resolves the one-slot question; shares its count/aggregate helper with UE12,
    and the device_class types depend on UE7.
12. **UE12** — report rows: a synthetic "entity" that renders a *computed
    aggregate* over the panel's other entities (lights on / off, totals per
    domain, min / max / avg of a sensor group, open-door / low-battery / offline
    summaries, panel self-diagnostics) instead of one HA state. Rides the
    existing entity-row pipeline (new `RenderClass`, recomputed from `on_state_`),
    so it reuses `make_entity_row` + `rebuild_entity_row_` + `size:`. Aggregating
    only over panel-known entities keeps it inside the "no new HA data" rule; an
    all-HA count is a flagged follow-up.
13. **UE13** — per-row text styles: `style: [bold, italic, underline]` on any row
    (entity or report) styles its name/title label. Underline is a runtime LVGL
    `text_decor`; bold/italic need baked Montserrat variants (LVGL has no
    synthetic weighting), auto-wired via a merged `style-fonts` package so the
    user only writes `style:`. Reuses `make_entity_row`; styles set at build time.

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

**Status:** ✅ done — validated on-device (numeric gauge tracks current reading
alongside the trend chart; binary sensors unaffected). Compiles + links clean
(RAM 16.5%, Flash 19.5%). · target tag: `ue4-gauge`

**Pairs with (existing):** numeric `sensor` history sheet
([build_history_sheet_](components/ha_panel/ha_panel.cpp#L3398)). The current
value is already shown as a big label (`history_value_`,
[:3440](components/ha_panel/ha_panel.cpp#L3440)) above the line chart.

**Change:** Add an `lv_scale` (v9 replacement for the removed `lv_meter`) with a
needle to render the **current** value as an analog gauge, complementing the
existing chart (gauge = now, chart = history). Zero new data — same value the
label already shows.

Tasks:
- [x] Added an `lv_scale` (`LV_SCALE_MODE_ROUND_INNER`, 270° sweep / rotation
      135) + `lv_line` needle (`history_gauge_` / `history_gauge_needle_`),
      top-right just under the ✕. **Placement:** the chart was shortened
      230→176 px (top y96→150, bottom stays 326) to free the band; the gauge
      (96×96) sits in it without crowding, chart stays the dominant element.
      Spinner + binary strip re-centered to the new chart footprint.
- [x] Driven from `redraw_history_`, which runs on open *and* on every live-tail
      append — so the needle tracks the same value as `history_value_` (prefers
      the live state via `state_to_value_`, falls back to the freshest in-window
      sample).
- [x] Range = the window's `[vmin,vmax]` (the same min/max the chart axis
      computes) padded ±15 % so the needle floats off the ends; degenerate/flat
      series get a synthetic span. Scaled ×10 internally for one-decimal needle
      resolution.
- [ ] ~~Colored zones~~ — **skipped for v1.** Green→amber→red has no universal
      meaning across sensor kinds (high temp = bad, high battery = good), and
      v1 is "config-free." Needle + ticks on the teal accent only. Revisit if a
      per-entity hint lands.
- [x] Gauge hidden for `binary_sensor` (strip branch) and for the no-data path;
      `lv_scale_set_label_show(false)` keeps the 96 px dial uncluttered (numbers
      already live in `history_value_` + `history_range_label_`).

**Exit criteria:** Tapping a numeric sensor shows a live analog gauge of the
current reading alongside the trend chart; binary sensors are unaffected.

**Risks / unknowns:**
- Screen real estate at 480×480 with title + value + 432×230 chart + time row
  already placed — the gauge must fit without crowding. Decide placement first.
- `lv_scale` needle API differs from the old `lv_meter`; budget time to wire the
  needle indicator correctly.

---

## UE5 — Glowing LED on binary_sensor rows

**Status:** ✅ done — validated on-device (binary sensors show a glowing dot that
tracks state across small/medium/large rows). · target tag: `ue5-led`

**Pairs with (existing):** read-only `binary_sensor` rows. State styling lives in
[rebuild_entity_row_](components/ha_panel/ha_panel.cpp#L441); binary/read-only
text is colored in the `SUMMARY_TEXT` / `READ_ONLY_TEXT` arm
([:523-537](components/ha_panel/ha_panel.cpp#L523-L537)).

**Change:** For binary sensors, add a small `lv_led` status dot driven by the
existing on/off state — green glow when "on"/active, dim/grey when "off", red on
unavailable — using `lv_led_set_color` + `lv_led_set_brightness`. No new state;
read the same `e.state` the text arm reads.

Tasks:
- [x] Confirmed: binary_sensor falls under **`READ_ONLY_TEXT`** (the shared
      lock/cover/summary/read-only arm). LED created at **row-build time** in
      `make_entity_row` (new `out_led` out-param, binary-only) and **driven** in
      `rebuild_entity_row_`'s READ_ONLY_TEXT arm — no new timer, rides the
      existing rebuild path.
- [x] Added an `lv_led` at the **far-right** slot for binary sensors only; the
      on/off word shifts left of it (right-anchored to `label_x - led_sz - 8`, so
      it never overlaps the fixed dot). Other render classes untouched (`out_led`
      stays nullptr; `leds_by_entity_[ei]` nullptr). LED size = `m.height / 4`
      (13/16/20 px across small/med/large rows).
- [x] State → colour reuses the arm's existing `col` (on = green `0x66BB66`,
      off = grey `0x888888`, unavailable/unknown = red `0xCC4444`); brightness
      carries the glow: `255` on, `60` off (dim ember), `160` unavailable.
- [x] Cheap: one `lv_led` per binary-sensor row, stored in `leds_by_entity_`,
      updated only in `rebuild_entity_row_`. No extra timer.

**Exit criteria:** Door/motion/window binary sensors show a glowing dot that
tracks state, distinct from the flat text rows around them.

**Risks / unknowns:**
- Don't regress the existing unavailable-overlay handling that other render
  classes use.
- Per-entity `size` (small/medium/large) affects row height — LED size should
  scale or at least look right across all three.

---

## UE6 — History fetch on a core-pinned worker task

**Status:** ✅ done — validated on-device (async worker fetch; 24h/287-point loads;
flat sensors render a flat line; no reboot/bad_alloc/HTTP -1). Required a heap fix
first — `lvgl buffer_size 25%→10%` freed ~69 KB internal so the worker's task
stack + esp_http_client buffers fit (committed separately). · target tag:
`ue6-history-worker`

**Why (a real crash, not polish):** the E9 history backfill issues a **blocking**
`http_request->get()` on the main loop while HA computes the
`/api/history/period` query. On 6h/24h windows HA takes several seconds; the
default ~5 s task watchdog fired mid-fetch and **rebooted the panel** (`task_wdt:
loopTask (CPU 1) did not reset the watchdog`). 1h usually responds fast enough to
survive. A band-aid (raise `CONFIG_ESP_TASK_WDT_TIMEOUT_S` to 15 s + `feed_wdt()`
before the JSON parse) stops the crash but leaves the UI frozen for up to the 8 s
http timeout, and the UE2 loading "spinner" is a hand-rotated `lv_arc` precisely
because the loop is blocked. This phase does the **proper** fix.

**This is infra, not a widget swap** — it intentionally leaves the "drop-in LVGL
widget" theme of the other UE items. Sequenced here because it's a live crash and
because it retires the UE2 arc hack.

**Change:** move the blocking work (TCP connect + HTTP body read + JSON parse —
none of which touches LVGL) onto a **persistent FreeRTOS worker task pinned to
core 0**. `loopTask`/LVGL stay on core 1, keep ticking `lv_timer_handler`, feed
their own WDT, and animate a **real `lv_spinner`**. LVGL is single-threaded, so
the split is along the "touches no `lv_*`" line: the worker only touches the
socket, a PSRAM buffer, and a staging `vector<HistorySample>` it owns.

Handshake (ordering matters):
- [x] Worker created once at the end of `setup()`, pinned to **core 0**
      (`xTaskCreatePinnedToCore`, 16 KB stack, prio 2), blocked on a binary
      semaphore (`hist_req_sem_`). Persistent loop, not spawn-per-open. Create
      failure nulls the handles → falls back to the synchronous ring-buffer path.
- [x] **Main → worker:** `dispatch_history_fetch_()` builds the URL, sets
      `hist_req_url_`/`hist_req_is_binary_`/`hist_req_seq_`, stores
      `hist_fetch_state_ = HIST_RUNNING` (release), gives the semaphore. Worker
      reads only those request fields + the immutable client/token — never
      `entities_` or any `lv_*`.
- [x] **Worker** (`run_history_fetch_`): `get()` + chunked read into the PSRAM
      buffer + `parse_json` → fills `hist_staging_` (the vector it owns); the
      trampoline stores `HIST_DONE_OK`/`HIST_DONE_FAIL` with **release** ordering.
- [x] **Worker → main:** `loop()` → `poll_history_fetch_()` loads the atomic with
      **acquire**; on done it `swap()`s `hist_staging_` → `history_samples_` and
      calls `redraw_history_()`. Release/acquire is the barrier — no mutex (main
      touches staging only after done; worker is parked on the semaphore by then).
- [x] **Cancellation / supersede:** each user action bumps `hist_seq_want_`. On
      completion, `hist_req_seq_ != hist_seq_want_` ⇒ stale result dropped (no
      redraw); a request that arrived while the worker was busy is re-dispatched.
      Also guards: redraw only if the sheet is still open on the same entity, and
      the live-tail skips its redraw while a fetch is `HIST_RUNNING`.
- [x] Swapped the UE2 hand-rotated `history_spinner_` arc for a real
      `lv_spinner` (loop runs during the fetch now); removed `spin_history_()` +
      `history_spin_step_` + the read-loop `lv_refr_now`/`feed_wdt` pumping.
- [x] Lowered the WDT backstop 15 s → **10 s** (covers the worker briefly
      blocking core-0 idle during the one-shot parse; the read loop `yield()`s).

**Gotchas:**
- **Stack size:** ArduinoJson recursion + HTTP need headroom. LAN `http` ≈ 8 KB;
  if the HA URL is **https**, mbedTLS pushes it to ~16–20 KB (`verify_ssl: false`
  does *not* remove the TLS stack cost). Size the task stack accordingly or it
  stack-overflows.
- **Single consumer:** only history uses `ha_http`, so no locking on the client —
  but don't let any other code call it concurrently with the worker.
- **Worker WDT during connect:** the connect phase has no yield; register the
  worker to the task WDT and feed it in the read loop, or rely on the ~10 s
  backstop. loopTask's own WDT is satisfied because the loop is no longer blocked.

**Exit criteria:** opening a numeric sensor and cycling 1h/6h/24h repeatedly never
reboots; the chart paints when data lands; a real `lv_spinner` turns smoothly
during the fetch instead of the stepped arc; closing or switching mid-fetch
doesn't redraw stale data.

**Risks / unknowns:**
- ESPHome's `http_request` / `esp_http_client` called from a non-loop task —
  verify it has no implicit main-task affinity (it shouldn't; it's a plain client).
- Task stack tuning is empirical; watch for `Stack canary watchpoint triggered`.

---

## UE7 — device_class-aware severity for the binary_sensor LED

**Status:** ⬜ not started · target tag: `ue7-led-severity`

**Why:** UE5 shipped a status LED that paints **green = "on"** for every
binary_sensor. That's semantically wrong for a whole class of sensors where "on"
means *bad*: a water-leak (`moisture`) sensor "on" = a leak, smoke/gas/CO "on" =
an alarm, `battery` "on" = low battery, `problem`/`safety`/`tamper` "on" = a
fault. Painting those green reads as "all good" at exactly the moment something is
wrong. binary_sensor "on" doesn't mean "good" — it means "detected/active", and
whether that's good, bad, or neutral depends on the entity's **`device_class`**.

This is the one item in this plan that **leaves the "no new HA data" scope**: it
needs the `device_class` attribute, which the panel doesn't subscribe today. That
is acceptable and intentional — it's the same kind of standard-attribute fetch
UE3 used for climate (`current_temperature` et al.), not an HA-side change. Flag
it, don't pretend it fits the original constraint.

**Step 1 — DECIDE the approach (do this first, before any code).** Options, in
rough order of correctness:

- **A — device_class-aware severity (LEAN / recommended; what HA does).**
  Subscribe `device_class` at connect time (E7/UE3 precedent: rides the initial
  `state_subs` cursor walk, no re-arm), classify each binary_sensor into a
  severity bucket, and colour the LED by **(bucket, state)** instead of by raw
  on/off:
  - *Problem/alarm classes* — `moisture`, `smoke`, `gas`, `co`, `safety`,
    `problem`, `tamper`, `heat`, `cold`, `battery` (low when "on") — "on" = **red
    glow**, "off" = dim green/grey (all clear).
  - *Neutral/activity classes* — `motion`, `occupancy`, `presence`, `door`,
    `window`, `opening`, `garage_door`, `light`, `sound`, `running`, `power`,
    `plug`, `vibration`, `moving` — "on" = **amber/accent glow** (active), "off"
    = dim grey. No good/bad claim.
  - *Positive classes* — `connectivity` ("on" = connected), `battery_charging`
    ("on" = charging) — "on" = **green**, "off" = dim/red.
  - Unknown / no device_class → fall back to the UE5 behaviour (green on / grey
    off) so nothing regresses.
  This matches Home Assistant's own frontend, which colours/swaps the
  binary_sensor presentation by device_class (problem classes alert red/amber on
  detection; door/motion are neutral "active" highlights, not green). **Pick this
  unless something below changes the calculus.**

- **B — activity-only, no value judgment.** Drop green/red entirely: "on" =
  bright accent, "off" = dim. The dot only signals "active now", never good/bad.
  Never wrong, needs no new attribute — but loses the at-a-glance alarm signal
  that makes a leak/smoke dot actually useful. Fallback if A proves too costly.

- **C — amber-active (HA-ish middle, no new attribute).** "on" = amber glow
  (HA's real "active" colour), "off" = dim grey, unavailable = red. Kills the
  false-"green = good" problem immediately without subscribing `device_class`,
  but still can't make a leak sensor go *red* on detection. A reasonable interim
  if we want the fix now and A later.

- **D — per-entity YAML override (`on_color:` / `severity:`).** Maximum control,
  maximum config; against the "config-free v1" principle. Only worth it as an
  escape hatch layered on top of A for misclassified entities.

**Decision:** lean **A**. Revisit only if the connect-time attr cost or the
classification table proves not worth it on-device; C is the documented fallback.

Tasks (assuming A):
- [ ] **Decide** (above) and record the choice + reasoning in `DEVELOPMENT.md`.
- [ ] Subscribe `device_class` for binary_sensors at connect time (mirror the
      UE3 climate-attr connect-time subscription; scoped to binary_sensors only
      to keep the TX burst small — see the P7d/P7e TX-saturation lesson).
- [ ] Add a `binary_sensor_severity_(device_class)` classifier → enum
      {PROBLEM, ACTIVITY, POSITIVE, UNKNOWN} with the class lists above.
- [ ] Replace the UE5 colour/brightness logic in `rebuild_entity_row_`'s
      READ_ONLY_TEXT arm with a **(severity, state)** lookup; keep the UE5
      green-on/grey-off as the UNKNOWN fallback so missing device_class doesn't
      regress.
- [ ] Re-run the row on `device_class` arrival (`on_attr_` →
      `rebuild_entity_row_`, same pattern UE3 used for `current_temperature`) so
      the colour is right once the attr lands, not just on first paint.
- [ ] Optional: also swap the row **icon** by device_class+state the way HA does
      (e.g. leak drip, open vs closed door) — likely its own follow-up, not part
      of this LED-colour phase.

**Exit criteria:** A water-leak / smoke / low-battery binary_sensor shows a **red**
glowing dot when "on" (alarm), not green; motion/door/window show a neutral
amber "active" dot when "on"; connectivity/charging show green when "on";
sensors with no `device_class` keep the UE5 behaviour. No connect-time
`Buffer full` / unresponsive-disconnect from the added subscriptions.

**Risks / unknowns:**
- New connect-time subscription — watch the TX budget (the P7d iter-1 lesson).
  binary_sensors are usually few; scope strictly to them.
- `device_class` can be absent or non-standard (custom integrations); the
  UNKNOWN fallback must be solid.
- Colour-only severity is invisible to colour-blind users — the on/off **word**
  stays (already kept in UE5) as the redundant channel; don't drop it.

---

## UE8 — Inline trend sparkline behind sensor rows

**Status:** ⬜ not started · target tag: `ue8-row-sparkline`

**Why:** A chartable sensor row today shows only the latest value as text on the
right. The trend is one tap away (the E9 history sheet), but at-a-glance "is it
rising or falling" needs a tap + a window pick. A faint sparkline painted *behind*
the row text — the kind of thing HA's own Lovelace `sensor` card does inline —
gives the trend for free without leaving the page, scaled to the row's `size:`.

**Opt-in:** strictly per-entity via a new `plot_preview: true` YAML flag (mirrors
`realtime` / `confirm` / `size`). Rows without it render byte-for-byte as today.
Opt-in is also the memory guard — internal heap is tight (see UE6's
`buffer_size 25%→10%` fix and the 082f308 note), so only flagged rows allocate a
chart.

**Pairs with / reuses (existing):**
- Per-entity history ring buffer `Entity::history`, already populated since boot
  from [on_state_](components/ha_panel/ha_panel.cpp#L449) →
  [record_history_](components/ha_panel/ha_panel.cpp#L3933). **The data source
  already exists** — no new capture path.
- `realtime` flag (UE7-era) → deeper ring (`HISTORY_CAP_RT = 600`) + the 30 s
  "Live" window; non-realtime entities use the 240-sample ring + the 1 h window.
  See [the window/cap constants](components/ha_panel/ha_panel.cpp#L3856-L3883).
- Decimation + roll-mode draw already written for the full sheet:
  [redraw_history_](components/ha_panel/ha_panel.cpp#L4425) and
  [redraw_live_roll_](components/ha_panel/ha_panel.cpp#L4369). UE8 is a slimmer
  second render target driven by the same algorithm.
- Per-`size:` row geometry in [RowMetrics](components/ha_panel/ha_panel.cpp#L719)
  / [row_metrics_for](components/ha_panel/ha_panel.cpp#L732) — the sparkline fills
  the row (`LV_PCT(100) × m.height`), so it scales with small/medium/large for
  free (no per-size chart math).

**Change:** For a `plot_preview: true` chartable entity, add an `lv_chart`
(`LV_CHART_TYPE_LINE`) as the **first** child of the row button in
[make_entity_row](components/ha_panel/ha_panel.cpp#L744) so it sits *behind* the
icon / name / right-side widget (those are created after → foreground). Style it
as a backdrop: transparent/near-transparent bg so the row's `0x1A1A1A` shows
through, no axis/divider lines, a thin low-opacity accent line so the name +
value text stay legible on top. Window is picked from the same `realtime` rule
the history sheet uses: realtime → 30 s Live roll, non-realtime → 1 h.

Tasks:
- [ ] **Config plumbing.** Add `CONF_PLOT_PREVIEW = "plot_preview"` in
      [components/ha_panel/__init__.py](components/ha_panel/__init__.py#L43)
      (mirror `realtime` exactly), thread through
      [add_entity](components/ha_panel/ha_panel.h#L121) → new `bool
      plot_preview{false}` on the [Entity struct](components/ha_panel/ha_panel.h#L62).
- [ ] **Row chart widget.** In `make_entity_row`, when `plot_preview &&
      is_chartable_(e)`, create the backdrop `lv_chart` as the first child;
      transparent bg, `border_width 0`, `div_line_count 0,0`, thin
      semi-transparent accent line, dots hidden. Store handles in new
      `bg_charts_by_entity_` / `bg_series_by_entity_` vectors (sized like
      `widgets_by_entity_`; nullptr for non-preview rows).
- [ ] **`redraw_row_chart_(entity_idx)`** — slim clone of the sheet path: window
      `= e.realtime ? 30 s roll : 3600 s`; decimate `e.history` to the row's point
      budget (scale with width, ~60–100 pts); set the Y axis range from the
      in-window min/max; push points. No labels, gauge, strip, or anchor-seed
      faff. Factor the shared decimation out of `redraw_history_` if it's clean to
      do so; otherwise duplicate the ~40 lines and note the divergence.
- [ ] **Wire updates.** In `on_state_`, after `record_history_(i)`, call
      `redraw_row_chart_(i)` when `entities_[i].plot_preview`. Add an initial draw
      after the row-build loop in
      [build_ui_](components/ha_panel/ha_panel.cpp#L1275).
- [ ] **Numeric-only v1.** Guard on `is_chartable_` *and* skip `binary_sensor`
      (the sheet uses an on/off band strip, not a line — a row backdrop step plot
      is a separate decision). `plot_preview` on a non-chartable row is a silent
      no-op.
- [ ] **Optional perf gate.** All rows are built at boot, so a `plot_preview` row
      on a non-visible page still redraws on every state change. If it bites,
      redraw only when the row's page == the active tile.

**Exit criteria:** A numeric sensor row with `plot_preview: true` shows a faint
trend line behind its name + value that updates live — a 30 s rolling scope for
`realtime: true` sensors, a 1 h trend for the rest — legible text on top, and the
sparkline scales cleanly across small / medium / large rows. Rows without the
flag are unchanged. Compiles + links clean; no `bad_alloc` / heap regression with
a handful of preview rows enabled.

**Risks / unknowns:**
- **Internal heap.** Each row chart = an `lv_obj` (~100–200 B) + a series point
  array. Opt-in keeps the cost bounded, but document "use sparingly" and watch
  internal RAM (already tight per UE6). Enabling it across dozens of rows will
  hurt.
- **Z-order + legibility.** The chart must read as a *backdrop*, not compete with
  the text — transparent bg + low-opacity line, tuned on-device. The line chart's
  default opaque `0x111111` bg from the sheet is wrong here.
- **Redraw cost.** A 1 Hz realtime sensor redraws its row chart every second;
  fine for a few, watch it if many. The optional page-active gate is the lever.
- **Binary sensors** are deliberately out of v1 — decide the step-plot vs.
  band-strip backdrop separately if asked.

---

## UE9 — `readonly: true` per-entity lock (view + plots, no edits)

**Status:** ⬜ not started · target tag: `ue9-readonly`

**Why:** Some entities should be visible on the panel but never actuated from it
— a thermostat a guest shouldn't retune, a lock that must only be driven from HA,
a switch on a shared wall panel. Today every controllable row fires a service on
tap (or opens an editing modal). `readonly: true` keeps the row's state read-out
(and its UE8 sparkline / E9 history sheet, if applicable) but disables every path
that mutates HA.

**Opt-in:** new per-entity `readonly: true` YAML flag (mirrors `confirm` /
`realtime` / `size`). Default false → every row behaves exactly as today. When
both `confirm: true` and `readonly: true` are set, **readonly wins** (a confirm
sheet is still an action surface).

**Pairs with / gates (existing):** all edit dispatch funnels through a few sites,
which is what makes this a small, well-contained gate rather than a per-domain
edit:
- [tap_entity_](components/ha_panel/ha_panel.cpp#L622) — the actual
  `call_homeassistant_service` site (toggle / lock / cover / action).
- [on_entity_row_clicked_](components/ha_panel/ha_panel.cpp#L2105) — short-tap
  router → confirm sheet / history sheet / detail modal / `tap_entity_`.
- [on_entity_row_long_pressed_](components/ha_panel/ha_panel.cpp#L3266) →
  [open_detail_](components/ha_panel/ha_panel.cpp#L2461) /
  [open_confirm_action_](components/ha_panel/ha_panel.cpp#L3710).
- [open_confirm_or_detail_](components/ha_panel/ha_panel.cpp#L3695) — the
  `confirm`-flag entry.
- **Kept open:** [open_history_](components/ha_panel/ha_panel.cpp#L4147) is
  view-only — readonly rows still open it (and still draw a UE8 sparkline).

**Behaviour by render class when `readonly`:**
- `BINARY_SWITCH` (light/switch/fan/input_boolean) — tap no longer toggles; the
  switch still mirrors state (already non-interactive; parent button just stops
  dispatching). Optional dim/lock-glyph cue (task below).
- `LOCK_TEXT` / `COVER_TEXT` — tap no longer fires lock/cover services; state
  text + colour still render.
- `SUMMARY_TEXT` (climate/media/number/select) — tap / long-press no longer
  opens the editing detail modal; the row still shows the state summary
  (`"<mode>  <temp>°"` etc.).
- `ACTION_ICON` (scene/script/automation/button) — tap is inert. These carry no
  viewable state, so `readonly` on them is mostly pointless but must no-op
  cleanly (and ideally drop the play-glyph affordance — task below).
- `READ_ONLY_TEXT` (sensor/binary_sensor) — already non-mutating; `readonly`
  changes nothing. History sheet + UE8 sparkline still work.

**Change:** add a `bool readonly{false}` to the Entity; gate the dispatch routers
on it; keep the view paths (state render, history sheet, sparkline) untouched.

Tasks:
- [ ] **Config plumbing.** Add `CONF_READONLY = "readonly"` in
      [components/ha_panel/__init__.py](components/ha_panel/__init__.py) (mirror
      `confirm`), thread through
      [add_entity](components/ha_panel/ha_panel.h#L121) → new `bool
      readonly{false}` on the [Entity struct](components/ha_panel/ha_panel.h#L62).
- [ ] **Hard gate at the service site.** Early-return `false` at the top of
      `tap_entity_` when `ent.readonly` — backstops `tap()` (programmatic /
      future automations) and any router path that's missed, so nothing can fire
      a service for a readonly entity.
- [ ] **Short-tap router.** In `on_entity_row_clicked_`, when `en.readonly`:
      route chartable rows to `open_history_` (view plots) and everything else to
      a no-op; skip the `confirm` and `open_detail_` branches.
- [ ] **Long-press router.** In `on_entity_row_long_pressed_`, no-op when
      `en.readonly` (the detail modal is the edit surface). Cleaner still: at
      build time in [build_ui_](components/ha_panel/ha_panel.cpp#L1259), skip
      registering the `LONG_PRESSED` callback for readonly rows so the gesture is
      never wired.
- [ ] **Confirm entry.** Guard `open_confirm_or_detail_` (and its short-tap
      caller) so a `confirm + readonly` row opens neither sheet.
- [ ] **Visual cue (optional, v1-or-followup).** Signal non-interactivity so a
      readonly row doesn't look broken: e.g. dim the `BINARY_SWITCH` switch /
      hide the `ACTION_ICON` play glyph / drop the row's pressed-state bg
      highlight ([make_entity_row](components/ha_panel/ha_panel.cpp#L766)). A
      small lock glyph in the icon column is the clearest hint — decide scope.

**Exit criteria:** A `readonly: true` light/switch/lock/cover/climate row shows
live state but never actuates on tap or long-press (no service call, no confirm
sheet, no detail modal); a readonly numeric sensor still opens its history sheet
and draws its UE8 sparkline; `tap()` returns false for readonly entities; rows
without the flag are unchanged. Compiles + links clean.

**Risks / unknowns:**
- **Silent dead tap feels broken.** Without a visual cue, a readonly control row
  looks identical to a live one but does nothing — ship at least a minimal
  affordance (dim / lock glyph) or users will think it's frozen.
- **Miss a dispatch path.** The hard gate in `tap_entity_` is the safety net;
  make sure every service-firing path ultimately routes through it (the
  immediate media/cover/fan buttons in the detail modal don't — but the modal
  never opens for a readonly row, so they're unreachable; confirm that holds).
- **`confirm` + `readonly` interaction** — readonly must win; add a test/inspect
  pass for a row carrying both.

---

## UE10 — User-editable idle timeouts + reset-source selection + burn-in guard

**Status:** 🧪 code complete — compile + on-device validation pending · target tag:
`ue10-screen-protection`

**Decisions taken:** slider control (5 s steps, 0–600 s; 0 = "Never" for dim/blank;
sleep min 5 s). `is_amoled` is declared **board-only** (`waveshare-2.16.yaml` =
`"true"`); the main YAML references `${is_amoled}` and the C++ default is `false`,
so a future board that forgets the substitution gets the safe no-warning path
(top-level default omitted on purpose — ESPHome main-file substitutions override
package ones, which would have inverted the board's value). Sleep stays gated on
the blank tier (enter from `s == 2`), preserving P8 exactly.

**Why:** The dim / blank / sleep timings are baked into compile-time
substitutions ([packages/idle.yaml:22-29](packages/idle.yaml#L22-L29)) and the
settings sheet only *displays* them as a dead read-only label
([ha_panel.cpp:942-946](components/ha_panel/ha_panel.cpp#L942-L946), comment:
"read-only display — substitution-driven, no runtime edit yet"). Users can't
retune how aggressively the panel dims, blanks, or sleeps without reflashing.
This phase makes those three timings editable + persisted, lets the user choose
which inputs (touch / motion) reset the screen, and — because turning all of it
off invites AMOLED burn-in — warns on Apply when no protection is left, but only
on boards that actually have an AMOLED panel.

**Config/infra, not a widget swap** — like UE6 it leaves the "drop-in LVGL
widget" theme. Still inside the "no HA-side changes" cross-cutting rule: pure
on-device settings, no new states or services.

**Defaults = today's hard-coded values** (so nothing changes until the user
edits): dim after **15 s**, blank after **45 s total**, sleep after **60 s**,
both reset sources (touch + motion) **on**, sleep **enabled**. Seed every new
global's `initial_value` from the existing substitution so a fresh flash behaves
exactly as it does now.

**Pairs with / reuses (existing):**
- Idle state machine substitutions + interval lambda
  ([idle.yaml:20-41](packages/idle.yaml#L20-L41),
  [:164-191](packages/idle.yaml#L164-L191)) — the dim/blank/sleep math to be
  driven from globals instead of `${...}`.
- `notify_input` script ([idle.yaml:96](packages/idle.yaml#L96)) and its two call
  sites: touch `on_touch`
  ([waveshare-2.16.yaml:191-198](boards/waveshare-2.16.yaml#L191-L198)) and IMU
  `on_press` ([waveshare-2.16.yaml:136-138](boards/waveshare-2.16.yaml#L136-L138))
  — the reset-source gate sits here.
- The **sleep settings** persist+commit pattern as the exact template:
  `set_sleep_committer` / `set_sleep_settings` / staged
  `apply_sleep_`+`revert_sleep_`+`*_dirty_`
  ([ha_panel.cpp:1816-1875](components/ha_panel/ha_panel.cpp#L1816-L1875),
  wired at [ha-amoled-panel.yaml:47-55](ha-amoled-panel.yaml#L47-L55)). UE10 adds
  parallel committers/setters the same way.
- Brightness **slider + live value label** as the editable-control idiom
  ([ha_panel.cpp:913-927](components/ha_panel/ha_panel.cpp#L913-L927)).
- `confirm_sheet_` (Proceed/Cancel overlay) for the burn-in warning, and
  `update_sleep_mode_enabled_`'s grey-when-disabled pattern
  ([ha_panel.cpp:1833-1844](components/ha_panel/ha_panel.cpp#L1833-L1844)) for the
  sleep-timeout control.

---

### Part A — make dim / blank / sleep timeouts runtime globals

**Change:** Promote the three substitutions to `restore_value: yes` globals
(mirroring `active_brightness_g` / `sleep_enabled_g`), seed each `initial_value`
from the current substitution, and read the globals in the interval lambda.

- `dim_timeout_g` (uint16_t, sec) ← `${dim_timeout_s}` = 15
- `blank_timeout_g` (uint16_t, sec) — **store TOTAL blank time** to match the
  UI's "Blank after N total". Seed = 45 (today's 15 + 30). Drop the additive
  `blank_at = dim_at + ...` at [idle.yaml:174](packages/idle.yaml#L174); compute
  `blank_at = blank_timeout_g * 1000` directly. (Decouples the two so editing
  dim doesn't shift blank.)
- `sleep_timeout_g` (uint16_t, sec) ← `${sleep_timeout_s}` = 60

**Disable semantics:** value `0` = that tier never fires. Interval guards become
`if (dim_g != 0 && since >= dim_at)` etc. Lets the user turn dim and/or blank
fully off (the input to the burn-in check in Part D). Sleep already has its
`sleep_enabled_g` master gate; `sleep_timeout_g` just sets the delay.

**Validation (clamp on Apply, in the committer or `apply_timeouts_`):** when a
tier is enabled (non-zero), keep the ordering sane — `dim ≤ blank_total ≤
sleep`. Clamp rather than reject so the UI never wedges. Document the chosen
min/max range (suggest 5–600 s; 0 = Never for dim/blank).

Tasks:
- [x] Add the three globals to [idle.yaml](packages/idle.yaml#L43) (`restore_value:
      yes`, seeded from the existing substitutions; keep the substitutions as the
      `initial_value` source so the default literally *is* today's value).
      `blank_timeout_g` seeded `${dim_timeout_s} + ${blank_timeout_s}` = 45 total.
- [x] Rewrite the interval lambda
      ([idle.yaml:169-190](packages/idle.yaml#L169-L190)) to read the globals and
      honor the `0 = disabled` guards; evaluated deepest-first so a disabled dim
      jumps active→blank.
- [x] Add `set_timeouts_committer(std::function<void(uint16_t dim, uint16_t
      blankTotal, uint16_t sleepSec)>)` + `set_timeout_settings(dim, blankTotal,
      sleepSec)` seeder on `HAPanel` (template: the sleep committer/setter).
- [x] Wire both in [ha-amoled-panel.yaml](ha-amoled-panel.yaml#L47) `on_boot`:
      committer writes the three globals; seeder pushes the restored globals into
      the settings controls (mirror the `set_sleep_*` block).

### Part B — editable controls in the settings sheet

**Change:** Replace the read-only `to_dim` label
([ha_panel.cpp:942-946](components/ha_panel/ha_panel.cpp#L942-L946)) with three
editable rows under the existing "Power saving & burn-in protection" heading,
each staged like brightness (`staged_*`, `*_dirty_`, `apply_*`, `revert_*`).

**DECIDE the input control (pick before coding):**
- **Slider + live "N s" label (recommended).** Matches the brightness slider
  already in this sheet; one `lv_slider` per timeout, value label beside it,
  min position = `0`/"Never" for dim & blank. Lowest new-code, consistent idiom.
- **+/- stepper buttons.** More precise for discrete seconds, but new widget
  plumbing (two buttons + label per row) not used elsewhere here.
- **`lv_roller`.** Compact discrete picker; UE1 already enabled `LV_USE_ROLLER`.
  Good for a fixed value list (5/10/15/30/45/60/120/Never) but heavier to wire.

Recommend the slider for parity; revisit if fingertip precision on the 480-round
panel proves fiddly on-device.

Tasks:
- [x] Three labeled rows: "Dim after", "Blank after (total)", "Sleep after",
      each with a 5 s-step slider + a live seconds label (0 renders as
      "Never" for dim/blank; sleep min 5 s).
- [x] Grey/disable the **Sleep after** control when the existing "Sleep when
      idle" switch is off — reuse `update_sleep_mode_enabled_`'s
      disable+`LV_OPA_50` treatment (extended to the sleep-after slider).
- [x] Stage edits + add `apply_timeouts_` / `revert_timeouts_` and hook them into
      the settings Apply / Cancel handlers alongside `apply_sleep_` etc.
      `apply_timeouts_` clamps `dim ≤ blank ≤ sleep` on commit.

### Part C — choose what resets the screen (touch / motion / both / none)

**Change:** Two persisted bool globals gate the two `notify_input` call sites so
the user picks which inputs un-dim/un-blank the screen.

- `reset_on_touch_g` (bool, default **true**)
- `reset_on_motion_g` (bool, default **true**)

Gate by wrapping each board call site in an `if` on its global (touch `on_touch`
→ `reset_on_touch_g`; IMU `on_press` → `reset_on_motion_g`), or split
`notify_input` into `notify_input_touch` / `notify_input_motion` thin wrappers
that check the gate then delegate. Either source independently selectable → all
four combinations (both / touch / motion / none).

**⚠️ Scope boundary — this is NOT the sleep-wake control.** The gate sits only at
`notify_input`, which drives the dim/blank **idle timer**. Waking from a full
*sleep* tier is separate hardware/IDF: deep sleep wakes on the touch INT pin
(`deep_sleep.wakeup_pin` GPIO11,
[waveshare-2.16.yaml:164-168](boards/waveshare-2.16.yaml#L164-L168)) and light
sleep blocks in `ha_power::enter_light_sleep()` until a touch
([idle.yaml:156](packages/idle.yaml#L156)). So disabling "touch resets screen"
still lets touch wake the panel from sleep — by design, per the request ("not
the wake from sleep option"). Call this out in the UI copy / DEVELOPMENT.md so
it doesn't read as a bug.

Tasks:
- [x] Add the two globals to [idle.yaml](packages/idle.yaml#L43)
      (`restore_value: yes`, default true).
- [x] Gate the touch + IMU call sites in
      [waveshare-2.16.yaml](boards/waveshare-2.16.yaml#L136) on the respective
      global (wrapped each `script.execute: notify_input` in an `if:` lambda).
- [x] UI: two switches ("Touch resets screen", "Motion resets screen") + a
      caption ("Sleep still wakes on touch") in the settings sheet — staged +
      committed via a new `set_reset_sources_committer` / `set_reset_sources`
      pair (sleep-committer template).

### Part D — AMOLED burn-in warning when all protection is off

**Trigger:** On settings **Apply**, if dim is disabled (`0`) **and** blank is
disabled (`0`) **and** sleep is disabled (`!sleep_enabled_`) — i.e. the screen
will sit at full brightness indefinitely — **and** the board is AMOLED, show a
confirm overlay before committing: warn about permanent burn-in, with
**Proceed** (commit anyway) / **Cancel** (return to settings). Non-AMOLED boards
skip the warning entirely (LCD/IPS don't burn in the same way). Reset-source
"none" is a *separate* usability footgun (see Risks), not part of this burn-in
gate.

**AMOLED flag (per-board groundwork):** add `bool is_amoled_{false}` +
`set_is_amoled(bool)` to `HAPanel`. The board package declares the screen type;
forward it from `on_boot` like the other setters. Default **false** is the safe
choice (no warning) for any future board that forgets to set it.
- `boards/waveshare-2.16.yaml` declares `is_amoled: "true"` (it's a CO5300
  AMOLED, [waveshare-2.16.yaml:83-86](boards/waveshare-2.16.yaml#L83-L86)).
- A `defaults:`/top-level substitution `is_amoled: "false"` guards boards that
  don't override it (future-board phase will set their own).

Tasks:
- [x] Add `is_amoled_` + `set_is_amoled` to
      [ha_panel.h](components/ha_panel/ha_panel.h#L150) / `.cpp`; forward the board
      `${is_amoled}` substitution from
      [ha-amoled-panel.yaml](ha-amoled-panel.yaml#L41) `on_boot`.
- [x] Set `is_amoled: "true"` in
      [waveshare-2.16.yaml](boards/waveshare-2.16.yaml). Declared **board-only**
      (no top-level default) so it resolves unambiguously; the C++ `false` default
      covers a future board that omits it (a top-level default would override the
      board because main-file substitutions win in ESPHome).
- [x] In the settings Apply path, detect "no protection" + `is_amoled_` and route
      through the `confirm_sheet_` warning (`open_burnin_warning_`); Proceed
      (`on_burnin_proceed_`) → `commit_settings_`; Cancel / bg-tap → back to the
      settings sheet, nothing committed.

**Exit criteria:** Settings sheet lets the user edit Dim-after, Blank-after-total
and Sleep-after seconds (0 = Never for dim/blank); values persist across reboot
and take effect live; the Sleep-after control greys out when sleep is off. Touch
and Motion are independently selectable as screen-reset sources (both/one/none),
persisted, and gating them does **not** affect sleep-wake. Applying with dim +
blank + sleep all disabled pops a burn-in warning on the AMOLED board (and only
there); Proceed commits, Cancel doesn't. Compiles + links clean; defaults match
today's behavior with no user edits.

**Risks / unknowns:**
- **Slider precision** on a round 480 panel for a wide seconds range — the
  recommended slider may need coarse snapping (5 s steps) or the roller fallback.
  Tune on-device.
- **Reset "none" footgun.** Disabling both reset sources means a dimmed/blanked
  screen can only be revived by the sleep-wake path (or reboot) — touch won't
  un-dim it. Not a burn-in issue, but confusing; consider a secondary
  caution (out of this phase's required scope) or at least document it.
- **Blank semantics migration.** Changing `blank_timeout_g` from "additional"
  (idle.yaml's `blank_timeout_s`) to "total" must update the interval math
  (drop the `dim_at +` add) — a silent off-by-one here shifts every blank time.
- **`restore_value` of a re-typed global.** New globals start fresh; verify a
  device flashed from an older build doesn't read stale NVS into the new keys
  (use distinct global ids, not reused ones).
- **Validation ordering.** If the user sets blank < dim (or sleep < blank), clamp
  on Apply rather than letting the interval skip a tier silently.

---

## UE11 — Page-picker count badges

**Status:** 🧪 code complete — compiles + links clean (RAM 16.8%, Flash 20.9%);
on-device validation pending · target tag: `ue11-picker-badges`

**Scope note (delivered):** a page may declare **one badge OR a list** of badges
(`picker_badge: [lights_on, climate_active]`), shown stacked horizontally and
right-aligned; the page name keeps its left alignment + ellipsizes. A **quiet**
badge (0/empty) stays visible but **dimmed grey** — its presence signals "actively
monitoring", its grey "nothing to report" — brightening to its semantic colour
when notable; a badge **hides (and collapses out of the flex bar) only when
nothing is in scope to monitor** (`total == 0`). The config-free types ship live;
the `device_class` / numeric types parse + evaluate but stay hidden until UE7
subscribes `device_class` (no in-scope entities → hidden, not dim). A **local evaluator**
(`eval_picker_badge_`) is used rather than UE12's `compute_report_` — the badge
predicates are mostly *negative* (`unlocked` = not locked, `open_covers` = not
closed, `climate_active` ≠ off) which the report `{domains, match_state}` filter
can't express; the divergence is noted for a future shared-helper pass.

**Why:** The page picker
([build_ui_:1484-1502](components/ha_panel/ha_panel.cpp#L1484-L1502)) lists page
names only — to know "is anything on over there" the user has to open each page.
A small count + icon to the right of each page name ("🔆 3" = three lights on)
turns the picker into an at-a-glance house overview. Cheapest item in this plan:
no new widget (so no UE1 build-config work), no new HA data, and — because the
picker is a transient modal — no live update path.

**Cheap because the picker is built-once and shown on demand.** The picker is a
hidden overlay (`picker_`) built once in `build_ui_`; tapping the header calls
[open_picker_()](components/ha_panel/ha_panel.cpp#L1560), which just clears the
HIDDEN flag and raises it. That open call is the **only** recompute point needed
— counts are computed fresh each time the picker is shown, so there's no
`on_state_` wiring and no per-frame cost. Entity states are already current at
open time (API subscriptions stay live whether or not the picker is visible), so
no fetch is required.

**Config/UI polish, no HA-side change** — badges aggregate only over panel-known
entities, the same v1 scope as UE12-A.

**Design: per-page config — the page declares its badge.** Each page picks **one**
badge via a new page-level `picker_badge:` key (default omitted = no badge, so
existing configs render unchanged — same opt-in convention as `size` / `confirm`
/ `realtime`). Per-page selection sidesteps the "one slot, which metric wins"
problem entirely: a Living-Room page shows lights-on, a security page shows
open-doors, a server page shows offline-count — each page's most useful glance.

`picker_badge:` accepts either a bare **type name** or a block when a type takes
parameters:

```yaml
pages:
  - name: "Living Room"
    picker_badge: lights_on            # bare form
    entities: [ ... ]
  - name: "Climate"
    picker_badge: { type: temperature, agg: avg }   # block form (params)
    entities: [ ... ]
  - name: "Batteries"
    picker_badge: { type: low_battery, threshold: 20 }
    entities: [ ... ]
```

### Badge-type menu (the selectable values)

Grouped by data requirement, because that sets which ship in v1. Each renders as
**icon + short value**, hidden when the value is 0 / empty (configurable per type).

**Config-free — domain + state only (v1):**
- `lights_on` — `light` in `on` → 🔆 N
- `devices_on` — switch / fan / input_boolean (rest of `BINARY_SWITCH`) in `on` → 🔌 N
- `unlocked` — `lock` not `locked` → 🔓 N
- `open_covers` — `cover` not `closed` → ▤ N
- `media_playing` — `media_player` == `playing` → ▶ N
- `climate_active` — `climate` state ≠ `off` → 🔥 N
- `running` — `script` / `automation` `on`, active `timer` → ⏱ N
- `offline` — matched entity `unavailable` / `unknown` → ⚠ N
- `entities` — total entities on the page (no state filter) → N

**Needs `device_class` (depends on UE7's connect-time attr subscription — flag it;
leaves the strict no-new-data line exactly as UE12's device_class types do):**
- `open_doors` — binary_sensor `door` / `window` / `garage_door` = `on` → 🚪 N
- `motion` — binary_sensor `motion` / `occupancy` / `presence` = `on` → 👣 N
- `low_battery` — `battery` device_class below `threshold:` (default 20) → 🔋 N
- `alarm` — `smoke` / `moisture` / `co` / `gas` / `problem` / `safety` = `on` →
  🔴 (red **presence dot**, no number)

**Numeric aggregate — env value (needs numeric parse; `device_class` to pick the
right sensor):**
- `temperature` — `agg:` avg | min | max of temp sensors → 🌡 22°
- `humidity` — avg humidity → 💧 45%
- `power` — sum of power sensors → ⚡ 1.2kW (`unit:` override)
- `co2` / `aqi` — value, or over-`threshold:` flag

**Composite (collapse many signals to one glyph):**
- `severity` — OR(`alarm`, `open_doors`, `offline`) → red / amber / none dot, no
  number. The "anything wrong over there?" badge.
- `idle` — ✓ when nothing on / open (calm-state indicator)

v1 ships the **config-free** group + the schema for the rest; `device_class` and
numeric types light up once UE7's `device_class` subscription / UE12's
filter+aggregation helper land (sequencing note below). An unknown
`picker_badge:` value is a **compile-time error** (strict `cv.one_of`, like
`size`), so a typo can't silently no-op.

**Pairs with / reuses (existing):**
- Picker row build loop
  ([:1484-1502](components/ha_panel/ha_panel.cpp#L1484-L1502)) — each row already
  stores its page index in `user_data` and maps to `pages_[pi].entity_indices`.
- [open_picker_()](components/ha_panel/ha_panel.cpp#L1560) — the recompute hook.
- `Entity::domain` + `Entity::state` for the predicate; the `BINARY_SWITCH`
  domain group (light/switch/fan/input_boolean); `state_to_value_` for numeric agg.
- MDI glyph font (`mdi_lv_font`) + `resolve_icon_` domain defaults for the badge
  icon — bulb/lock/etc are already in the baked subset (new glyphs may need
  `tools/build-mdi-glyphs.py`).
- **UE12's `{domains, device_class, match_state}` filter + aggregation helper** —
  badge types are the same computation as UE12 reports over a different surface.
  Whichever phase lands first owns the helper; the other calls it (don't fork two
  count paths).
- **UE7's `device_class` subscription** — prerequisite for the device_class group.

**Change:** Add a page-level `picker_badge:` config → a `PickerBadge` spec on
`Page` (type enum + optional agg/threshold/unit). In the picker row loop add a
right-aligned badge `lv_label` per row (store handles in a `picker_badges_`
vector). Add `update_picker_badges_()` that, for each page, evaluates its badge
spec over `pages_[pi].entity_indices`, sets the icon+value text (or hides it),
and call it from the top of `open_picker_()`.

Tasks:
- [x] **Config plumbing.** Added `CONF_PICKER_BADGE` + `PICKER_BADGE_SCHEMA`
      (`cv.ensure_list(_picker_badge)` — bare type name *or* `{type, agg,
      threshold, unit}` block, single *or* list) to
      [__init__.py](components/ha_panel/__init__.py), at **page** level in
      `PAGE_SCHEMA`. Emits `add_page_badge(...)` per badge (append to the page
      just added). `BADGE_TYPES` / `BADGE_AGGS` are the validation source of
      truth; an unknown type is a compile error (`cv.one_of`).
- [x] **Model.** `enum class BadgeType` + `BadgeAgg` + `PickerBadge` struct;
      `std::vector<PickerBadge> badges` on the `Page` struct
      ([ha_panel.h](components/ha_panel/ha_panel.h)).
- [x] **Badge widget.** Per picker row, a right-aligned flex **bar**
      (`LV_ALIGN_RIGHT_MID`, -12 x) holds one [icon][value] **group** per badge;
      handles in `std::vector<std::vector<lv_obj_t*>> picker_badges_`. The
      page-name label is capped (`max_width 300`) + `LONG_DOT` so it can't run
      under the bar; hidden groups collapse out of the flex layout.
- [x] **Evaluator.** `eval_picker_badge_()` dispatches on `BadgeType` over the
      page's own `entity_indices` (count / numeric-agg / severity / idle),
      returns icon name + value + colour or "hide"; `update_picker_badges_()`
      paints each group. Colours: neutral counts, amber active (doors/motion),
      red alarm/severity/offline/low-battery, green `idle`. Local evaluator (not
      UE12's helper) — negative predicates don't fit `match_state` (noted above
      + in DEVELOPMENT.md).
- [x] **Hook.** `update_picker_badges_()` called at the top of `open_picker_()`
      before the unhide, so every open reflects current state.
- [x] **Docs** (see below).

**Docs to update (so the available badge types are discoverable):**
- [x] **`packages/ha-entities.example.yaml`** — added a `# Page picker badges
      (UE11): ...` block listing **every** type + the bare/list/block forms +
      params (agg / threshold / unit) + the default (omitted = none), and live
      `picker_badge:` lines on four example pages (bare single, block single, and
      two list forms).
- [x] **`README.md`** — (1) a **Navigation & UI** Features bullet; (2) the
      **Defining your pages** inline YAML example gains a `picker_badge:` line +
      a page-keys-table row; (3) a full **Page picker badges** reference section
      (every type, bare/list/block forms, the device_class-gated note).
- [x] **`components/ha_panel/__init__.py`** — `BADGE_TYPES` / `BADGE_AGGS` lists
      + a doc-comment above `CONF_PICKER_BADGE` (developer-facing; doubles as the
      compile-time validation that rejects unknown values).
- [x] **`DEVELOPMENT.md`** — phase log entry (badge-type table + per-page-config
      + list-of-badges + local-evaluator/UE12-divergence note).

**Exit criteria:** A page can declare `picker_badge: <type>`; opening the picker
shows that page's icon+value to the right of its name, computed live (correct on
every open), hidden when zero/empty, and not overlapping long names. All
config-free types work in v1; device_class / numeric types either work (if UE7 /
UE12 helpers are in) or are clearly gated. An unknown `picker_badge:` value fails
at compile time. Pages with no `picker_badge:` show no badge and render exactly as
today. Every selectable type is documented in `ha-entities.example.yaml` (+ the
README pointer). Compiles + links clean.

**Risks / unknowns:**
- **Row width on the 480-round panel.** Page name (left) + badge (right) on a
  460 px row; long names collide. Right-align the badge, ellipsize / margin the
  name. One badge per page (the per-page-config model already enforces this).
- **Colour semantics.** Counts are informational, not alarms — keep `*_on` /
  `entities` / env neutral; reserve red for `alarm` / `severity` / `offline`,
  green for `idle` (same trap flagged in UE4 / UE7 / UE12).
- **device_class / numeric types are gated on UE7 + UE12.** Don't promise the full
  menu in the first commit — ship config-free types, schema-validate the rest, and
  enable them as the shared helpers land. Document which are live.
- **Stale if shown indefinitely.** ~~Badges refresh on open only~~ — **resolved:**
  `update_picker_badges_()` runs on open AND from `on_state_` while the picker is
  visible (`!HIDDEN` guard), so an open picker tracks state live; closed = zero-cost.
- **Helper drift vs UE12.** Shipping before UE12 with a local evaluator risks two
  count paths — make the later refactor actually collapse them.
- **Glyph coverage.** Some badge icons (door, motion, battery, leak) may not be in
  the current baked MDI subset; add them via `tools/build-mdi-glyphs.py` or fall
  back to a generic glyph.

---

## UE12 — Report rows (computed aggregates as a row type)

**Status:** ✅ done — validated on-device (count / bool / offline / sum / avg /
min / max report rows render live + recompute on state change; page + all
scope; honour `size:`; inert on tap). Compiles + links clean (RAM 16.6%, Flash
19.6%). · target tag: `ue12-report-rows`

**Why:** Every row today maps to exactly one HA entity
([build_ui_ row loop](components/ha_panel/ha_panel.cpp#L1373-L1404) walks
`pages_[pi].entity_indices` → one `make_entity_row` per entity). There is no way
to show a *summary* of many entities at once — "how many lights are on", "is
anything still open", "what's the warmest room". Those are exactly the
glance-value lines a wall panel wants at the top of a page. UE12 adds a **report
row**: a row whose text is computed locally by scanning the entities the panel
already subscribes to, with no extra HA state and no new widget — it reuses the
read-only text row verbatim.

**This is config + a render class, not a widget swap.** It introduces no new
LVGL widget type (so no UE1-style build-config work); it adds a new
`RenderClass` and a small aggregation pass. Like the other items it stays inside
the **"no HA-side changes"** rule for v1 (see the data-universe DECIDE below).

---

### Step 1 — DECIDE the data universe (do this first)

A report can only aggregate over states the panel actually has. Pick the scope:

- **A — panel-known entities only (LEAN / recommended, config-free).** Aggregate
  over `entities_` — everything already listed under any page's `entities:`. No
  new subscription, no new HA data, fits the cross-cutting rule exactly. "Lights
  on" = lights *you put on the panel*, which for a wall panel is usually the
  intended set anyway. **Pick this for v1.** Limitation: it can't count lights
  you never added as rows.
- **A′ — A + track-only entities (small extension).** Let a report declare its
  own `entities:` / `filter:` that get **subscribed but not rendered as rows**
  (added to `entities_`, never pushed to a page's `entity_indices`). Bridges A's
  gap — count things you don't want as visible rows — still no HA-side change,
  just more `subscribe_homeassistant_state` calls. **Watch the connect-time TX
  burst** (the P7d/P7e saturation lesson); keep the track-only set small.
- **B — all HA entities of a domain (out of v1).** True "count *every* light in
  HA" needs a new data source: an HA template sensor exposing the counts, or a
  REST `/api/states` scan on the UE6 worker. This **leaves the no-new-HA-data
  scope** (same boundary UE7/UE10 flagged). Defer; note it as the follow-up.

**Decision:** ship **A** (optionally A′ if a real need appears); B is a separate,
flagged phase.

### Step 2 — DECIDE the row model

A report is **not** an HA entity, but it should look and lay out like a row.
Cleanest path that reuses the most code:

- **Synthetic entity (recommended).** Represent a report as an `Entity` with a
  reserved `domain == "report"` and a new `RenderClass::REPORT_TEXT`. Its
  `state` field holds the *computed* string; it has no HA subscription. It slots
  straight into `entity_indices` → the `build_ui_` loop → `make_entity_row`, so
  it inherits row geometry, `size:`, the icon column, press/no-press styling for
  free — mirroring how UE5's LED and UE8's sparkline rode existing row plumbing.
  `rebuild_entity_row_`'s text/colour path renders it; a new
  `recompute_reports_()` fills `state` before the rebuild.
- *Rejected:* a separate `reports:` list / separate widget — more plumbing,
  loses row reuse, and the user asked for these to live "on rows on a page".

### Step 3 — the report-type menu (what a report can compute)

This is the menu of `type:`s. **v1** = the user's core ask (counts) plus the
cheap high-value status/aggregate ones; the rest are flagged as follow-ups so the
schema is designed once to fit them.

**Counts & status** (over filtered entities):
- `count` *(v1)* — N entities matching `{domains[], device_class, match_state[]}`.
  Covers the whole original ask: **lights on** (`domains:[light]
  match_state:[on]`), **lights off**, **total entities** (no filter), **total
  light entities** (`domains:[light]`), **total X-type** (any domain/class).
  Optional `show_total: true` renders **"3 / 5"** (matched / in-scope).
- `bool` *(v1)* — collapses a count to a glance flag with colour: **"All lights
  off ✓"** (green) vs **"2 on"** (accent), **"All closed"** vs **"1 open"**
  (amber). This is where colour belongs (see the colour-semantics risk).
- `offline` *(v1, high value)* — count of matched entities whose state is
  `unavailable`/`unknown`. Surfaces a dead Zigbee/Wi-Fi device or a broken
  integration at a glance. Pure config-free win.
- `security` *(follow-up)* — composite "house secure": doors/windows
  (binary_sensor `door`/`window`/`garage_door` = on), locks unlocked, covers
  open → one red/green line. Leans on the UE7 `device_class` classifier.
- `active` *(follow-up)* — motion/occupancy/presence currently detecting.

**Numeric aggregates** (over numeric `sensor`s in the filter):
- `sum` *(v1)* — e.g. **total power draw (W)** across power sensors, **energy
  today (kWh)**. Carries a `unit:`; skips non-numeric / unavailable samples.
- `avg` / `min` / `max` *(v1 for `min`/`max`/`avg`)* — **average indoor temp**;
  **warmest / coldest room**. `show_source: true` appends the extreme entity's
  name ("Warmest: Office 24°") — uses `friendly_name`, already stored.
- `low_battery` *(follow-up)* — lowest battery % + count below a `threshold:`
  ("2 low ≤20%"). Great maintenance glance; needs `device_class: battery`.

**Activity / time** (slightly out of strict no-new-data, flag each):
- `last_changed` *(follow-up)* — most-recently-changed matched entity + ago
  ("Last: Front door 2m"). Needs a `last_changed` timestamp the panel doesn't
  track today; flag like UE7's `device_class`.

**Panel self-diagnostics** (zero HA dependency — the panel's *own* state):
- `diagnostic` *(follow-up, neat)* — uptime, Wi-Fi RSSI, free heap, HA-link
  status, current brightness. Fed by ESPHome's own sensors / `WiFi` / `App` via
  setters (like the history-http/time setters), **not** from HA. Genuinely
  useful on a wall panel and fully config-free.

**Action (deliberately deferred):** a report row whose tap acts on its matched
set ("Lights: 3 on" → tap = `light.turn_off` all). Powerful, but it's an action
surface that collides with **UE9 `readonly`** + the confirm sheet; keep reports
**view-only in v1** and revisit as its own phase.

---

### Pairs with / reuses (existing)

- Entity/page pipeline: [add_page](components/ha_panel/ha_panel.cpp#L30) /
  [add_entity](components/ha_panel/ha_panel.cpp#L36), the per-page
  [row build loop](components/ha_panel/ha_panel.cpp#L1373-L1404), and
  [make_entity_row](components/ha_panel/ha_panel.cpp#L744) — a synthetic entity
  rides all of it unchanged.
- [rebuild_entity_row_](components/ha_panel/ha_panel.cpp#L491) read-only
  text/colour path renders the computed string (new `REPORT_TEXT` arm).
- [on_state_](components/ha_panel/ha_panel.cpp#L449) is the recompute trigger —
  any subscribed entity changing re-runs the aggregation.
- `EntitySize` / [row_metrics_for](components/ha_panel/ha_panel.cpp#L732) so
  reports honour `size: small|medium|large` like every other row.
- `Entity::domain` (already split on `.`) + `Entity::state` drive the filter and
  the count/min/max math; numeric reads reuse `state_to_value_`.
- Config-plumbing precedent: `CONF_CONFIRM` / `CONF_SIZE` and the
  `ENTITY_SCHEMA` / `PAGE_SCHEMA` shape in
  [__init__.py](components/ha_panel/__init__.py#L75-L99).
- The UE7 `device_class` classifier (if/when it lands) powers `device_class`
  filters and the `security` / `low_battery` types.
- **UE11 page-picker badges** are the same count over a different surface — the
  `{domains, match_state}` filter routine should be shared (whichever lands first
  owns it; the other calls it).

### Change

Add a `report:` block as an alternative to `entity_id:` in a page's `entities:`
list (validate **exactly one** of the two per row). At codegen emit an
`add_report(...)` that pushes a synthetic `Entity` (`domain="report"`,
`render_class=REPORT_TEXT`, holding the parsed spec). At runtime a
`recompute_reports_()` scans `entities_`, computes each report's string + colour
into its `state`, and `rebuild_entity_row_` paints it.

Illustrative YAML (final shape per Step 2/3 DECIDE):

```yaml
pages:
  - name: Home
    entities:
      - report: { type: count, title: "Lights on",
                  domains: [light], match_state: [on], show_total: true }
        size: medium
      - report: { type: offline, title: "Offline" }
      - report: { type: min, title: "Coldest", domains: [sensor],
                  device_class: temperature, show_source: true, unit: "°" }
      - entity_id: light.living_room      # normal rows unchanged
```

Tasks:
- [x] **Config plumbing.** Added `CONF_REPORT` + `REPORT_SCHEMA`
      (`type` enum, `title`, optional `domains`/`match_state`/`device_class`/
      `unit`/`show_total`/`show_source`) in
      [__init__.py](components/ha_panel/__init__.py); `ENTITY_SCHEMA` now wraps
      `cv.has_exactly_one_key(CONF_ENTITY_ID, CONF_REPORT)`. Lists pass to C++
      comma-joined (re-split with `parse_ha_list_`); a bare-`on`→bool YAML quirk
      is mapped back in the `match_state` validator. Emits `add_report(...)`.
- [x] **Model.** Added `RenderClass::REPORT_TEXT` + `ReportType` enum + a
      `ReportSpec` (filter + format flags) on `Entity` in
      [ha_panel.h](components/ha_panel/ha_panel.h). `add_report` builds a
      synthetic `Entity` (`domain="report"`, no entity_id) and appends it like a
      normal row; skipped in the state-subscription loop.
- [x] **Aggregation engine.** `recompute_reports_()` + `compute_report_()` scan
      `entities_` by `{domains, device_class}`, then compute per `type` (count /
      bool / offline / sum / avg / min / max). Reports excluded from their own
      scan; numeric types skip `unavailable`/non-numeric; empty set renders em-dash
      (no div-by-zero). `min`/`max` track the extreme entity for `show_source`.
- [x] **Render.** `REPORT_TEXT` folds into `make_entity_row`'s label arm (title
      left, value right). `recompute_reports_` paints text + colour; counts/numeric
      neutral, `bool`/`offline` coloured (green/amber/red). `rebuild_entity_row_`
      REPORT_TEXT arm is a no-op (recompute owns painting).
- [x] **Recompute hook.** Called from `on_state_` after any entity updates, and
      once after the build loop for the initial paint. v1 recomputes **all** report
      rows per state change (bounded); per-domain indexing left as the future lever.
- [x] **No-tap.** `REPORT_TEXT` added to `tap_entity_` (returns false); the
      `on_entity_row_clicked_` branches don't match `domain=="report"` → tap inert;
      no long-press cb registered. Pressed-bg flash disabled in `make_entity_row`.
- [x] **Docs.** README Features bullet + Defining-your-pages example;
      `ha-entities.example.yaml` schema block + live example rows; DEVELOPMENT.md
      Phase UE12 log. v1 universe (panel-known) + deferred `type:`s documented;
      `device_class` parsed but inert until UE7.

**Exit criteria:** A page can carry report rows that show, live and correct: "N
lights on" (with optional "N / M"), total entities, total of a given domain, an
"all off / X on" boolean with colour, an offline-entity count, and min/max/avg
of a numeric sensor group (with the extreme entity's name when asked). Reports
recompute when any underlying entity changes, honour `size:`, never fire a
service on tap, and pages with no report rows render byte-for-byte as today.
Compiles + links clean; no heap regression.

**Risks / unknowns:**
- **Universe is the configured set, not all of HA.** Set expectations in docs;
  "lights on" counts panel lights. A′ (track-only) or B (template sensor) are the
  escape hatches — don't silently imply a whole-home count.
- **Colour semantics trap (same as UE4 gauge / UE7 LED).** "3 lights on" isn't
  *bad* — don't auto-red counts. Reserve colour for `bool`/threshold/`offline`
  where good/bad is well-defined; keep `count`/numeric neutral, with an optional
  `alert_when:` if a user wants a count to alarm.
- **Recompute cost.** O(reports × entities) on every state change — fine for
  tens, watch it if a config grows large or has a 1 Hz `realtime` sensor; the
  domain-index optimisation is the lever.
- **Numeric hygiene.** Mixed units (W vs kW) silently sum wrong; skip
  non-numeric/unavailable; empty match set must render "—" not NaN/0-div.
- **A′ connect-time TX.** Track-only subscriptions re-introduce the connect
  burst (P7d iter-1 `Buffer full`); scope strictly and stagger if needed.
- **Schema union validation.** `entity_id` xor `report` must be a clean
  compile-time error, not a confusing runtime no-op.

---

## UE13 — Per-row text styles (bold / italic / underline)

**Status:** ✅ done — validated on-device (bold / italic / underline + combos
render on the row title at small/medium/large; unstyled rows unchanged).
Compiles + links clean (firmware ~+100 KB for the baked fonts). · target tag:
`ue13-row-text-styles`

**Why:** Rows had one fixed weight. A `style:` per row lets a title stand out
(bold a report total, italicise a caption, underline a header-ish row). Applies
to the row's **name/title** label (entity rows + report rows alike).

**Key constraint — LVGL has no runtime bold/italic.** Weight/slant in LVGL is a
*font swap*, and the built-in `lv_font_montserrat_*` are regular-only, so
bold/italic need **baked font data**; only **underline** is a runtime
`lv_text_decor`. So the feature spans two layers: a free LVGL decor (underline)
+ ESPHome-baked Montserrat Bold/Italic at the three row sizes (bold/italic).

**Change:** `style: [bold, italic, underline]` (list, any combination) on a row.
- `Entity::name_style` uint8 bitmask (`STYLE_BOLD/ITALIC/UNDERLINE`).
- `make_entity_row` takes `name_font_override` + `name_underline`; the build loop
  computes them via `resolve_name_font_(e)` (style bits + size → baked
  `font::Font*`, or nullptr = regular) + the underline bit.
- Baked fonts: new `packages/style-fonts.yaml` bakes Montserrat Bold + Italic at
  18/24/32 from `gfonts://` (build-time download, like the MDI webfont), anchored
  in `lvgl-ui.yaml`, and **package-merged** into the `ha_panel:` config
  (`style_fonts: {bold:[…], italic:[…]}`) so the user only writes `style:`.
- `bold|italic` with no bold-italic font baked → bold (documented).

Tasks:
- [x] `STYLE_*` flags + `Entity::name_style`; `add_entity`/`add_report` gain a
      `name_style` arg; codegen maps the `style:` list → flags (`_style_flags`).
- [x] `style_fonts:` config (`STYLE_FONTS_SCHEMA`) + `set_style_font` setter +
      `style_fonts_[3][2]` slots; `resolve_name_font_` lookup.
- [x] `make_entity_row` applies the override font + `LV_TEXT_DECOR_UNDERLINE`;
      build loop passes per-row style; styles set at build time (name is static).
- [x] `packages/style-fonts.yaml` (6 gfonts + merged `ha_panel:` wiring); 6
      hidden anchors in `lvgl-ui.yaml`; include before `lvgl_ui` in the main yaml.
- [x] Docs: README "Row text styles" + options-table row + report block;
      `ha-entities.example.yaml` (bold report title + comment); this plan +
      DEVELOPMENT.md.

**Exit criteria:** `style: [bold]` / `[italic]` / `[underline]` / `[bold,
underline]` render the row title in the right weight / slant / decoration at
small/medium/large; combos work (bold+italic → bold); unstyled rows are
byte-for-byte unchanged. Compiles + links clean.

**Risks / unknowns:**
- **Flash + build-time download.** 6 baked fonts (~+100 KB) + a gfonts fetch at
  compile. Dropping the `style_fonts:` include reclaims it if unused.
- **bold-italic gap.** No bold-italic font baked → that combo degrades to bold;
  bake a third variant + extend `style_fonts_[3][3]` if it's wanted.
- **Glyph coverage.** Baked variants use the default ASCII glyph set; a title with
  non-ASCII (°, accents) in bold/italic would miss those glyphs. Add a `glyphs:`
  set to `style-fonts.yaml` if needed.
- **Package merge.** Relies on ESPHome deep-merging two `ha_panel:` blocks (same
  id) — verified via `esphome config`; keep the ids in sync.

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
