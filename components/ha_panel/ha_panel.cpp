#include "ha_panel.h"
#include "mdi_icons.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"
#include "esphome/components/api/api_server.h"

namespace esphome {
namespace ha_panel {

static const char *const TAG = "ha_panel";

// ---------- codegen-time builders ----------

void HAPanel::add_area(const std::string &name) {
  Area a;
  a.name = name;
  this->areas_.push_back(std::move(a));
}

void HAPanel::add_entity(const std::string &entity_id, const std::string &friendly_name,
                         const std::string &icon_override, bool confirm) {
  if (this->areas_.empty()) {
    ESP_LOGE(TAG, "add_entity called before any area — codegen bug");
    return;
  }
  Entity e;
  e.entity_id = entity_id;
  e.friendly_name = friendly_name.empty() ? entity_id : friendly_name;
  e.domain = HAPanel::extract_domain_(entity_id);
  e.render_class = HAPanel::render_class_for_(e.domain);
  e.icon_override = icon_override;
  e.confirm = confirm;
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

bool HAPanel::has_detail_(const std::string &d) {
  // P7d: domains that long-press → modal. Binary domains (switch/input_boolean)
  // and action domains have no value to set beyond on/off and intentionally
  // skip the modal — single tap already does the right thing.
  return d == "light" || d == "climate" || d == "media_player" ||
         d == "number" || d == "select" || d == "fan" || d == "cover";
}

bool HAPanel::confirm_meaningful_(const std::string &d) {
  // P7f: confirm has a surface for any domain that does something on tap —
  // i.e. anything that isn't READ_ONLY_TEXT. Mirrors render_class_for_.
  return HAPanel::render_class_for_(d) != RenderClass::READ_ONLY_TEXT;
}

std::vector<std::string> HAPanel::parse_ha_list_(const std::string &raw) {
  // Tolerates "['a', 'b']" / "[\"a\",\"b\"]" / bare "a,b". Returns each token
  // stripped of surrounding whitespace and single/double quotes.
  std::vector<std::string> out;
  std::string s = raw;
  if (!s.empty() && s.front() == '[')
    s.erase(0, 1);
  if (!s.empty() && s.back() == ']')
    s.pop_back();
  std::string token;
  auto flush = [&]() {
    size_t b = 0;
    while (b < token.size() && (token[b] == ' ' || token[b] == '\t' ||
                                token[b] == '\'' || token[b] == '"'))
      b++;
    size_t en = token.size();
    while (en > b && (token[en - 1] == ' ' || token[en - 1] == '\t' ||
                      token[en - 1] == '\'' || token[en - 1] == '"'))
      en--;
    if (en > b)
      out.push_back(token.substr(b, en - b));
    token.clear();
  };
  for (char c : s) {
    if (c == ',')
      flush();
    else
      token.push_back(c);
  }
  flush();
  return out;
}

bool HAPanel::get_attr_(size_t entity_idx, const char *name, std::string *out) const {
  if (entity_idx >= this->entities_.size())
    return false;
  const auto &a = this->entities_[entity_idx].attrs;
  auto it = a.find(name);
  if (it == a.end())
    return false;
  *out = it->second;
  return true;
}

float HAPanel::get_attr_float_(size_t entity_idx, const char *name, float def) const {
  // ESP-IDF builds run with -fno-exceptions, so std::stof would terminate on
  // bad input. strtof returns 0 + sets end==start on failure — use that.
  std::string s;
  if (!this->get_attr_(entity_idx, name, &s) || s.empty())
    return def;
  const char *c = s.c_str();
  char *end = nullptr;
  float v = strtof(c, &end);
  if (end == c)
    return def;
  return v;
}

int HAPanel::get_attr_int_(size_t entity_idx, const char *name, int def) const {
  std::string s;
  if (!this->get_attr_(entity_idx, name, &s) || s.empty())
    return def;
  const char *c = s.c_str();
  char *end = nullptr;
  float v = strtof(c, &end);
  if (end == c)
    return def;
  return (int) std::round(v);
}

void HAPanel::subscribe_attr_(size_t entity_idx, const char *attr_name) {
  if (entity_idx >= this->entities_.size())
    return;
  const std::string &eid = this->entities_[entity_idx].entity_id;
  std::string attr_str = attr_name;
  // Skip the CustomAPIDevice template wrapper — it only supports a member
  // function pointer with no extra captures, so we'd need N callback methods
  // (one per attribute name). Going straight to the api_server lets a single
  // lambda capture (entity_idx, attr_name) and dispatch into on_attr_.
  api::global_api_server->subscribe_home_assistant_state(
      eid, optional<std::string>(attr_str),
      [this, entity_idx, attr_str](StringRef value) {
        this->on_attr_(entity_idx, attr_str, value);
      });
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

// ---------- P7e icon resolution ----------

uint32_t HAPanel::mdi_codepoint_(const std::string &name) {
  for (int i = 0; i < MDI_GLYPH_COUNT; i++) {
    if (name == MDI_GLYPHS[i].name)
      return MDI_GLYPHS[i].codepoint;
  }
  return 0;
}

const char *HAPanel::domain_default_icon_(const std::string &domain) {
  for (int i = 0; i < MDI_DOMAIN_DEFAULT_COUNT; i++) {
    if (domain == MDI_DOMAIN_DEFAULTS[i].domain)
      return MDI_DOMAIN_DEFAULTS[i].icon;
  }
  return nullptr;
}

std::string HAPanel::utf8_encode_(uint32_t cp) {
  std::string out;
  if (cp < 0x80) {
    out.push_back((char) cp);
  } else if (cp < 0x800) {
    out.push_back((char) (0xC0 | (cp >> 6)));
    out.push_back((char) (0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back((char) (0xE0 | (cp >> 12)));
    out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char) (0x80 | (cp & 0x3F)));
  } else {
    out.push_back((char) (0xF0 | (cp >> 18)));
    out.push_back((char) (0x80 | ((cp >> 12) & 0x3F)));
    out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char) (0x80 | (cp & 0x3F)));
  }
  return out;
}

const std::string &HAPanel::resolve_icon_(const Entity &e) const {
  if (e.icon_cached_)
    return e.icon_resolved_;
  e.icon_cached_ = true;

  // No MDI font configured → icons disabled. Empty result, caller skips the
  // icon column and keeps the legacy name-at-left layout.
  if (this->mdi_font_ == nullptr) {
    e.icon_resolved_.clear();
    return e.icon_resolved_;
  }

  // Step 1: YAML override. Accept "mdi:foo" or bare "foo".
  std::string name;
  if (!e.icon_override.empty()) {
    name = e.icon_override;
    if (name.rfind("mdi:", 0) == 0)
      name.erase(0, 4);
  } else {
    // Step 2: domain default.
    const char *d = HAPanel::domain_default_icon_(e.domain);
    if (d != nullptr)
      name = d;
  }

  uint32_t cp = name.empty() ? 0 : HAPanel::mdi_codepoint_(name);
  if (cp == 0) {
    // Step 3: fallback glyph. Log once per unresolved name so the baked subset
    // can be grown (rerun tools/build-mdi-glyphs.py) without flooding the log.
    static std::set<std::string> logged;
    std::string key = name.empty() ? e.domain : name;
    if (logged.insert(key).second) {
      ESP_LOGW(TAG, "no baked MDI glyph for '%s' (%s) — using fallback",
               key.c_str(), e.entity_id.c_str());
    }
    cp = MDI_FALLBACK_CP;
  }
  e.icon_resolved_ = HAPanel::utf8_encode_(cp);
  return e.icon_resolved_;
}

// ---------- setup / dump ----------

void HAPanel::setup() {
  ESP_LOGCONFIG(TAG, "Subscribing to %u entities across %u areas",
                (unsigned) this->entities_.size(), (unsigned) this->areas_.size());
  for (const auto &e : this->entities_) {
    this->subscribe_homeassistant_state(&HAPanel::on_state_, e.entity_id);
  }
  // P7d follow-up: per-domain attribute subscriptions DISABLED.
  //
  // First on-device test (2026-05-29) showed `homeassistant.turn_on/off` calls
  // dispatched from firmware (log says `tap … → homeassistant.turn_off`) but
  // the corresponding light never reacted, and HA log carried no rejection
  // message. Same firmware also produced `[W][api.connection]: Buffer full,
  // ping queued` ~60 s after connect, followed by `Home Assistant … is
  // unresponsive; disconnecting`. Smoking gun: ~30 lights × 6 attrs + 2
  // climates × 6 attrs ≈ 190 extra subscribe_homeassistant_state calls sent
  // in a burst at connect, on top of 88 state subscriptions. The TX path
  // never settles, and ESPHome's send_homeassistant_action returns silently
  // on `!flags_.service_call_subscription` so a dropped HA-side
  // SubscribeHomeassistantServicesRequest during the flood means every later
  // service call disappears with no error.
  //
  // Fix here is to ship P7d without persistent attr subs and fetch values
  // on demand at modal open (one-shot `get_homeassistant_state` follow-up).
  // Until that lands, modals fall back to defaults for their per-domain
  // ranges/options — usable for binary controls (the light switch + ad-hoc
  // sliders/dropdowns) just without "show me the current target temp" etc.
  ESP_LOGCONFIG(TAG, "P7d attribute subs deferred to lazy-on-first-modal-open");
  this->attrs_subscribed_.assign(this->entities_.size(), false);
  this->widgets_by_entity_.assign(this->entities_.size(), nullptr);
  this->unavail_labels_by_entity_.assign(this->entities_.size(), nullptr);
  this->icons_by_entity_.assign(this->entities_.size(), nullptr);
  ESP_LOGCONFIG(TAG, "P7e icon column %s",
                this->mdi_font_ != nullptr ? "enabled (mdi_font set)" : "disabled (no mdi_font)");
  this->build_ui_();
}

void HAPanel::on_attr_(size_t entity_idx, const std::string &attr_name,
                       StringRef value) {
  if (entity_idx >= this->entities_.size())
    return;
  auto &attrs = this->entities_[entity_idx].attrs;
  bool first_arrival = attrs.find(attr_name) == attrs.end();
  attrs[attr_name] = value.str();
  ESP_LOGD(TAG, "%s.%s = %s", this->entities_[entity_idx].entity_id.c_str(),
           attr_name.c_str(), value.c_str());
  // Drive the pending counter ONLY on first arrival per attr. Subsequent
  // pushes (HA state change while modal still open) just refresh the cache
  // — modal stays sticky during user edit, picks up new values on next open.
  if (!first_arrival)
    return;
  if (!this->detail_open_ ||
      this->detail_pending_entity_idx_ != entity_idx)
    return;
  if (this->pending_attr_responses_ <= 0)
    return;
  if (--this->pending_attr_responses_ == 0) {
    this->cancel_timeout("detail_load");
    this->build_detail_for_(entity_idx);
  }
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
      const char *txt = e.has_state ? e.state.c_str() : "...";
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
      const char *txt = e.has_state ? e.state.c_str() : "...";
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
      lv_label_set_text(w, e.has_state ? e.state.c_str() : "...");
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
                                 lv_obj_t **out_unavail_label,
                                 const char *icon_utf8,
                                 const lv_font_t *mdi_lv_font,
                                 lv_obj_t **out_icon) {
  *out_unavail_label = nullptr;
  *out_icon = nullptr;
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
  // P7d: SHORT_CLICKED (not CLICKED) so a long-press doesn't double-fire as
  // both tap-dispatch and detail-modal open. LV_EVENT_CLICKED fires on any
  // release; SHORT_CLICKED only fires when released before long_press_time.
  lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, user_data);

