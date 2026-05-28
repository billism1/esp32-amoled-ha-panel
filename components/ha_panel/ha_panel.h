// ha_panel.h — runtime model + state subscription for the static HA entity map.
#pragma once

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/api/custom_api_device.h"

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
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  // Called from generated to_code at compile time.
  void add_area(const std::string &name);
  void add_entity(const std::string &entity_id, const std::string &friendly_name);

  // Called from UI (Phase 6). Returns true if a service was dispatched.
  bool tap(size_t area_idx, size_t entity_idx);

  size_t num_areas() const { return this->areas_.size(); }
  size_t num_entities_in_area(size_t a) const {
    return a < this->areas_.size() ? this->areas_[a].entity_indices.size() : 0;
  }
  const Area *area(size_t i) const {
    return i < this->areas_.size() ? &this->areas_[i] : nullptr;
  }
  const Entity *entity_in_area(size_t a, size_t e) const {
    if (a >= this->areas_.size())
      return nullptr;
    const auto &ai = this->areas_[a].entity_indices;
    if (e >= ai.size())
      return nullptr;
    return &this->entities_[ai[e]];
  }

 protected:
  void on_state_(const std::string &entity_id, StringRef state);
  static std::string extract_domain_(const std::string &entity_id);

  std::vector<Area> areas_;
  std::vector<Entity> entities_;
};

}  // namespace ha_panel
}  // namespace esphome
