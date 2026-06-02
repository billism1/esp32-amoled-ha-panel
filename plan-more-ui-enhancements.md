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
