# Roadmap

Forward-looking work not yet implemented. Each phase below links to its own
sub-document with the full design, tasks, exit criteria, and risks.

**Completed phases are not listed here.** Shipped work — including the MVP phases
(0–8), the UI-enhancement phases that have landed (UE1–UE6, UE10–UE13), and their
design rationale — is recorded in [DEVELOPMENT.md](../DEVELOPMENT.md). As a
roadmap item ships, remove it from this file; its history lives in DEVELOPMENT.md.

---

## UI enhancements (drop-in LVGL widgets)

On-device polish that swaps a widget the UI already builds for a flashier LVGL v9
widget, reusing the same HA state and the same service call. Sequenced easy → hard;
each lands as an independent, shippable commit. Shared conventions are at the
bottom of this file.

| Phase | Name | What it does | Depends on |
|-------|------|--------------|------------|
| [UE7](roadmap-ue7-device-class.md) | binary_sensor `device_class` subscription | Subscribe binary_sensor `device_class` at connect time + a severity classifier. Shared prerequisite that unlocks LED severity colouring (leak/smoke = red, motion/door = amber, connectivity = green) **and** the class-gated report aggregations (open doors, low battery, alarm) + picker badges that UE11/UE12 parse-but-gate today. | — |
| [UE8](roadmap-ue8-row-sparkline.md) | Inline trend sparkline | Opt-in `plot_preview: true` paints a faint trend line behind a numeric sensor row, scaled to its `size:`, fed by the existing per-entity history ring buffer. | reuses UE6 |
| [UE9](roadmap-ue9-readonly.md) | `readonly: true` per-entity lock | A flag that keeps a row's state read-out + history/sparkline but disables every path that actuates HA (taps, confirm sheet, detail modal). | — |
| [UE14](roadmap-ue14-report-drilldown.md) | Report-row drill-down | Tap a UE12 report row to open a modal listing the member entities behind the number; the listed rows are fully interactive (toggle / detail / confirm), exactly as on a page. | UE12 (done) |

---

## Portability

| Phase | Name | What it does |
|-------|------|--------------|
| [P9](roadmap-p9-multiboard.md) | Multi-board support | Make adding a second AMOLED board a matter of dropping in one board package (pins, display, touch, dimensions) — no changes to the UI / HA logic. |

---

## On hold (scope subject to change)

| Phase | Name | What it does |
|-------|------|--------------|
| [P10](roadmap-p10-dynamic-discovery.md) | Dynamic area + entity discovery | Move the source-of-truth for pages + entities from the static `packages/ha-entities.yaml` into a single HA-side template sensor, so re-arranging a home updates the panel without a firmware rebuild. |

> ⚠️ **P10 is on hold and its scope is subject to change.** We may not implement
> it as written; if we do, the scope will likely change. The
> [sub-document](roadmap-p10-dynamic-discovery.md) preserves the original
> functionality outline as a starting point to revisit, not a committed spec.

---

## Conventions (all UI-enhancement phases)

- **🛑 No git commit until on-device validation passes.** Compiling clean is not
  enough — flash the panel and confirm the feature works before any commit.
- **Validate the acceptance criteria before commit.**
- **One commit per UE item**, each independently shippable and revertible.
- **No HA-side changes** for these items — same states, same service calls. If an
  item starts needing a new attribute or service, it has left this scope; stop and
  reassess. (UE7's `device_class` subscription is the one flagged, intentional
  exception — a standard-attribute fetch, not an HA-side change.)
- **Verify on-device**, not just on compile: touch ergonomics, layout fit, and
  animation behaviour can only be judged on the 480×480 panel.
- Update [README.md](../README.md) Features + [DEVELOPMENT.md](../DEVELOPMENT.md)
  log as each item ships.
