// ha_panel.h — runtime model + state subscription + LVGL UI tree.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/font/font.h"

#include "lvgl.h"

namespace esphome {
namespace ha_panel {

// P7c: how an entity row paints itself + how a short-tap dispatches. Picked
// once at codegen time from the entity_id domain. A new HA domain that we
// don't recognise falls into READ_ONLY_TEXT — safe default, no crash.
enum class RenderClass : uint8_t {
  BINARY_SWITCH,    // light/switch/fan/input_boolean → lv_switch indicator
  ACTION_ICON,      // scene/script/automation/button → LV_SYMBOL_PLAY badge
  LOCK_TEXT,        // lock → glyph + Locked/Unlocked text
  COVER_TEXT,       // cover → chevron + Open/Closed text
  SUMMARY_TEXT,     // climate/media_player/number/select → state summary (P7d adds modal)
  READ_ONLY_TEXT,   // sensor/binary_sensor/everything else → text badge
};

struct Entity {
  std::string entity_id;
  std::string friendly_name;
  std::string domain;     // prefix before '.'  (light/switch/sensor/…)
  std::string state;      // last known HA state; empty until first callback
  bool has_state{false};
  RenderClass render_class{RenderClass::READ_ONLY_TEXT};
  // P7d: subscribed-attribute storage (key = attribute name, value = last
  // seen string). Populated as HA pushes updates via attribute subscriptions.
  // Shared scaffolding with P7e (HA icon attribute).
  std::map<std::string, std::string> attrs;
  // P7e: optional "mdi:foo" override from YAML. Empty = resolve from domain.
  std::string icon_override;
  // P7f: short-tap opens a confirm sheet / detail modal instead of firing the
  // action immediately. Set at codegen; read in the row-click pre-flight.
  bool confirm{false};
  // Cached UTF-8 glyph for the icon column. v1 resolution (override → domain
  // default → fallback) never changes at runtime, so resolve once on first
  // render. The HA-`icon` attribute tier (P9 batched sensor) will invalidate
  // this when live icons land.
  mutable std::string icon_resolved_;
  mutable bool icon_cached_{false};
};

struct Area {
  std::string name;
  std::vector<size_t> entity_indices;  // indexes into HAPanel::entities_
};

class HAPanel : public Component, public api::CustomAPIDevice {
 public:
  void setup() override;
  void dump_config() override;
  // Run after LVGL (PROCESSOR) is initialised so lv_scr_act() is valid.
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  // Codegen-time API.
  void add_area(const std::string &name);
  void add_entity(const std::string &entity_id, const std::string &friendly_name,
                  const std::string &icon_override = "", bool confirm = false);
  // P7e: MDI glyph font for the per-entity icon column. nullptr → icons off,
  // rows fall back to the pre-P7e name-at-left layout.
  void set_mdi_font(font::Font *f) { this->mdi_font_ = f; }

  // Programmatic action (tests / future automations).
  bool tap(size_t area_idx, size_t entity_idx);

  size_t num_areas() const { return this->areas_.size(); }

  // YAML-side hooks (P7).
  void set_brightness_setter(std::function<void(uint8_t)> setter) {
    this->brightness_setter_ = std::move(setter);
  }
  // P7b: committer writes the persisted active-brightness global. Separate
  // from setter so the slider can drive a live preview without committing
  // until Apply is tapped.
  void set_brightness_committer(std::function<void(uint8_t)> committer) {
    this->brightness_committer_ = std::move(committer);
  }
  void set_active_brightness(uint8_t v);
  // P8: commit persisted sleep settings (enabled flag + mode) on Apply.
  void set_sleep_committer(std::function<void(bool, uint8_t)> committer) {
    this->sleep_committer_ = std::move(committer);
  }
  // Seed the settings-tile sleep controls from the boot-restored globals.
  void set_sleep_settings(bool enabled, uint8_t mode);
  void set_clock_text(const std::string &text);
  void set_api_connected(bool connected);
  // P7b: header status icons.
  void set_wifi_rssi(int rssi);
  void set_battery_voltage(float volts);

 protected:
  // HA state callback.
  void on_state_(const std::string &entity_id, StringRef state);
  // P7d: attribute callback. Dispatched off a per-(entity,attr) lambda
  // registered against the underlying api_server, so attr_name is captured
  // at subscription time.
  void on_attr_(size_t entity_idx, const std::string &attr_name, StringRef value);
  void subscribe_attr_(size_t entity_idx, const char *attr_name);

  // Domain dispatch: returns true if a service was sent.
  bool tap_entity_(size_t entity_idx);
  static std::string extract_domain_(const std::string &entity_id);
  static RenderClass render_class_for_(const std::string &domain);

