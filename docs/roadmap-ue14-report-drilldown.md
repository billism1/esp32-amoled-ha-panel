> Part of the [Roadmap](roadmap.md). Shared conventions for all UI-enhancement
> (UE) phases are listed at the bottom of the roadmap.

# UE14 — Report-row drill-down (tap a report → list its member entities)

**Status:** ✅ implemented (compiles + links clean; on-device verification
pending) · target tag: `ue14-report-drilldown`

**Why:** UE12 report rows answer "*how many*" ("3 lights on", "2 offline",
"Coldest 18°") but not "*which ones*". A glance line that says two devices are
unavailable is only half useful if you still have to hunt every page to find
them. This phase makes the report row tappable: tap it and a modal lists the
exact entities behind the number — the three lights that are on, the two
unavailable devices, the temp sensors in the averaged group — so the summary
becomes a jumping-off point, not a dead end. Close the modal (✕ or background
tap) to return to the page exactly where you were.

**This re-opens a door UE12 deliberately shut — but only for the report row's own
tap.** UE12 made report rows **inert on tap** (`tap_entity_` returns false for
`REPORT_TEXT`, no long-press cb, pressed-bg flash disabled — see UE12 tasks).
UE14 re-enables the **report row's** short tap to open the member list; the report
row itself *still fires no service*, so it does **not** resurrect the deferred
"tap acts on the matched set" action report. The distinction that matters: the
report row opens a list, while the **entities listed inside that list are full,
live rows** — tapping a switch toggles it, long-press opens detail, exactly as on
a page (the user's explicit requirement). So "view-only" describes the report row,
not its members.

**No HA-side change.** It lists entities the panel already subscribes to, using
the same `{domains, device_class, match_state}` filter `compute_report_` already
runs — so the list can never disagree with the count above it. Same v1 universe
as UE12-A (panel-known entities only).

---

### Step 1 — DECIDE what the list shows per report `type:`

The member set is **type-specific** — it must match the semantics of the number
the row displays, or the list contradicts the count. Mapping (recommended):

- `count` **with `match_state:`** → the entities matching the filter **and** in
  one of the `match_state` values (the "lights *on*" set). With
  `show_total: true` (the "3 / 5" form), optionally list the full in-scope set
  with the matched ones marked, or list only the matched 3 — **recommend matched
  only** (the "3", which is what the user tapped toward); note the choice.
- `count` **without `match_state:`** → the full in-scope set (it counts
  everything matching `{domains, device_class}`).
- `offline` → the matched entities whose state is `unavailable` / `unknown`.
- `bool` → the "active / not-clear" set (the entities that make it read "2 on" /
  "1 open"); when all-clear (green ✓) the list is empty → show an empty-state
  line ("All clear"), don't open a blank modal.
- `sum` / `avg` → every in-scope numeric sensor that contributed, each with its
  current value (so the user sees what's being summed/averaged); skip the
  unavailable/non-numeric ones the aggregate skipped, or show them greyed as
  "excluded".
- `min` / `max` → the in-scope numeric sensors with values, the extreme one
  highlighted (it's the entity `show_source` already names).
- Deferred UE12 types (`security`, `active`, `low_battery`, `last_changed`,
  `diagnostic`) → define their member set when each lands; `diagnostic` is the
  one with no entity list (it's panel self-state) → its row stays inert (no
  drill-down) or shows a static info panel. Flag, don't block v1 on them.

The cleanest implementation is a `report_members_(const Entity &report,
std::vector<int> &out)` that **reuses the exact same scan + predicate as
`compute_report_`**, collecting indices instead of (or alongside) tallying. Best
of all: refactor `compute_report_` to optionally emit the member vector so the
count and the list share one filter pass and can never drift (the same
"whichever owns the helper" discipline UE11/UE12 already follow). Note the
divergence risk if it's duplicated instead.

### Step 2 — DECIDE the list surface (which overlay to reuse)

- **Clone the page-picker pattern (LEAN / recommended).** The picker
  ([open_picker_()](../components/ha_panel/ha_panel.cpp#L1560)) is already a
  built-once hidden overlay holding a **scrollable column of rows** that clears
  HIDDEN on demand — exactly the shape needed here. Build one reusable
  `report_members_sheet_` overlay the same way: a titled, scrollable list raised
  over the page, ✕ + background-tap to dismiss. Rebuild its rows per open from
  `report_members_()`. Lowest new-widget cost; matches an idiom already on the
  device.
- **Reuse `make_entity_row` for each listed entity (REQUIRED — see Step 3).** The
  members must behave exactly like rows on a normal page (tap a switch toggles it,
  long-press opens the detail modal), so they have to be **real entity rows built
  by `make_entity_row` and wired to the same `on_entity_row_clicked_` /
  `on_entity_row_long_pressed_` routers** — not a display-only label line. Each
  member inherits state colour / LED / switch / styling and full press routing for
  free. Cost: rows are normally built once at boot; here they're (re)built per open
  for the member set. That throwaway-per-open build is the price of full
  interactivity — accept it, bound it (Step 1 sets the member list; cap if huge).
- *Rejected:* a display-only label line — it can't toggle or open detail, which
  the requirement forbids.
- *Rejected:* the detail modal (`open_detail_`) — it's a single-entity editor,
  wrong shape for a list.

### Step 3 — Listed entities are fully interactive (REQUIRED)

The drill-down list is **not** read-only: each member row behaves identically to
the same entity's row on its own page.
- **Short tap** routes through `on_entity_row_clicked_` exactly as a page row
  does → a switch/light toggles in place, a lock/cover fires its service, a
  climate/media row opens its detail modal, a `confirm: true` row opens its
  confirm sheet. Same dispatch, same outcomes.
- **Long-press** routes through `on_entity_row_long_pressed_` → the detail modal /
  confirm action, same as on a page.
- **Per-entity flags still apply** because the same routers + `tap_entity_` run:
  `readonly` members don't actuate, `confirm` members confirm, `size:` and UE13
  `style:` render as configured. No special-casing — reuse the page-row path
  wholesale.

**Back-stack matters.** A member tap that opens a detail modal / confirm sheet /
history sheet must, on close, return to the **member list** (not jump to the
page). Decide the return target before wiring: track that the list is the
"parent" overlay so detail-close re-shows it. A toggle that fires in place needs
no navigation — it just updates the member row live (Step: live refresh).

---

### Pairs with / reuses (existing)

- [compute_report_ / recompute_reports_](../components/ha_panel/ha_panel.cpp#L491)
  — the filter + per-type scan UE12 added; `report_members_()` shares its
  `{domains, device_class, match_state}` predicate (refactor to emit members).
- The `ReportSpec` (filter + format flags) on the synthetic report `Entity`
  (UE12) — the tapped row already carries everything needed to recompute its set.
- [open_picker_()](../components/ha_panel/ha_panel.cpp#L1560) + the picker overlay
  build — the built-once-hidden, scrollable, ✕/bg-dismiss pattern to clone.
- [on_entity_row_clicked_](../components/ha_panel/ha_panel.cpp#L2105) — the short-tap
  router; add a `domain=="report"` branch → `open_report_members_()` (UE12 left
  this branch unmatched on purpose).
- `make_entity_row` / `state_to_value_` / `resolve_icon_` — for rendering each
  listed member (full row or light label line per Step 2).
- `confirm_sheet_` / detail-modal background-dismiss handling — the close idiom
  (✕ button + background tap → unhide page).

### Change

Re-enable the short-tap route for `domain=="report"` rows to a new
`open_report_members_()` that (1) calls `report_members_()` to collect the member
entity indices for the tapped report, (2) populates a reusable scrollable
`report_members_sheet_` overlay (title = the report's title) by running
`make_entity_row` for each member and wiring it to the **same press / long-press
routers a page row uses**, and (3) raises it. ✕ or a background tap hides it and
returns to the page. The **report row's own** tap opens the list and never fires
a service (`tap_entity_` stays false for `domain=="report"`); the **member rows
inside** the list are fully live (toggle / detail / confirm, per Step 3).

Illustrative behaviour (members are live — the switch toggles, the row opens
detail on long-press):

```
Page row:   "Lights on            3 / 5"   ← tap
            ┌─────────────────────────────┐
            │  Lights on              ✕    │
            │  🔆 Kitchen           [on ●] │  ← tap toggles, long-press = detail
            │  🔆 Living Room       [on ●] │
            │  🔆 Hallway           [on ●] │
            └─────────────────────────────┘   ← ✕ / bg tap → back to page
```

Tasks:
- [x] **DECIDE** Steps 1–3 (member set per type · list surface · member
      tap-through) and record the choices + reasoning in `DEVELOPMENT.md`.
- [x] **Member helper.** Added `report_members_(size_t report_idx,
      std::vector<size_t> &out)` (took the index, not `const Entity &`, so it can
      resolve PAGE scope) — a thin wrapper over `compute_report_`, which now
      optionally emits the member vector in its ONE filter pass. Scope resolution
      factored to `report_scope_for_`, shared with `recompute_reports_`. No
      duplication, so no drift risk.
- [x] **List overlay.** Built `report_members_sheet_` (cloned the picker's
      built-once-hidden, scrollable, titled overlay) with a ✕ button +
      background-tap dismiss.
- [x] **Populate on open with REAL rows.** `open_report_members_(report_idx)`
      fills the overlay from `report_members_()` — title from the report's name,
      one `make_entity_row` per member wired to the same `on_entity_row_clicked_`
      / `on_entity_row_long_pressed_` routers a page row uses. Empty-state line
      when the set is empty (`bool` all-clear). `lv_obj_clean` + clear the
      tracking vector each open (and on close) to free the previous set.
- [x] **Member interactivity = page parity.** Members carry the real entity index
      and route through the unchanged routers — no report-specific dispatch — so
      toggle / lock / cover / detail / confirm / long-press all behave as on a
      page. *(Behavioural verify pending on-device.)*
- [x] **Back-stack.** Solved by z-order, not explicit parent tracking: the member
      sheet stays visible beneath a member-spawned detail/confirm/history modal
      (each raised with `move_foreground`), so closing the modal reveals the list;
      the page is reached only by closing the list. The sheet's hidden flag
      distinguishes "opened from a member" (list visible) vs "opened from a page
      row" (list hidden → close reveals page) automatically.
- [x] **Live refresh while open.** `refresh_report_members_()` repaints the open
      list's rows from `on_state_` (+ the relevant `on_attr_` branches) behind a
      `!HIDDEN` guard. **Decision:** membership held STABLE until re-open (the
      plan's recommended option) — a toggled member's switch flips in place, but a
      now-excluded entity stays until re-open so a row can't vanish under the
      finger. The page's count still updates live via `recompute_reports_`.
- [x] **Re-enable the report-row tap.** `on_entity_row_clicked_` routes
      `REPORT_TEXT` → `open_report_members_()`; the pressed-bg flash is re-enabled
      in `make_entity_row` (UE12's flatten removed). `tap_entity_` still returns
      false for the report row (fires no service); the deferred UE12 *action*
      report stays deferred.
- [x] **readonly co-existence.** Unchanged gates: a `readonly` report still opens
      its drill-down (its tap never fired a service); a `readonly` member still
      won't actuate (shared `tap_entity_` / UE9 gate). Neither blocks the other.
- [x] **Inert types.** N/A for v1 — the `ReportType` enum has only the seven live
      types, all with a meaningful member set. Deferred panel-self types
      (`diagnostic`, …) aren't implemented yet; revisit when one lands.
- [x] **Docs.** README report section + new "Report drill-down" subsection;
      `ha-entities.example.yaml` note; DEVELOPMENT.md UE14 phase log; the UE12
      "view-only" note marked superseded for the report-row *tap* (the
      action-report-over-the-matched-set remains deferred).
- [ ] **Min/max extreme highlight (deferred polish).** The contributing sensors
      are listed with their values; flagging *which* row is the extreme is not yet
      done (the count's `show_source` already names it on the row above).

**Exit criteria:** Tapping a report row opens a modal listing the exact entities
behind its number — the on-lights for a "lights on" count, the unavailable
devices for `offline`, the contributing sensors (with values, extreme
highlighted) for min/max/avg — matching the count shown on the row. **Each listed
entity is fully interactive, exactly as on a page:** tapping a switch/light
toggles it, a lock/cover fires its service, a climate/media row opens its detail
modal, and long-press matches a page-row long-press; `readonly` / `confirm` /
`size` / `style` members behave as configured. Toggling a member updates the list
live (and can drop it out of the set). Closing a detail modal opened from the
list returns to the **list**, not the page. An all-clear `bool` report shows an
empty-state line, not a blank modal. ✕ or a background tap closes the list and
returns to the same page. The report row itself fires no service. Report rows
without a meaningful member set stay inert. Pages with no report rows are
unchanged. Compiles + links clean.

**Risks / unknowns:**
- **List ≠ count drift.** If the member helper duplicates `compute_report_`'s
  filter instead of sharing it, the list can disagree with the number above it.
  Share the one filter pass (the whole point of the UE11/UE12 shared-helper note).
- **Per-open row cost.** Member rows are **real `make_entity_row` rows built per
  open** (the price of full interactivity), not throwaway labels — fine for a
  short list, but a report matching dozens of entities builds a long scroll of
  real rows + handlers each open. Cap the list or virtualise if it bites (the
  picker is short by nature; reports may not be), and free the previous set on
  each rebuild to avoid leaks.
- **Empty / single-member sets.** `bool` all-clear → empty; a `min`/`max` over one
  sensor → a one-line list. Handle both without an awkward modal.
- **Report-row tap vs member-row tap.** Two different surfaces: the **report row**
  opens the list and fires no service (`tap_entity_` stays false, UE12 action
  report stays deferred); the **member rows** are full live rows that *do* actuate.
  Don't cross the wires — re-enabling the report row's tap must not turn it into an
  action surface, and the member rows must not be neutered into display-only.
- **Back-stack (now required, not optional).** A member tap opening a detail modal
  / confirm sheet / history sheet must close back to the **list**, not the page.
  The existing close handlers re-show the page; they need a "return-to" target so
  a list-spawned modal returns to the list. Get this wrong and closing a member's
  detail dumps the user on the page, losing their place. Design the parent-overlay
  tracking before wiring.
- **Live refresh + set membership.** With members interactive, toggling one
  changes state *and* can move it out of the report's set ("3 on" → "2 on"). Wire
  the list to `on_state_` behind a `!HIDDEN` guard (UE11 pattern) and recompute the
  member set, not just the row text — decide whether a now-excluded entity stays
  visible until close or disappears immediately (recommend: keep visible until
  re-open to avoid the row vanishing under the user's finger).