  // P7e: per-entity icon at the left edge, drawn in the MDI font. Only when a
  // glyph resolved AND the font is available — otherwise the row keeps the
  // pre-P7e layout (name flush left, full 280 px width).
  const bool have_icon = icon_utf8 != nullptr && icon_utf8[0] != '\0' &&
                         mdi_lv_font != nullptr;
  if (have_icon) {
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, icon_utf8);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(icon, mdi_lv_font, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 12, 0);
    *out_icon = icon;
  }

  lv_obj_t *name = lv_label_create(btn);
  lv_label_set_text(name, e.friendly_name.c_str());
  lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(name, &lv_font_montserrat_18, 0);
  // With an icon: shift name right (+48) and trim width to 240 so the ellipsis
  // still lands before the right-side widget. Without: legacy +12 / 280 px.
  lv_obj_align(name, LV_ALIGN_LEFT_MID, have_icon ? 48 : 12, 0);
  lv_obj_set_width(name, have_icon ? 240 : 280);
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
      // C++20 deprecates bitwise OR between distinct scoped enums
      // (lv_part_t | lv_state_t); LVGL's style selector parameter is
      // lv_style_selector_t (uint32_t), so cast explicitly.
      lv_obj_set_style_bg_color(
          w, lv_color_hex(0x66BB66),
          (lv_style_selector_t) LV_PART_INDICATOR | LV_STATE_CHECKED);
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
      lv_label_set_text(w, e.has_state ? e.state.c_str() : "...");
      lv_obj_set_style_text_color(w, lv_color_hex(0xAAAAAA), 0);
      lv_obj_set_style_text_font(w, &lv_font_montserrat_18, 0);
      lv_obj_align(w, LV_ALIGN_RIGHT_MID, -12, 0);
      break;
    }
  }
  *out_widget = w;
  return btn;
}

