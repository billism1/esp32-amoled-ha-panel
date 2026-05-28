#include "ha_panel.h"

#include <cstdint>
#include <map>

#include "esphome/core/log.h"

namespace esphome {
namespace ha_panel {

static const char *const TAG = "ha_panel";

// ---------- codegen-time builders ----------

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

// ---------- setup / dump ----------

void HAPanel::setup() {
  ESP_LOGCONFIG(TAG, "Subscribing to %u entities across %u areas",
                (unsigned) this->entities_.size(), (unsigned) this->areas_.size());
  for (const auto &e : this->entities_) {
    this->subscribe_homeassistant_state(&HAPanel::on_state_, e.entity_id);
  }
  this->badges_by_entity_.assign(this->entities_.size(), nullptr);
  this->build_ui_();
}

void HAPanel::dump_config() {
  ESP_LOGCONFIG(TAG, "HA Panel model:");
  for (size_t ai = 0; ai < this->areas_.size(); ai++) {
    const auto &area = this->areas_[ai];
    ESP_LOGCONFIG(TAG, "  [%u] %s (%u entities)", (unsigned) ai, area.name.c_str(),
                  (unsigned) area.entity_indices.size());
  }
}

// ---------- HA state ----------

void HAPanel::on_state_(const std::string &entity_id, StringRef state) {
  for (size_t i = 0; i < this->entities_.size(); i++) {
    if (this->entities_[i].entity_id != entity_id)
      continue;
    bool first = !this->entities_[i].has_state;
    this->entities_[i].state = state.str();
    this->entities_[i].has_state = true;
    if (first) {
      ESP_LOGI(TAG, "%s = %s  (first state)", entity_id.c_str(),
               this->entities_[i].state.c_str());
    } else {
      ESP_LOGD(TAG, "%s = %s", entity_id.c_str(), this->entities_[i].state.c_str());
    }
    this->rebuild_entity_row_text_(i);
    return;
  }
  ESP_LOGW(TAG, "state callback for unknown entity %s", entity_id.c_str());
}

void HAPanel::rebuild_entity_row_text_(size_t entity_idx) {
  if (entity_idx >= this->badges_by_entity_.size())
    return;
  lv_obj_t *badge = this->badges_by_entity_[entity_idx];
  if (badge == nullptr)
    return;
  const auto &e = this->entities_[entity_idx];
  lv_label_set_text(badge, e.has_state ? e.state.c_str() : "…");
  // Colour cue: green for on/open/home/active, grey for off/closed,
  // red for unavailable/unknown, white for anything else.
  uint32_t col = 0xFFFFFF;
  if (e.state == "on" || e.state == "open" || e.state == "home" || e.state == "active")
    col = 0x66BB66;
  else if (e.state == "off" || e.state == "closed" || e.state == "away" || e.state == "idle")
    col = 0x888888;
  else if (e.state == "unavailable" || e.state == "unknown")
    col = 0xCC4444;
  lv_obj_set_style_text_color(badge, lv_color_hex(col), 0);
}

// ---------- tap dispatch ----------

bool HAPanel::tap_entity_(size_t entity_idx) {
  if (entity_idx >= this->entities_.size()) {
    ESP_LOGW(TAG, "tap_entity_(%u) out of range", (unsigned) entity_idx);
    return false;
  }
  const Entity &ent = this->entities_[entity_idx];
  const std::string &d = ent.domain;
  std::map<std::string, std::string> data;
  data["entity_id"] = ent.entity_id;

  if (d == "light" || d == "switch" || d == "fan" || d == "input_boolean" || d == "cover") {
    this->call_homeassistant_service("homeassistant.toggle", data);
    ESP_LOGI(TAG, "tap %s → homeassistant.toggle", ent.entity_id.c_str());
    return true;
  } else if (d == "script") {
    this->call_homeassistant_service("script.turn_on", data);
    ESP_LOGI(TAG, "tap %s → script.turn_on", ent.entity_id.c_str());
    return true;
  } else if (d == "automation") {
    this->call_homeassistant_service("automation.trigger", data);
    ESP_LOGI(TAG, "tap %s → automation.trigger", ent.entity_id.c_str());
    return true;
  }
  ESP_LOGI(TAG, "tap %s (domain '%s') is read-only — no action", ent.entity_id.c_str(),
           d.c_str());
  return false;
}

bool HAPanel::tap(size_t area_idx, size_t entity_idx) {
  if (area_idx >= this->areas_.size())
    return false;
  const auto &ai = this->areas_[area_idx].entity_indices;
  if (entity_idx >= ai.size())
    return false;
  return this->tap_entity_(ai[entity_idx]);
}

// ---------- LVGL UI build ----------

// Convenience to create a styled "row" button representing one entity.
static lv_obj_t *make_entity_row(lv_obj_t *parent, const Entity &e, void *user_data,
                                 lv_event_cb_t cb, uintptr_t entity_idx,
                                 lv_obj_t **out_badge) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_width(btn, LV_PCT(100));
  lv_obj_set_height(btn, 60);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_set_style_pad_all(btn, 0, 0);
  lv_obj_set_user_data(btn, (void *) entity_idx);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