  // P7e icon resolution. resolve_icon_ implements the v1 chain (override →
  // domain default → fallback) and caches the UTF-8 glyph on the Entity.
  // Returns empty string when no MDI font is configured (icons disabled).
  const std::string &resolve_icon_(const Entity &e) const;
  // mdi name (no "mdi:" prefix) → codepoint, 0 if not in the baked subset.
  static uint32_t mdi_codepoint_(const std::string &name);
  // domain → default mdi name, nullptr if domain has no default.
  static const char *domain_default_icon_(const std::string &domain);
  // Encode a Unicode codepoint as a UTF-8 std::string (MDI glyphs are 4-byte).
  static std::string utf8_encode_(uint32_t cp);
  // P7d: which domains expose a long-press detail modal.
  static bool has_detail_(const std::string &domain);

  // P7f confirm guard.
  // Domains where `confirm: true` does something (detail modal or action
  // sheet). False = read-only domain → flag ignored at runtime.
  static bool confirm_meaningful_(const std::string &domain);
  // Short-tap dispatcher for confirm-flagged entities: routes to the P7d
  // detail modal (light/climate/media_player/number/select/fan) or the new
  // action confirm sheet (action-only / lock / cover / switch / input_boolean).
  void open_confirm_or_detail_(size_t entity_idx);
  void build_confirm_sheet_(lv_obj_t *scr);
  void open_confirm_action_(size_t entity_idx);
  void close_confirm_();
  // Build+commit helper: fires `service` for the confirm entity, then closes.
  void fire_confirm_service_(const char *service);

  // LVGL build + helpers.
  void build_ui_();
  // E1: settings is a full-screen overlay sheet (built once, hidden), not a
  // tileview tile. Same pattern as detail_modal_ / confirm_sheet_.
  void build_settings_sheet_(lv_obj_t *scr);
  void open_settings_();
  void close_settings_();
  // E1: step the active area by ±1 with wrap-around (bottom-bar arrows).
  void step_area_(int delta);
  // P7c: dispatches on Entity::render_class to update the right child widget.
  void rebuild_entity_row_(size_t entity_idx);
  void open_picker_();
  void close_picker_();
  void update_status_dot_();
  void update_wifi_icon_();
  void update_battery_icon_();
  bool is_settings_active_() const;
  // P7b: stage / commit / revert brightness slider edits.
  void apply_brightness_();
  void revert_brightness_();
  // P8: stage / commit / revert sleep toggle + mode dropdown edits. Same
  // dirty-flag + revert-on-navigate-away recipe as brightness.
  void apply_sleep_();
  void revert_sleep_();
  // Grey out the mode dropdown when the master toggle is off (mode irrelevant).
  void update_sleep_mode_enabled_();

  // P7d detail modal.
  void build_detail_modal_(lv_obj_t *scr);
  void open_detail_(size_t entity_idx);
  void close_detail_();
  void clear_detail_widgets_();
  void apply_detail_();
  // Lazy persistent subscribe for an entity's modal attrs. First open per
  // entity registers subs + re-arms state_subs_at_ on every client so the
  // new entries actually get transmitted (post-connect-added subs sit
  // silent otherwise). Subsequent opens reuse the cache.
  void request_detail_attrs_(size_t entity_idx);
  void ensure_attrs_subscribed_(size_t entity_idx);
  // Dispatch on Entity::domain to one of the build_detail_<domain>_ methods.
  void build_detail_for_(size_t entity_idx);
  static std::vector<const char *> attrs_for_domain_(const std::string &domain);
  void build_detail_light_(lv_obj_t *parent, size_t entity_idx);
  void build_detail_climate_(lv_obj_t *parent, size_t entity_idx);
  void build_detail_media_player_(lv_obj_t *parent, size_t entity_idx);
  void build_detail_number_(lv_obj_t *parent, size_t entity_idx);
  void build_detail_select_(lv_obj_t *parent, size_t entity_idx);
  void build_detail_fan_(lv_obj_t *parent, size_t entity_idx);
  void build_detail_cover_(lv_obj_t *parent, size_t entity_idx);

  // Attribute lookup helpers — return value via out param, false if missing.
  bool get_attr_(size_t entity_idx, const char *name, std::string *out) const;
  float get_attr_float_(size_t entity_idx, const char *name, float def) const;
  int get_attr_int_(size_t entity_idx, const char *name, int def) const;
  // Parse HA list-of-string attribute (e.g. "['off', 'heat', 'cool']") into
  // a vector of strings. Tolerates single/double quotes + whitespace.
  static std::vector<std::string> parse_ha_list_(const std::string &raw);

