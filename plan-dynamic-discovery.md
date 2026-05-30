# Build Plan — Dynamic area + entity discovery

> Split out of [plan-mvp.md](plan-mvp.md) (was "Phase 10"). The MVP plan
> ships a static, gitignored `packages/ha-entities.yaml` (see its Phase 5).
> This document covers replacing that static file with HA-side dynamic
> config so re-arranging a home no longer needs a firmware rebuild.

Background reference: [docs/esp32-s3-amoled-ha-guide.md](docs/esp32-s3-amoled-ha-guide.md).

---

## Phase 10 — Dynamic area + entity discovery (replaces static YAML)

**Status:** ⬜ not started · target tag: `p10-dynamic`

**Goal:** Move source-of-truth for areas + entities from `packages/ha-entities.yaml` into Home Assistant itself. Re-arranging a home no longer requires a firmware rebuild.

### Mechanism

Single HA template sensor exposes the full area→entity map as a JSON attribute. Device subscribes to that attribute, parses, and builds LVGL tiles at runtime.

**HA side** (lives in HA's `configuration.yaml`, not in this repo — but we'll ship a sample snippet in `docs/ha-template-sensor.yaml`):

```yaml
template:
  - sensor:
      - name: "AMOLED Panel Config"
        unique_id: amoled_panel_config
        state: "ok"
        attributes:
          # Areas in carousel order. Override by sorting via labels or a manual list.
          areas: >
            {{ areas() | map('area_name') | list | tojson }}
          # { "Living Room": ["light.lamp", "switch.fan", ...], ... }
          entities_by_area: >
            {%- set ns = namespace(out={}) -%}
            {%- for a in areas() -%}
              {%- set ents = area_entities(a)
                  | reject('match', '^(sun|zone|person|device_tracker|update)\\.')
                  | list -%}
              {%- set ns.out = dict(ns.out, **{area_name(a): ents}) -%}
            {%- endfor -%}
            {{ ns.out | tojson }}
```

User can refine the reject/include filter to taste. Optionally support a `label` ("show_on_panel") on entities and filter to only labelled ones — cleaner than blacklist.

**Device side:**

```yaml
text_sensor:
  - platform: homeassistant
    id: panel_config_json
    entity_id: sensor.amoled_panel_config
    attribute: entities_by_area
    on_value:
      - lambda: |-
          // 1. Parse x via ArduinoJson
          // 2. Diff against currently-rendered area/entity set
          // 3. Rebuild LVGL tiles via lv_obj_create / lv_label_create / lv_btn_create
          // 4. Subscribe to per-entity states for the new set
```

### Hard parts (call out so we don't kid ourselves)

1. **Runtime LVGL widget creation.** ESPHome's YAML LVGL is declarative; building tiles in a lambda means calling the underlying LVGL C API directly. Works, but examples are sparse — budget a real spike. Pre-build by Phase 6 a small lambda that creates one tile programmatically as a proof.
2. **Runtime per-entity state subscriptions.** `homeassistant.text_sensor` is declared at compile time. Workaround: declare a *pool* of N (say 64) generic subscriptions at compile time, bind each one to whichever entity_id we currently care about via the C++ `set_entity_id()` setter. Confirm that ESPHome's native API client supports re-subscribing on `set_entity_id()` change — if not, a Phase 10 blocker.
3. **JSON payload size.** Native API protobuf message limit isn't tiny but isn't infinite. A 50-entity home is fine; a 500-entity home may overflow. Filter on the HA side aggressively.
4. **Domain → behaviour map stays in firmware.** Even with dynamic entity lists, knowing that `light.*` toggles and `sensor.*` is read-only is still a compile-time table. Acceptable.

### Migration

- Keep `packages/ha-entities.yaml` schema working. Add a top-level `discovery_mode: static | dynamic` substitution. `dynamic` ignores the static file; `static` keeps the MVP path. Lets us flip per board / per install without deleting code.

**Exit criteria:**
- Adding a new HA area + light + flashing nothing → panel reflects the change within a few seconds.
- Removing an entity from HA → panel drops the tile.
- Reordering areas via the HA template → panel carousel order updates.

**Risks / unknowns:**
- Re-subscription via `set_entity_id()` at runtime is the single biggest unknown. If it doesn't work, fallback: at boot, read the JSON once, restart device with state cached, declare subscriptions on next boot via generated config — much worse UX, only as a backstop.
- LVGL teardown on reconfigure must not leak memory. Track widget pointers and delete cleanly when an entity disappears.

### Related

- **Live-attr modal preload** (parked in the MVP plan's "Post-P7 TODO") naturally folds into this work: a HA-side template sensor batching per-entity attributes into one payload solves the connect-time TX-saturation problem. See [plan-mvp.md](plan-mvp.md) §"Post-P7 TODO — live attrs in modal".