void HAPanel::build_settings_sheet_(lv_obj_t *scr) {
  // E1: full-screen overlay sheet (built once, hidden) replacing the old
  // settings tileview tile. Same lifecycle as detail_modal_ / confirm_sheet_:
  // opened by the bottom-bar gear, closed on Apply / Cancel / bg-tap.
  this->settings_sheet_ = lv_obj_create(scr);
  lv_obj_remove_style_all(this->settings_sheet_);
  lv_obj_set_size(this->settings_sheet_, 480, 480);
  lv_obj_set_pos(this->settings_sheet_, 0, 0);
  lv_obj_set_style_bg_color(this->settings_sheet_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->settings_sheet_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(this->settings_sheet_, 0, 0);
  lv_obj_set_style_border_width(this->settings_sheet_, 0, 0);
  lv_obj_add_flag(this->settings_sheet_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(this->settings_sheet_, LV_OBJ_FLAG_CLICKABLE);
  // Bg tap (on the sheet itself, not a child) = revert + close.
  lv_obj_add_event_cb(this->settings_sheet_, &HAPanel::on_settings_bg_clicked_,
                      LV_EVENT_CLICKED, this);
  lv_obj_t *parent = this->settings_sheet_;

  // Scrollable content area below the rounded top corner; Apply/Cancel sit in a
  // fixed 60 px row at the bottom (drawn after this block).
  lv_obj_t *content = lv_obj_create(parent);
  lv_obj_remove_style_all(content);
  lv_obj_set_size(content, 480, 356);
  lv_obj_set_pos(content, 0, 40);
  lv_obj_set_style_bg_color(content, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
  // 16 px side padding because the settings labels run nearly edge-to-edge;
  // gives the side rounded corners enough clearance not to clip the text.
  lv_obj_set_style_pad_all(content, 16, 0);
  // Slider knob extends ~12 px past the LV_PCT(100) track edges, so at the
  // right limit the knob bled past the panel's curved bezel. Bump side
  // padding to 32 px to keep the knob clear at the rightmost slider value.
  lv_obj_set_style_pad_left(content, 32, 0);
  lv_obj_set_style_pad_right(content, 32, 0);
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

  // ---- Power saving (P8) ----
  lv_obj_t *pwr_title = lv_label_create(content);
  lv_label_set_text(pwr_title, "Power saving");
  lv_obj_set_style_text_color(pwr_title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(pwr_title, &lv_font_montserrat_18, 0);

  // Master toggle row: label left, lv_switch right.
  lv_obj_t *sleep_row = lv_obj_create(content);
  lv_obj_remove_style_all(sleep_row);
  lv_obj_set_width(sleep_row, LV_PCT(100));
  lv_obj_set_height(sleep_row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(sleep_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(sleep_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(sleep_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *sleep_lbl = lv_label_create(sleep_row);
  lv_label_set_text(sleep_lbl, "Sleep when idle");
  lv_obj_set_style_text_color(sleep_lbl, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(sleep_lbl, &lv_font_montserrat_18, 0);

  this->sleep_switch_ = lv_switch_create(sleep_row);
  if (this->sleep_enabled_)
    lv_obj_add_state(this->sleep_switch_, LV_STATE_CHECKED);
  lv_obj_add_event_cb(this->sleep_switch_, &HAPanel::on_sleep_switch_,
                      LV_EVENT_VALUE_CHANGED, this);

  // Mode dropdown: Light sleep (0) / Deep sleep (1). Greyed when toggle off.
  this->sleep_mode_dropdown_ = lv_dropdown_create(content);
  lv_obj_set_width(this->sleep_mode_dropdown_, LV_PCT(100));
  lv_dropdown_set_options(this->sleep_mode_dropdown_, "Light sleep\nDeep sleep");
  lv_dropdown_set_selected(this->sleep_mode_dropdown_,
                           this->sleep_mode_ == 1 ? 1 : 0);
  lv_obj_add_event_cb(this->sleep_mode_dropdown_, &HAPanel::on_sleep_mode_dropdown_,
                      LV_EVENT_VALUE_CHANGED, this);
  this->update_sleep_mode_enabled_();

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
  // Sits below the scrolling content, above the bottom rounded corner. Same
  // y=396 geometry as the detail / confirm sheets. Buttons are inset 32 px
  // horizontally so the panel's bottom-corner radius doesn't clip them.
  lv_obj_t *btn_row = lv_obj_create(parent);
  lv_obj_remove_style_all(btn_row);
  lv_obj_set_size(btn_row, 480, 60);
  lv_obj_set_pos(btn_row, 0, 396);
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

  // ---- Tileview (between header and bottom nav bar) ----
  // E1: shrunk 440 → 392 px (y = 40–432) to make room for the 48 px bottom bar.
  this->tileview_ = lv_tileview_create(scr);
  lv_obj_remove_style_all(this->tileview_);
  lv_obj_set_size(this->tileview_, 480, 392);
  lv_obj_set_pos(this->tileview_, 0, 40);
  lv_obj_set_style_bg_color(this->tileview_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->tileview_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(this->tileview_, 0, 0);
  lv_obj_add_event_cb(this->tileview_, &HAPanel::on_tileview_changed_,
                      LV_EVENT_VALUE_CHANGED, this);

  this->tile_objs_.reserve(this->areas_.size());
  // E1: settings is no longer a tile — tiles = areas only.
  for (size_t ai = 0; ai < this->areas_.size(); ai++) {
    // E1: with settings gone, the last area is the right boundary (no swipe
    // past it). First area is the left boundary; middle areas swipe both ways.
    lv_dir_t dir = LV_DIR_HOR;
    if (ai == 0)
      dir = (this->areas_.size() == 1) ? (lv_dir_t) LV_DIR_NONE : LV_DIR_RIGHT;
    else if (ai == this->areas_.size() - 1)
      dir = LV_DIR_LEFT;
    lv_obj_t *tile = lv_tileview_add_tile(this->tileview_, (uint8_t) ai, 0, dir);
    lv_obj_set_style_pad_all(tile, 0, 0);
    this->tile_objs_.push_back(tile);

    lv_obj_t *list = lv_obj_create(tile);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 480, 392);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    // 8 px side padding is enough — entity rows already inset their text by
    // 12 px more, so the side rounded corners don't bite into the row text.
    lv_obj_set_style_pad_all(list, 8, 0);
    // E1: the bottom bar now covers the corner zone, so the list ends at 432
    // (well above the curve) — bottom padding drops 28 → 8 px.
    lv_obj_set_style_pad_bottom(list, 8, 0);
    lv_obj_set_style_pad_row(list, 4, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    // P7e: resolve the MDI font once (get_lv_font is cheap but the null check
    // belongs out of the per-row loop). nullptr when no mdi_font configured.
    const lv_font_t *mdi_lv_font =
        this->mdi_font_ != nullptr ? this->mdi_font_->get_lv_font() : nullptr;
    for (size_t ei : this->areas_[ai].entity_indices) {
      Entity &e = this->entities_[ei];
      lv_obj_t *widget = nullptr;
      lv_obj_t *unavail = nullptr;
      lv_obj_t *icon = nullptr;
      const std::string &glyph = this->resolve_icon_(e);
      lv_obj_t *btn = make_entity_row(list, e, this, &HAPanel::on_entity_row_clicked_,
                                      (uintptr_t) ei, &widget, &unavail,
                                      glyph.c_str(), mdi_lv_font, &icon);
      // P7d: long-press → detail modal, only for domains that have one.
      // P7f: also register long-press for confirm-flagged action-only entities
      // (no detail modal) so a long-press opens the same confirm sheet as a
      // short-tap — otherwise the gesture would do nothing.
      if (HAPanel::has_detail_(e.domain) ||
          (e.confirm && HAPanel::confirm_meaningful_(e.domain))) {
        lv_obj_add_event_cb(btn, &HAPanel::on_entity_row_long_pressed_,
                            LV_EVENT_LONG_PRESSED, this);
      }
      this->widgets_by_entity_[ei] = widget;
      this->unavail_labels_by_entity_[ei] = unavail;
      this->icons_by_entity_[ei] = icon;
      this->rebuild_entity_row_(ei);
    }
  }
  // ---- E1: bottom navigation bar (y = 432–480, 48 px) ----
  // Persistent across all areas: ◀ area-step / ⚙ settings / ▶ area-step.
  lv_obj_t *navbar = lv_obj_create(scr);
  lv_obj_remove_style_all(navbar);
  lv_obj_set_size(navbar, 480, 48);
  lv_obj_set_pos(navbar, 0, 432);
  lv_obj_set_style_bg_color(navbar, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(navbar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(navbar, 0, 0);
  lv_obj_set_style_border_width(navbar, 0, 0);
  lv_obj_clear_flag(navbar, LV_OBJ_FLAG_SCROLLABLE);

  // Shared button styling: 56 px touch target, pressed-state feedback like the
  // rest of the panel, transparent fill so only the glyph shows.
  struct {
    const char *glyph;
    lv_align_t align;
    int32_t x_ofs;
    lv_event_cb_t cb;
  } nav_btns[] = {
      {LV_SYMBOL_LEFT, LV_ALIGN_LEFT_MID, 44, &HAPanel::on_nav_left_},
      {LV_SYMBOL_SETTINGS, LV_ALIGN_CENTER, 0, &HAPanel::on_gear_clicked_},
      {LV_SYMBOL_RIGHT, LV_ALIGN_RIGHT_MID, -44, &HAPanel::on_nav_right_},
  };
  for (auto &nb : nav_btns) {
    lv_obj_t *btn = lv_button_create(navbar);
    lv_obj_set_size(btn, 56, 44);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E3640), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_align(btn, nb.align, nb.x_ofs, 0);
    lv_obj_add_event_cb(btn, nb.cb, LV_EVENT_CLICKED, this);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, nb.glyph);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl);
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
  // E1: the picker lists areas only — Settings moved to the bottom-bar gear.

  // ---- P7d detail modal (built once, hidden) ----
  this->build_detail_modal_(scr);

  // ---- P7f action confirm sheet (built once, hidden) ----
  this->build_confirm_sheet_(scr);

  // ---- E1 settings overlay sheet (built once, hidden) ----
  this->build_settings_sheet_(scr);

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

void HAPanel::open_settings_() {
  if (this->settings_sheet_ == nullptr)
    return;
  lv_obj_clear_flag(this->settings_sheet_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(this->settings_sheet_);
  this->settings_open_ = true;
  ESP_LOGD(TAG, "settings open");
}

void HAPanel::close_settings_() {
  if (this->settings_sheet_ == nullptr)
    return;
  lv_obj_add_flag(this->settings_sheet_, LV_OBJ_FLAG_HIDDEN);
  this->settings_open_ = false;
  ESP_LOGD(TAG, "settings close");
}

void HAPanel::step_area_(int delta) {
  if (this->tileview_ == nullptr || this->areas_.empty())
    return;
  // Find the current area index from the active tile, step by delta with
  // wrap-around, then set the tile programmatically (not bound by the per-tile
  // LV_DIR_* swipe constraints).
  lv_obj_t *active = lv_tileview_get_tile_active(this->tileview_);
  size_t cur = 0;
  for (size_t ai = 0; ai < this->tile_objs_.size(); ai++) {
    if (this->tile_objs_[ai] == active) {
      cur = ai;
      break;
    }
  }
  const size_t n = this->areas_.size();
  size_t next = (size_t)(((int) cur + delta % (int) n + (int) n) % (int) n);
  if (next == cur)
    return;
  lv_tileview_set_tile_by_index(this->tileview_, (uint32_t) next, 0, LV_ANIM_ON);
  if (this->header_label_ != nullptr)
    lv_label_set_text(this->header_label_, this->areas_[next].name.c_str());
}

void HAPanel::update_status_dot_() {
  if (this->status_dot_ == nullptr)
    return;
  // E2: three states.
  //   wifi down            → red (can't even attempt the HA link yet).
  //   wifi up, api down     → amber blink ("link not yet re-established").
  //   api connected        → green.
  uint32_t col;
  if (!this->wifi_connected_)
    col = 0xCC4444;
  else if (this->api_connected_)
    col = 0x66BB66;
  else
    col = this->blink_on_ ? 0xDDAA33 : 0x5A4A1A;
  lv_obj_set_style_bg_color(this->status_dot_, lv_color_hex(col), 0);
}

bool HAPanel::is_settings_active_() const {
  // E1: settings is an overlay sheet now, not a tile.
  return this->settings_open_;
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
  this->update_blink_timer_();
  this->update_status_dot_();
  if (connected && this->splash_ != nullptr) {
    lv_obj_add_flag(this->splash_, LV_OBJ_FLAG_HIDDEN);
  }
  ESP_LOGI(TAG, "api %s", connected ? "connected" : "disconnected");
}

void HAPanel::set_wifi_connected(bool connected) {
  if (this->wifi_connected_ == connected)
    return;
  this->wifi_connected_ = connected;
  this->update_blink_timer_();
  this->update_wifi_icon_();
  this->update_status_dot_();
  ESP_LOGI(TAG, "wifi %s", connected ? "connected" : "disconnected");
}

void HAPanel::update_blink_timer_() {
  // Pending = at least one indicator is amber. Wi-Fi down makes the Wi-Fi icon
  // pending; wifi-up-api-down makes the dot pending. Either way the condition is
  // "not fully connected".
  const bool pending = !this->wifi_connected_ || !this->api_connected_;
  if (pending && this->blink_timer_ == nullptr) {
    this->blink_on_ = true;
    this->blink_timer_ = lv_timer_create(&HAPanel::blink_timer_cb_, 500, this);
  } else if (!pending && this->blink_timer_ != nullptr) {
    lv_timer_delete(this->blink_timer_);
    this->blink_timer_ = nullptr;
    // Reset so the next redraw of a stable indicator uses its full colour.
    this->blink_on_ = true;
  }
}

void HAPanel::blink_timer_cb_(lv_timer_t *t) {
  auto *self = static_cast<HAPanel *>(lv_timer_get_user_data(t));
  if (self == nullptr)
    return;
  self->blink_on_ = !self->blink_on_;
  self->update_wifi_icon_();
  self->update_status_dot_();
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

// ---------- P8 sleep settings (staged like brightness) ----------

void HAPanel::set_sleep_settings(bool enabled, uint8_t mode) {
  this->sleep_enabled_ = enabled;
  this->sleep_mode_ = (mode == 1) ? 1 : 0;
  this->staged_sleep_enabled_ = this->sleep_enabled_;
  this->staged_sleep_mode_ = this->sleep_mode_;
  this->sleep_dirty_ = false;
  if (this->sleep_switch_ != nullptr) {
    if (this->sleep_enabled_)
      lv_obj_add_state(this->sleep_switch_, LV_STATE_CHECKED);
    else
      lv_obj_remove_state(this->sleep_switch_, LV_STATE_CHECKED);
  }
  if (this->sleep_mode_dropdown_ != nullptr)
    lv_dropdown_set_selected(this->sleep_mode_dropdown_, this->sleep_mode_);
  this->update_sleep_mode_enabled_();
}

void HAPanel::update_sleep_mode_enabled_() {
  if (this->sleep_mode_dropdown_ == nullptr)
    return;
  // Mode is irrelevant when the master toggle is off — disable + dim it.
  if (this->staged_sleep_enabled_) {
    lv_obj_remove_state(this->sleep_mode_dropdown_, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(this->sleep_mode_dropdown_, LV_OPA_COVER, 0);
  } else {
    lv_obj_add_state(this->sleep_mode_dropdown_, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(this->sleep_mode_dropdown_, LV_OPA_50, 0);
  }
}

void HAPanel::apply_sleep_() {
  if (!this->sleep_dirty_)
    return;
  this->sleep_enabled_ = this->staged_sleep_enabled_;
  this->sleep_mode_ = this->staged_sleep_mode_;
  if (this->sleep_committer_)
    this->sleep_committer_(this->sleep_enabled_, this->sleep_mode_);
  this->sleep_dirty_ = false;
  ESP_LOGI(TAG, "sleep applied: enabled=%d mode=%u",
           (int) this->sleep_enabled_, (unsigned) this->sleep_mode_);
}

void HAPanel::revert_sleep_() {
  if (!this->sleep_dirty_)
    return;
  this->staged_sleep_enabled_ = this->sleep_enabled_;
  this->staged_sleep_mode_ = this->sleep_mode_;
  if (this->sleep_switch_ != nullptr) {
    if (this->sleep_enabled_)
      lv_obj_add_state(this->sleep_switch_, LV_STATE_CHECKED);
    else
      lv_obj_remove_state(this->sleep_switch_, LV_STATE_CHECKED);
  }
  if (this->sleep_mode_dropdown_ != nullptr)
    lv_dropdown_set_selected(this->sleep_mode_dropdown_, this->sleep_mode_);
  this->update_sleep_mode_enabled_();
  this->sleep_dirty_ = false;
  ESP_LOGI(TAG, "sleep reverted to enabled=%d mode=%u",
           (int) this->sleep_enabled_, (unsigned) this->sleep_mode_);
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
  // E2: Wi-Fi down → "connecting" (ESPHome auto-reconnects), shown amber and
  // blinking. The dim phase uses a darkened amber so the glyph pulses rather
  // than fully vanishing.
  if (!this->wifi_connected_) {
    lv_obj_set_style_text_color(
        this->wifi_icon_, lv_color_hex(this->blink_on_ ? 0xDDAA33 : 0x5A4A1A), 0);
    return;
  }
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
  // E1: settings is no longer a tile, and its dirty state can only exist while
  // the (full-screen) sheet is open over the tileview — so swiping areas no
  // longer needs the navigate-away revert. Just retitle the header.
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
  self->open_picker_();
}

void HAPanel::on_entity_row_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  lv_obj_t *row = lv_event_get_target_obj(e);
  size_t entity_idx = (size_t) (uintptr_t) lv_obj_get_user_data(row);
  // P7f: confirm-flagged entities with a meaningful surface defer the action —
  // short-tap opens a confirm sheet or detail modal instead of firing now.
  if (entity_idx < self->entities_.size()) {
    const Entity &en = self->entities_[entity_idx];
    if (en.confirm && HAPanel::confirm_meaningful_(en.domain)) {
      self->open_confirm_or_detail_(entity_idx);
      return;
    }
  }
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
  if (col >= self->areas_.size())  // E1: picker lists areas only
    return;
  lv_tileview_set_tile_by_index(self->tileview_, (uint32_t) col, 0, LV_ANIM_ON);
  if (self->header_label_ != nullptr)
    lv_label_set_text(self->header_label_, self->areas_[col].name.c_str());
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
  self->apply_sleep_();
  self->close_settings_();  // E1: Apply commits then closes the sheet.
}

void HAPanel::on_cancel_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  self->revert_brightness_();
  self->revert_sleep_();
  self->close_settings_();  // E1: Cancel reverts then closes the sheet.
}

void HAPanel::on_settings_bg_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  // Bg tap (on the sheet itself, not a child widget) = revert + close.
  if (lv_event_get_target_obj(e) != self->settings_sheet_)
    return;
  self->revert_brightness_();
  self->revert_sleep_();
  self->close_settings_();
}

void HAPanel::on_gear_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  self->open_settings_();
}

void HAPanel::on_nav_left_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->step_area_(-1);
}

void HAPanel::on_nav_right_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->step_area_(+1);
}

void HAPanel::on_sleep_switch_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->sleep_switch_ == nullptr)
    return;
  self->staged_sleep_enabled_ =
      lv_obj_has_state(self->sleep_switch_, LV_STATE_CHECKED);
  self->sleep_dirty_ =
      (self->staged_sleep_enabled_ != self->sleep_enabled_) ||
      (self->staged_sleep_mode_ != self->sleep_mode_);
  self->update_sleep_mode_enabled_();
}

void HAPanel::on_sleep_mode_dropdown_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->sleep_mode_dropdown_ == nullptr)
    return;
  self->staged_sleep_mode_ =
      (uint8_t) lv_dropdown_get_selected(self->sleep_mode_dropdown_);
  self->sleep_dirty_ =
      (self->staged_sleep_enabled_ != self->sleep_enabled_) ||
      (self->staged_sleep_mode_ != self->sleep_mode_);
}

// ---------- P7d detail modal ----------

static lv_obj_t *add_section_label(lv_obj_t *parent, const char *text,
                                   uint32_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
  return l;
}

void HAPanel::build_detail_modal_(lv_obj_t *scr) {
  this->detail_modal_ = lv_obj_create(scr);
  lv_obj_remove_style_all(this->detail_modal_);
  lv_obj_set_size(this->detail_modal_, 480, 480);
  lv_obj_set_pos(this->detail_modal_, 0, 0);
  lv_obj_set_style_bg_color(this->detail_modal_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->detail_modal_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(this->detail_modal_, 0, 0);
  lv_obj_set_style_border_width(this->detail_modal_, 0, 0);
  lv_obj_add_flag(this->detail_modal_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(this->detail_modal_, LV_OBJ_FLAG_CLICKABLE);
  // Bg tap (on the modal itself, not a child) = cancel + close.
  lv_obj_add_event_cb(this->detail_modal_, &HAPanel::on_detail_bg_clicked_,
                      LV_EVENT_CLICKED, this);

  // Title centered at top.
  this->detail_title_ = lv_label_create(this->detail_modal_);
  lv_label_set_text(this->detail_title_, "");
  lv_obj_set_style_text_color(this->detail_title_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(this->detail_title_, &lv_font_montserrat_18, 0);
  lv_obj_set_width(this->detail_title_, 400);
  lv_obj_set_style_text_align(this->detail_title_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(this->detail_title_, LV_LABEL_LONG_DOT);
  lv_obj_align(this->detail_title_, LV_ALIGN_TOP_MID, 0, 16);

  // Scrollable content column. Sits between the title and the button row;
  // 24 px side inset clears the panel's side rounded corners.
  this->detail_content_ = lv_obj_create(this->detail_modal_);
  lv_obj_remove_style_all(this->detail_content_);
  lv_obj_set_size(this->detail_content_, 432, 332);
  lv_obj_set_pos(this->detail_content_, 24, 52);
  lv_obj_set_style_bg_color(this->detail_content_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->detail_content_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(this->detail_content_, 4, 0);
  // Slider knob extends ~12 px past its LV_PCT(100) track edge. With only
  // 4 px content side-pad the knob bled past the panel's curved bezel on
  // the right. Bump left+right to 16 px so the knob clears the corner zone.
  lv_obj_set_style_pad_left(this->detail_content_, 16, 0);
  lv_obj_set_style_pad_right(this->detail_content_, 16, 0);
  lv_obj_set_style_pad_row(this->detail_content_, 10, 0);
  lv_obj_set_flex_flow(this->detail_content_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(this->detail_content_, LV_DIR_VER);

  // Apply / Cancel button row pinned to bottom (same dimensions as settings
  // tile so the panel's bottom inset behaviour is consistent).
  lv_obj_t *btn_row = lv_obj_create(this->detail_modal_);
  lv_obj_remove_style_all(btn_row);
  lv_obj_set_size(btn_row, 480, 60);
  lv_obj_set_pos(btn_row, 0, 396);
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
  lv_obj_add_event_cb(cancel, &HAPanel::on_detail_cancel_clicked_,
                      LV_EVENT_CLICKED, this);
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
  lv_obj_add_event_cb(apply, &HAPanel::on_detail_apply_clicked_,
                      LV_EVENT_CLICKED, this);
  lv_obj_t *albl = lv_label_create(apply);
  lv_label_set_text(albl, "Apply");
  lv_obj_set_style_text_color(albl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(albl, &lv_font_montserrat_18, 0);
  lv_obj_center(albl);
}

void HAPanel::clear_detail_widgets_() {
  this->dw_light_switch_ = nullptr;
  this->dw_brightness_slider_ = nullptr;
  this->dw_brightness_label_ = nullptr;
  this->dw_ct_slider_ = nullptr;
  this->dw_ct_label_ = nullptr;
  this->dw_temp_slider_ = nullptr;
  this->dw_temp_label_ = nullptr;
  this->dw_temp_step_ = 0.1f;
  this->dw_hvac_dropdown_ = nullptr;
  this->dw_hvac_modes_.clear();
  this->dw_volume_slider_ = nullptr;
  this->dw_volume_label_ = nullptr;
  this->dw_number_slider_ = nullptr;
  this->dw_number_label_ = nullptr;
  this->dw_number_min_ = 0.0f;
  this->dw_number_step_ = 1.0f;
  this->dw_select_dropdown_ = nullptr;
  this->dw_select_options_.clear();
  this->dw_fan_slider_ = nullptr;
  this->dw_fan_label_ = nullptr;
  this->dw_cover_slider_ = nullptr;
  this->dw_cover_label_ = nullptr;
  if (this->detail_content_ != nullptr)
    lv_obj_clean(this->detail_content_);
}

void HAPanel::open_detail_(size_t entity_idx) {
  if (this->detail_modal_ == nullptr || entity_idx >= this->entities_.size())
    return;
  const Entity &e = this->entities_[entity_idx];
  if (!has_detail_(e.domain)) {
    ESP_LOGD(TAG, "long-press on %s — no detail for domain '%s'",
             e.entity_id.c_str(), e.domain.c_str());
    return;
  }
  this->clear_detail_widgets_();
  this->detail_entity_idx_ = entity_idx;
  this->detail_pending_entity_idx_ = entity_idx;
  lv_label_set_text(this->detail_title_, e.friendly_name.c_str());

  // P7d-attrs is parked (see plan §"post-P7 TODO: live attrs in modal").
  // Two attempts — one-shot get_home_assistant_state and lazy
  // subscribe-with-cursor-rearm — both failed because ESPHome's per-client
  // `state_subs_at_` cursor only walks while >= 0, and the only way to
  // re-arm it from outside (`on_subscribe_home_assistant_states_request`)
  // restarts from 0 instead of from "new entries only". That re-walks the
  // 88 existing state subs + the 6 new attr subs every modal open, which
  // takes ~3 s on this LAN at one message per loop tick — way past the
  // 1500 ms safety timeout, and a lot of repeat traffic for HA to dedupe.
  // Until we have a non-invasive way to extend the walk (e.g. a future
  // ESPHome API addition, or a HA-side template-sensor that batches all
  // needed attrs into one entity we sub once), modal builds immediately
  // with whatever's already in `Entity::attrs` (empty on first open) and
  // builders fall back to sensible defaults. Apply still commits whatever
  // the user dialed in — usable, just doesn't preload the current value.
  this->build_detail_for_(entity_idx);

  lv_obj_clear_flag(this->detail_modal_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(this->detail_modal_);
  this->detail_open_ = true;
  ESP_LOGI(TAG, "detail open: %s", e.entity_id.c_str());
}

void HAPanel::close_detail_() {
  if (this->detail_modal_ == nullptr)
    return;
  lv_obj_add_flag(this->detail_modal_, LV_OBJ_FLAG_HIDDEN);
  this->detail_open_ = false;
  this->pending_attr_responses_ = 0;
  this->cancel_timeout("detail_load");
  ESP_LOGD(TAG, "detail close");
}

std::vector<const char *> HAPanel::attrs_for_domain_(const std::string &d) {
  if (d == "light")
    return {"brightness", "color_temp_kelvin", "min_color_temp_kelvin",
            "max_color_temp_kelvin", "supported_color_modes", "color_mode"};
  if (d == "climate")
    return {"current_temperature", "temperature", "hvac_modes", "min_temp",
            "max_temp", "target_temp_step"};
  if (d == "media_player")
    return {"media_title", "volume_level", "is_volume_muted"};
  if (d == "number")
    return {"min", "max", "step"};
  if (d == "select")
    return {"options"};
  if (d == "fan")
    return {"percentage"};
  if (d == "cover")
    return {"current_position"};
  return {};
}

void HAPanel::ensure_attrs_subscribed_(size_t entity_idx) {
  if (entity_idx >= this->entities_.size())
    return;
  if (this->attrs_subscribed_[entity_idx])
    return;
  const Entity &e = this->entities_[entity_idx];
  auto attrs = attrs_for_domain_(e.domain);
  if (attrs.empty())
    return;
  for (const char *a : attrs)
    this->subscribe_attr_(entity_idx, a);
  this->attrs_subscribed_[entity_idx] = true;
  // ESPHome only transmits state-subs while the per-client `state_subs_at_`
  // cursor is >= 0. HA arms it once with SubscribeHomeAssistantStatesRequest
  // at connect; after the cursor walks past the end it parks at -1 and any
  // subs added later sit silently in `state_subs_` (api_connection.cpp:2435).
  // First-time use of get_home_assistant_state had the same problem in the
  // 2026-05-29 P7d-attrs attempt — the modal always hit the 1500 ms safety
  // timeout because the request itself never went out. Re-arming via the
  // same public method HA uses forces a full re-walk on every active
  // client, so the new entries (plus the existing ones, harmlessly
  // duplicated to HA which dedupes) actually get sent.
  for (const auto &client : api::global_api_server->active_clients())
    client->on_subscribe_home_assistant_states_request();
  ESP_LOGI(TAG, "subscribed %u attrs for %s + re-armed state_subs cursor",
           (unsigned) attrs.size(), e.entity_id.c_str());
}

void HAPanel::request_detail_attrs_(size_t entity_idx) {
  if (entity_idx >= this->entities_.size())
    return;
  const Entity &e = this->entities_[entity_idx];
  auto attrs = attrs_for_domain_(e.domain);
  if (attrs.empty()) {
    this->build_detail_for_(entity_idx);
    return;
  }
  // Cache hit fast path — every needed attr already in Entity::attrs from a
  // prior open. Skip Loading entirely.
  bool all_cached = true;
  int missing_count = 0;
  for (const char *a : attrs) {
    if (e.attrs.find(a) == e.attrs.end()) {
      all_cached = false;
      missing_count++;
    }
  }
  if (all_cached) {
    this->build_detail_for_(entity_idx);
    return;
  }
  // Cold path — subscribe (idempotent) and wait for arrivals.
  this->ensure_attrs_subscribed_(entity_idx);
  this->pending_attr_responses_ = missing_count;
  // Safety net — if HA never responds (disconnect, dropped re-arm, attr
  // missing on the entity so HA simply never emits a value), build with
  // whatever we have so the modal isn't stuck on "Loading..." forever.
  this->set_timeout("detail_load", 1500, [this, entity_idx]() {
    if (!this->detail_open_ ||
        this->detail_pending_entity_idx_ != entity_idx)
      return;
    ESP_LOGW(TAG, "detail attr fetch timed out (%d still pending) — building "
                  "with cached defaults",
             this->pending_attr_responses_);
    this->pending_attr_responses_ = 0;
    this->build_detail_for_(entity_idx);
  });
}

void HAPanel::build_detail_for_(size_t entity_idx) {
  if (this->detail_content_ == nullptr ||
      entity_idx >= this->entities_.size())
    return;
  // Wipe the "Loading…" placeholder (and any widgets a previous build might
  // have left if this is called more than once).
  lv_obj_clean(this->detail_content_);
  const Entity &e = this->entities_[entity_idx];
  if (e.domain == "light") {
    this->build_detail_light_(this->detail_content_, entity_idx);
  } else if (e.domain == "climate") {
    this->build_detail_climate_(this->detail_content_, entity_idx);
  } else if (e.domain == "media_player") {
    this->build_detail_media_player_(this->detail_content_, entity_idx);
  } else if (e.domain == "number") {
    this->build_detail_number_(this->detail_content_, entity_idx);
  } else if (e.domain == "select") {
    this->build_detail_select_(this->detail_content_, entity_idx);
  } else if (e.domain == "fan") {
    this->build_detail_fan_(this->detail_content_, entity_idx);
  } else if (e.domain == "cover") {
    this->build_detail_cover_(this->detail_content_, entity_idx);
  }
}

// ---- per-domain builders ----

void HAPanel::build_detail_light_(lv_obj_t *parent, size_t entity_idx) {
  const Entity &e = this->entities_[entity_idx];

  // Power row: "Power" label + on/off switch (interactive — Apply commits).
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 40);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_t *plbl = add_section_label(row, "Power", 0xFFFFFF);
  lv_obj_align(plbl, LV_ALIGN_LEFT_MID, 0, 0);
  this->dw_light_switch_ = lv_switch_create(row);
  lv_obj_set_size(this->dw_light_switch_, 50, 26);
  lv_obj_align(this->dw_light_switch_, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(
      this->dw_light_switch_, lv_color_hex(0x66BB66),
      (lv_style_selector_t) LV_PART_INDICATOR | LV_STATE_CHECKED);
  if (e.state == "on")
    lv_obj_add_state(this->dw_light_switch_, LV_STATE_CHECKED);

  // Brightness slider 0-100%. HA reports `brightness` 0-255; we display %.
  int cur_pct = 100;
  int b_raw = this->get_attr_int_(entity_idx, "brightness", -1);
  if (b_raw >= 0)
    cur_pct = (b_raw * 100 + 127) / 255;
  add_section_label(parent, "Brightness", 0xFFFFFF);
  this->dw_brightness_slider_ = lv_slider_create(parent);
  lv_obj_set_width(this->dw_brightness_slider_, LV_PCT(100));
  lv_obj_set_height(this->dw_brightness_slider_, 22);
  lv_slider_set_range(this->dw_brightness_slider_, 0, 100);
  lv_slider_set_value(this->dw_brightness_slider_, cur_pct, LV_ANIM_OFF);
  lv_obj_add_event_cb(this->dw_brightness_slider_,
                      &HAPanel::on_detail_brightness_slider_,
                      LV_EVENT_VALUE_CHANGED, this);
  this->dw_brightness_label_ = lv_label_create(parent);
  char buf[32];
  snprintf(buf, sizeof(buf), "%d %%", cur_pct);
  lv_label_set_text(this->dw_brightness_label_, buf);
  lv_obj_set_style_text_color(this->dw_brightness_label_,
                              lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(this->dw_brightness_label_,
                             &lv_font_montserrat_18, 0);

  // Color temperature slider — only if entity reports color_temp support.
  std::string scm_raw;
  bool has_ct = false;
  if (this->get_attr_(entity_idx, "supported_color_modes", &scm_raw)) {
    for (auto &m : parse_ha_list_(scm_raw)) {
      if (m == "color_temp" || m == "color_temp_kelvin") {
        has_ct = true;
        break;
      }
    }
  }
  if (has_ct) {
    int ct_min = this->get_attr_int_(entity_idx, "min_color_temp_kelvin", 2000);
    int ct_max = this->get_attr_int_(entity_idx, "max_color_temp_kelvin", 6500);
    if (ct_max <= ct_min) ct_max = ct_min + 1;
    int ct_cur = this->get_attr_int_(entity_idx, "color_temp_kelvin",
                                     (ct_min + ct_max) / 2);
    if (ct_cur < ct_min) ct_cur = ct_min;
    if (ct_cur > ct_max) ct_cur = ct_max;
    add_section_label(parent, "Color temperature", 0xFFFFFF);
    this->dw_ct_slider_ = lv_slider_create(parent);
    lv_obj_set_width(this->dw_ct_slider_, LV_PCT(100));
    lv_obj_set_height(this->dw_ct_slider_, 22);
    lv_slider_set_range(this->dw_ct_slider_, ct_min, ct_max);
    lv_slider_set_value(this->dw_ct_slider_, ct_cur, LV_ANIM_OFF);
    lv_obj_add_event_cb(this->dw_ct_slider_, &HAPanel::on_detail_ct_slider_,
                        LV_EVENT_VALUE_CHANGED, this);
    this->dw_ct_label_ = lv_label_create(parent);
    snprintf(buf, sizeof(buf), "%d K", ct_cur);
    lv_label_set_text(this->dw_ct_label_, buf);
    lv_obj_set_style_text_color(this->dw_ct_label_, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(this->dw_ct_label_, &lv_font_montserrat_18, 0);
  }
}

void HAPanel::build_detail_climate_(lv_obj_t *parent, size_t entity_idx) {
  const Entity &e = this->entities_[entity_idx];
  char buf[48];

  float cur_t = this->get_attr_float_(entity_idx, "current_temperature", NAN);
  if (std::isnan(cur_t))
    snprintf(buf, sizeof(buf), "Current: --");
  else
    snprintf(buf, sizeof(buf), "Current: %.1f", cur_t);
  add_section_label(parent, buf, 0xAAAAAA);

  // HVAC mode dropdown. Fall back to a sensible default list if HA didn't
  // expose hvac_modes (e.g. attribute hadn't landed yet at modal-open time).
  std::string modes_raw;
  if (this->get_attr_(entity_idx, "hvac_modes", &modes_raw))
    this->dw_hvac_modes_ = parse_ha_list_(modes_raw);
  if (this->dw_hvac_modes_.empty())
    this->dw_hvac_modes_ = {"off", "heat", "cool", "auto"};
  add_section_label(parent, "Mode", 0xFFFFFF);
  this->dw_hvac_dropdown_ = lv_dropdown_create(parent);
  lv_obj_set_width(this->dw_hvac_dropdown_, LV_PCT(100));
  std::string opt;
  for (size_t i = 0; i < this->dw_hvac_modes_.size(); i++) {
    if (i) opt += "\n";
    opt += this->dw_hvac_modes_[i];
  }
  lv_dropdown_set_options(this->dw_hvac_dropdown_, opt.c_str());
  for (size_t i = 0; i < this->dw_hvac_modes_.size(); i++) {
    if (this->dw_hvac_modes_[i] == e.state) {
      lv_dropdown_set_selected(this->dw_hvac_dropdown_, (uint16_t) i);
      break;
    }
  }

  // Target temperature slider. Stored as int = temp / step so the slider can
  // hit non-integer values without LVGL having float ranges.
  float min_t = this->get_attr_float_(entity_idx, "min_temp", 7.0f);
  float max_t = this->get_attr_float_(entity_idx, "max_temp", 35.0f);
  float step = this->get_attr_float_(entity_idx, "target_temp_step", 0.5f);
  float cur_target = this->get_attr_float_(entity_idx, "temperature",
                                            (min_t + max_t) / 2.0f);
  if (step < 0.1f) step = 0.1f;
  this->dw_temp_step_ = step;
  int scale = (int) std::round(1.0f / step);
  if (scale < 1) scale = 1;
  int rng_min = (int) std::round(min_t * scale);
  int rng_max = (int) std::round(max_t * scale);
  if (rng_max <= rng_min) rng_max = rng_min + 1;
  int rng_cur = (int) std::round(cur_target * scale);
  if (rng_cur < rng_min) rng_cur = rng_min;
  if (rng_cur > rng_max) rng_cur = rng_max;
  add_section_label(parent, "Target", 0xFFFFFF);
  this->dw_temp_slider_ = lv_slider_create(parent);
  lv_obj_set_width(this->dw_temp_slider_, LV_PCT(100));
  lv_obj_set_height(this->dw_temp_slider_, 22);
  lv_slider_set_range(this->dw_temp_slider_, rng_min, rng_max);
  lv_slider_set_value(this->dw_temp_slider_, rng_cur, LV_ANIM_OFF);
  lv_obj_add_event_cb(this->dw_temp_slider_, &HAPanel::on_detail_temp_slider_,
                      LV_EVENT_VALUE_CHANGED, this);
  this->dw_temp_label_ = lv_label_create(parent);
  snprintf(buf, sizeof(buf), "%.1f", (float) rng_cur / scale);
  lv_label_set_text(this->dw_temp_label_, buf);
  lv_obj_set_style_text_color(this->dw_temp_label_, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(this->dw_temp_label_, &lv_font_montserrat_18, 0);
}

void HAPanel::build_detail_media_player_(lv_obj_t *parent, size_t entity_idx) {
  std::string title;
  if (!this->get_attr_(entity_idx, "media_title", &title) || title.empty())
    title = this->entities_[entity_idx].state;
  add_section_label(parent, title.c_str(), 0xAAAAAA);

  // Transport row: prev / play_pause / next / mute. Immediate (no Apply).
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 60);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  struct BSpec { const char *glyph; lv_event_cb_t cb; };
  BSpec specs[] = {
      {LV_SYMBOL_PREV, &HAPanel::on_media_prev_},
      {LV_SYMBOL_PLAY, &HAPanel::on_media_play_pause_},
      {LV_SYMBOL_NEXT, &HAPanel::on_media_next_},
      {LV_SYMBOL_MUTE, &HAPanel::on_media_mute_},
  };
  for (auto &s : specs) {
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_size(b, 80, 50);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x222A33), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A4A6A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, s.cb, LV_EVENT_CLICKED, this);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, s.glyph);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
    lv_obj_center(l);
  }

  // Volume slider 0-100. HA volume_level is 0.0-1.0; Apply sends /100.
  float vol = this->get_attr_float_(entity_idx, "volume_level", 0.5f);
  int v100 = (int) std::round(vol * 100.0f);
  if (v100 < 0) v100 = 0;
  if (v100 > 100) v100 = 100;
  add_section_label(parent, "Volume", 0xFFFFFF);
  this->dw_volume_slider_ = lv_slider_create(parent);
  lv_obj_set_width(this->dw_volume_slider_, LV_PCT(100));
  lv_obj_set_height(this->dw_volume_slider_, 22);
  lv_slider_set_range(this->dw_volume_slider_, 0, 100);
  lv_slider_set_value(this->dw_volume_slider_, v100, LV_ANIM_OFF);
  lv_obj_add_event_cb(this->dw_volume_slider_,
                      &HAPanel::on_detail_volume_slider_,
                      LV_EVENT_VALUE_CHANGED, this);
  this->dw_volume_label_ = lv_label_create(parent);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d %%", v100);
  lv_label_set_text(this->dw_volume_label_, buf);
  lv_obj_set_style_text_color(this->dw_volume_label_, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(this->dw_volume_label_, &lv_font_montserrat_18, 0);
}

void HAPanel::build_detail_number_(lv_obj_t *parent, size_t entity_idx) {
  const Entity &e = this->entities_[entity_idx];
  float mn = this->get_attr_float_(entity_idx, "min", 0.0f);
  float mx = this->get_attr_float_(entity_idx, "max", 100.0f);
  if (mx <= mn) mx = mn + 1.0f;
  float step = this->get_attr_float_(entity_idx, "step", 1.0f);
  if (step <= 0.0f) step = 1.0f;
  float cur = mn;
  if (e.has_state && !e.state.empty()) {
    const char *c = e.state.c_str();
    char *end = nullptr;
    float v = strtof(c, &end);
    if (end != c)
      cur = v;
  }
  this->dw_number_min_ = mn;
  this->dw_number_step_ = step;
  int scale = (int) std::round(1.0f / step);
  if (scale < 1) scale = 1;
  int rng_min = (int) std::round(mn * scale);
  int rng_max = (int) std::round(mx * scale);
  if (rng_max <= rng_min) rng_max = rng_min + 1;
  int rng_cur = (int) std::round(cur * scale);
  if (rng_cur < rng_min) rng_cur = rng_min;
  if (rng_cur > rng_max) rng_cur = rng_max;
  add_section_label(parent, "Value", 0xFFFFFF);
  this->dw_number_slider_ = lv_slider_create(parent);
  lv_obj_set_width(this->dw_number_slider_, LV_PCT(100));
  lv_obj_set_height(this->dw_number_slider_, 22);
  lv_slider_set_range(this->dw_number_slider_, rng_min, rng_max);
  lv_slider_set_value(this->dw_number_slider_, rng_cur, LV_ANIM_OFF);
  lv_obj_add_event_cb(this->dw_number_slider_,
                      &HAPanel::on_detail_number_slider_,
                      LV_EVENT_VALUE_CHANGED, this);
  this->dw_number_label_ = lv_label_create(parent);
  char buf[32];
  if (step >= 1.0f)
    snprintf(buf, sizeof(buf), "%d", (int) std::round(cur));
  else
    snprintf(buf, sizeof(buf), "%.2f", cur);
  lv_label_set_text(this->dw_number_label_, buf);
  lv_obj_set_style_text_color(this->dw_number_label_, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(this->dw_number_label_, &lv_font_montserrat_18, 0);
}

void HAPanel::build_detail_select_(lv_obj_t *parent, size_t entity_idx) {
  const Entity &e = this->entities_[entity_idx];
  std::string opts_raw;
  if (this->get_attr_(entity_idx, "options", &opts_raw))
    this->dw_select_options_ = parse_ha_list_(opts_raw);
  if (this->dw_select_options_.empty()) {
    add_section_label(parent, "No options available", 0xCC4444);
    return;
  }
  add_section_label(parent, "Option", 0xFFFFFF);
  this->dw_select_dropdown_ = lv_dropdown_create(parent);
  lv_obj_set_width(this->dw_select_dropdown_, LV_PCT(100));
  std::string opt;
  for (size_t i = 0; i < this->dw_select_options_.size(); i++) {
    if (i) opt += "\n";
    opt += this->dw_select_options_[i];
  }
  lv_dropdown_set_options(this->dw_select_dropdown_, opt.c_str());
  for (size_t i = 0; i < this->dw_select_options_.size(); i++) {
    if (this->dw_select_options_[i] == e.state) {
      lv_dropdown_set_selected(this->dw_select_dropdown_, (uint16_t) i);
      break;
    }
  }
}

void HAPanel::build_detail_fan_(lv_obj_t *parent, size_t entity_idx) {
  int cur_pct = this->get_attr_int_(entity_idx, "percentage", 0);
  if (cur_pct < 0) cur_pct = 0;
  if (cur_pct > 100) cur_pct = 100;
  add_section_label(parent, "Speed", 0xFFFFFF);
  this->dw_fan_slider_ = lv_slider_create(parent);
  lv_obj_set_width(this->dw_fan_slider_, LV_PCT(100));
  lv_obj_set_height(this->dw_fan_slider_, 22);
  lv_slider_set_range(this->dw_fan_slider_, 0, 100);
  lv_slider_set_value(this->dw_fan_slider_, cur_pct, LV_ANIM_OFF);
  lv_obj_add_event_cb(this->dw_fan_slider_, &HAPanel::on_detail_fan_slider_,
                      LV_EVENT_VALUE_CHANGED, this);
  this->dw_fan_label_ = lv_label_create(parent);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d %%", cur_pct);
  lv_label_set_text(this->dw_fan_label_, buf);
  lv_obj_set_style_text_color(this->dw_fan_label_, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(this->dw_fan_label_, &lv_font_montserrat_18, 0);

  // Immediate Turn-off button (not staged) — most common single action on a fan.
  lv_obj_t *off = lv_button_create(parent);
  lv_obj_set_size(off, LV_PCT(100), 50);
  lv_obj_set_style_bg_color(off, lv_color_hex(0x553A2A), 0);
  lv_obj_set_style_bg_color(off, lv_color_hex(0x885633), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(off, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(off, 8, 0);
  lv_obj_set_style_border_width(off, 0, 0);
  lv_obj_add_event_cb(off, &HAPanel::on_fan_off_, LV_EVENT_CLICKED, this);
  lv_obj_t *l = lv_label_create(off);
  lv_label_set_text(l, "Turn off");
  lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
  lv_obj_center(l);
}

void HAPanel::build_detail_cover_(lv_obj_t *parent, size_t entity_idx) {
  // Three-button transport row, immediate (no Apply).
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_PCT(100), 60);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  struct BSpec { const char *text; lv_event_cb_t cb; };
  BSpec specs[] = {
      {LV_SYMBOL_UP "  Open", &HAPanel::on_cover_open_},
      {"Stop", &HAPanel::on_cover_stop_},
      {LV_SYMBOL_DOWN "  Close", &HAPanel::on_cover_close_},
  };
  for (auto &s : specs) {
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_size(b, 130, 50);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x222A33), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A4A6A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_add_event_cb(b, s.cb, LV_EVENT_CLICKED, this);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, s.text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
    lv_obj_center(l);
  }

  // Position slider — only if entity reports current_position. Otherwise the
  // cover is open/closed only and the buttons above are the whole interface.
  int cur_pos = this->get_attr_int_(entity_idx, "current_position", -1);
  if (cur_pos < 0) {
    add_section_label(parent, "Position not reported", 0xAAAAAA);
    return;
  }
  if (cur_pos > 100) cur_pos = 100;
  add_section_label(parent, "Position", 0xFFFFFF);
  this->dw_cover_slider_ = lv_slider_create(parent);
  lv_obj_set_width(this->dw_cover_slider_, LV_PCT(100));
  lv_obj_set_height(this->dw_cover_slider_, 22);
  lv_slider_set_range(this->dw_cover_slider_, 0, 100);
  lv_slider_set_value(this->dw_cover_slider_, cur_pos, LV_ANIM_OFF);
  lv_obj_add_event_cb(this->dw_cover_slider_,
                      &HAPanel::on_detail_cover_slider_,
                      LV_EVENT_VALUE_CHANGED, this);
  this->dw_cover_label_ = lv_label_create(parent);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d %%", cur_pos);
  lv_label_set_text(this->dw_cover_label_, buf);
  lv_obj_set_style_text_color(this->dw_cover_label_, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(this->dw_cover_label_, &lv_font_montserrat_18, 0);
}

// ---- detail apply ----

void HAPanel::apply_detail_() {
  if (!this->detail_open_)
    return;
  size_t i = this->detail_entity_idx_;
  if (i >= this->entities_.size())
    return;
  const Entity &e = this->entities_[i];
  const std::string &d = e.domain;
  std::map<std::string, std::string> data;
  data["entity_id"] = e.entity_id;

  if (d == "light") {
    bool on = this->dw_light_switch_ != nullptr &&
              lv_obj_has_state(this->dw_light_switch_, LV_STATE_CHECKED);
    if (!on) {
      this->call_homeassistant_service("light.turn_off", data);
      ESP_LOGI(TAG, "apply %s → light.turn_off", e.entity_id.c_str());
    } else {
      if (this->dw_brightness_slider_ != nullptr) {
        int v = lv_slider_get_value(this->dw_brightness_slider_);
        data["brightness_pct"] = std::to_string(v);
      }
      if (this->dw_ct_slider_ != nullptr) {
        int v = lv_slider_get_value(this->dw_ct_slider_);
        data["color_temp_kelvin"] = std::to_string(v);
      }
      this->call_homeassistant_service("light.turn_on", data);
      ESP_LOGI(TAG, "apply %s → light.turn_on (bri=%s, ct=%s)",
               e.entity_id.c_str(),
               data.count("brightness_pct") ? data["brightness_pct"].c_str() : "-",
               data.count("color_temp_kelvin") ? data["color_temp_kelvin"].c_str() : "-");
    }
  } else if (d == "climate") {
    // Always send both — HA tolerates redundant calls, and trying to track
    // "did the user change this" adds complexity for little win.
    if (this->dw_hvac_dropdown_ != nullptr && !this->dw_hvac_modes_.empty()) {
      uint16_t idx = lv_dropdown_get_selected(this->dw_hvac_dropdown_);
      if (idx < this->dw_hvac_modes_.size()) {
        std::map<std::string, std::string> hdata;
        hdata["entity_id"] = e.entity_id;
        hdata["hvac_mode"] = this->dw_hvac_modes_[idx];
        this->call_homeassistant_service("climate.set_hvac_mode", hdata);
        ESP_LOGI(TAG, "apply %s → climate.set_hvac_mode=%s",
                 e.entity_id.c_str(), this->dw_hvac_modes_[idx].c_str());
      }
    }
    if (this->dw_temp_slider_ != nullptr) {
      int v = lv_slider_get_value(this->dw_temp_slider_);
      float temp = (float) v * this->dw_temp_step_;
      char buf[32];
      snprintf(buf, sizeof(buf), "%.1f", temp);
      std::map<std::string, std::string> tdata;
      tdata["entity_id"] = e.entity_id;
      tdata["temperature"] = buf;
      this->call_homeassistant_service("climate.set_temperature", tdata);
      ESP_LOGI(TAG, "apply %s → climate.set_temperature=%s",
               e.entity_id.c_str(), buf);
    }
  } else if (d == "media_player") {
    if (this->dw_volume_slider_ != nullptr) {
      int v = lv_slider_get_value(this->dw_volume_slider_);
      char buf[16];
      snprintf(buf, sizeof(buf), "%.2f", (float) v / 100.0f);
      data["volume_level"] = buf;
      this->call_homeassistant_service("media_player.volume_set", data);
      ESP_LOGI(TAG, "apply %s → media_player.volume_set=%s",
               e.entity_id.c_str(), buf);
    }
  } else if (d == "number") {
    if (this->dw_number_slider_ != nullptr) {
      int v = lv_slider_get_value(this->dw_number_slider_);
      float val = (float) v * this->dw_number_step_;
      char buf[32];
      if (this->dw_number_step_ >= 1.0f)
        snprintf(buf, sizeof(buf), "%d", (int) std::round(val));
      else
        snprintf(buf, sizeof(buf), "%.2f", val);
      data["value"] = buf;
      this->call_homeassistant_service("number.set_value", data);
      ESP_LOGI(TAG, "apply %s → number.set_value=%s",
               e.entity_id.c_str(), buf);
    }
  } else if (d == "select") {
    if (this->dw_select_dropdown_ != nullptr &&
        !this->dw_select_options_.empty()) {
      uint16_t idx = lv_dropdown_get_selected(this->dw_select_dropdown_);
      if (idx < this->dw_select_options_.size()) {
        data["option"] = this->dw_select_options_[idx];
        this->call_homeassistant_service("select.select_option", data);
        ESP_LOGI(TAG, "apply %s → select.select_option=%s",
                 e.entity_id.c_str(), this->dw_select_options_[idx].c_str());
      }
    }
  } else if (d == "fan") {
    if (this->dw_fan_slider_ != nullptr) {
      int v = lv_slider_get_value(this->dw_fan_slider_);
      data["percentage"] = std::to_string(v);
      this->call_homeassistant_service("fan.set_percentage", data);
      ESP_LOGI(TAG, "apply %s → fan.set_percentage=%d", e.entity_id.c_str(), v);
    }
  } else if (d == "cover") {
    if (this->dw_cover_slider_ != nullptr) {
      int v = lv_slider_get_value(this->dw_cover_slider_);
      data["position"] = std::to_string(v);
      this->call_homeassistant_service("cover.set_cover_position", data);
      ESP_LOGI(TAG, "apply %s → cover.set_cover_position=%d",
               e.entity_id.c_str(), v);
    }
  }
  this->close_detail_();
}

// ---- P7d event trampolines ----

void HAPanel::on_entity_row_long_pressed_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  lv_obj_t *row = lv_event_get_target_obj(e);
  size_t entity_idx = (size_t) (uintptr_t) lv_obj_get_user_data(row);
  // P7f: a confirm-flagged action-only entity (no detail modal — e.g.
  // script.panic) has no long-press target otherwise; route it to the confirm
  // sheet so long-press matches short-tap. Domains WITH a detail modal (incl.
  // cover, whose confirm short-tap is the no-slider sheet) still open the full
  // modal on long-press.
  if (entity_idx < self->entities_.size()) {
    const Entity &en = self->entities_[entity_idx];
    if (en.confirm && HAPanel::confirm_meaningful_(en.domain) &&
        !HAPanel::has_detail_(en.domain)) {
      self->open_confirm_action_(entity_idx);
      return;
    }
  }
  self->open_detail_(entity_idx);
}

void HAPanel::on_detail_apply_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  self->apply_detail_();
}

void HAPanel::on_detail_cancel_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  self->close_detail_();
}

void HAPanel::on_detail_bg_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  // Only dismiss if the click was on the modal bg itself, not a child widget.
  if (lv_event_get_target_obj(e) == self->detail_modal_)
    self->close_detail_();
}

// Slider live-update trampolines. Each just reads the slider value and
// refreshes its companion label so the user sees the value as they drag.

void HAPanel::on_detail_brightness_slider_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->dw_brightness_slider_ == nullptr ||
      self->dw_brightness_label_ == nullptr)
    return;
  int v = lv_slider_get_value(self->dw_brightness_slider_);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d %%", v);
  lv_label_set_text(self->dw_brightness_label_, buf);
}

void HAPanel::on_detail_ct_slider_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->dw_ct_slider_ == nullptr ||
      self->dw_ct_label_ == nullptr)
    return;
  int v = lv_slider_get_value(self->dw_ct_slider_);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d K", v);
  lv_label_set_text(self->dw_ct_label_, buf);
}

void HAPanel::on_detail_temp_slider_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->dw_temp_slider_ == nullptr ||
      self->dw_temp_label_ == nullptr)
    return;
  int v = lv_slider_get_value(self->dw_temp_slider_);
  char buf[24];
  snprintf(buf, sizeof(buf), "%.1f", (float) v * self->dw_temp_step_);
  lv_label_set_text(self->dw_temp_label_, buf);
}

void HAPanel::on_detail_number_slider_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->dw_number_slider_ == nullptr ||
      self->dw_number_label_ == nullptr)
    return;
  int v = lv_slider_get_value(self->dw_number_slider_);
  float val = (float) v * self->dw_number_step_;
  char buf[32];
  if (self->dw_number_step_ >= 1.0f)
    snprintf(buf, sizeof(buf), "%d", (int) std::round(val));
  else
    snprintf(buf, sizeof(buf), "%.2f", val);
  lv_label_set_text(self->dw_number_label_, buf);
}

void HAPanel::on_detail_volume_slider_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->dw_volume_slider_ == nullptr ||
      self->dw_volume_label_ == nullptr)
    return;
  int v = lv_slider_get_value(self->dw_volume_slider_);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d %%", v);
  lv_label_set_text(self->dw_volume_label_, buf);
}

void HAPanel::on_detail_fan_slider_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->dw_fan_slider_ == nullptr ||
      self->dw_fan_label_ == nullptr)
    return;
  int v = lv_slider_get_value(self->dw_fan_slider_);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d %%", v);
  lv_label_set_text(self->dw_fan_label_, buf);
}