  // Event trampolines (LVGL takes raw C callbacks).
  static void on_tileview_changed_(lv_event_t *e);
  static void on_header_clicked_(lv_event_t *e);
  static void on_entity_row_clicked_(lv_event_t *e);
  static void on_entity_row_long_pressed_(lv_event_t *e);
  static void on_picker_row_clicked_(lv_event_t *e);
  static void on_picker_bg_clicked_(lv_event_t *e);
  static void on_brightness_slider_(lv_event_t *e);
  static void on_apply_clicked_(lv_event_t *e);
  static void on_cancel_clicked_(lv_event_t *e);
  // E1: settings bg-tap (revert + close) and bottom-bar controls.
  static void on_settings_bg_clicked_(lv_event_t *e);
  static void on_nav_left_(lv_event_t *e);
  static void on_nav_right_(lv_event_t *e);
  static void on_gear_clicked_(lv_event_t *e);
  // P8 sleep controls.
  static void on_sleep_switch_(lv_event_t *e);
  static void on_sleep_mode_dropdown_(lv_event_t *e);
  static void on_detail_apply_clicked_(lv_event_t *e);
  static void on_detail_cancel_clicked_(lv_event_t *e);
  static void on_detail_bg_clicked_(lv_event_t *e);
  // Live label updates as detail-modal sliders move.
  static void on_detail_brightness_slider_(lv_event_t *e);
  static void on_detail_ct_slider_(lv_event_t *e);
  static void on_detail_temp_slider_(lv_event_t *e);
  static void on_detail_number_slider_(lv_event_t *e);
  static void on_detail_volume_slider_(lv_event_t *e);
  static void on_detail_fan_slider_(lv_event_t *e);
  static void on_detail_cover_slider_(lv_event_t *e);
  // Immediate (no Apply) buttons for media transport / cover / fan.
  static void on_media_prev_(lv_event_t *e);
  static void on_media_next_(lv_event_t *e);
  static void on_media_play_pause_(lv_event_t *e);
  static void on_media_mute_(lv_event_t *e);
  static void on_cover_open_(lv_event_t *e);
  static void on_cover_stop_(lv_event_t *e);
  static void on_cover_close_(lv_event_t *e);
  static void on_fan_off_(lv_event_t *e);
  // P7f confirm-sheet trampolines. All read confirm_entity_idx_.
  static void on_confirm_cancel_(lv_event_t *e);
  static void on_confirm_bg_clicked_(lv_event_t *e);
  static void on_confirm_single_(lv_event_t *e);  // scene/script/automation/button + switch toggle
  static void on_confirm_lock_(lv_event_t *e);
  static void on_confirm_unlock_(lv_event_t *e);
  static void on_confirm_cover_open_(lv_event_t *e);
  static void on_confirm_cover_stop_(lv_event_t *e);
  static void on_confirm_cover_close_(lv_event_t *e);

  std::vector<Area> areas_;
  std::vector<Entity> entities_;

  // LVGL refs. nullptr until build_ui_ runs.
  lv_obj_t *header_label_{nullptr};
  lv_obj_t *clock_label_{nullptr};
  lv_obj_t *status_dot_{nullptr};
  lv_obj_t *wifi_icon_{nullptr};
  lv_obj_t *battery_icon_{nullptr};
  lv_obj_t *tileview_{nullptr};
  lv_obj_t *picker_{nullptr};
  lv_obj_t *splash_{nullptr};
  // E1: settings overlay sheet (was a tileview tile). settings_open_ tracks
  // visibility; the revert-on-close trigger replaces the old
  // revert-on-navigate-away-from-tile path.
  lv_obj_t *settings_sheet_{nullptr};
  bool settings_open_{false};
  lv_obj_t *brightness_slider_{nullptr};
  lv_obj_t *brightness_value_label_{nullptr};
  // P8 settings-tile sleep controls.
  lv_obj_t *sleep_switch_{nullptr};
  lv_obj_t *sleep_mode_dropdown_{nullptr};
  std::vector<lv_obj_t *> tile_objs_;            // [area_idx]
  // P7c: per-entity right-side widget — lv_switch for binaries, lv_label
  // for everything else. Renamed from badges_by_entity_ since it's no longer
  // always a label.
  std::vector<lv_obj_t *> widgets_by_entity_;    // [entity_idx]
  // P7e: per-entity left-side icon label (mdi_font glyph). nullptr when icons
  // are disabled (no mdi_font) or for the rare row that skipped one.
  std::vector<lv_obj_t *> icons_by_entity_;      // [entity_idx]
  // P7c follow-up: BINARY_SWITCH rows only. When state == unavailable/unknown
  // we hide the switch (which would otherwise look like a normal "off") and
  // show this red text label in its slot. nullptr for non-binary rows.
  std::vector<lv_obj_t *> unavail_labels_by_entity_;

