#include "ha_panel.h"

#include <map>

#include "esphome/core/log.h"

namespace esphome {
namespace ha_panel {

static const char *const TAG = "ha_panel";

void HAPanel::add_area(const std::string &name) {
  Area a;
  a.name = name;
  this->areas_.push_back(std::move(a));
}

void HAPanel::add_entity(const std::string &entity_id, const std::string &friendly_name) {
  if (this->areas_.empty()) {
    ESP_LOGE(TAG, "add_entity called before any area — codegen bug");
    return;
  }
  Entity e;
  e.entity_id = entity_id;
  e.friendly_name = friendly_name.empty() ? entity_id : friendly_name;
  e.domain = HAPanel::extract_domain_(entity_id);
  size_t idx = this->entities_.size();
  this->entities_.push_back(std::move(e));
  this->areas_.back().entity_indices.push_back(idx);
}

std::string HAPanel::extract_domain_(const std::string &entity_id) {
  auto dot = entity_id.find('.');
  if (dot == std::string::npos)
    return "";
  return entity_id.substr(0, dot);
}

void HAPanel::setup() {
  ESP_LOGCONFIG(TAG, "Subscribing to %u entities across %u areas",
                (unsigned) this->entities_.size(), (unsigned) this->areas_.size());
  for (const auto &e : this->entities_) {
    this->subscribe_homeassistant_state(&HAPanel::on_state_, e.entity_id);
  }
}

void HAPanel::dump_config() {
  ESP_LOGCONFIG(TAG, "HA Panel model:");
  for (size_t ai = 0; ai < this->areas_.size(); ai++) {
    const auto &area = this->areas_[ai];
    ESP_LOGCONFIG(TAG, "  [%u] %s (%u entities)", (unsigned) ai, area.name.c_str(),
                  (unsigned) area.entity_indices.size());
    for (size_t ei = 0; ei < area.entity_indices.size(); ei++) {
      const auto &e = this->entities_[area.entity_indices[ei]];
      ESP_LOGCONFIG(TAG, "      %u  %s  (%s)", (unsigned) ei,
                    e.entity_id.c_str(), e.friendly_name.c_str());
    }
  }
}

void HAPanel::on_state_(const std::string &entity_id, StringRef state) {
  for (auto &e : this->entities_) {
    if (e.entity_id != entity_id)
      continue;
    bool first = !e.has_state;
    e.state = state.str();
    e.has_state = true;
    if (first) {
      ESP_LOGI(TAG, "%s = %s  (first state)", entity_id.c_str(), e.state.c_str());
    } else {
      ESP_LOGD(TAG, "%s = %s", entity_id.c_str(), e.state.c_str());
    }
    return;
  }
  ESP_LOGW(TAG, "state callback for unknown entity %s", entity_id.c_str());
}

bool HAPanel::tap(size_t area_idx, size_t entity_idx) {
  const Entity *ent = this->entity_in_area(area_idx, entity_idx);
  if (ent == nullptr) {
    ESP_LOGW(TAG, "tap(%u, %u) out of range", (unsigned) area_idx, (unsigned) entity_idx);
    return false;
  }
  const std::string &d = ent->domain;
  std::map<std::string, std::string> data;
  data["entity_id"] = ent->entity_id;

  // Domain → service map mirrors plan.md §P6 table.
  if (d == "light" || d == "switch" || d == "fan" || d == "input_boolean" || d == "cover") {
    this->call_homeassistant_service("homeassistant.toggle", data);
    ESP_LOGI(TAG, "tap %s → homeassistant.toggle", ent->entity_id.c_str());
    return true;
  } else if (d == "script") {
    this->call_homeassistant_service("script.turn_on", data);
    ESP_LOGI(TAG, "tap %s → script.turn_on", ent->entity_id.c_str());
    return true;
  } else if (d == "automation") {
    this->call_homeassistant_service("automation.trigger", data);
    ESP_LOGI(TAG, "tap %s → automation.trigger", ent->entity_id.c_str());
    return true;
  }
  ESP_LOGI(TAG, "tap %s (domain '%s') is read-only — no action", ent->entity_id.c_str(),
           d.c_str());
  return false;
}

}  // namespace ha_panel
}  // namespace esphome
