> Part of the [Roadmap](roadmap.md). Shared conventions for all UI-enhancement
> (UE) phases are listed at the bottom of the roadmap.

# UE7 — binary_sensor `device_class` subscription (severity + aggregations)

**Status:** ⬜ not started · target tag: `ue7-device-class`

**Scope:** UE7's deliverable is the **connect-time `device_class` subscription**
for binary_sensors plus a severity/semantics classifier on top of it. That data
is a **shared prerequisite** for several features, not just one:
- **LED severity colouring** (the first consumer, detailed below) — paint the
  binary_sensor status dot by meaning instead of always-green.
- **Report-row aggregations** (UE12) that need class — e.g. counting **open
  doors / windows**, **low battery**, **alarm/leak** entities.
- **Picker badges** (UE11) gated on class — `open_doors`, `motion`,
  `low_battery`, `alarm`.

UE11 and UE12 already ship their config-free types and **parse-but-gate** the
device_class types until this phase lands. So UE7 is the unlock for every
class-aware glance feature, with the LED as its first and most visible use.

**Why (the LED, the first consumer):** UE5 shipped a status LED that paints
**green = "on"** for every
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
