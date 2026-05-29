#include "ha_panel.h"

#include <cstdint>
#include <cstdio>
#include <map>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

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
  e.render_class = HAPanel::render_class_for_(e.domain);
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

RenderClass HAPanel::render_class_for_(const std::string &d) {
  if (d == "light" || d == "switch" || d == "fan" || d == "input_boolean")
    return RenderClass::BINARY_SWITCH;
  if (d == "scene" || d == "script" || d == "automation" || d == "button")
    return RenderClass::ACTION_ICON;
  if (d == "lock")
    return RenderClass::LOCK_TEXT;
  if (d == "cover")
    return RenderClass::COVER_TEXT;
  if (d == "climate" || d == "media_player" || d == "number" || d == "select")
    return RenderClass::SUMMARY_TEXT;
  return RenderClass::READ_ONLY_TEXT;
}

// ---------- setup / dump ----------

void HAPanel::setup() {
  ESP_LOGCONFIG(TAG, "Subscribing to %u entities across %u areas",
                (unsigned) this->entities_.size(), (unsigned) this->areas_.size());
  for (const auto &e : this->entities_) {
    this->subscribe_homeassistant_state(&HAPanel::on_state_, e.entity_id);
  }
  this->widgets_by_entity_.assign(this->entities_.size(), nullptr);
  this->unavail_labels_by_entity_.assign(this->entities_.size(), nullptr);
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
    this->rebuild_entity_row_(i);
    return;
  }
  ESP_LOGW(TAG, "state callback for unknown entity %s", entity_id.c_str());
}