void HAPanel::on_detail_cover_slider_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->dw_cover_slider_ == nullptr ||
      self->dw_cover_label_ == nullptr)
    return;
  int v = lv_slider_get_value(self->dw_cover_slider_);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d %%", v);
  lv_label_set_text(self->dw_cover_label_, buf);
}

// Immediate (no Apply) transport / off buttons.

void HAPanel::on_media_prev_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr) return;
  const Entity &en = self->entities_[self->detail_entity_idx_];
  std::map<std::string, std::string> d;
  d["entity_id"] = en.entity_id;
  self->call_homeassistant_service("media_player.media_previous_track", d);
  ESP_LOGI(TAG, "media prev: %s", en.entity_id.c_str());
}

void HAPanel::on_media_next_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr) return;
  const Entity &en = self->entities_[self->detail_entity_idx_];
  std::map<std::string, std::string> d;
  d["entity_id"] = en.entity_id;
  self->call_homeassistant_service("media_player.media_next_track", d);
  ESP_LOGI(TAG, "media next: %s", en.entity_id.c_str());
}

void HAPanel::on_media_play_pause_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr) return;
  const Entity &en = self->entities_[self->detail_entity_idx_];
  std::map<std::string, std::string> d;
  d["entity_id"] = en.entity_id;
  self->call_homeassistant_service("media_player.media_play_pause", d);
  ESP_LOGI(TAG, "media play_pause: %s", en.entity_id.c_str());
}