  lv_obj_t *name = lv_label_create(btn);
  lv_label_set_text(name, e.friendly_name.c_str());
  lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
  lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_set_width(name, 300);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

  lv_obj_t *badge = lv_label_create(btn);
  lv_label_set_text(badge, e.has_state ? e.state.c_str() : "…");
  lv_obj_set_style_text_color(badge, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(badge, &lv_font_montserrat_18, 0);
  lv_obj_align(badge, LV_ALIGN_RIGHT_MID, -12, 0);
  *out_badge = badge;

  return btn;
}

void HAPanel::build_ui_() {
  if (this->areas_.empty()) {
    ESP_LOGW(TAG, "no areas; skipping UI build");
    return;
  }
  lv_obj_t *scr = lv_scr_act();
  if (scr == nullptr) {
    ESP_LOGE(TAG, "lv_scr_act() returned null — LVGL not ready");
    return;
  }
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_pad_all(scr, 0, 0);
  lv_obj_set_style_border_width(scr, 0, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // ---- Header (top 40 px). Tappable to open area picker. ----
  lv_obj_t *header = lv_obj_create(scr);
  lv_obj_remove_style_all(header);
  lv_obj_set_size(header, 480, 40);
  lv_obj_set_pos(header, 0, 0);
  lv_obj_set_style_bg_color(header, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(header, 0, 0);
  lv_obj_set_style_border_width(header, 0, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(header, &HAPanel::on_header_clicked_, LV_EVENT_CLICKED, this);

  this->header_label_ = lv_label_create(header);
  lv_label_set_text(this->header_label_, this->areas_[0].name.c_str());
  lv_obj_set_style_text_color(this->header_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(this->header_label_, &lv_font_montserrat_18, 0);
  lv_obj_align(this->header_label_, LV_ALIGN_CENTER, -12, 0);

  lv_obj_t *chev = lv_label_create(header);
  lv_label_set_text(chev, LV_SYMBOL_DOWN);
  lv_obj_set_style_text_color(chev, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(chev, &lv_font_montserrat_18, 0);
  lv_obj_align_to(chev, this->header_label_, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

  // ---- Tileview (rest of screen) ----
  this->tileview_ = lv_tileview_create(scr);
  lv_obj_remove_style_all(this->tileview_);
  lv_obj_set_size(this->tileview_, 480, 440);
  lv_obj_set_pos(this->tileview_, 0, 40);
  lv_obj_set_style_bg_color(this->tileview_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->tileview_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(this->tileview_, 0, 0);
  lv_obj_add_event_cb(this->tileview_, &HAPanel::on_tileview_changed_,
                      LV_EVENT_VALUE_CHANGED, this);

  this->tile_objs_.reserve(this->areas_.size());
  for (size_t ai = 0; ai < this->areas_.size(); ai++) {
    lv_dir_t dir = LV_DIR_HOR;
    if (ai == 0)
      dir = LV_DIR_RIGHT;
    else if (ai == this->areas_.size() - 1)
      dir = LV_DIR_LEFT;
    lv_obj_t *tile = lv_tileview_add_tile(this->tileview_, (uint8_t) ai, 0, dir);
    lv_obj_set_style_pad_all(tile, 0, 0);
    this->tile_objs_.push_back(tile);

    // Scrollable container for entity rows. Flex column gives auto vertical
    // layout; the parent tile clips and lv_obj's built-in scroll handles overflow.
    lv_obj_t *list = lv_obj_create(tile);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 480, 440);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(list, 8, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (size_t ei : this->areas_[ai].entity_indices) {
      Entity &e = this->entities_[ei];
      lv_obj_t *badge = nullptr;
      make_entity_row(list, e, this, &HAPanel::on_entity_row_clicked_, (uintptr_t) ei,
                      &badge);
      this->badges_by_entity_[ei] = badge;
      this->rebuild_entity_row_text_(ei);
    }
  }

  // ---- Area picker (full-screen modal, hidden until header tapped) ----
  this->picker_ = lv_obj_create(scr);
  lv_obj_remove_style_all(this->picker_);
  lv_obj_set_size(this->picker_, 480, 480);
  lv_obj_set_pos(this->picker_, 0, 0);
  lv_obj_set_style_bg_color(this->picker_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->picker_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(this->picker_, 0, 0);
  lv_obj_set_style_border_width(this->picker_, 0, 0);
  lv_obj_add_flag(this->picker_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(this->picker_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(this->picker_, &HAPanel::on_picker_bg_clicked_, LV_EVENT_CLICKED, this);

  lv_obj_t *ptitle = lv_label_create(this->picker_);
  lv_label_set_text(ptitle, "Pick area");
  lv_obj_set_style_text_color(ptitle, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(ptitle, &lv_font_montserrat_18, 0);
  lv_obj_align(ptitle, LV_ALIGN_TOP_MID, 0, 10);

  lv_obj_t *plist = lv_obj_create(this->picker_);
  lv_obj_remove_style_all(plist);
  lv_obj_set_size(plist, 460, 420);
  lv_obj_align(plist, LV_ALIGN_TOP_MID, 0, 44);
  lv_obj_set_style_bg_color(plist, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(plist, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(plist, 4, 0);
  lv_obj_set_style_pad_row(plist, 4, 0);
  lv_obj_set_flex_flow(plist, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(plist, LV_DIR_VER);

  for (size_t ai = 0; ai < this->areas_.size(); ai++) {
    lv_obj_t *row = lv_button_create(plist);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 56);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_user_data(row, (void *) (uintptr_t) ai);
    lv_obj_add_event_cb(row, &HAPanel::on_picker_row_clicked_, LV_EVENT_CLICKED, this);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, this->areas_[ai].name.c_str());
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);
  }
}

void HAPanel::open_picker_() {
  if (this->picker_ == nullptr)
    return;
  lv_obj_clear_flag(this->picker_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(this->picker_);
  ESP_LOGD(TAG, "picker open");
}

void HAPanel::close_picker_() {
  if (this->picker_ == nullptr)
    return;
  lv_obj_add_flag(this->picker_, LV_OBJ_FLAG_HIDDEN);
  ESP_LOGD(TAG, "picker close");
}

// ---------- LVGL event trampolines ----------

void HAPanel::on_tileview_changed_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->tileview_ == nullptr)
    return;
  lv_obj_t *tile = lv_tileview_get_tile_active(self->tileview_);
  for (size_t ai = 0; ai < self->tile_objs_.size(); ai++) {
    if (self->tile_objs_[ai] != tile)
      continue;
    if (self->header_label_ != nullptr)
      lv_label_set_text(self->header_label_, self->areas_[ai].name.c_str());
    ESP_LOGD(TAG, "area → %u %s", (unsigned) ai, self->areas_[ai].name.c_str());
    return;
  }
}

void HAPanel::on_header_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  self->open_picker_();
}

void HAPanel::on_entity_row_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  lv_obj_t *row = lv_event_get_target_obj(e);
  size_t entity_idx = (size_t) (uintptr_t) lv_obj_get_user_data(row);
  self->tap_entity_(entity_idx);
}

void HAPanel::on_picker_row_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  lv_obj_t *row = lv_event_get_target_obj(e);
  size_t ai = (size_t) (uintptr_t) lv_obj_get_user_data(row);
  self->close_picker_();
  if (self->tileview_ != nullptr && ai < self->areas_.size()) {
    lv_tileview_set_tile_by_index(self->tileview_, (uint32_t) ai, 0, LV_ANIM_ON);
    if (self->header_label_ != nullptr)
      lv_label_set_text(self->header_label_, self->areas_[ai].name.c_str());
  }
}

void HAPanel::on_picker_bg_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  if (lv_event_get_target_obj(e) == self->picker_)
    self->close_picker_();
}

}  // namespace ha_panel
}  // namespace esphome
