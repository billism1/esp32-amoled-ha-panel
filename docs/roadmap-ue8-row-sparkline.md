> Part of the [Roadmap](roadmap.md). Shared conventions for all UI-enhancement
> (UE) phases are listed at the bottom of the roadmap.

# UE8 — Inline trend sparkline behind sensor rows

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
  from [on_state_](../components/ha_panel/ha_panel.cpp#L449) →
  [record_history_](../components/ha_panel/ha_panel.cpp#L3933). **The data source
  already exists** — no new capture path.
- `realtime` flag (UE7-era) → deeper ring (`HISTORY_CAP_RT = 600`) + the 30 s
  "Live" window; non-realtime entities use the 240-sample ring + the 1 h window.
  See [the window/cap constants](../components/ha_panel/ha_panel.cpp#L3856-L3883).
- Decimation + roll-mode draw already written for the full sheet:
  [redraw_history_](../components/ha_panel/ha_panel.cpp#L4425) and
  [redraw_live_roll_](../components/ha_panel/ha_panel.cpp#L4369). UE8 is a slimmer
  second render target driven by the same algorithm.
- Per-`size:` row geometry in [RowMetrics](../components/ha_panel/ha_panel.cpp#L719)
  / [row_metrics_for](../components/ha_panel/ha_panel.cpp#L732) — the sparkline fills
  the row (`LV_PCT(100) × m.height`), so it scales with small/medium/large for
  free (no per-size chart math).

**Change:** For a `plot_preview: true` chartable entity, add an `lv_chart`
(`LV_CHART_TYPE_LINE`) as the **first** child of the row button in
[make_entity_row](../components/ha_panel/ha_panel.cpp#L744) so it sits *behind* the
icon / name / right-side widget (those are created after → foreground). Style it
as a backdrop: transparent/near-transparent bg so the row's `0x1A1A1A` shows
through, no axis/divider lines, a thin low-opacity accent line so the name +
value text stay legible on top. Window is picked from the same `realtime` rule
the history sheet uses: realtime → 30 s Live roll, non-realtime → 1 h.

Tasks:
- [ ] **Config plumbing.** Add `CONF_PLOT_PREVIEW = "plot_preview"` in
      [components/ha_panel/__init__.py](../components/ha_panel/__init__.py#L43)
      (mirror `realtime` exactly), thread through
      [add_entity](../components/ha_panel/ha_panel.h#L121) → new `bool
      plot_preview{false}` on the [Entity struct](../components/ha_panel/ha_panel.h#L62).
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
      [build_ui_](../components/ha_panel/ha_panel.cpp#L1275).
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