void HAPanel::on_media_mute_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr) return;
  size_t i = self->detail_entity_idx_;
  const Entity &en = self->entities_[i];
  // Flip current mute state. Default to muting if attribute hasn't arrived.
  std::string cur;
  bool muted_now = self->get_attr_(i, "is_volume_muted", &cur) && cur == "True";
  std::map<std::string, std::string> d;
  d["entity_id"] = en.entity_id;
  d["is_volume_muted"] = muted_now ? "false" : "true";
  self->call_homeassistant_service("media_player.volume_mute", d);
  ESP_LOGI(TAG, "media mute %s → %s", en.entity_id.c_str(),
           d["is_volume_muted"].c_str());
}

void HAPanel::on_cover_open_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr) return;
  const Entity &en = self->entities_[self->detail_entity_idx_];
  std::map<std::string, std::string> d;
  d["entity_id"] = en.entity_id;
  self->call_homeassistant_service("cover.open_cover", d);
  ESP_LOGI(TAG, "cover open: %s", en.entity_id.c_str());
}

void HAPanel::on_cover_stop_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr) return;
  const Entity &en = self->entities_[self->detail_entity_idx_];
  std::map<std::string, std::string> d;
  d["entity_id"] = en.entity_id;
  self->call_homeassistant_service("cover.stop_cover", d);
  ESP_LOGI(TAG, "cover stop: %s", en.entity_id.c_str());
}

