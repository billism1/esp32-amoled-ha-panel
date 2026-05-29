// ha_panel.h — runtime model + state subscription + LVGL UI tree.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/api/custom_api_device.h"

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
  void add_entity(const std::string &entity_id, const std::string &friendly_name);

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
  void set_clock_text(const std::string &text);
  void set_api_connected(bool connected);
  // P7b: header status icons.
  void set_wifi_rssi(int rssi);
  void set_battery_voltage(float volts);

 protected:
  // HA state callback.
  void on_state_(const std::string &entity_id, StringRef state);

  // Domain dispatch: returns true if a service was sent.
  bool tap_entity_(size_t entity_idx);
  static std::string extract_domain_(const std::string &entity_id);
  static RenderClass render_class_for_(const std::string &domain);

  // LVGL build + helpers.
  void build_ui_();
  void build_settings_tile_(lv_obj_t *parent);
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

  // Event trampolines (LVGL takes raw C callbacks).
  static void on_tileview_changed_(lv_event_t *e);
  static void on_header_clicked_(lv_event_t *e);
  static void on_entity_row_clicked_(lv_event_t *e);
  static void on_picker_row_clicked_(lv_event_t *e);
  static void on_picker_bg_clicked_(lv_event_t *e);
  static void on_brightness_slider_(lv_event_t *e);
  static void on_apply_clicked_(lv_event_t *e);
  static void on_cancel_clicked_(lv_event_t *e);

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
  lv_obj_t *settings_tile_{nullptr};
  lv_obj_t *brightness_slider_{nullptr};
  lv_obj_t *brightness_value_label_{nullptr};
  std::vector<lv_obj_t *> tile_objs_;            // [area_idx]
  // P7c: per-entity right-side widget — lv_switch for binaries, lv_label
  // for everything else. Renamed from badges_by_entity_ since it's no longer
  // always a label.
  std::vector<lv_obj_t *> widgets_by_entity_;    // [entity_idx]
  // P7c follow-up: BINARY_SWITCH rows only. When state == unavailable/unknown
  // we hide the switch (which would otherwise look like a normal "off") and
  // show this red text label in its slot. nullptr for non-binary rows.
  std::vector<lv_obj_t *> unavail_labels_by_entity_;

  std::function<void(uint8_t)> brightness_setter_;
  std::function<void(uint8_t)> brightness_committer_;
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
};

}  // namespace ha_panel
}  // namespace esphome
