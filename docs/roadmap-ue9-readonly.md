> Part of the [Roadmap](roadmap.md). Shared conventions for all UI-enhancement
> (UE) phases are listed at the bottom of the roadmap.

# UE9 — `readonly: true` per-entity lock (view + plots, no edits)

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
- [tap_entity_](../components/ha_panel/ha_panel.cpp#L622) — the actual
  `call_homeassistant_service` site (toggle / lock / cover / action).
- [on_entity_row_clicked_](../components/ha_panel/ha_panel.cpp#L2105) — short-tap
  router → confirm sheet / history sheet / detail modal / `tap_entity_`.
- [on_entity_row_long_pressed_](../components/ha_panel/ha_panel.cpp#L3266) →
  [open_detail_](../components/ha_panel/ha_panel.cpp#L2461) /
  [open_confirm_action_](../components/ha_panel/ha_panel.cpp#L3710).
- [open_confirm_or_detail_](../components/ha_panel/ha_panel.cpp#L3695) — the
  `confirm`-flag entry.
- **Kept open:** [open_history_](../components/ha_panel/ha_panel.cpp#L4147) is
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
      [components/ha_panel/__init__.py](../components/ha_panel/__init__.py) (mirror
      `confirm`), thread through
      [add_entity](../components/ha_panel/ha_panel.h#L121) → new `bool
      readonly{false}` on the [Entity struct](../components/ha_panel/ha_panel.h#L62).
- [ ] **Hard gate at the service site.** Early-return `false` at the top of
      `tap_entity_` when `ent.readonly` — backstops `tap()` (programmatic /
      future automations) and any router path that's missed, so nothing can fire
      a service for a readonly entity.
- [ ] **Short-tap router.** In `on_entity_row_clicked_`, when `en.readonly`:
      route chartable rows to `open_history_` (view plots) and everything else to
      a no-op; skip the `confirm` and `open_detail_` branches.
- [ ] **Long-press router.** In `on_entity_row_long_pressed_`, no-op when
      `en.readonly` (the detail modal is the edit surface). Cleaner still: at
      build time in [build_ui_](../components/ha_panel/ha_panel.cpp#L1259), skip
      registering the `LONG_PRESSED` callback for readonly rows so the gesture is
      never wired.
- [ ] **Confirm entry.** Guard `open_confirm_or_detail_` (and its short-tap
      caller) so a `confirm + readonly` row opens neither sheet.
- [ ] **Visual cue (optional, v1-or-followup).** Signal non-interactivity so a
      readonly row doesn't look broken: e.g. dim the `BINARY_SWITCH` switch /
      hide the `ACTION_ICON` play glyph / drop the row's pressed-state bg
      highlight ([make_entity_row](../components/ha_panel/ha_panel.cpp#L766)). A
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