void HAPanel::on_cover_close_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr) return;
  const Entity &en = self->entities_[self->detail_entity_idx_];
  std::map<std::string, std::string> d;
  d["entity_id"] = en.entity_id;
  self->call_homeassistant_service("cover.close_cover", d);
  ESP_LOGI(TAG, "cover close: %s", en.entity_id.c_str());
}

void HAPanel::on_fan_off_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr) return;
  const Entity &en = self->entities_[self->detail_entity_idx_];
  std::map<std::string, std::string> d;
  d["entity_id"] = en.entity_id;
  self->call_homeassistant_service("fan.turn_off", d);
  ESP_LOGI(TAG, "fan off: %s", en.entity_id.c_str());
}

// ---------- P7f action confirm sheet ----------

// 200x70 coloured action button on the confirm sheet. enabled=false (entity
// unavailable) paints it grey, drops the click handler, and dims the label.
static lv_obj_t *add_confirm_button(lv_obj_t *parent, const char *text,
                                    uint32_t bg, uint32_t text_col,
                                    lv_event_cb_t cb, void *ud, bool enabled) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, 200, 70);
  lv_obj_set_style_radius(b, 12, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  if (enabled) {
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    // Press feedback: dim the fill rather than carry a per-colour pressed tint.
    lv_obj_set_style_bg_opa(b, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  } else {
    lv_obj_set_style_bg_color(b, lv_color_hex(0x333333), 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    text_col = 0x777777;
  }
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_color(l, lv_color_hex(text_col), 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_18, 0);
  lv_obj_center(l);
  return b;
}

void HAPanel::build_confirm_sheet_(lv_obj_t *scr) {
  this->confirm_sheet_ = lv_obj_create(scr);
  lv_obj_remove_style_all(this->confirm_sheet_);
  lv_obj_set_size(this->confirm_sheet_, 480, 480);
  lv_obj_set_pos(this->confirm_sheet_, 0, 0);
  lv_obj_set_style_bg_color(this->confirm_sheet_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->confirm_sheet_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(this->confirm_sheet_, 0, 0);
  lv_obj_set_style_border_width(this->confirm_sheet_, 0, 0);
  lv_obj_add_flag(this->confirm_sheet_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(this->confirm_sheet_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(this->confirm_sheet_, &HAPanel::on_confirm_bg_clicked_,
                      LV_EVENT_CLICKED, this);

  // Title centered at top.
  this->confirm_title_ = lv_label_create(this->confirm_sheet_);
  lv_label_set_text(this->confirm_title_, "");
  lv_obj_set_style_text_color(this->confirm_title_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(this->confirm_title_, &lv_font_montserrat_18, 0);
  lv_obj_set_width(this->confirm_title_, 400);
  lv_obj_set_style_text_align(this->confirm_title_, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(this->confirm_title_, LV_LABEL_LONG_DOT);
  lv_obj_align(this->confirm_title_, LV_ALIGN_TOP_MID, 0, 16);

  // "Currently unavailable" note under the title — shown only when the entity
  // has no live/usable state at open time (action buttons disabled too).
  this->confirm_unavail_label_ = lv_label_create(this->confirm_sheet_);
  lv_label_set_text(this->confirm_unavail_label_, "Currently unavailable");
  lv_obj_set_style_text_color(this->confirm_unavail_label_, lv_color_hex(0xCC4444), 0);
  lv_obj_set_style_text_font(this->confirm_unavail_label_, &lv_font_montserrat_18, 0);
  lv_obj_align(this->confirm_unavail_label_, LV_ALIGN_TOP_MID, 0, 46);
  lv_obj_add_flag(this->confirm_unavail_label_, LV_OBJ_FLAG_HIDDEN);

  // Body: centered flex column of big action buttons. Wiped + repopulated per
  // open. 24 px side inset clears the panel's curved corners; buttons are
  // 200 px and centred so they sit well within the [44, 436] safe band.
  this->confirm_body_ = lv_obj_create(this->confirm_sheet_);
  lv_obj_remove_style_all(this->confirm_body_);
  lv_obj_set_size(this->confirm_body_, 432, 312);
  lv_obj_set_pos(this->confirm_body_, 24, 76);
  lv_obj_set_style_bg_opa(this->confirm_body_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(this->confirm_body_, 4, 0);
  lv_obj_set_style_pad_row(this->confirm_body_, 14, 0);
  lv_obj_set_flex_flow(this->confirm_body_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(this->confirm_body_, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(this->confirm_body_, LV_DIR_VER);

  // Cancel row pinned to bottom (same geometry as detail / settings).
  lv_obj_t *btn_row = lv_obj_create(this->confirm_sheet_);
  lv_obj_remove_style_all(btn_row);
  lv_obj_set_size(btn_row, 480, 60);
  lv_obj_set_pos(btn_row, 0, 396);
  lv_obj_set_style_bg_color(btn_row, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(btn_row, LV_OPA_COVER, 0);
  lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);

  lv_obj_t *cancel = lv_button_create(btn_row);
  lv_obj_set_size(cancel, 200, 50);
  lv_obj_set_style_bg_color(cancel, lv_color_hex(0x222A33), 0);
  lv_obj_set_style_bg_color(cancel, lv_color_hex(0x3A4A6A), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(cancel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(cancel, 8, 0);
  lv_obj_set_style_border_width(cancel, 0, 0);
  lv_obj_add_event_cb(cancel, &HAPanel::on_confirm_cancel_, LV_EVENT_CLICKED, this);
  lv_obj_t *clbl = lv_label_create(cancel);
  lv_label_set_text(clbl, "Cancel");
  lv_obj_set_style_text_color(clbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(clbl, &lv_font_montserrat_18, 0);
  lv_obj_center(clbl);
}

void HAPanel::open_confirm_or_detail_(size_t entity_idx) {
  if (entity_idx >= this->entities_.size())
    return;
  const std::string &d = this->entities_[entity_idx].domain;
  // Domains with a P7d detail modal use it as the confirm path — EXCEPT cover,
  // whose confirm short-tap is the no-slider Open/Stop/Close sheet (the full
  // position modal stays on long-press).
  if (HAPanel::has_detail_(d) && d != "cover") {
    this->open_detail_(entity_idx);
    return;
  }
  // Action-only / lock / cover / switch / input_boolean → action confirm sheet.
  this->open_confirm_action_(entity_idx);
}

void HAPanel::open_confirm_action_(size_t entity_idx) {
  if (this->confirm_sheet_ == nullptr || entity_idx >= this->entities_.size())
    return;
  const Entity &e = this->entities_[entity_idx];
  this->confirm_entity_idx_ = entity_idx;
  lv_label_set_text(this->confirm_title_, e.friendly_name.c_str());
  lv_obj_clean(this->confirm_body_);

  const bool avail = e.has_state && e.state != "unavailable" &&
                     e.state != "unknown";
  if (this->confirm_unavail_label_ != nullptr) {
    if (avail)
      lv_obj_add_flag(this->confirm_unavail_label_, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_clear_flag(this->confirm_unavail_label_, LV_OBJ_FLAG_HIDDEN);
  }

  // Accent blue gets dark text for contrast; the muted green/amber/grey fills
  // carry white text like the rest of the panel.
  const uint32_t ACCENT = 0x44CCDD, ACCENT_TXT = 0x05151A;
  const std::string &d = e.domain;
  if (d == "scene") {
    add_confirm_button(this->confirm_body_, "Activate scene", ACCENT, ACCENT_TXT,
                       &HAPanel::on_confirm_single_, this, avail);
  } else if (d == "script") {
    add_confirm_button(this->confirm_body_, "Run script", ACCENT, ACCENT_TXT,
                       &HAPanel::on_confirm_single_, this, avail);
  } else if (d == "automation") {
    add_confirm_button(this->confirm_body_, "Trigger automation", ACCENT,
                       ACCENT_TXT, &HAPanel::on_confirm_single_, this, avail);
  } else if (d == "button") {
    add_confirm_button(this->confirm_body_, "Press button", ACCENT, ACCENT_TXT,
                       &HAPanel::on_confirm_single_, this, avail);
  } else if (d == "lock") {
    add_confirm_button(this->confirm_body_, "Lock", 0x2A553A, 0xFFFFFF,
                       &HAPanel::on_confirm_lock_, this, avail);
    add_confirm_button(this->confirm_body_, "Unlock", 0x553A2A, 0xFFFFFF,
                       &HAPanel::on_confirm_unlock_, this, avail);
  } else if (d == "cover") {
    add_confirm_button(this->confirm_body_, LV_SYMBOL_UP "  Open", 0x2A553A,
                       0xFFFFFF, &HAPanel::on_confirm_cover_open_, this, avail);
    add_confirm_button(this->confirm_body_, "Stop", 0x2E3640, 0xFFFFFF,
                       &HAPanel::on_confirm_cover_stop_, this, avail);
    add_confirm_button(this->confirm_body_, LV_SYMBOL_DOWN "  Close", 0x553A2A,
                       0xFFFFFF, &HAPanel::on_confirm_cover_close_, this, avail);
  } else if (d == "switch" || d == "input_boolean") {
    // Binary domains have no P7d modal; the confirm path is a single On/Off
    // button labelled for the action the current state implies.
    const char *txt = (e.has_state && e.state == "on") ? "Turn off" : "Turn on";
    add_confirm_button(this->confirm_body_, txt, ACCENT, ACCENT_TXT,
                       &HAPanel::on_confirm_single_, this, avail);
  } else {
    ESP_LOGW(TAG, "confirm sheet opened for unhandled domain '%s' (%s)",
             d.c_str(), e.entity_id.c_str());
  }

  lv_obj_clear_flag(this->confirm_sheet_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(this->confirm_sheet_);
  this->confirm_open_ = true;
  ESP_LOGI(TAG, "confirm sheet open: %s%s", e.entity_id.c_str(),
           avail ? "" : " (unavailable)");
}

void HAPanel::close_confirm_() {
  if (this->confirm_sheet_ == nullptr)
    return;
  lv_obj_add_flag(this->confirm_sheet_, LV_OBJ_FLAG_HIDDEN);
  this->confirm_open_ = false;
  ESP_LOGD(TAG, "confirm close");
}

void HAPanel::fire_confirm_service_(const char *service) {
  if (this->confirm_entity_idx_ >= this->entities_.size()) {
    this->close_confirm_();
    return;
  }
  const Entity &e = this->entities_[this->confirm_entity_idx_];
  std::map<std::string, std::string> data;
  data["entity_id"] = e.entity_id;
  this->call_homeassistant_service(service, data);
  ESP_LOGI(TAG, "confirm %s → %s", e.entity_id.c_str(), service);
  this->close_confirm_();
}

// ---- P7f confirm-sheet trampolines ----

void HAPanel::on_confirm_cancel_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->close_confirm_();
}

void HAPanel::on_confirm_bg_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  if (lv_event_get_target_obj(e) == self->confirm_sheet_)
    self->close_confirm_();
}

void HAPanel::on_confirm_single_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  // Reuse the normal dispatch — correct for action domains (scene/script/
  // automation/button) and for switch/input_boolean (state-based turn_on/off).
  self->tap_entity_(self->confirm_entity_idx_);
  self->close_confirm_();
}

void HAPanel::on_confirm_lock_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->fire_confirm_service_("lock.lock");
}

void HAPanel::on_confirm_unlock_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->fire_confirm_service_("lock.unlock");
}

void HAPanel::on_confirm_cover_open_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->fire_confirm_service_("cover.open_cover");
}

void HAPanel::on_confirm_cover_stop_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->fire_confirm_service_("cover.stop_cover");
}

void HAPanel::on_confirm_cover_close_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->fire_confirm_service_("cover.close_cover");
}

}  // namespace ha_panel
}  // namespace esphome