void HAPanel::rebuild_entity_row_(size_t entity_idx) {
  if (entity_idx >= this->widgets_by_entity_.size())
    return;
  lv_obj_t *w = this->widgets_by_entity_[entity_idx];
  if (w == nullptr)
    return;
  const Entity &e = this->entities_[entity_idx];

  switch (e.render_class) {
    case RenderClass::BINARY_SWITCH: {
      // Mirror state. Switch is non-interactive (parent button drives the
      // tap) so we drive CHECKED purely from `state`. When unavailable we
      // hide the switch (a disabled-looking switch still reads as "off"
      // from across the room) and show the red overlay label instead.
      const bool unavail = !e.has_state || e.state == "unavailable" ||
                            e.state == "unknown";
      lv_obj_t *overlay = entity_idx < this->unavail_labels_by_entity_.size()
                              ? this->unavail_labels_by_entity_[entity_idx]
                              : nullptr;
      if (unavail) {
        lv_obj_add_flag(w, LV_OBJ_FLAG_HIDDEN);
        if (overlay != nullptr)
          lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
      } else {
        if (overlay != nullptr)
          lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(w, LV_OBJ_FLAG_HIDDEN);
        if (e.state == "on")
          lv_obj_add_state(w, LV_STATE_CHECKED);
        else
          lv_obj_remove_state(w, LV_STATE_CHECKED);
      }
      return;
    }
    case RenderClass::ACTION_ICON: {
      // Static glyph — no per-state update. Action domains expose no
      // meaningful state (button has only last-pressed timestamp; scene/
      // script/automation report `unknown` or a timestamp). Leave alone.
      return;
    }
    case RenderClass::LOCK_TEXT: {
      const char *txt = e.has_state ? e.state.c_str() : "…";
      uint32_t col = 0x888888;
      if (e.state == "locked") {
        txt = LV_SYMBOL_CLOSE "  Locked";
        col = 0x66BB66;  // green: secured is the "good" state
      } else if (e.state == "unlocked") {
        // Amber, not green — an unlocked lock is a warning to most users.
        txt = LV_SYMBOL_OK "  Unlocked";
        col = 0xDDAA33;
      } else if (e.state == "locking" || e.state == "unlocking") {
        col = 0xCCCCCC;
      } else if (e.state == "jammed" || e.state == "unavailable" ||
                 e.state == "unknown") {
        col = 0xCC4444;
      }
      lv_label_set_text(w, txt);
      lv_obj_set_style_text_color(w, lv_color_hex(col), 0);
      return;
    }
    case RenderClass::COVER_TEXT: {
      const char *txt = e.has_state ? e.state.c_str() : "…";
      uint32_t col = 0xCCCCCC;
      if (e.state == "open") {
        txt = LV_SYMBOL_UP "  Open";
        col = 0x66BB66;
      } else if (e.state == "closed") {
        txt = LV_SYMBOL_DOWN "  Closed";
        col = 0x888888;
      } else if (e.state == "opening") {
        txt = LV_SYMBOL_UP "  Opening";
        col = 0xCCCCCC;
      } else if (e.state == "closing") {
        txt = LV_SYMBOL_DOWN "  Closing";
        col = 0xCCCCCC;
      } else if (e.state == "unavailable" || e.state == "unknown") {
        col = 0xCC4444;
      }
      lv_label_set_text(w, txt);
      lv_obj_set_style_text_color(w, lv_color_hex(col), 0);
      return;
    }
    case RenderClass::SUMMARY_TEXT:
    case RenderClass::READ_ONLY_TEXT: {
      lv_label_set_text(w, e.has_state ? e.state.c_str() : "…");
      uint32_t col = 0xFFFFFF;
      if (e.state == "on" || e.state == "open" || e.state == "home" ||
          e.state == "active" || e.state == "playing")
        col = 0x66BB66;
      else if (e.state == "off" || e.state == "closed" || e.state == "away" ||
               e.state == "idle" || e.state == "paused")
        col = 0x888888;
      else if (e.state == "unavailable" || e.state == "unknown")
        col = 0xCC4444;
      lv_obj_set_style_text_color(w, lv_color_hex(col), 0);
      return;
    }
  }
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

  switch (ent.render_class) {
    case RenderClass::BINARY_SWITCH: {
      // Explicit turn_on/turn_off when state is "on"/"off". Anything else —
      // including unavailable/unknown and transient values like a light's
      // "transitioning" — falls through to toggle so we don't refuse to act
      // just because HA hasn't reported back yet.
      if (ent.has_state && ent.state == "on") {
        this->call_homeassistant_service("homeassistant.turn_off", data);
        ESP_LOGI(TAG, "tap %s → homeassistant.turn_off", ent.entity_id.c_str());
      } else if (ent.has_state && ent.state == "off") {
        this->call_homeassistant_service("homeassistant.turn_on", data);
        ESP_LOGI(TAG, "tap %s → homeassistant.turn_on", ent.entity_id.c_str());
      } else {
        this->call_homeassistant_service("homeassistant.toggle", data);
        ESP_LOGI(TAG, "tap %s → homeassistant.toggle (state '%s')",
                 ent.entity_id.c_str(), ent.state.c_str());
      }
      return true;
    }
    case RenderClass::ACTION_ICON: {
      const char *svc = nullptr;
      if (d == "scene")
        svc = "scene.turn_on";
      else if (d == "script")
        svc = "script.turn_on";
      else if (d == "automation")
        svc = "automation.trigger";
      else if (d == "button")
        svc = "button.press";
      if (svc == nullptr) {
        ESP_LOGW(TAG, "ACTION_ICON with unmapped domain '%s'", d.c_str());
        return false;
      }
      this->call_homeassistant_service(svc, data);
      ESP_LOGI(TAG, "tap %s → %s", ent.entity_id.c_str(), svc);
      return true;
    }
    case RenderClass::LOCK_TEXT: {
      // Mid-transition (locking/unlocking) and jammed/unavailable explicitly
      // no-op. Better to wait for a stable state than commit the wrong action.
      if (ent.has_state && ent.state == "locked") {
        this->call_homeassistant_service("lock.unlock", data);
        ESP_LOGI(TAG, "tap %s → lock.unlock", ent.entity_id.c_str());
        return true;
      }
      if (ent.has_state && ent.state == "unlocked") {
        this->call_homeassistant_service("lock.lock", data);
        ESP_LOGI(TAG, "tap %s → lock.lock", ent.entity_id.c_str());
        return true;
      }
      ESP_LOGI(TAG, "tap %s lock in transient/unknown state '%s' — no-op",
               ent.entity_id.c_str(), ent.state.c_str());
      return false;
    }
    case RenderClass::COVER_TEXT: {
      // homeassistant.toggle forwards to the right cover service based on
      // current position — keeps the dispatch flat. P7d's modal will expose
      // explicit open/stop/close + position slider.
      this->call_homeassistant_service("homeassistant.toggle", data);
      ESP_LOGI(TAG, "tap %s → homeassistant.toggle (cover)", ent.entity_id.c_str());
      return true;
    }
    case RenderClass::SUMMARY_TEXT:
    case RenderClass::READ_ONLY_TEXT: {
      ESP_LOGI(TAG, "tap %s (domain '%s') is read-only — no action",
               ent.entity_id.c_str(), d.c_str());
      return false;
    }
  }
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

static lv_obj_t *make_entity_row(lv_obj_t *parent, const Entity &e, void *user_data,
                                 lv_event_cb_t cb, uintptr_t entity_idx,
                                 lv_obj_t **out_widget,
                                 lv_obj_t **out_unavail_label) {
  *out_unavail_label = nullptr;
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_width(btn, LV_PCT(100));
  lv_obj_set_height(btn, 60);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_set_style_border_width(btn, 0, 0);
  lv_obj_set_style_pad_all(btn, 0, 0);

  // P7: tap visual feedback — pressed state lightens the bg.
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A4A6A), LV_STATE_PRESSED);

  lv_obj_set_user_data(btn, (void *) entity_idx);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

  lv_obj_t *name = lv_label_create(btn);
  lv_label_set_text(name, e.friendly_name.c_str());
  lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
  lv_obj_align(name, LV_ALIGN_LEFT_MID, 12, 0);
  // Trim name width to 280 px (was 300) to leave room for a ~50 px switch.
  lv_obj_set_width(name, 280);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

  // P7c: right-side widget varies by render class.
  lv_obj_t *w = nullptr;
  switch (e.render_class) {
    case RenderClass::BINARY_SWITCH: {
      w = lv_switch_create(btn);
      lv_obj_set_size(w, 50, 26);
      lv_obj_align(w, LV_ALIGN_RIGHT_MID, -16, 0);
      // Non-interactive — parent button handles the tap. Without this the
      // switch fires its own LV_EVENT_VALUE_CHANGED + the click bubbles to
      // the parent, producing a double dispatch.
      lv_obj_clear_flag(w, LV_OBJ_FLAG_CLICKABLE);
      // Green when checked; default LVGL accent looks fine but force the
      // on-tint to match the rest of the panel's "on" colour for cohesion.
      lv_obj_set_style_bg_color(w, lv_color_hex(0x66BB66),
                                LV_PART_INDICATOR | LV_STATE_CHECKED);
      // Unavailable overlay: a switch in DISABLED state still looks like a
      // normal off-toggle from a meter away. Stack a red label in the same
      // right-mid slot, hidden by default; rebuild_entity_row_ flips
      // visibility based on state. Keeps the legacy "red Unavailable text"
      // affordance the old text-badge had.
      lv_obj_t *unavail = lv_label_create(btn);
      lv_label_set_text(unavail, "Unavailable");
      lv_obj_set_style_text_color(unavail, lv_color_hex(0xCC4444), 0);
      lv_obj_set_style_text_font(unavail, &lv_font_montserrat_18, 0);
      lv_obj_align(unavail, LV_ALIGN_RIGHT_MID, -12, 0);
      lv_obj_add_flag(unavail, LV_OBJ_FLAG_HIDDEN);
      *out_unavail_label = unavail;
      break;
    }
    case RenderClass::ACTION_ICON: {
      // Single play glyph on the right — communicates "tap fires action".
      // Cyan accent so it reads as an affordance, not a status label.
      w = lv_label_create(btn);
      lv_label_set_text(w, LV_SYMBOL_PLAY);
      lv_obj_set_style_text_color(w, lv_color_hex(0x44CCDD), 0);
      lv_obj_set_style_text_font(w, &lv_font_montserrat_18, 0);
      lv_obj_align(w, LV_ALIGN_RIGHT_MID, -16, 0);
      break;
    }
    case RenderClass::LOCK_TEXT:
    case RenderClass::COVER_TEXT:
    case RenderClass::SUMMARY_TEXT:
    case RenderClass::READ_ONLY_TEXT: {
      w = lv_label_create(btn);
      lv_label_set_text(w, e.has_state ? e.state.c_str() : "…");
      lv_obj_set_style_text_color(w, lv_color_hex(0xAAAAAA), 0);
      lv_obj_set_style_text_font(w, &lv_font_montserrat_18, 0);
      lv_obj_align(w, LV_ALIGN_RIGHT_MID, -12, 0);
      break;
    }
  }
  *out_widget = w;
  return btn;
}

