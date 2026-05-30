# Build Plan — Multi-board support

> Split out of [plan-mvp.md](plan-mvp.md) (was "Phase 9"). The MVP plan
> targets the Waveshare ESP32-S3-Touch-AMOLED-2.16 only. This document
> covers making the firmware portable across AMOLED boards by dropping in a
> new board package.

Background reference: [docs/esp32-s3-amoled-ha-guide.md](docs/esp32-s3-amoled-ha-guide.md).

---

## Phase 9 — Multi-board support

**Status:** ⬜ not started · target tag: `p9-multiboard`

**Goal:** Adding a second AMOLED board = adding one board package, nothing else.

Tasks:
- [ ] Extract anything still board-specific from `ha-amoled-panel.yaml` into the board package.
- [ ] Add a second board: `boards/waveshare-1.75.yaml`. Same UI YAML, different pins + dimensions.
- [ ] Document the "add a new board" recipe in `README.md`.

**Exit criteria:** Switching boards by changing one `!include` line, no other edits, panel works.

**Risks / unknowns:**
- Different touch ICs across boards = different external_component for each. Make the touch component an include from the board package, not the top YAML.
- 480×480 vs 466×466 vs other sizes — LVGL layout should pull dimensions from substitutions defined in the board package.