  // P7e: MDI glyph font for the icon column. nullptr → icons disabled.
  font::Font *mdi_font_{nullptr};

  std::function<void(uint8_t)> brightness_setter_;
  std::function<void(uint8_t)> brightness_committer_;
  // P8 sleep settings. *_committed_ mirror the persisted globals; staged_*
  // track the current widget state; sleep_dirty_ flips on any edit, cleared on
  // Apply / revert. mode: 0 = light, 1 = deep.
  std::function<void(bool, uint8_t)> sleep_committer_;
  bool sleep_enabled_{true};
  uint8_t sleep_mode_{0};
  bool staged_sleep_enabled_{true};
  uint8_t staged_sleep_mode_{0};
  bool sleep_dirty_{false};
  // active_brightness_ = persisted (last committed) value, mirrors the YAML
  // global. staged_brightness_ = current slider position (live preview).
  // brightness_dirty_ flips true on first slider drag, false on Apply/Cancel.
  uint8_t active_brightness_{0xD0};
  uint8_t staged_brightness_{0xD0};
  bool brightness_dirty_{false};
  bool api_connected_{false};
  int wifi_rssi_{0};
  bool have_wifi_rssi_{false};
  float battery_voltage_{0.0f};
  bool have_battery_{false};

  // P7d detail modal state. Modal is built once at setup, hidden by default.
  // open_detail_ wipes detail_content_, dispatches on domain, populates the
  // per-domain widget pointers below for Apply to read back.
  lv_obj_t *detail_modal_{nullptr};
  lv_obj_t *detail_title_{nullptr};
  lv_obj_t *detail_content_{nullptr};
  size_t detail_entity_idx_{0};
  bool detail_open_{false};
  // P7d-attrs: outstanding one-shot fetches for the current modal. Build
  // fires when this hits zero, OR when the 1500 ms safety timeout expires.
  int pending_attr_responses_{0};
  // Tracks which entity the in-flight fetches belong to so that a fast
  // open-A → close → open-B sequence doesn't mis-trigger B's build with
  // stragglers from A. Caching into Entity::attrs is always safe; only the
  // build trigger gates on identity.
  size_t detail_pending_entity_idx_{0};
  // Per-entity: have we ever subscribed to this entity's modal attrs? First
  // open registers them; subsequent opens skip the sub (persistent — the
  // callback stays alive and keeps Entity::attrs fresh).
  std::vector<bool> attrs_subscribed_;
  // Per-domain widget refs. Cleared on every open; populated by the matching
  // builder. nullptr means "not applicable to this modal".
  lv_obj_t *dw_light_switch_{nullptr};
  lv_obj_t *dw_brightness_slider_{nullptr};
  lv_obj_t *dw_brightness_label_{nullptr};
  lv_obj_t *dw_ct_slider_{nullptr};
  lv_obj_t *dw_ct_label_{nullptr};
  lv_obj_t *dw_temp_slider_{nullptr};   // climate target temp, ×10 units
  lv_obj_t *dw_temp_label_{nullptr};
  float dw_temp_step_{0.1f};
  lv_obj_t *dw_hvac_dropdown_{nullptr};
  std::vector<std::string> dw_hvac_modes_;
  lv_obj_t *dw_volume_slider_{nullptr};
  lv_obj_t *dw_volume_label_{nullptr};
  lv_obj_t *dw_number_slider_{nullptr};
  lv_obj_t *dw_number_label_{nullptr};
  float dw_number_min_{0.0f};
  float dw_number_step_{1.0f};
  lv_obj_t *dw_select_dropdown_{nullptr};
  std::vector<std::string> dw_select_options_;
  lv_obj_t *dw_fan_slider_{nullptr};
  lv_obj_t *dw_fan_label_{nullptr};
  lv_obj_t *dw_cover_slider_{nullptr};
  lv_obj_t *dw_cover_label_{nullptr};

  // P7f action confirm sheet. Third overlay alongside picker_ / detail_modal_.
  // Built once at setup, hidden; confirm_body_ is wiped + repopulated per open.
  lv_obj_t *confirm_sheet_{nullptr};
  lv_obj_t *confirm_title_{nullptr};
  lv_obj_t *confirm_body_{nullptr};
  lv_obj_t *confirm_unavail_label_{nullptr};
  size_t confirm_entity_idx_{0};
  bool confirm_open_{false};
};

}  // namespace ha_panel
}  // namespace esphome