void HAPanel::build_settings_tile_(lv_obj_t *parent) {
  // Scrollable content area occupies the top portion of the tile. Apply/
  // Cancel sit in a fixed 60 px row at the bottom (drawn after this block).
  // Content height = 440 (tile) - 60 (button row) - 8 (gap above buttons).
  lv_obj_t *content = lv_obj_create(parent);
  lv_obj_remove_style_all(content);
  lv_obj_set_size(content, 480, 372);
  lv_obj_set_pos(content, 0, 0);
  lv_obj_set_style_bg_color(content, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
  // 16 px side padding because the settings labels run nearly edge-to-edge;
  // gives the side rounded corners enough clearance not to clip the text.
  lv_obj_set_style_pad_all(content, 16, 0);
  lv_obj_set_style_pad_row(content, 14, 0);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(content, LV_DIR_VER);

  // Title.
  lv_obj_t *title = lv_label_create(content);
  lv_label_set_text(title, "Brightness");
  lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);

  // Brightness slider + value label.
  this->brightness_slider_ = lv_slider_create(content);
  lv_obj_set_width(this->brightness_slider_, LV_PCT(100));
  lv_obj_set_height(this->brightness_slider_, 22);
  lv_slider_set_range(this->brightness_slider_, 16, 255);
  lv_slider_set_value(this->brightness_slider_, this->active_brightness_, LV_ANIM_OFF);
  lv_obj_add_event_cb(this->brightness_slider_, &HAPanel::on_brightness_slider_,
                      LV_EVENT_VALUE_CHANGED, this);

  this->brightness_value_label_ = lv_label_create(content);
  char buf[24];
  snprintf(buf, sizeof(buf), "%u / 255", (unsigned) this->active_brightness_);
  lv_label_set_text(this->brightness_value_label_, buf);
  lv_obj_set_style_text_color(this->brightness_value_label_, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(this->brightness_value_label_, &lv_font_montserrat_18, 0);

  // Spacer.
  lv_obj_t *spacer = lv_obj_create(content);
  lv_obj_remove_style_all(spacer);
  lv_obj_set_size(spacer, 1, 8);

  // Timeouts (read-only display — substitution-driven, no runtime edit yet).
  lv_obj_t *to_title = lv_label_create(content);
  lv_label_set_text(to_title, "Idle timeouts");
  lv_obj_set_style_text_color(to_title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(to_title, &lv_font_montserrat_18, 0);

  lv_obj_t *to_dim = lv_label_create(content);
  lv_label_set_text(to_dim, "Dim after 15 s\nBlank after 45 s total");
  lv_obj_set_style_text_color(to_dim, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(to_dim, &lv_font_montserrat_18, 0);

  // About block.
  lv_obj_t *about_title = lv_label_create(content);
  lv_label_set_text(about_title, "About");
  lv_obj_set_style_text_color(about_title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(about_title, &lv_font_montserrat_18, 0);

  lv_obj_t *about = lv_label_create(content);
  // get_build_time_string takes a fixed-size buffer (BUILD_TIME_STR_SIZE,
  // compile-time enforced). Old get_compilation_time() returned std::string
  // but is deprecated and removed in 2026.7.0.
  char build_buf[Application::BUILD_TIME_STR_SIZE];
  App.get_build_time_string(build_buf);
  char abuf[160];
  snprintf(abuf, sizeof(abuf), "%s\nESPHome %s\nBuilt %s",
           App.get_name().c_str(), ESPHOME_VERSION, build_buf);
  lv_label_set_text(about, abuf);
  lv_obj_set_style_text_color(about, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(about, &lv_font_montserrat_18, 0);

  // ---- Apply / Cancel button row (P7b) ----
  // Sits below the scrolling content, above the bottom rounded corner.
  // Tile is 480x440; button row 60 px tall at y=380 → 0 px bottom inset still
  // safe because buttons are inset 32 px horizontally on each side and the
  // panel's bottom-corner radius bites less than the side radius.
  lv_obj_t *btn_row = lv_obj_create(parent);
  lv_obj_remove_style_all(btn_row);
  lv_obj_set_size(btn_row, 480, 60);
  lv_obj_set_pos(btn_row, 0, 380);
  lv_obj_set_style_bg_color(btn_row, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(btn_row, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_left(btn_row, 32, 0);
  lv_obj_set_style_pad_right(btn_row, 32, 0);
  lv_obj_set_style_pad_column(btn_row, 12, 0);
  lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *cancel = lv_button_create(btn_row);
  lv_obj_set_size(cancel, 200, 50);
  lv_obj_set_style_bg_color(cancel, lv_color_hex(0x222A33), 0);
  lv_obj_set_style_bg_color(cancel, lv_color_hex(0x3A4A6A), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(cancel, 8, 0);
  lv_obj_set_style_border_width(cancel, 0, 0);
  lv_obj_add_event_cb(cancel, &HAPanel::on_cancel_clicked_, LV_EVENT_CLICKED, this);
  lv_obj_t *clbl = lv_label_create(cancel);
  lv_label_set_text(clbl, "Cancel");
  lv_obj_set_style_text_color(clbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(clbl, &lv_font_montserrat_18, 0);
  lv_obj_center(clbl);

  lv_obj_t *apply = lv_button_create(btn_row);
  lv_obj_set_size(apply, 200, 50);
  lv_obj_set_style_bg_color(apply, lv_color_hex(0x2A553A), 0);
  lv_obj_set_style_bg_color(apply, lv_color_hex(0x3F8556), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(apply, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(apply, 8, 0);
  lv_obj_set_style_border_width(apply, 0, 0);
  lv_obj_add_event_cb(apply, &HAPanel::on_apply_clicked_, LV_EVENT_CLICKED, this);
  lv_obj_t *albl = lv_label_create(apply);
  lv_label_set_text(albl, "Apply");
  lv_obj_set_style_text_color(albl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(albl, &lv_font_montserrat_18, 0);
  lv_obj_center(albl);
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

  // P7b header layout:
  //   [ HH:MM ────── Area ▼ ──────  📶 🔋 ● ]
  // Clock left, area + chevron center, wifi → battery → status right.
  // 44 px corner inset on both far ends per the empirically-measured panel
  // radius (see P7a notes).

  // Clock at far left.
  this->clock_label_ = lv_label_create(header);
  lv_label_set_text(this->clock_label_, "--:--");
  lv_obj_set_style_text_color(this->clock_label_, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(this->clock_label_, &lv_font_montserrat_18, 0);
  lv_obj_align(this->clock_label_, LV_ALIGN_LEFT_MID, 44, 0);

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

  // Connection status dot at far right (44 px corner inset).
  this->status_dot_ = lv_obj_create(header);
  lv_obj_remove_style_all(this->status_dot_);
  lv_obj_set_size(this->status_dot_, 10, 10);
  lv_obj_set_style_radius(this->status_dot_, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(this->status_dot_, lv_color_hex(0xCC4444), 0);
  lv_obj_set_style_bg_opa(this->status_dot_, LV_OPA_COVER, 0);
  lv_obj_align(this->status_dot_, LV_ALIGN_RIGHT_MID, -44, 0);

  // Battery icon, to the left of the status dot. Single LV_SYMBOL_BATTERY_*
  // glyph (5 variants) bucketed by voltage. Tint matches level.
  this->battery_icon_ = lv_label_create(header);
  lv_label_set_text(this->battery_icon_, LV_SYMBOL_BATTERY_EMPTY);
  lv_obj_set_style_text_color(this->battery_icon_, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(this->battery_icon_, &lv_font_montserrat_18, 0);
  lv_obj_align_to(this->battery_icon_, this->status_dot_, LV_ALIGN_OUT_LEFT_MID,
                  -12, 0);

  // Wi-Fi icon, to the left of the battery icon. LVGL ships a single
  // LV_SYMBOL_WIFI glyph, so we tint by RSSI bucket instead of swapping
  // variants. Grey until a reading lands.
  this->wifi_icon_ = lv_label_create(header);
  lv_label_set_text(this->wifi_icon_, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(this->wifi_icon_, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(this->wifi_icon_, &lv_font_montserrat_18, 0);
  lv_obj_align_to(this->wifi_icon_, this->battery_icon_, LV_ALIGN_OUT_LEFT_MID,
                  -12, 0);

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
  // Total cols = areas_.size() + 1 (settings tile appended at end).
  const uint8_t total_cols = (uint8_t)(this->areas_.size() + 1);
  for (size_t ai = 0; ai < this->areas_.size(); ai++) {
    lv_dir_t dir = LV_DIR_HOR;
    if (ai == 0)
      dir = LV_DIR_RIGHT;
    lv_obj_t *tile = lv_tileview_add_tile(this->tileview_, (uint8_t) ai, 0, dir);
    lv_obj_set_style_pad_all(tile, 0, 0);
    this->tile_objs_.push_back(tile);

    lv_obj_t *list = lv_obj_create(tile);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 480, 440);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    // 8 px side padding is enough — entity rows already inset their text by
    // 12 px more, so the side rounded corners don't bite into the row text.
    lv_obj_set_style_pad_all(list, 8, 0);
    // Bottom needs more (28 px) so the last entity row clears the bottom
    // rounded corners (~16 px panel inset + a little visual breathing room).
    lv_obj_set_style_pad_bottom(list, 28, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (size_t ei : this->areas_[ai].entity_indices) {
      Entity &e = this->entities_[ei];
      lv_obj_t *widget = nullptr;
      lv_obj_t *unavail = nullptr;
      make_entity_row(list, e, this, &HAPanel::on_entity_row_clicked_, (uintptr_t) ei,
                      &widget, &unavail);
      this->widgets_by_entity_[ei] = widget;
      this->unavail_labels_by_entity_[ei] = unavail;
      this->rebuild_entity_row_(ei);
    }
  }
  // Settings tile (last col). LV_DIR_LEFT only — can't scroll right past it.
  this->settings_tile_ = lv_tileview_add_tile(this->tileview_, (uint8_t)(total_cols - 1),
                                              0, LV_DIR_LEFT);
  lv_obj_set_style_pad_all(this->settings_tile_, 0, 0);
  this->build_settings_tile_(this->settings_tile_);

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
  // Same 28 px bottom inset as the entity list — keeps the last area row in
  // the picker from being clipped by the bottom rounded corners.
  lv_obj_set_style_pad_bottom(plist, 28, 0);
  lv_obj_set_style_pad_row(plist, 4, 0);
  lv_obj_set_flex_flow(plist, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(plist, LV_DIR_VER);

  for (size_t ai = 0; ai < this->areas_.size(); ai++) {
    lv_obj_t *row = lv_button_create(plist);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 56);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x3A4A6A), LV_STATE_PRESSED);
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
  // Settings entry in picker so user can jump straight to it.
  {
    lv_obj_t *row = lv_button_create(plist);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 56);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x222A33), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x3A4A6A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_user_data(row, (void *) (uintptr_t)(total_cols - 1));
    lv_obj_add_event_cb(row, &HAPanel::on_picker_row_clicked_, LV_EVENT_CLICKED, this);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, LV_SYMBOL_SETTINGS "  Settings");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);
  }

  // ---- Boot splash (hides everything until API connects) ----
  this->splash_ = lv_obj_create(scr);
  lv_obj_remove_style_all(this->splash_);
  lv_obj_set_size(this->splash_, 480, 480);
  lv_obj_set_pos(this->splash_, 0, 0);
  lv_obj_set_style_bg_color(this->splash_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->splash_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(this->splash_, 0, 0);
  lv_obj_set_style_border_width(this->splash_, 0, 0);
  lv_obj_add_flag(this->splash_, LV_OBJ_FLAG_CLICKABLE);  // eat taps while shown

  lv_obj_t *splash_name = lv_label_create(this->splash_);
  lv_label_set_text(splash_name, App.get_name().c_str());
  lv_obj_set_style_text_color(splash_name, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(splash_name, &lv_font_montserrat_18, 0);
  lv_obj_align(splash_name, LV_ALIGN_CENTER, 0, -20);

  lv_obj_t *splash_status = lv_label_create(this->splash_);
  lv_label_set_text(splash_status, "Connecting to Home Assistant...");
  lv_obj_set_style_text_color(splash_status, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(splash_status, &lv_font_montserrat_18, 0);
  lv_obj_align(splash_status, LV_ALIGN_CENTER, 0, 20);

  this->update_status_dot_();
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

void HAPanel::update_status_dot_() {
  if (this->status_dot_ == nullptr)
    return;
  lv_obj_set_style_bg_color(this->status_dot_,
                            lv_color_hex(this->api_connected_ ? 0x66BB66 : 0xCC4444), 0);
}

bool HAPanel::is_settings_active_() const {
  if (this->tileview_ == nullptr || this->settings_tile_ == nullptr)
    return false;
  return lv_tileview_get_tile_active(this->tileview_) == this->settings_tile_;
}

void HAPanel::set_clock_text(const std::string &text) {
  if (this->clock_label_ == nullptr)
    return;
  lv_label_set_text(this->clock_label_, text.c_str());
}

void HAPanel::set_api_connected(bool connected) {
  if (this->api_connected_ == connected)
    return;
  this->api_connected_ = connected;
  this->update_status_dot_();
  if (connected && this->splash_ != nullptr) {
    lv_obj_add_flag(this->splash_, LV_OBJ_FLAG_HIDDEN);
  }
  ESP_LOGI(TAG, "api %s", connected ? "connected" : "disconnected");
}

void HAPanel::set_active_brightness(uint8_t v) {
  this->active_brightness_ = v;
  this->staged_brightness_ = v;
  this->brightness_dirty_ = false;
  if (this->brightness_slider_ != nullptr) {
    lv_slider_set_value(this->brightness_slider_, v, LV_ANIM_OFF);
  }
  if (this->brightness_value_label_ != nullptr) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%u / 255", (unsigned) v);
    lv_label_set_text(this->brightness_value_label_, buf);
  }
}

void HAPanel::apply_brightness_() {
  if (!this->brightness_dirty_)
    return;
  this->active_brightness_ = this->staged_brightness_;
  if (this->brightness_committer_)
    this->brightness_committer_(this->active_brightness_);
  this->brightness_dirty_ = false;
  ESP_LOGI(TAG, "brightness applied: %u", (unsigned) this->active_brightness_);
}

void HAPanel::revert_brightness_() {
  if (!this->brightness_dirty_)
    return;
  this->staged_brightness_ = this->active_brightness_;
  if (this->brightness_setter_)
    this->brightness_setter_(this->active_brightness_);
  if (this->brightness_slider_ != nullptr)
    lv_slider_set_value(this->brightness_slider_, this->active_brightness_,
                        LV_ANIM_OFF);
  if (this->brightness_value_label_ != nullptr) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%u / 255", (unsigned) this->active_brightness_);
    lv_label_set_text(this->brightness_value_label_, buf);
  }
  this->brightness_dirty_ = false;
  ESP_LOGI(TAG, "brightness reverted to %u", (unsigned) this->active_brightness_);
}

void HAPanel::set_wifi_rssi(int rssi) {
  this->wifi_rssi_ = rssi;
  this->have_wifi_rssi_ = true;
  this->update_wifi_icon_();
}

void HAPanel::set_battery_voltage(float volts) {
  this->battery_voltage_ = volts;
  this->have_battery_ = true;
  this->update_battery_icon_();
}

void HAPanel::update_wifi_icon_() {
  if (this->wifi_icon_ == nullptr)
    return;
  // RSSI bucketing (typical 2.4 GHz indoor):
  //   >= -55  excellent (green)
  //   >= -65  good      (lime)
  //   >= -75  fair      (yellow)
  //   >= -85  weak      (orange)
  //   <  -85  poor      (red)
  // No reading yet → grey.
  uint32_t col = 0x888888;
  if (this->have_wifi_rssi_) {
    if (this->wifi_rssi_ >= -55)
      col = 0x66BB66;
    else if (this->wifi_rssi_ >= -65)
      col = 0x88CC44;
    else if (this->wifi_rssi_ >= -75)
      col = 0xDDAA33;
    else if (this->wifi_rssi_ >= -85)
      col = 0xCC7733;
    else
      col = 0xCC4444;
  }
  lv_obj_set_style_text_color(this->wifi_icon_, lv_color_hex(col), 0);
}

void HAPanel::update_battery_icon_() {
  if (this->battery_icon_ == nullptr)
    return;
  // LiPo voltage → icon variant + tint. Same buckets documented in plan §P7b.
  const char *glyph = LV_SYMBOL_BATTERY_EMPTY;
  uint32_t col = 0x888888;
  if (this->have_battery_) {
    const float v = this->battery_voltage_;
    if (v >= 4.00f) {
      glyph = LV_SYMBOL_BATTERY_FULL;
      col = 0x66BB66;
    } else if (v >= 3.85f) {
      glyph = LV_SYMBOL_BATTERY_3;
      col = 0x88CC44;
    } else if (v >= 3.70f) {
      glyph = LV_SYMBOL_BATTERY_2;
      col = 0xDDAA33;
    } else if (v >= 3.55f) {
      glyph = LV_SYMBOL_BATTERY_1;
      col = 0xCC7733;
    } else {
      glyph = LV_SYMBOL_BATTERY_EMPTY;
      col = 0xCC4444;
    }
  }
  lv_label_set_text(this->battery_icon_, glyph);
  lv_obj_set_style_text_color(this->battery_icon_, lv_color_hex(col), 0);
}

// ---------- LVGL event trampolines ----------

void HAPanel::on_tileview_changed_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->tileview_ == nullptr)
    return;
  lv_obj_t *tile = lv_tileview_get_tile_active(self->tileview_);
  if (tile == self->settings_tile_) {
    if (self->header_label_ != nullptr)
      lv_label_set_text(self->header_label_, "Settings");
    return;
  }
  // P7b: leaving the settings tile with un-applied changes silently reverts.
  if (self->brightness_dirty_)
    self->revert_brightness_();
  for (size_t ai = 0; ai < self->tile_objs_.size(); ai++) {
    if (self->tile_objs_[ai] != tile)
      continue;
    if (self->header_label_ != nullptr)
      lv_label_set_text(self->header_label_, self->areas_[ai].name.c_str());
    return;
  }
}

void HAPanel::on_header_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  // P7b: header tap from the settings tile counts as navigating away — revert.
  if (self->brightness_dirty_)
    self->revert_brightness_();
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
  size_t col = (size_t) (uintptr_t) lv_obj_get_user_data(row);
  self->close_picker_();
  if (self->tileview_ == nullptr)
    return;
  const size_t total = self->areas_.size() + 1;
  if (col >= total)
    return;
  lv_tileview_set_tile_by_index(self->tileview_, (uint32_t) col, 0, LV_ANIM_ON);
  if (self->header_label_ != nullptr) {
    if (col < self->areas_.size())
      lv_label_set_text(self->header_label_, self->areas_[col].name.c_str());
    else
      lv_label_set_text(self->header_label_, "Settings");
  }
}

void HAPanel::on_picker_bg_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  if (lv_event_get_target_obj(e) == self->picker_)
    self->close_picker_();
}

void HAPanel::on_brightness_slider_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->brightness_slider_ == nullptr)
    return;
  int32_t v = lv_slider_get_value(self->brightness_slider_);
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  // P7b: stage only — display previews live, but the persisted global is not
  // written until Apply. brightness_dirty_ tracks "user has touched slider"
  // so navigating away can revert silently.
  self->staged_brightness_ = (uint8_t) v;
  self->brightness_dirty_ = (self->staged_brightness_ != self->active_brightness_);
  if (self->brightness_value_label_ != nullptr) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%u / 255", (unsigned) v);
    lv_label_set_text(self->brightness_value_label_, buf);
  }
  if (self->brightness_setter_)
    self->brightness_setter_((uint8_t) v);
}

void HAPanel::on_apply_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  self->apply_brightness_();
}

void HAPanel::on_cancel_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  self->revert_brightness_();
}

}  // namespace ha_panel
}  // namespace esphome
