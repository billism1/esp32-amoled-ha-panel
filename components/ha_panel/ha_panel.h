// ha_panel.h — runtime model + state subscription + LVGL UI tree.
#pragma once

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/api/custom_api_device.h"

#include "lvgl.h"

namespace esphome {
namespace ha_panel {

struct Entity {
  std::string entity_id;
  std::string friendly_name;
  std::string domain;     // prefix before '.'  (light/switch/sensor/…)
  std::string state;      // last known HA state; empty until first callback
  bool has_state{false};
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

  // Programmatic action (used by tests / future automations).
  bool tap(size_t area_idx, size_t entity_idx);

  size_t num_areas() const { return this->areas_.size(); }

 protected:
  // HA state callback.
  void on_state_(const std::string &entity_id, StringRef state);

  // Domain dispatch: returns true if a service was sent.
  bool tap_entity_(size_t entity_idx);
  static std::string extract_domain_(const std::string &entity_id);

  // LVGL build + helpers.
  void build_ui_();
  void rebuild_entity_row_text_(size_t entity_idx);
  void open_picker_();
  void close_picker_();

  // Event trampolines (LVGL takes raw C callbacks).
  static void on_tileview_changed_(lv_event_t *e);
  static void on_header_clicked_(lv_event_t *e);
  static void on_entity_row_clicked_(lv_event_t *e);
  static void on_picker_row_clicked_(lv_event_t *e);
  static void on_picker_bg_clicked_(lv_event_t *e);

  std::vector<Area> areas_;
  std::vector<Entity> entities_;

  // LVGL refs. nullptr until build_ui_ runs.
  lv_obj_t *header_label_{nullptr};
  lv_obj_t *tileview_{nullptr};
  lv_obj_t *picker_{nullptr};
  std::vector<lv_obj_t *> tile_objs_;            // [area_idx]
  std::vector<lv_obj_t *> badges_by_entity_;     // [entity_idx]
};

}  // namespace ha_panel
}  // namespace esphome
