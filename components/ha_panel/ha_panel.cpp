#include "ha_panel.h"
#include "mdi_icons.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "esp_heap_caps.h"  // UE6 diagnostics: internal-heap free / largest block

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/time.h"
#include "esphome/core/version.h"
#include "esphome/components/api/api_server.h"
#include "esphome/components/json/json_util.h"

namespace esphome {
namespace ha_panel {

static const char *const TAG = "ha_panel";

// ---------- codegen-time builders ----------

void HAPanel::add_page(const std::string &name) {
  Page p;
  p.name = name;
  this->pages_.push_back(std::move(p));
}

void HAPanel::add_entity(const std::string &entity_id, const std::string &friendly_name,
                         const std::string &icon_override, bool confirm,
                         EntitySize size, bool realtime, uint8_t name_style) {
  if (this->pages_.empty()) {
    ESP_LOGE(TAG, "add_entity called before any page — codegen bug");
    return;
  }
  Entity e;
  e.entity_id = entity_id;
  e.friendly_name = friendly_name.empty() ? entity_id : friendly_name;
  e.domain = HAPanel::extract_domain_(entity_id);
  e.render_class = HAPanel::render_class_for_(e.domain);
  e.icon_override = icon_override;
  e.confirm = confirm;
  e.size = size;
  e.realtime = realtime;
  e.name_style = name_style;
  size_t idx = this->entities_.size();
  this->entities_.push_back(std::move(e));
  this->pages_.back().entity_indices.push_back(idx);
}

// UE12: map the validated `type:` string to the enum (codegen guarantees one of
// the known values; default COUNT is just a belt-and-suspenders fallback).
static ReportType report_type_from_(const std::string &t) {
  if (t == "bool") return ReportType::BOOLEAN;
  if (t == "offline") return ReportType::OFFLINE;
  if (t == "sum") return ReportType::SUM;
  if (t == "avg") return ReportType::AVG;
  if (t == "min") return ReportType::MIN;
  if (t == "max") return ReportType::MAX;
  return ReportType::COUNT;
}

// UE12: "page" → only the report row's own page; anything else → all pages.
static ReportScope report_scope_from_(const std::string &s) {
  return s == "page" ? ReportScope::PAGE : ReportScope::ALL;
}

void HAPanel::add_report(const std::string &title, const std::string &type,
                         const std::string &domains_csv, const std::string &match_state_csv,
                         const std::string &device_class, const std::string &unit,
                         bool show_total, bool show_source, const std::string &scope,
                         const std::string &icon, EntitySize size, uint8_t name_style) {
  if (this->pages_.empty()) {
    ESP_LOGE(TAG, "add_report called before any page — codegen bug");
    return;
  }
  Entity e;
  // Synthetic: no entity_id (never subscribed), domain "report" routes the row
  // through RenderClass::REPORT_TEXT for layout + inert tap.
  e.friendly_name = title;
  e.domain = "report";
  e.render_class = RenderClass::REPORT_TEXT;
  // UE12: optional icon. Empty → no icon column (resolve_icon_ short-circuits
  // "report" + empty override); set → resolved like any other row's icon.
  e.icon_override = icon;
  e.size = size;
  e.report.type = report_type_from_(type);
  e.report.scope = report_scope_from_(scope);
  e.report.domains = HAPanel::parse_ha_list_(domains_csv);
  e.report.match_state = HAPanel::parse_ha_list_(match_state_csv);
  e.report.device_class = device_class;
  e.report.unit = unit;
  e.report.show_total = show_total;
  e.report.show_source = show_source;
  e.name_style = name_style;
  size_t idx = this->entities_.size();
  this->entities_.push_back(std::move(e));
  this->pages_.back().entity_indices.push_back(idx);
}

// UE11: map the validated `picker_badge:` type / agg strings to enums. Codegen
// guarantees a known value; NONE / AVG are belt-and-suspenders defaults.
static BadgeType badge_type_from_(const std::string &t) {
  if (t == "lights_on") return BadgeType::LIGHTS_ON;
  if (t == "devices_on") return BadgeType::DEVICES_ON;
  if (t == "unlocked") return BadgeType::UNLOCKED;
  if (t == "open_covers") return BadgeType::OPEN_COVERS;
  if (t == "media_playing") return BadgeType::MEDIA_PLAYING;
  if (t == "climate_active") return BadgeType::CLIMATE_ACTIVE;
  if (t == "running") return BadgeType::RUNNING;
  if (t == "offline") return BadgeType::OFFLINE;
  if (t == "entities") return BadgeType::ENTITIES;
  if (t == "open_doors") return BadgeType::OPEN_DOORS;
  if (t == "motion") return BadgeType::MOTION;
  if (t == "low_battery") return BadgeType::LOW_BATTERY;
  if (t == "alarm") return BadgeType::ALARM;
  if (t == "temperature") return BadgeType::TEMPERATURE;
  if (t == "humidity") return BadgeType::HUMIDITY;
  if (t == "power") return BadgeType::POWER;
  if (t == "co2") return BadgeType::CO2;
  if (t == "aqi") return BadgeType::AQI;
  if (t == "severity") return BadgeType::SEVERITY;
  if (t == "idle") return BadgeType::IDLE;
  return BadgeType::NONE;
}

static BadgeAgg badge_agg_from_(const std::string &a) {
  if (a == "min") return BadgeAgg::MIN;
  if (a == "max") return BadgeAgg::MAX;
  if (a == "sum") return BadgeAgg::SUM;
  return BadgeAgg::AVG;
}

void HAPanel::add_page_badge(const std::string &type, const std::string &agg,
                             int threshold, const std::string &unit) {
  if (this->pages_.empty()) {
    ESP_LOGE(TAG, "add_page_badge called before any page — codegen bug");
    return;
  }
  PickerBadge b;
  b.type = badge_type_from_(type);
  b.agg = badge_agg_from_(agg);
  b.threshold = threshold;
  b.unit = unit;
  if (b.type == BadgeType::NONE)
    return;  // "none" → leave the page badgeless
  this->pages_.back().badges.push_back(b);
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

  // UE12: report rows carry no domain icon (there's no "report" glyph and a
  // fallback "?" would read as broken). Empty result → name flush-left layout.
  if (e.domain == "report" && e.icon_override.empty()) {
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

const lv_font_t *HAPanel::resolve_name_font_(const Entity &e) const {
  // UE13: only bold / italic need a baked font; underline is decor, plain uses
  // the RowMetrics built-in. Returns nullptr to mean "keep the regular font".
  const bool bold = e.name_style & STYLE_BOLD;
  const bool italic = e.name_style & STYLE_ITALIC;
  if (!bold && !italic)
    return nullptr;
  uint8_t si = e.size == EntitySize::MEDIUM ? 1 : e.size == EntitySize::LARGE ? 2 : 0;
  // variant 0 = bold, 1 = italic. bold (or bold+italic) prefers the bold font;
  // italic-only uses the italic font; bold+italic falls back to bold if that's
  // the only one wired.
  font::Font *f = nullptr;
  if (bold)
    f = this->style_fonts_[si][0];
  if (f == nullptr && italic)
    f = this->style_fonts_[si][1];
  return f != nullptr ? f->get_lv_font() : nullptr;
}

// ---------- setup / dump ----------

void HAPanel::setup() {
  ESP_LOGCONFIG(TAG, "Subscribing to %u entities across %u pages",
                (unsigned) this->entities_.size(), (unsigned) this->pages_.size());
  for (const auto &e : this->entities_) {
    // UE12: report rows are synthetic — no entity_id, no HA state to subscribe.
    if (e.render_class == RenderClass::REPORT_TEXT)
      continue;
    this->subscribe_homeassistant_state(&HAPanel::on_state_, e.entity_id);
  }
  // E7 Step 0 prototype: connect-time `brightness` subscription for lights
  // only. Rides the initial state_subs cursor walk (no re-arm), so the value
  // is cached before the first modal open and the light detail slider can show
  // truth instead of a fake 100 %. Scope is deliberately narrow — one attr,
  // lights only — to stay well under the ~278-sub burst that saturated the
  // P7d iter-1 attempt (the full per-domain attr set). ~88 state subs + ~30
  // light brightness subs ≈ 118 total. Watch the connect log for `Buffer full`
  // / unresponsive-disconnect before extending this to the D/UI tasks.
  {
    unsigned light_subs = 0;
    for (size_t i = 0; i < this->entities_.size(); i++) {
      if (this->entities_[i].domain == "light") {
        this->subscribe_attr_(i, "brightness");
        light_subs++;
      }
    }
    ESP_LOGCONFIG(TAG, "E7: subscribed brightness for %u light entities", light_subs);
  }
  // UE3 follow-up: connect-time climate attribute subscriptions. The detail
  // modal's setpoint dial + "Current" label need the standard climate attrs
  // (current_temperature, temperature, min_temp, max_temp, target_temp_step,
  // hvac_modes) — all part of HA's ClimateEntity schema, not integration custom
  // props. Like E7 brightness, these ride the initial state_subs cursor walk
  // (no re-arm), so they're cached before the first modal open. Without them
  // the modal fell back to a Celsius 7-35 range (dial pinned at the 21 midpoint)
  // and "Current: --" for an actually-°F thermostat. Scoped to climate only —
  // 6 attrs per climate entity, a handful of entities — staying far under the
  // ~278-sub burst that saturated the P7d iter-1 attempt (88 state + ~30
  // brightness + climate ≈ 130 for a typical config). Watch the connect log for
  // `Buffer full` before extending this to other detail domains.
  {
    unsigned climate_subs = 0;
    for (size_t i = 0; i < this->entities_.size(); i++) {
      if (this->entities_[i].domain != "climate")
        continue;
      for (const char *a : attrs_for_domain_("climate")) {
        this->subscribe_attr_(i, a);
        climate_subs++;
      }
    }
    ESP_LOGCONFIG(TAG, "UE3: subscribed %u climate attrs", climate_subs);
  }
  // P7d follow-up: per-domain attribute subscriptions REMAIN DISABLED for the
  // high-count domains (lights × full attr set, media, number, select, fan,
  // cover). Only the two narrow, connect-time exceptions above ride the cursor
  // walk: E7 brightness (lights) and UE3 climate attrs. The original failure
  // below was a burst-SIZE problem, not an attr-subs-are-impossible one.
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
  this->leds_by_entity_.assign(this->entities_.size(), nullptr);  // UE5
  ESP_LOGCONFIG(TAG, "P7e icon column %s",
                this->mdi_font_ != nullptr ? "enabled (mdi_font set)" : "disabled (no mdi_font)");
#ifdef USE_SPEAKER
  // Precompute the touch click. 16-bit signed mono, little-endian, 16 kHz —
  // matches the i2s_audio speaker + ES8311. A tap just re-queues this buffer.
  {
    // An IMPULSIVE click, not a sustained tone. The pleasant sound the codec
    // makes on stream-start (heard on boot from the warm-up) is a short
    // broadband transient — a "tick/knock" — and a sustained sine at full gain
    // just buzzed this small speaker. So: very fast decay (a few ms), a touch
    // of decaying noise for broadband "knock" body, mid-low pitch, low level.
    //   - dur ~12 ms, tau ~1.8 ms → percussive, dies almost immediately;
    //   - tone (≈900 Hz) + a little filtered-ish noise mixed in → texture;
    //   - 0.4 ms cosine attack so the onset isn't a hard step;
    //   - cosine tail to exactly 0 for a clean return to the silence stream.
    // Tunable by ear: tau (snap), freq (pitch), noise_mix (knock vs tick), peak.
    const float sample_rate = 16000.0f;
    const float freq = 900.0f;
    const float dur_s = 0.012f;
    const float tau = 0.0018f;            // very fast decay → impulsive
    const float peak = 0.005f * 32767.0f;
    const float noise_mix = 0.35f;        // 0 = pure tone, 1 = pure noise
    const float pi = 3.14159265358979f;
    const size_t n = (size_t) (sample_rate * dur_s);
    const size_t attack = 6;              // ~0.4 ms cosine onset
    const size_t tail = 16;               // ~1 ms cosine fade to 0
    // Deterministic noise (xorshift) so the click is identical every build/boot.
    uint32_t rng = 0x9E3779B9u;
    auto noise = [&rng]() -> float {
      rng ^= rng << 13;
      rng ^= rng >> 17;
      rng ^= rng << 5;
      return (float) ((int32_t) rng) / 2147483648.0f;  // [-1, 1)
    };
    this->click_pcm_.resize(n * 2);
    for (size_t i = 0; i < n; i++) {
      float t = (float) i / sample_rate;
      float env = expf(-t / tau);
      if (i < attack)
        env *= 0.5f * (1.0f - cosf(pi * (float) i / attack));
      if (i >= n - tail)
        env *= 0.5f * (1.0f - cosf(pi * (float) (n - 1 - i) / tail));
      float sig = (1.0f - noise_mix) * sinf(2.0f * pi * freq * t) +
                  noise_mix * noise();
      int16_t s = (int16_t) lroundf(peak * env * sig);
      this->click_pcm_[2 * i] = (uint8_t) (s & 0xFF);
      this->click_pcm_[2 * i + 1] = (uint8_t) ((s >> 8) & 0xFF);
    }
    ESP_LOGCONFIG(TAG, "touch click PCM ready (%u bytes)",
                  (unsigned) this->click_pcm_.size());
  }
#endif
  this->build_ui_();
  // UE6: the history worker task is created lazily on first history open, NOT
  // here — its internal-RAM stack would otherwise shrink the free heap during the
  // memory-tight boot/WiFi-connect window and abort the WiFi scan allocator
  // (bad_alloc in wifi_process_event_). See ensure_history_worker_.
  ESP_LOGCONFIG(TAG, "UE6 heap @setup-end: internal free=%u largest=%u",
                (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void HAPanel::loop() { this->poll_history_fetch_(); }

void HAPanel::on_attr_(size_t entity_idx, const std::string &attr_name,
                       StringRef value) {
  if (entity_idx >= this->entities_.size())
    return;
  auto &attrs = this->entities_[entity_idx].attrs;
  bool first_arrival = attrs.find(attr_name) == attrs.end();
  attrs[attr_name] = value.str();
  ESP_LOGD(TAG, "%s.%s = %s", this->entities_[entity_idx].entity_id.c_str(),
           attr_name.c_str(), value.c_str());
  // E7: cache last non-null brightness (HA sends 0-255; "None" when off) so an
  // off light's detail slider can seed from the prior on-level on toggle-on.
  if (attr_name == "brightness") {
    int raw = this->get_attr_int_(entity_idx, "brightness", -1);
    if (raw >= 0)
      this->entities_[entity_idx].last_bri_pct = (raw * 100 + 127) / 255;
  }
  // Climate rows render the current temperature beside the mode; refresh the row
  // whenever it changes (first arrival at connect, and live updates after).
  if (attr_name == "current_temperature" &&
      this->entities_[entity_idx].domain == "climate")
    this->rebuild_entity_row_(entity_idx);
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
  for (size_t pi = 0; pi < this->pages_.size(); pi++) {
    const auto &page = this->pages_[pi];
    ESP_LOGCONFIG(TAG, "  [%u] %s (%u entities)", (unsigned) pi, page.name.c_str(),
                  (unsigned) page.entity_indices.size());
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
    // E9: capture chartable values into the history ring buffer, and live-tail
    // the chart if its sheet is currently open on this entity.
    this->record_history_(i);
    // UE6: don't live-tail while a worker backfill is in flight — the redraw
    // would hide the spinner and paint the soon-to-be-replaced old samples. The
    // fetch result supersedes anything we'd append here anyway.
    if (this->history_open_ && this->history_entity_idx_ == i &&
        this->hist_fetch_state_.load(std::memory_order_relaxed) != HIST_RUNNING) {
      // Live tail: extend the open sheet's working set with the new value,
      // stamped in the same timebase as the sheet's current samples (epoch for
      // REST mode, uptime-seconds otherwise), then redraw.
      float v;
      if (HAPanel::state_to_value_(this->entities_[i], &v)) {
        uint32_t ts = millis() / 1000u;
        if (this->history_rest_mode_ && this->history_time_ != nullptr) {
          auto t = this->history_time_->utcnow();
          if (t.is_valid())
            ts = (uint32_t) t.timestamp;
        }
        this->history_samples_.push_back({ts, v});
      }
      this->redraw_history_();
    }
    // UE12: any state change can shift a report's aggregate — recompute all
    // report rows. Bounded (tens of entities); see the plan's perf note.
    this->recompute_reports_();
    // UE11: keep the picker badges live while it's open (cheap — only runs when
    // the picker is visible, which is brief). Closed → recomputed on next open.
    if (this->picker_ != nullptr &&
        !lv_obj_has_flag(this->picker_, LV_OBJ_FLAG_HIDDEN))
      this->update_picker_badges_();
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
      // Climate rows show "<mode>  <current temp>°" so the page conveys the room
      // reading at a glance, not just the hvac-mode word. current_temperature
      // arrives via the UE3 connect-time attr subs; on_attr_ re-runs this row.
      if (e.domain == "climate" && e.has_state) {
        float ct = this->get_attr_float_(entity_idx, "current_temperature", NAN);
        char cbuf[48];
        if (std::isnan(ct))
          snprintf(cbuf, sizeof(cbuf), "%s", e.state.c_str());
        else
          snprintf(cbuf, sizeof(cbuf), "%s  %.0f\xC2\xB0", e.state.c_str(), ct);
        lv_label_set_text(w, cbuf);
      } else {
        lv_label_set_text(w, e.has_state ? e.state.c_str() : "...");
      }
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
      // UE5: drive the binary_sensor status LED from the same state. Reuse `col`
      // (on=green, off=grey, unavailable=red); brightness carries the "glow":
      // full when active, a dim ember when off, mid when unavailable.
      if (e.domain == "binary_sensor" &&
          entity_idx < this->leds_by_entity_.size()) {
        lv_obj_t *led = this->leds_by_entity_[entity_idx];
        if (led != nullptr) {
          lv_led_set_color(led, lv_color_hex(col));
          uint8_t bright = 60;  // off: dim ember
          if (e.state == "on")
            bright = 255;  // active: full glow
          else if (e.state == "unavailable" || e.state == "unknown")
            bright = 160;
          lv_led_set_brightness(led, bright);
        }
      }
      return;
    }
    case RenderClass::REPORT_TEXT: {
      // UE12: report value + colour are computed by recompute_reports_, which
      // runs after build_ui_ and on every state change. Nothing per-entity to
      // refresh here (a report has no own HA state).
      return;
    }
  }
}

// ---------- UE12 report aggregation ----------

void HAPanel::recompute_reports_() {
  for (size_t i = 0; i < this->entities_.size(); i++) {
    Entity &r = this->entities_[i];
    if (r.render_class != RenderClass::REPORT_TEXT)
      continue;
    // UE12: page scope → limit the scan to the page that holds this report row
    // (each entity index lives in exactly one page). all scope → nullptr.
    const std::vector<size_t> *scope = nullptr;
    if (r.report.scope == ReportScope::PAGE) {
      for (const auto &p : this->pages_) {
        bool here = false;
        for (size_t ei : p.entity_indices)
          if (ei == i) { here = true; break; }
        if (here) {
          scope = &p.entity_indices;
          break;
        }
      }
    }
    std::string text;
    uint32_t col = 0xFFFFFF;
    this->compute_report_(r.report, scope, &text, &col);
    r.state = text;
    r.has_state = true;
    lv_obj_t *w = (i < this->widgets_by_entity_.size())
                      ? this->widgets_by_entity_[i]
                      : nullptr;
    if (w != nullptr) {
      lv_label_set_text(w, text.c_str());
      lv_obj_set_style_text_color(w, lv_color_hex(col), 0);
    }
  }
}

void HAPanel::compute_report_(const ReportSpec &s, const std::vector<size_t> *scope_indices,
                              std::string *out_text, uint32_t *out_color) {
  // Shared palette with rebuild_entity_row_: white neutral, grey idle, green
  // all-clear, amber active, red alert.
  constexpr uint32_t NEUTRAL = 0xFFFFFF, GREY = 0x888888, GREEN = 0x66BB66,
                     AMBER = 0xDDAA33, RED = 0xCC4444;
  *out_color = NEUTRAL;

  // UE12: visit each candidate entity — the report's own page (page scope) or
  // every entity (all scope, scope_indices == nullptr). `fn` returning is the
  // per-candidate body; use `return` inside it where a loop would `continue`.
  auto for_each = [&](const std::function<void(const Entity &)> &fn) {
    if (scope_indices != nullptr) {
      for (size_t idx : *scope_indices)
        if (idx < this->entities_.size())
          fn(this->entities_[idx]);
    } else {
      for (const auto &e : this->entities_)
        fn(e);
    }
  };

  // A real (non-report) entity is "in scope" when it passes the domain +
  // device_class filter. Empty domains = any. device_class needs UE7's attr
  // subscription, so it matches nothing until that lands (documented).
  auto in_scope = [&](const Entity &e) -> bool {
    if (e.render_class == RenderClass::REPORT_TEXT)
      return false;
    if (!s.domains.empty()) {
      bool ok = false;
      for (const auto &d : s.domains)
        if (e.domain == d) { ok = true; break; }
      if (!ok)
        return false;
    }
    if (!s.device_class.empty()) {
      auto it = e.attrs.find("device_class");
      if (it == e.attrs.end() || it->second != s.device_class)
        return false;
    }
    return true;
  };

  switch (s.type) {
    case ReportType::COUNT:
    case ReportType::BOOLEAN: {
      int total = 0, matched = 0;
      for_each([&](const Entity &e) {
        if (!in_scope(e))
          return;
        total++;
        bool m = s.match_state.empty();
        for (const auto &st : s.match_state)
          if (e.state == st) { m = true; break; }
        if (m)
          matched++;
      });
      if (s.type == ReportType::BOOLEAN) {
        if (matched == 0) {
          *out_text = LV_SYMBOL_OK;  // all clear
          *out_color = GREEN;
        } else {
          char buf[16];
          snprintf(buf, sizeof(buf), "%d", matched);
          *out_text = buf;
          *out_color = AMBER;
        }
      } else {
        char buf[24];
        if (s.show_total)
          snprintf(buf, sizeof(buf), "%d / %d", matched, total);
        else
          snprintf(buf, sizeof(buf), "%d", matched);
        *out_text = buf;
      }
      return;
    }
    case ReportType::OFFLINE: {
      int n = 0;
      for_each([&](const Entity &e) {
        if (!in_scope(e))
          return;
        if (!e.has_state || e.state == "unavailable" || e.state == "unknown")
          n++;
      });
      char buf[16];
      snprintf(buf, sizeof(buf), "%d", n);
      *out_text = buf;
      *out_color = n > 0 ? RED : GREY;
      return;
    }
    default: {  // SUM / AVG / MIN / MAX
      int cnt = 0;
      float acc = 0.0f, best = 0.0f;
      const Entity *best_e = nullptr;
      for_each([&](const Entity &e) {
        if (!in_scope(e))
          return;
        float v;
        if (!HAPanel::state_to_value_(e, &v))
          return;  // skip non-numeric / unavailable
        cnt++;
        acc += v;
        if (best_e == nullptr) {
          best = v;
          best_e = &e;
        } else if (s.type == ReportType::MIN && v < best) {
          best = v;
          best_e = &e;
        } else if (s.type == ReportType::MAX && v > best) {
          best = v;
          best_e = &e;
        }
      });
      if (cnt == 0) {
        *out_text = "--";  // no numeric data (ASCII — em dash isn't baked)
        *out_color = GREY;
        return;
      }
      float result = best;
      if (s.type == ReportType::SUM)
        result = acc;
      else if (s.type == ReportType::AVG)
        result = acc / cnt;
      char num[24];
      if (result == floorf(result))
        snprintf(num, sizeof(num), "%.0f", result);
      else
        snprintf(num, sizeof(num), "%.1f", result);
      char buf[80];
      bool extremum = (s.type == ReportType::MIN || s.type == ReportType::MAX);
      if (s.show_source && extremum && best_e != nullptr)
        snprintf(buf, sizeof(buf), "%s %s%s", best_e->friendly_name.c_str(), num,
                 s.unit.c_str());
      else
        snprintf(buf, sizeof(buf), "%s%s", num, s.unit.c_str());
      *out_text = buf;
      return;
    }
  }
}

// ---------- UE11 page-picker badges ----------

void HAPanel::update_picker_badges_() {
  for (size_t pi = 0;
       pi < this->pages_.size() && pi < this->picker_badges_.size(); pi++) {
    const std::vector<PickerBadge> &specs = this->pages_[pi].badges;
    std::vector<lv_obj_t *> &groups = this->picker_badges_[pi];
    for (size_t bi = 0; bi < groups.size() && bi < specs.size(); bi++) {
      lv_obj_t *grp = groups[bi];
      if (grp == nullptr)
        continue;
      std::string icon_name, value;
      uint32_t color = 0xFFFFFF;
      bool show = this->eval_picker_badge_(pi, specs[bi], &icon_name, &value, &color);
      if (!show) {
        lv_obj_add_flag(grp, LV_OBJ_FLAG_HIDDEN);
        continue;
      }
      lv_obj_t *bicon = lv_obj_get_child(grp, 0);
      lv_obj_t *bval = lv_obj_get_child(grp, 1);
      if (bicon != nullptr) {
        if (!icon_name.empty() && this->mdi_font_ != nullptr) {
          uint32_t cp = HAPanel::mdi_codepoint_(icon_name);
          if (cp == 0)
            cp = MDI_FALLBACK_CP;
          lv_label_set_text(bicon, HAPanel::utf8_encode_(cp).c_str());
          lv_obj_set_style_text_color(bicon, lv_color_hex(color), 0);
          lv_obj_clear_flag(bicon, LV_OBJ_FLAG_HIDDEN);
        } else {
          lv_obj_add_flag(bicon, LV_OBJ_FLAG_HIDDEN);
        }
      }
      if (bval != nullptr) {
        lv_label_set_text(bval, value.c_str());
        lv_obj_set_style_text_color(bval, lv_color_hex(color), 0);
        if (value.empty())
          lv_obj_add_flag(bval, LV_OBJ_FLAG_HIDDEN);
        else
          lv_obj_clear_flag(bval, LV_OBJ_FLAG_HIDDEN);
      }
      lv_obj_clear_flag(grp, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

bool HAPanel::eval_picker_badge_(size_t page_idx, const PickerBadge &spec,
                                 std::string *icon, std::string *value,
                                 uint32_t *color) {
  // Shared palette with compute_report_ / rebuild_entity_row_.
  constexpr uint32_t NEUTRAL = 0xFFFFFF, GREEN = 0x66BB66, AMBER = 0xDDAA33,
                     RED = 0xCC4444;
  *color = NEUTRAL;
  *icon = "";
  *value = "";
  const std::vector<size_t> &idxs = this->pages_[page_idx].entity_indices;

  // Count the page's OWN entities matching `pred` (report rows skipped — they're
  // synthetic, not HA state). Page-scoped by construction: idxs is this page.
  auto count_if = [&](const std::function<bool(const Entity &)> &pred) -> int {
    int n = 0;
    for (size_t ei : idxs) {
      if (ei >= this->entities_.size())
        continue;
      const Entity &e = this->entities_[ei];
      if (e.render_class == RenderClass::REPORT_TEXT)
        continue;
      if (pred(e))
        n++;
    }
    return n;
  };
  // device_class lookup — empty until UE7 subscribes the attr, so every
  // device_class-gated badge naturally evaluates to 0 / hidden today.
  auto dclass = [](const Entity &e) -> const std::string & {
    static const std::string empty;
    auto it = e.attrs.find("device_class");
    return it == e.attrs.end() ? empty : it->second;
  };
  auto offline_pred = [](const Entity &e) -> bool {
    return !e.has_state || e.state == "unavailable" || e.state == "unknown";
  };
  auto set_count = [&](int n) {
    char b[16];
    snprintf(b, sizeof(b), "%d", n);
    *value = b;
  };
  // Numeric aggregate over sensors with device_class `dc` (gated until UE7).
  auto numeric = [&](const char *dc, BadgeAgg agg, const char *def_unit,
                     const char *icon_name) -> bool {
    int cnt = 0;
    float acc = 0.0f, best = 0.0f;
    for (size_t ei : idxs) {
      if (ei >= this->entities_.size())
        continue;
      const Entity &e = this->entities_[ei];
      if (e.render_class == RenderClass::REPORT_TEXT || dclass(e) != dc)
        continue;
      float v;
      if (!HAPanel::state_to_value_(e, &v))
        continue;
      if (cnt == 0)
        best = v;
      else if (agg == BadgeAgg::MIN && v < best)
        best = v;
      else if (agg == BadgeAgg::MAX && v > best)
        best = v;
      acc += v;
      cnt++;
    }
    if (cnt == 0)
      return false;
    float r = best;
    if (agg == BadgeAgg::SUM)
      r = acc;
    else if (agg == BadgeAgg::AVG)
      r = acc / cnt;
    char num[24];
    if (r == floorf(r))
      snprintf(num, sizeof(num), "%.0f", r);
    else
      snprintf(num, sizeof(num), "%.1f", r);
    *value = std::string(num) + (spec.unit.empty() ? def_unit : spec.unit);
    *icon = icon_name;
    return true;
  };
  // True when an "on" binary_sensor's device_class is in `classes`.
  auto bs_class_on = [&](const Entity &e, const std::vector<std::string> &classes) -> bool {
    if (e.domain != "binary_sensor" || !e.has_state || e.state != "on")
      return false;
    const std::string &dc = dclass(e);
    for (const auto &c : classes)
      if (dc == c)
        return true;
    return false;
  };
  static const std::vector<std::string> ALARM_CLASSES = {
      "smoke", "moisture", "co", "gas", "problem", "safety"};
  static const std::vector<std::string> DOOR_CLASSES = {"door", "window",
                                                        "garage_door"};
  static const std::vector<std::string> MOTION_CLASSES = {"motion", "occupancy",
                                                          "presence"};

  switch (spec.type) {
    case BadgeType::LIGHTS_ON: {
      int n = count_if([](const Entity &e) {
        return e.domain == "light" && e.has_state && e.state == "on";
      });
      if (n == 0)
        return false;
      *icon = "lightbulb-on";
      set_count(n);
      return true;
    }
    case BadgeType::DEVICES_ON: {
      int n = count_if([](const Entity &e) {
        return (e.domain == "switch" || e.domain == "fan" ||
                e.domain == "input_boolean") &&
               e.has_state && e.state == "on";
      });
      if (n == 0)
        return false;
      *icon = "power-plug";
      set_count(n);
      return true;
    }
    case BadgeType::UNLOCKED: {
      int n = count_if([](const Entity &e) {
        return e.domain == "lock" && e.has_state && e.state != "locked" &&
               e.state != "unavailable" && e.state != "unknown";
      });
      if (n == 0)
        return false;
      *icon = "lock-open";
      set_count(n);
      return true;
    }
    case BadgeType::OPEN_COVERS: {
      int n = count_if([](const Entity &e) {
        return e.domain == "cover" && e.has_state && e.state != "closed" &&
               e.state != "unavailable" && e.state != "unknown";
      });
      if (n == 0)
        return false;
      *icon = "window-shutter-open";
      set_count(n);
      return true;
    }
    case BadgeType::MEDIA_PLAYING: {
      int n = count_if([](const Entity &e) {
        return e.domain == "media_player" && e.has_state && e.state == "playing";
      });
      if (n == 0)
        return false;
      *icon = "play";
      set_count(n);
      return true;
    }
    case BadgeType::CLIMATE_ACTIVE: {
      int n = count_if([](const Entity &e) {
        return e.domain == "climate" && e.has_state && e.state != "off" &&
               e.state != "unavailable" && e.state != "unknown";
      });
      if (n == 0)
        return false;
      *icon = "thermostat";
      set_count(n);
      return true;
    }
    case BadgeType::RUNNING: {
      int n = count_if([](const Entity &e) {
        if ((e.domain == "script" || e.domain == "automation") && e.has_state &&
            e.state == "on")
          return true;
        return e.domain == "timer" && e.state == "active";
      });
      if (n == 0)
        return false;
      *icon = "cog";
      set_count(n);
      return true;
    }
    case BadgeType::OFFLINE: {
      int n = count_if(offline_pred);
      if (n == 0)
        return false;
      *icon = "alert";
      set_count(n);
      *color = RED;
      return true;
    }
    case BadgeType::ENTITIES: {
      int n = count_if([](const Entity &) { return true; });
      if (n == 0)
        return false;
      set_count(n);
      return true;
    }
    case BadgeType::OPEN_DOORS: {
      int n = count_if([&](const Entity &e) { return bs_class_on(e, DOOR_CLASSES); });
      if (n == 0)
        return false;
      *icon = "door-open";
      set_count(n);
      *color = AMBER;
      return true;
    }
    case BadgeType::MOTION: {
      int n = count_if([&](const Entity &e) { return bs_class_on(e, MOTION_CLASSES); });
      if (n == 0)
        return false;
      *icon = "motion-sensor";
      set_count(n);
      *color = AMBER;
      return true;
    }
    case BadgeType::LOW_BATTERY: {
      int n = count_if([&](const Entity &e) {
        if (dclass(e) != "battery")
          return false;
        float v;
        return HAPanel::state_to_value_(e, &v) && v <= (float) spec.threshold;
      });
      if (n == 0)
        return false;
      *icon = "battery";
      set_count(n);
      *color = RED;
      return true;
    }
    case BadgeType::ALARM: {
      int n = count_if([&](const Entity &e) { return bs_class_on(e, ALARM_CLASSES); });
      if (n == 0)
        return false;
      *icon = "alert-circle";  // red presence dot, no number
      *color = RED;
      return true;
    }
    case BadgeType::TEMPERATURE:
      return numeric("temperature", spec.agg, "°", "thermometer");
    case BadgeType::HUMIDITY:
      return numeric("humidity", BadgeAgg::AVG, "%", "water-percent");
    case BadgeType::POWER:
      return numeric("power", BadgeAgg::SUM, "W", "power");
    case BadgeType::CO2:
      return numeric("carbon_dioxide", BadgeAgg::AVG, "ppm", "gauge");
    case BadgeType::AQI:
      return numeric("aqi", BadgeAgg::AVG, "", "gauge");
    case BadgeType::SEVERITY: {
      int alarms = count_if([&](const Entity &e) { return bs_class_on(e, ALARM_CLASSES); });
      int doors = count_if([&](const Entity &e) { return bs_class_on(e, DOOR_CLASSES); });
      int off = count_if(offline_pred);
      if (alarms > 0) {
        *icon = "alert-circle";
        *color = RED;
        return true;
      }
      if (doors > 0) {
        *icon = "alert-circle";
        *color = AMBER;
        return true;
      }
      if (off > 0) {
        *icon = "alert";
        *color = AMBER;
        return true;
      }
      return false;  // nothing wrong → no dot
    }
    case BadgeType::IDLE: {
      int on = count_if([](const Entity &e) {
        if (e.domain == "light" && e.has_state && e.state == "on")
          return true;
        if ((e.domain == "switch" || e.domain == "fan" ||
             e.domain == "input_boolean") &&
            e.has_state && e.state == "on")
          return true;
        if (e.domain == "media_player" && e.state == "playing")
          return true;
        if (e.domain == "cover" && e.has_state && e.state != "closed" &&
            e.state != "unavailable" && e.state != "unknown")
          return true;
        if (e.domain == "climate" && e.has_state && e.state != "off" &&
            e.state != "unavailable" && e.state != "unknown")
          return true;
        return false;
      });
      if (on > 0)
        return false;  // not idle
      *icon = "checkbox-marked-circle-outline";
      *color = GREEN;
      return true;
    }
    case BadgeType::NONE:
    default:
      return false;
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
    case RenderClass::REPORT_TEXT:
      // UE12: report rows are view-only — a tap fires nothing.
      return false;
  }
  return false;
}

bool HAPanel::tap(size_t page_idx, size_t entity_idx) {
  if (page_idx >= this->pages_.size())
    return false;
  const auto &ents = this->pages_[page_idx].entity_indices;
  if (entity_idx >= ents.size())
    return false;
  return this->tap_entity_(ents[entity_idx]);
}

// ---------- LVGL UI build ----------

// E8: per-size row geometry. SMALL holds exactly the historical hard-coded
// constants — an entity with no `size:` renders byte-for-byte as before. The
// right-side widget, insets, name font and icon glyph all scale together so a
// large row reads as one cohesive block, not just a tall thin one.
struct RowMetrics {
  int16_t height;
  const lv_font_t *name_font;
  int16_t icon_x;       // icon left inset (LV_ALIGN_LEFT_MID x)
  int16_t name_x_icon;  // name left inset when an icon is present
  int16_t name_x_noicon;
  int16_t name_w_icon;  // name width (ellipsis budget) with / without icon
  int16_t name_w_noicon;
  int16_t sw_w, sw_h;   // BINARY_SWITCH widget size
  int16_t sw_x;         // switch right inset (LV_ALIGN_RIGHT_MID x)
  int16_t label_x;      // text-widget right inset
};

static RowMetrics row_metrics_for(EntitySize size) {
  switch (size) {
    case EntitySize::MEDIUM:
      return {66, &lv_font_montserrat_24, 16, 66, 16, 300, 340, 66, 34, -18, -16};
    case EntitySize::LARGE:
      return {82, &lv_font_montserrat_32, 20, 84, 20, 330, 360, 84, 44, -20, -20};
    case EntitySize::SMALL:
    default:
      return {52, &lv_font_montserrat_18, 12, 48, 12, 240, 280, 50, 26, -16, -12};
  }
}

static lv_obj_t *make_entity_row(lv_obj_t *parent, const Entity &e, void *user_data,
                                 lv_event_cb_t cb, uintptr_t entity_idx,
                                 lv_obj_t **out_widget,
                                 lv_obj_t **out_unavail_label,
                                 const char *icon_utf8,
                                 const lv_font_t *mdi_lv_font,
                                 lv_obj_t **out_icon,
                                 lv_obj_t **out_led,
                                 const RowMetrics &m,
                                 const lv_font_t *name_font_override,
                                 bool name_underline) {
  *out_unavail_label = nullptr;
  *out_icon = nullptr;
  *out_led = nullptr;
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_width(btn, LV_PCT(100));
  lv_obj_set_height(btn, m.height);
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
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, m.icon_x, 0);
    *out_icon = icon;
  }

  lv_obj_t *name = lv_label_create(btn);
  lv_label_set_text(name, e.friendly_name.c_str());
  lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
  // UE13: bold/italic swap to a baked variant of the row's size; underline is a
  // runtime decor on top of whichever font. Fall back to the regular built-in.
  lv_obj_set_style_text_font(name, name_font_override != nullptr ? name_font_override : m.name_font, 0);
  if (name_underline)
    lv_obj_set_style_text_decor(name, LV_TEXT_DECOR_UNDERLINE, 0);
  // With an icon: shift name right and trim width so the ellipsis still lands
  // before the right-side widget. Without: name flush-left, full width.
  lv_obj_align(name, LV_ALIGN_LEFT_MID, have_icon ? m.name_x_icon : m.name_x_noicon, 0);
  lv_obj_set_width(name, have_icon ? m.name_w_icon : m.name_w_noicon);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

  // P7c: right-side widget varies by render class.
  lv_obj_t *w = nullptr;
  switch (e.render_class) {
    case RenderClass::BINARY_SWITCH: {
      w = lv_switch_create(btn);
      lv_obj_set_size(w, m.sw_w, m.sw_h);
      lv_obj_align(w, LV_ALIGN_RIGHT_MID, m.sw_x, 0);
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
      lv_obj_set_style_text_font(unavail, m.name_font, 0);
      lv_obj_align(unavail, LV_ALIGN_RIGHT_MID, m.label_x, 0);
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
      lv_obj_set_style_text_font(w, m.name_font, 0);
      lv_obj_align(w, LV_ALIGN_RIGHT_MID, m.sw_x, 0);
      break;
    }
    case RenderClass::LOCK_TEXT:
    case RenderClass::COVER_TEXT:
    case RenderClass::SUMMARY_TEXT:
    case RenderClass::READ_ONLY_TEXT:
    case RenderClass::REPORT_TEXT: {
      w = lv_label_create(btn);
      lv_label_set_text(w, e.has_state ? e.state.c_str() : "...");
      lv_obj_set_style_text_color(w, lv_color_hex(0xAAAAAA), 0);
      lv_obj_set_style_text_font(w, m.name_font, 0);
      lv_obj_align(w, LV_ALIGN_RIGHT_MID, m.label_x, 0);
      // UE5: binary_sensor rows also carry a glowing status dot at the far right.
      // The on/off word shifts left of it (deterministic, never overlaps — the
      // word is right-anchored to a slot left of the fixed LED). Colour +
      // brightness are driven from state in rebuild_entity_row_.
      if (e.domain == "binary_sensor") {
        const int led_sz = m.height / 4;  // 13/16/20 px across small/med/large
        lv_obj_t *led = lv_led_create(btn);
        lv_obj_set_size(led, led_sz, led_sz);
        lv_obj_align(led, LV_ALIGN_RIGHT_MID, m.label_x, 0);
        *out_led = led;
        lv_obj_align(w, LV_ALIGN_RIGHT_MID, m.label_x - led_sz - 8, 0);
      }
      break;
    }
  }
  // UE12: a report row is view-only — drop the pressed-state bg flash so it
  // doesn't read as a tappable control.
  if (e.render_class == RenderClass::REPORT_TEXT)
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1A1A), LV_STATE_PRESSED);

  *out_widget = w;
  return btn;
}

// UE10: idle timeout sliders work in units of 5 s (a coarse step that's usable
// on the round panel); seconds = slider value * 5, range 0..600 s. Dim/blank can
// be 0 ("Never"); sleep's min is 1 unit (5 s) since its master toggle disables it.
static constexpr int kTimeoutStepS = 5;
static constexpr int kTimeoutMaxUnits = 120;  // 600 s

// UE10: render a timeout's seconds as "Never" (0) / "N s" / "N min" / "Nm Ns".
static void fmt_timeout_(char *buf, size_t n, uint16_t secs) {
  if (secs == 0) {
    snprintf(buf, n, "Never");
  } else if (secs >= 60) {
    uint16_t m = secs / 60, r = secs % 60;
    if (r == 0)
      snprintf(buf, n, "%u min", (unsigned) m);
    else
      snprintf(buf, n, "%um %us", (unsigned) m, (unsigned) r);
  } else {
    snprintf(buf, n, "%u s", (unsigned) secs);
  }
}

// UE10: build one "Label ............ value" line + a full-width slider beneath
// it inside the settings content column. min_units = 0 lets the slider reach
// "Never" (dim/blank); 1 keeps sleep at ≥5 s. Returns the slider + value label.
static void add_timeout_row(lv_obj_t *parent, const char *label_text,
                            int min_units, uint16_t secs, lv_event_cb_t cb,
                            void *ud, lv_obj_t **out_slider, lv_obj_t **out_value) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_width(row, LV_PCT(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lbl = lv_label_create(row);
  lv_label_set_text(lbl, label_text);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);

  lv_obj_t *val = lv_label_create(row);
  char buf[24];
  fmt_timeout_(buf, sizeof(buf), secs);
  lv_label_set_text(val, buf);
  lv_obj_set_style_text_color(val, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(val, &lv_font_montserrat_18, 0);

  lv_obj_t *sl = lv_slider_create(parent);
  lv_obj_set_width(sl, LV_PCT(100));
  lv_obj_set_height(sl, 22);
  lv_slider_set_range(sl, min_units, kTimeoutMaxUnits);
  int units = (int) secs / kTimeoutStepS;
  if (units < min_units) units = min_units;
  if (units > kTimeoutMaxUnits) units = kTimeoutMaxUnits;
  lv_slider_set_value(sl, units, LV_ANIM_OFF);
  lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, ud);

  *out_slider = sl;
  *out_value = val;
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
  // CLICKABLE so backdrop taps are absorbed (not passed to the page beneath),
  // but no close-on-backdrop handler: only Apply / Cancel close this sheet.
  lv_obj_add_flag(this->settings_sheet_, LV_OBJ_FLAG_CLICKABLE);
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

  // ---- Power saving & burn-in protection (P8) ----
  // Combines idle dim/blank timeouts (AMOLED burn-in mitigation) with the
  // sleep-when-idle power controls under one heading.
  lv_obj_t *pwr_title = lv_label_create(content);
  lv_label_set_text(pwr_title, "Power saving & burn-in protection");
  lv_obj_set_style_text_color(pwr_title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(pwr_title, &lv_font_montserrat_18, 0);

  // ---- Screen reset sources (UE10) ----
  // Which inputs un-dim the screen / reset the idle timer. These GATE the dim +
  // blank timeouts below: with neither source on, a dimmed/blanked screen could
  // never recover, so dim/blank are suppressed entirely (the screen stays awake)
  // and their sliders grey out. NOT the sleep-wake control — a touch still wakes
  // the panel from the deep/light sleep tier regardless of these.
  lv_obj_t *reset_caption = lv_label_create(content);
  lv_label_set_text(reset_caption,
                    "What un-dims the screen (gates dim/blank).\n"
                    "Sleep still wakes on touch.");
  lv_obj_set_style_text_color(reset_caption, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(reset_caption, &lv_font_montserrat_18, 0);

  // Touch reset switch.
  lv_obj_t *rtouch_row = lv_obj_create(content);
  lv_obj_remove_style_all(rtouch_row);
  lv_obj_set_width(rtouch_row, LV_PCT(100));
  lv_obj_set_height(rtouch_row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rtouch_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rtouch_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(rtouch_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *rtouch_lbl = lv_label_create(rtouch_row);
  lv_label_set_text(rtouch_lbl, "Touch resets screen");
  lv_obj_set_style_text_color(rtouch_lbl, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(rtouch_lbl, &lv_font_montserrat_18, 0);
  this->reset_touch_switch_ = lv_switch_create(rtouch_row);
  if (this->reset_on_touch_)
    lv_obj_add_state(this->reset_touch_switch_, LV_STATE_CHECKED);
  lv_obj_add_event_cb(this->reset_touch_switch_, &HAPanel::on_reset_touch_switch_,
                      LV_EVENT_VALUE_CHANGED, this);

  // Motion reset switch.
  lv_obj_t *rmotion_row = lv_obj_create(content);
  lv_obj_remove_style_all(rmotion_row);
  lv_obj_set_width(rmotion_row, LV_PCT(100));
  lv_obj_set_height(rmotion_row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(rmotion_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(rmotion_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(rmotion_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *rmotion_lbl = lv_label_create(rmotion_row);
  lv_label_set_text(rmotion_lbl, "Motion resets screen");
  lv_obj_set_style_text_color(rmotion_lbl, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(rmotion_lbl, &lv_font_montserrat_18, 0);
  this->reset_motion_switch_ = lv_switch_create(rmotion_row);
  if (this->reset_on_motion_)
    lv_obj_add_state(this->reset_motion_switch_, LV_STATE_CHECKED);
  lv_obj_add_event_cb(this->reset_motion_switch_, &HAPanel::on_reset_motion_switch_,
                      LV_EVENT_VALUE_CHANGED, this);

  // ---- Dim / blank timeouts (gated by the reset sources above) ----
  // UE10: editable idle timeouts (replaces the old read-only label). Each is a
  // 5 s-step slider 0..600 s with a live seconds label; dim/blank reach "Never".
  // Greyed when neither reset source is on (an unrecoverable screen otherwise).
  add_timeout_row(content, "Dim after", 0, this->dim_timeout_,
                  &HAPanel::on_timeout_slider_, this,
                  &this->dim_timeout_slider_, &this->dim_timeout_label_);
  add_timeout_row(content, "Blank after (total)", 0, this->blank_timeout_,
                  &HAPanel::on_timeout_slider_, this,
                  &this->blank_timeout_slider_, &this->blank_timeout_label_);
  this->update_timeouts_enabled_();

  // ---- Sleep ----
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

  // UE10: Sleep-after delay (greyed with the mode dropdown when sleep is off).
  // min_units = 1 so it can't be dragged to 0 — sleep is toggled off via the
  // switch above, not by a zero delay.
  add_timeout_row(content, "Sleep after", 1, this->sleep_timeout_,
                  &HAPanel::on_timeout_slider_, this,
                  &this->sleep_timeout_slider_, &this->sleep_timeout_label_);
  // Greys both the mode dropdown and the sleep-after slider per the master toggle.
  this->update_sleep_mode_enabled_();

  // ---- Sound ----
  lv_obj_t *sound_title = lv_label_create(content);
  lv_label_set_text(sound_title, "Sound");
  lv_obj_set_style_text_color(sound_title, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(sound_title, &lv_font_montserrat_18, 0);

  lv_obj_t *sound_row = lv_obj_create(content);
  lv_obj_remove_style_all(sound_row);
  lv_obj_set_width(sound_row, LV_PCT(100));
  lv_obj_set_height(sound_row, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(sound_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(sound_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(sound_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *sound_lbl = lv_label_create(sound_row);
  lv_label_set_text(sound_lbl, "Click on press");
  lv_obj_set_style_text_color(sound_lbl, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(sound_lbl, &lv_font_montserrat_18, 0);

  this->sound_switch_ = lv_switch_create(sound_row);
  if (this->sound_on_press_)
    lv_obj_add_state(this->sound_switch_, LV_STATE_CHECKED);
  lv_obj_add_event_cb(this->sound_switch_, &HAPanel::on_sound_switch_,
                      LV_EVENT_VALUE_CHANGED, this);

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
  snprintf(abuf, sizeof(abuf),
           "%s\nESPHome %s\nLVGL %d.%d.%d\nBuilt %s\nCreated by William Krahmer",
           App.get_name().c_str(), ESPHOME_VERSION, LVGL_VERSION_MAJOR,
           LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, build_buf);
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
  if (this->pages_.empty()) {
    ESP_LOGW(TAG, "no pages; skipping UI build");
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

  // ---- Header (top 40 px). Tappable to open page picker. ----
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
  //   [ HH:MM ────── Page ▼ ──────  📶 🔋 ● ]
  // Clock left, page + chevron center, wifi → battery → status right.
  // 44 px corner inset on both far ends per the empirically-measured panel
  // radius (see P7a notes).

  // Clock at far left.
  this->clock_label_ = lv_label_create(header);
  lv_label_set_text(this->clock_label_, "--:--");
  lv_obj_set_style_text_color(this->clock_label_, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(this->clock_label_, &lv_font_montserrat_18, 0);
  lv_obj_align(this->clock_label_, LV_ALIGN_LEFT_MID, 44, 0);

  // E3: page name + chevron live in a centered horizontal flex row. The label
  // sizes to its content but is capped at 200 px via max_width with LONG_DOT,
  // so a long name ellipsizes ("Living Roo…") instead of overrunning. The flex
  // container lays the chevron immediately right of the (possibly truncated)
  // label and re-runs on every text change — no more one-shot align_to that
  // went stale when the page name grew. 200 px cap clears the worst-case clock
  // ("12:00 pm", left) and the Wi-Fi/battery/dot cluster (right).
  lv_obj_t *center = lv_obj_create(header);
  lv_obj_remove_style_all(center);
  lv_obj_set_height(center, 40);
  lv_obj_set_width(center, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_all(center, 0, 0);
  lv_obj_set_style_pad_column(center, 8, 0);
  lv_obj_set_flex_flow(center, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(center, LV_OBJ_FLAG_SCROLLABLE);
  // Not clickable, and bubble any taps up so the header still opens the picker.
  lv_obj_clear_flag(center, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(center, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_align(center, LV_ALIGN_CENTER, 0, 0);

  this->header_label_ = lv_label_create(center);
  lv_label_set_text(this->header_label_, this->pages_[0].name.c_str());
  lv_obj_set_style_text_color(this->header_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(this->header_label_, &lv_font_montserrat_18, 0);
  lv_obj_set_width(this->header_label_, LV_SIZE_CONTENT);
  lv_obj_set_style_max_width(this->header_label_, 200, 0);
  lv_label_set_long_mode(this->header_label_, LV_LABEL_LONG_DOT);

  lv_obj_t *chev = lv_label_create(center);
  lv_label_set_text(chev, LV_SYMBOL_DOWN);
  lv_obj_set_style_text_color(chev, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(chev, &lv_font_montserrat_18, 0);

  // Connection status dot at far right (44 px corner inset).
  this->status_dot_ = lv_obj_create(header);
  lv_obj_remove_style_all(this->status_dot_);
  lv_obj_set_size(this->status_dot_, 10, 10);
  lv_obj_set_style_radius(this->status_dot_, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(this->status_dot_, lv_color_hex(0xCC4444), 0);
  lv_obj_set_style_bg_opa(this->status_dot_, LV_OPA_COVER, 0);
  lv_obj_align(this->status_dot_, LV_ALIGN_RIGHT_MID, -44, 0);

  // Spinner shown in the dot's spot while the HA link is being established
  // (wifi up, api down) — replaces the old amber blink. Same style as the
  // boot-splash spinner (1.5 s/rev, 270° sweep, amber). Hidden unless
  // connecting; status_dot_ covers the red (wifi down) + green (connected)
  // states. Self-animated by LVGL's anim timer, no dependence on blink phase.
  this->status_spinner_ = lv_spinner_create(header);
  lv_spinner_set_anim_duration(this->status_spinner_, 1500);  // 1.5 s/rev
  lv_spinner_set_arc_sweep(this->status_spinner_, 270);       // long 270° arc
  lv_obj_set_size(this->status_spinner_, 18, 18);
  lv_obj_remove_flag(this->status_spinner_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(this->status_spinner_, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_color(this->status_spinner_, lv_color_hex(0x333333),
                             LV_PART_MAIN);
  lv_obj_set_style_arc_width(this->status_spinner_, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(this->status_spinner_, lv_color_hex(0xDDAA33),
                             LV_PART_INDICATOR);
  lv_obj_align(this->status_spinner_, LV_ALIGN_RIGHT_MID, -44, 0);
  lv_obj_add_flag(this->status_spinner_, LV_OBJ_FLAG_HIDDEN);

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

  this->tile_objs_.reserve(this->pages_.size());
  // E1: settings is no longer a tile — tiles = pages only.
  for (size_t pi = 0; pi < this->pages_.size(); pi++) {
    // E1: with settings gone, the last page is the right boundary (no swipe
    // past it). First page is the left boundary; middle pages swipe both ways.
    lv_dir_t dir = LV_DIR_HOR;
    if (pi == 0)
      dir = (this->pages_.size() == 1) ? (lv_dir_t) LV_DIR_NONE : LV_DIR_RIGHT;
    else if (pi == this->pages_.size() - 1)
      dir = LV_DIR_LEFT;
    lv_obj_t *tile = lv_tileview_add_tile(this->tileview_, (uint8_t) pi, 0, dir);
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

    // P7e: resolve the MDI fonts once (get_lv_font is cheap but the null check
    // belongs out of the per-row loop). nullptr when no mdi_font configured.
    // E8: the 36/48 px re-bakes fall back to the base 24 px font when not wired.
    const lv_font_t *mdi_lv_font =
        this->mdi_font_ != nullptr ? this->mdi_font_->get_lv_font() : nullptr;
    const lv_font_t *mdi_lv_font_med =
        this->mdi_font_med_ != nullptr ? this->mdi_font_med_->get_lv_font() : mdi_lv_font;
    const lv_font_t *mdi_lv_font_lg =
        this->mdi_font_lg_ != nullptr ? this->mdi_font_lg_->get_lv_font() : mdi_lv_font;
    for (size_t ei : this->pages_[pi].entity_indices) {
      Entity &e = this->entities_[ei];
      lv_obj_t *widget = nullptr;
      lv_obj_t *unavail = nullptr;
      lv_obj_t *icon = nullptr;
      lv_obj_t *led = nullptr;
      const std::string &glyph = this->resolve_icon_(e);
      // E8: pick the size-matched MDI glyph font + row geometry.
      const lv_font_t *row_mdi_font = mdi_lv_font;
      if (e.size == EntitySize::MEDIUM)
        row_mdi_font = mdi_lv_font_med;
      else if (e.size == EntitySize::LARGE)
        row_mdi_font = mdi_lv_font_lg;
      const RowMetrics metrics = row_metrics_for(e.size);
      // UE13: per-row name-label styling — bold/italic baked font + underline.
      const lv_font_t *name_font_ovr = this->resolve_name_font_(e);
      const bool name_underline = e.name_style & STYLE_UNDERLINE;
      lv_obj_t *btn = make_entity_row(list, e, this, &HAPanel::on_entity_row_clicked_,
                                      (uintptr_t) ei, &widget, &unavail,
                                      glyph.c_str(), row_mdi_font, &icon, &led, metrics,
                                      name_font_ovr, name_underline);
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
      this->leds_by_entity_[ei] = led;  // UE5: nullptr unless binary_sensor
      this->rebuild_entity_row_(ei);
    }
  }
  // UE12: paint report rows now that every row widget exists. States may not
  // have arrived yet (counts read 0 / "—"); on_state_ refreshes as they land.
  this->recompute_reports_();
  // ---- E1: bottom navigation bar (y = 432–480, 48 px) ----
  // Persistent across all pages: ◀ page-step / 🏠 home + ⚙ settings (centered
  // pair) / ▶ page-step. E6: Home jumps to the first page; gear shifted off
  // dead-center to +44 to make room, leaving a ~32 px gap between the pair.
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
      {LV_SYMBOL_HOME, LV_ALIGN_CENTER, -44, &HAPanel::on_home_clicked_},
      {LV_SYMBOL_SETTINGS, LV_ALIGN_CENTER, 44, &HAPanel::on_gear_clicked_},
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

  // ---- Page picker (full-screen modal, hidden until header tapped) ----
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
  lv_label_set_text(ptitle, "Pick page");
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
  // Same 28 px bottom inset as the entity list — keeps the last page row in
  // the picker from being clipped by the bottom rounded corners.
  lv_obj_set_style_pad_bottom(plist, 28, 0);
  lv_obj_set_style_pad_row(plist, 4, 0);
  lv_obj_set_flex_flow(plist, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(plist, LV_DIR_VER);

  // UE11: mdi font for the badge icon column (nullptr → icon hidden, value only).
  const lv_font_t *picker_mdi_font =
      this->mdi_font_ != nullptr ? this->mdi_font_->get_lv_font() : nullptr;
  this->picker_badges_.assign(this->pages_.size(), {});
  for (size_t pi = 0; pi < this->pages_.size(); pi++) {
    lv_obj_t *row = lv_button_create(plist);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 56);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x3A4A6A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_user_data(row, (void *) (uintptr_t) pi);
    lv_obj_add_event_cb(row, &HAPanel::on_picker_row_clicked_, LV_EVENT_CLICKED, this);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, this->pages_[pi].name.c_str());
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);
    // UE11: cap + ellipsize the name so a long one can't run under the badges.
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(lbl, 300, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);

    // UE11: right-aligned bar holding one group per declared badge. Each group
    // is [icon][value]; a group hides itself (and collapses out of the flex
    // layout) when its value is 0/empty, so the visible badges stay packed
    // against the right edge. Non-clickable + event-bubble so a tap on a badge
    // still selects the page row. The page name keeps its left alignment.
    const std::vector<PickerBadge> &page_badges = this->pages_[pi].badges;
    std::vector<lv_obj_t *> groups;
    if (!page_badges.empty()) {
      lv_obj_t *bar = lv_obj_create(row);
      lv_obj_remove_style_all(bar);
      lv_obj_set_size(bar, LV_SIZE_CONTENT, 56);
      lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
      lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                            LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_column(bar, 14, 0);  // gap between stacked badges
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(bar, LV_OBJ_FLAG_EVENT_BUBBLE);
      lv_obj_align(bar, LV_ALIGN_RIGHT_MID, -12, 0);

      for (size_t bi = 0; bi < page_badges.size(); bi++) {
        lv_obj_t *grp = lv_obj_create(bar);
        lv_obj_remove_style_all(grp);
        lv_obj_set_size(grp, LV_SIZE_CONTENT, 56);
        lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(grp, 6, 0);  // gap between icon + value
        lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(grp, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(grp, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(grp, LV_OBJ_FLAG_HIDDEN);

        // Child 0: mdi icon glyph (index-stable — always created even w/o font).
        lv_obj_t *bicon = lv_label_create(grp);
        lv_label_set_text(bicon, "");
        if (picker_mdi_font != nullptr)
          lv_obj_set_style_text_font(bicon, picker_mdi_font, 0);

        // Child 1: value text.
        lv_obj_t *bval = lv_label_create(grp);
        lv_label_set_text(bval, "");
        lv_obj_set_style_text_font(bval, &lv_font_montserrat_18, 0);

        groups.push_back(grp);
      }
    }
    this->picker_badges_[pi] = std::move(groups);
  }
  // E1: the picker lists pages only — Settings moved to the bottom-bar gear.

  // ---- P7d detail modal (built once, hidden) ----
  this->build_detail_modal_(scr);

  // ---- P7f action confirm sheet (built once, hidden) ----
  this->build_confirm_sheet_(scr);

  // ---- E1 settings overlay sheet (built once, hidden) ----
  this->build_settings_sheet_(scr);

  // ---- E9 read-only history chart sheet (built once, hidden) ----
  this->build_history_sheet_(scr);

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

  // E5: one row per init stage. Each row's amber dot blinks while in progress,
  // then becomes a green check when done. Wi-Fi first, HA API second.
  this->splash_wifi_stage_ =
      this->build_splash_stage_(this->splash_, "Connecting to Wi-Fi...", 20);
  this->splash_ha_stage_ =
      this->build_splash_stage_(this->splash_, "Connecting to Home Assistant...", 54);

  // E5: set indicator states from current link state (on_connect may have fired
  // before build).
  this->update_splash_status_();

  this->update_status_dot_();
  // E5: start the blink timer at boot so the splash dot (and the header
  // indicators) animate even when stuck on the very first gate — no setter
  // fires when the initial state already matches.
  this->update_blink_timer_();

  // One-shot 10 s watchdog: if the splash stages haven't passed by then (API
  // still not connected), explain the slow stage and drop the splash so the
  // app loads while reconnection keeps running. Cancelled in set_api_connected
  // if the link comes up first.
  this->splash_timeout_timer_ =
      lv_timer_create(&HAPanel::splash_timeout_cb_, 10000, this);
  lv_timer_set_repeat_count(this->splash_timeout_timer_, 1);
}

void HAPanel::open_picker_() {
  if (this->picker_ == nullptr)
    return;
  // UE11: recompute badges from current state before the unhide — entity states
  // are already live (subscriptions don't pause), so no fetch is needed.
  this->update_picker_badges_();
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

void HAPanel::step_page_(int delta) {
  if (this->tileview_ == nullptr || this->pages_.empty())
    return;
  // Find the current page index from the active tile, step by delta with
  // wrap-around, then jump there (not bound by the per-tile LV_DIR_* swipe
  // constraints — go_to_page_ uses a programmatic tile-set).
  lv_obj_t *active = lv_tileview_get_tile_active(this->tileview_);
  size_t cur = 0;
  for (size_t pi = 0; pi < this->tile_objs_.size(); pi++) {
    if (this->tile_objs_[pi] == active) {
      cur = pi;
      break;
    }
  }
  const size_t n = this->pages_.size();
  size_t next = (size_t)(((int) cur + delta % (int) n + (int) n) % (int) n);
  if (next == cur)
    return;
  this->go_to_page_(next);
}

// E6: shared tile-set + header-update tail used by step_page_ and the Home
// button. Programmatic tile-set bypasses the per-tile LV_DIR_* swipe limits.
void HAPanel::go_to_page_(size_t page_idx) {
  if (this->tileview_ == nullptr || page_idx >= this->pages_.size())
    return;
  lv_tileview_set_tile_by_index(this->tileview_, (uint32_t) page_idx, 0, LV_ANIM_ON);
  if (this->header_label_ != nullptr)
    lv_label_set_text(this->header_label_, this->pages_[page_idx].name.c_str());
}

void HAPanel::update_status_dot_() {
  if (this->status_dot_ == nullptr)
    return;
  // E2: three states.
  //   wifi down            → red dot (can't even attempt the HA link yet).
  //   wifi up, api down     → spinner ("link not yet re-established").
  //   api connected        → green dot.
  const bool connecting = this->wifi_connected_ && !this->api_connected_;
  if (connecting) {
    // HA link being established: show the animated spinner, hide the dot.
    if (this->status_spinner_ != nullptr)
      lv_obj_clear_flag(this->status_spinner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(this->status_dot_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  if (this->status_spinner_ != nullptr)
    lv_obj_add_flag(this->status_spinner_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(this->status_dot_, LV_OBJ_FLAG_HIDDEN);
  const uint32_t col = this->api_connected_ ? 0x66BB66 : 0xCC4444;
  lv_obj_set_style_bg_color(this->status_dot_, lv_color_hex(col), 0);
}

HAPanel::SplashStage HAPanel::build_splash_stage_(lv_obj_t *parent, const char *text,
                                                  lv_coord_t y) {
  SplashStage st;
  // Phrase, nudged left so the indicator to its right keeps the row centred.
  lv_obj_t *row = lv_label_create(parent);
  lv_label_set_text(row, text);
  lv_obj_set_style_text_color(row, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(row, &lv_font_montserrat_18, 0);
  lv_obj_align(row, LV_ALIGN_CENTER, -14, y);

  // Amber dim dot — sits just right of the phrase, after the "...". Shown only
  // while the stage is queued (a later gate not yet being worked on).
  st.dot = lv_obj_create(parent);
  lv_obj_remove_style_all(st.dot);
  lv_obj_set_size(st.dot, 12, 12);
  lv_obj_set_style_radius(st.dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(st.dot, lv_color_hex(0xDDAA33), 0);
  lv_obj_set_style_bg_opa(st.dot, LV_OPA_30, 0);
  lv_obj_align_to(st.dot, row, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

  // UE2: spinner shown while this stage is the active gate. Same spot as the
  // dot/check. The boot wait runs across loop ticks (lv_timer_handler keeps
  // firing), so unlike the blocking history fetch this one actually animates.
  st.spinner = lv_spinner_create(parent);
  lv_spinner_set_anim_duration(st.spinner, 1500);  // 1.5 s/rev
  lv_spinner_set_arc_sweep(st.spinner, 270);       // long 270° arc, calm look
  lv_obj_set_size(st.spinner, 18, 18);
  lv_obj_set_style_arc_width(st.spinner, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_color(st.spinner, lv_color_hex(0x333333), LV_PART_MAIN);
  lv_obj_set_style_arc_width(st.spinner, 3, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(st.spinner, lv_color_hex(0xDDAA33),
                             LV_PART_INDICATOR);
  lv_obj_align_to(st.spinner, row, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
  lv_obj_add_flag(st.spinner, LV_OBJ_FLAG_HIDDEN);

  // Green checkmark — same spot, hidden until the stage completes.
  st.check = lv_label_create(parent);
  lv_label_set_text(st.check, LV_SYMBOL_OK);
  lv_obj_set_style_text_color(st.check, lv_color_hex(0x66BB66), 0);
  lv_obj_set_style_text_font(st.check, &lv_font_montserrat_18, 0);
  lv_obj_align_to(st.check, row, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
  lv_obj_add_flag(st.check, LV_OBJ_FLAG_HIDDEN);
  return st;
}

void HAPanel::update_splash_stage_(const SplashStage &st, bool done, bool active) {
  if (st.dot == nullptr || st.check == nullptr)
    return;
  if (done) {
    // Stage complete: green check only.
    lv_obj_add_flag(st.dot, LV_OBJ_FLAG_HIDDEN);
    if (st.spinner != nullptr)
      lv_obj_add_flag(st.spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(st.check, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(st.check, LV_OBJ_FLAG_HIDDEN);
  if (active) {
    // UE2: the gate being worked on shows the animated spinner (self-driven by
    // LVGL's anim timer — no dependence on the blink phase), dot hidden.
    lv_obj_add_flag(st.dot, LV_OBJ_FLAG_HIDDEN);
    if (st.spinner != nullptr)
      lv_obj_clear_flag(st.spinner, LV_OBJ_FLAG_HIDDEN);
  } else {
    // Queued (a later gate not yet reached): dim steady dot, no spinner.
    if (st.spinner != nullptr)
      lv_obj_add_flag(st.spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(st.dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(st.dot, LV_OPA_30, 0);
  }
}

void HAPanel::update_splash_status_() {
  // E5: Wi-Fi is the first gate, the HA API the second. A stage is "done" once
  // its link is up, "active" while it is the current gate being worked on. The
  // HA stage flips to its green check on api-connect just before the splash
  // hides (set_api_connected) — done for consistency / future extra stages,
  // even though it is not on-screen long enough to see.
  const bool wifi_done = this->wifi_connected_;
  const bool ha_done = this->api_connected_;
  this->update_splash_stage_(this->splash_wifi_stage_, wifi_done, /*active=*/!wifi_done);
  this->update_splash_stage_(this->splash_ha_stage_, ha_done,
                             /*active=*/wifi_done && !ha_done);
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
  // E5: flip the HA stage to its green check before hiding the splash — not
  // visible for long, but keeps every stage consistent for future extra ones.
  this->update_splash_status_();
  if (connected) {
    // Link came up — cancel the pending splash watchdog and clear its popup if
    // it already fired.
    if (this->splash_timeout_timer_ != nullptr) {
      lv_timer_delete(this->splash_timeout_timer_);
      this->splash_timeout_timer_ = nullptr;
    }
    this->dismiss_splash_timeout_popup_();
    if (this->splash_ != nullptr)
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
  this->update_splash_status_();  // E5: Wi-Fi stage → green check, HA stage → active.
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
  self->update_splash_status_();  // E5: pulse the splash dot in step.
}

void HAPanel::splash_timeout_cb_(lv_timer_t *t) {
  auto *self = static_cast<HAPanel *>(lv_timer_get_user_data(t));
  // One-shot: LVGL deletes the timer after this returns (repeat_count 1), so
  // drop our handle either way.
  if (self != nullptr)
    self->splash_timeout_timer_ = nullptr;
  if (self == nullptr || self->api_connected_)
    return;  // already connected (timer not yet cancelled) — nothing to warn.
  // Explain the stage we're stuck on, then load the app behind the popup.
  self->show_splash_timeout_popup_();
  if (self->splash_ != nullptr)
    lv_obj_add_flag(self->splash_, LV_OBJ_FLAG_HIDDEN);
}

void HAPanel::show_splash_timeout_popup_() {
  // Which gate stalled: Wi-Fi is the first, the HA API the second.
  const char *stage =
      this->wifi_connected_ ? "Home Assistant" : "Wi-Fi";
  // The header status spinner keeps signalling the live reconnection attempts.
  std::string msg = std::string("Connecting to ") + stage +
                    " is taking longer than expected.\n\n"
                    "Loading the app without a Home Assistant connection. "
                    "It will keep trying to connect in the background.";
  if (this->splash_timeout_popup_ != nullptr) {
    // Built already (shouldn't recur — one-shot) — refresh text + reshow.
    if (this->splash_timeout_label_ != nullptr)
      lv_label_set_text(this->splash_timeout_label_, msg.c_str());
    lv_obj_clear_flag(this->splash_timeout_popup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(this->splash_timeout_popup_);
    return;
  }
  // Dim full-screen backdrop on the top layer so it floats above every page /
  // sheet. CLICKABLE eats taps to the app beneath while the popup is up.
  lv_obj_t *top = lv_layer_top();
  this->splash_timeout_popup_ = lv_obj_create(top);
  lv_obj_remove_style_all(this->splash_timeout_popup_);
  lv_obj_set_size(this->splash_timeout_popup_, 480, 480);
  lv_obj_set_pos(this->splash_timeout_popup_, 0, 0);
  lv_obj_set_style_bg_color(this->splash_timeout_popup_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->splash_timeout_popup_, LV_OPA_70, 0);
  lv_obj_add_flag(this->splash_timeout_popup_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(this->splash_timeout_popup_, LV_OBJ_FLAG_SCROLLABLE);

  // Centered card.
  lv_obj_t *card = lv_obj_create(this->splash_timeout_popup_);
  lv_obj_remove_style_all(card);
  lv_obj_set_size(card, 380, 280);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1A1A), 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(0x333333), 0);
  lv_obj_set_style_pad_all(card, 20, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, "Still connecting");
  lv_obj_set_style_text_color(title, lv_color_hex(0xDDAA33), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  this->splash_timeout_label_ = lv_label_create(card);
  lv_label_set_text(this->splash_timeout_label_, msg.c_str());
  lv_obj_set_style_text_color(this->splash_timeout_label_, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(this->splash_timeout_label_, &lv_font_montserrat_18, 0);
  lv_label_set_long_mode(this->splash_timeout_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(this->splash_timeout_label_, 340);
  lv_obj_align(this->splash_timeout_label_, LV_ALIGN_TOP_LEFT, 0, 34);

  // Dismiss button, bottom-right of the card.
  lv_obj_t *ok = lv_button_create(card);
  lv_obj_set_size(ok, 110, 40);
  lv_obj_set_style_bg_color(ok, lv_color_hex(0x2E3640), 0);
  lv_obj_set_style_bg_color(ok, lv_color_hex(0x3A4651), LV_STATE_PRESSED);
  lv_obj_set_style_radius(ok, 8, 0);
  lv_obj_set_style_border_width(ok, 0, 0);
  lv_obj_set_style_shadow_width(ok, 0, 0);
  lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_add_event_cb(ok, &HAPanel::on_splash_timeout_dismiss_, LV_EVENT_CLICKED,
                      this);
  lv_obj_t *oklbl = lv_label_create(ok);
  lv_label_set_text(oklbl, "Continue");
  lv_obj_set_style_text_color(oklbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(oklbl, &lv_font_montserrat_18, 0);
  lv_obj_center(oklbl);
}

void HAPanel::dismiss_splash_timeout_popup_() {
  if (this->splash_timeout_popup_ != nullptr)
    lv_obj_add_flag(this->splash_timeout_popup_, LV_OBJ_FLAG_HIDDEN);
}

void HAPanel::on_splash_timeout_dismiss_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->dismiss_splash_timeout_popup_();
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
  // Mode + sleep-after delay are irrelevant when the master toggle is off —
  // disable + dim both. UE10 added the sleep-after slider to this gate.
  const bool on = this->staged_sleep_enabled_;
  if (this->sleep_mode_dropdown_ != nullptr) {
    if (on) {
      lv_obj_remove_state(this->sleep_mode_dropdown_, LV_STATE_DISABLED);
      lv_obj_set_style_text_opa(this->sleep_mode_dropdown_, LV_OPA_COVER, 0);
    } else {
      lv_obj_add_state(this->sleep_mode_dropdown_, LV_STATE_DISABLED);
      lv_obj_set_style_text_opa(this->sleep_mode_dropdown_, LV_OPA_50, 0);
    }
  }
  if (this->sleep_timeout_slider_ != nullptr) {
    if (on) {
      lv_obj_remove_state(this->sleep_timeout_slider_, LV_STATE_DISABLED);
      lv_obj_set_style_opa(this->sleep_timeout_slider_, LV_OPA_COVER, 0);
    } else {
      lv_obj_add_state(this->sleep_timeout_slider_, LV_STATE_DISABLED);
      lv_obj_set_style_opa(this->sleep_timeout_slider_, LV_OPA_50, 0);
    }
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

// ---------- UE10 idle timeouts + reset sources (staged like sleep) ----------

void HAPanel::update_timeout_label_(uint8_t idx) {
  lv_obj_t *lbl = nullptr;
  uint16_t secs = 0;
  switch (idx) {
    case 0: lbl = this->dim_timeout_label_;   secs = this->staged_dim_timeout_;   break;
    case 1: lbl = this->blank_timeout_label_; secs = this->staged_blank_timeout_; break;
    case 2: lbl = this->sleep_timeout_label_; secs = this->staged_sleep_timeout_; break;
    default: return;
  }
  if (lbl == nullptr)
    return;
  char buf[24];
  fmt_timeout_(buf, sizeof(buf), secs);
  lv_label_set_text(lbl, buf);
}

void HAPanel::update_timeouts_enabled_() {
  // Dim + blank only make sense if something can un-dim the screen. With neither
  // reset source staged on, they're suppressed at runtime (see the idle interval
  // gate) — grey their sliders to match.
  const bool on = this->staged_reset_on_touch_ || this->staged_reset_on_motion_;
  lv_obj_t *sliders[2] = {this->dim_timeout_slider_, this->blank_timeout_slider_};
  lv_obj_t *labels[2] = {this->dim_timeout_label_, this->blank_timeout_label_};
  for (int i = 0; i < 2; i++) {
    if (sliders[i] != nullptr) {
      if (on) {
        lv_obj_remove_state(sliders[i], LV_STATE_DISABLED);
        lv_obj_set_style_opa(sliders[i], LV_OPA_COVER, 0);
      } else {
        lv_obj_add_state(sliders[i], LV_STATE_DISABLED);
        lv_obj_set_style_opa(sliders[i], LV_OPA_50, 0);
      }
    }
    if (labels[i] != nullptr)
      lv_obj_set_style_text_opa(labels[i], on ? LV_OPA_COVER : LV_OPA_50, 0);
  }
}

void HAPanel::set_timeout_settings(uint16_t dim, uint16_t blank_total, uint16_t sleep_s) {
  this->dim_timeout_ = dim;
  this->blank_timeout_ = blank_total;
  this->sleep_timeout_ = sleep_s;
  this->staged_dim_timeout_ = dim;
  this->staged_blank_timeout_ = blank_total;
  this->staged_sleep_timeout_ = sleep_s;
  this->timeouts_dirty_ = false;
  // Push the restored values into the sliders + labels (guarded: setup() may
  // build the sheet before or after this boot seeder runs).
  if (this->dim_timeout_slider_ != nullptr)
    lv_slider_set_value(this->dim_timeout_slider_, dim / kTimeoutStepS, LV_ANIM_OFF);
  if (this->blank_timeout_slider_ != nullptr)
    lv_slider_set_value(this->blank_timeout_slider_, blank_total / kTimeoutStepS, LV_ANIM_OFF);
  if (this->sleep_timeout_slider_ != nullptr) {
    int u = sleep_s / kTimeoutStepS;
    if (u < 1) u = 1;
    lv_slider_set_value(this->sleep_timeout_slider_, u, LV_ANIM_OFF);
  }
  this->update_timeout_label_(0);
  this->update_timeout_label_(1);
  this->update_timeout_label_(2);
}

void HAPanel::apply_timeouts_() {
  if (!this->timeouts_dirty_)
    return;
  uint16_t dim = this->staged_dim_timeout_;
  uint16_t blank = this->staged_blank_timeout_;
  uint16_t sleep = this->staged_sleep_timeout_;
  // Clamp the dim ≤ blank ≤ sleep ordering for enabled tiers (0 = disabled for
  // dim/blank, skipped in the comparison). Clamp rather than reject so the UI
  // can't wedge into an impossible order.
  if (dim != 0 && blank != 0 && blank < dim)
    blank = dim;
  if (blank != 0) {
    if (sleep < blank) sleep = blank;
  } else if (dim != 0 && sleep < dim) {
    sleep = dim;
  }
  if (sleep < kTimeoutStepS)
    sleep = kTimeoutStepS;  // sleep always has a positive delay
  this->dim_timeout_ = dim;
  this->blank_timeout_ = blank;
  this->sleep_timeout_ = sleep;
  // Reflect any clamp back into the staged values + UI so a later Cancel reverts
  // to the committed (clamped) state, not the pre-clamp slider position.
  this->staged_dim_timeout_ = dim;
  this->staged_blank_timeout_ = blank;
  this->staged_sleep_timeout_ = sleep;
  if (this->dim_timeout_slider_ != nullptr)
    lv_slider_set_value(this->dim_timeout_slider_, dim / kTimeoutStepS, LV_ANIM_OFF);
  if (this->blank_timeout_slider_ != nullptr)
    lv_slider_set_value(this->blank_timeout_slider_, blank / kTimeoutStepS, LV_ANIM_OFF);
  if (this->sleep_timeout_slider_ != nullptr)
    lv_slider_set_value(this->sleep_timeout_slider_, sleep / kTimeoutStepS, LV_ANIM_OFF);
  this->update_timeout_label_(0);
  this->update_timeout_label_(1);
  this->update_timeout_label_(2);
  if (this->timeouts_committer_)
    this->timeouts_committer_(dim, blank, sleep);
  this->timeouts_dirty_ = false;
  ESP_LOGI(TAG, "timeouts applied: dim=%u blank=%u sleep=%u",
           (unsigned) dim, (unsigned) blank, (unsigned) sleep);
}

void HAPanel::revert_timeouts_() {
  if (!this->timeouts_dirty_)
    return;
  this->staged_dim_timeout_ = this->dim_timeout_;
  this->staged_blank_timeout_ = this->blank_timeout_;
  this->staged_sleep_timeout_ = this->sleep_timeout_;
  if (this->dim_timeout_slider_ != nullptr)
    lv_slider_set_value(this->dim_timeout_slider_, this->dim_timeout_ / kTimeoutStepS, LV_ANIM_OFF);
  if (this->blank_timeout_slider_ != nullptr)
    lv_slider_set_value(this->blank_timeout_slider_, this->blank_timeout_ / kTimeoutStepS, LV_ANIM_OFF);
  if (this->sleep_timeout_slider_ != nullptr)
    lv_slider_set_value(this->sleep_timeout_slider_, this->sleep_timeout_ / kTimeoutStepS, LV_ANIM_OFF);
  this->update_timeout_label_(0);
  this->update_timeout_label_(1);
  this->update_timeout_label_(2);
  this->timeouts_dirty_ = false;
  ESP_LOGI(TAG, "timeouts reverted");
}

void HAPanel::set_reset_sources(bool touch, bool motion) {
  this->reset_on_touch_ = touch;
  this->reset_on_motion_ = motion;
  this->staged_reset_on_touch_ = touch;
  this->staged_reset_on_motion_ = motion;
  this->reset_sources_dirty_ = false;
  if (this->reset_touch_switch_ != nullptr) {
    if (touch)
      lv_obj_add_state(this->reset_touch_switch_, LV_STATE_CHECKED);
    else
      lv_obj_remove_state(this->reset_touch_switch_, LV_STATE_CHECKED);
  }
  if (this->reset_motion_switch_ != nullptr) {
    if (motion)
      lv_obj_add_state(this->reset_motion_switch_, LV_STATE_CHECKED);
    else
      lv_obj_remove_state(this->reset_motion_switch_, LV_STATE_CHECKED);
  }
  this->update_timeouts_enabled_();
}

void HAPanel::apply_reset_sources_() {
  if (!this->reset_sources_dirty_)
    return;
  this->reset_on_touch_ = this->staged_reset_on_touch_;
  this->reset_on_motion_ = this->staged_reset_on_motion_;
  if (this->reset_sources_committer_)
    this->reset_sources_committer_(this->reset_on_touch_, this->reset_on_motion_);
  this->reset_sources_dirty_ = false;
  ESP_LOGI(TAG, "reset sources applied: touch=%d motion=%d",
           (int) this->reset_on_touch_, (int) this->reset_on_motion_);
}

void HAPanel::revert_reset_sources_() {
  if (!this->reset_sources_dirty_)
    return;
  this->staged_reset_on_touch_ = this->reset_on_touch_;
  this->staged_reset_on_motion_ = this->reset_on_motion_;
  if (this->reset_touch_switch_ != nullptr) {
    if (this->reset_on_touch_)
      lv_obj_add_state(this->reset_touch_switch_, LV_STATE_CHECKED);
    else
      lv_obj_remove_state(this->reset_touch_switch_, LV_STATE_CHECKED);
  }
  if (this->reset_motion_switch_ != nullptr) {
    if (this->reset_on_motion_)
      lv_obj_add_state(this->reset_motion_switch_, LV_STATE_CHECKED);
    else
      lv_obj_remove_state(this->reset_motion_switch_, LV_STATE_CHECKED);
  }
  this->update_timeouts_enabled_();
  this->reset_sources_dirty_ = false;
  ESP_LOGI(TAG, "reset sources reverted");
}

bool HAPanel::staged_protection_all_off_() const {
  // No reset source → dim/blank are suppressed (they'd be unrecoverable), and
  // sleep can't reach the blank tier it enters from → the screen sits at full
  // brightness indefinitely. That's a burn-in risk regardless of the slider
  // values, so treat it as "no protection".
  if (!this->staged_reset_on_touch_ && !this->staged_reset_on_motion_)
    return true;
  // A reset source exists → protection is off only when every tier is disabled.
  return this->staged_dim_timeout_ == 0 && this->staged_blank_timeout_ == 0 &&
         !this->staged_sleep_enabled_;
}

void HAPanel::commit_settings_() {
  this->apply_brightness_();
  this->apply_sleep_();
  this->apply_sound_();
  this->apply_timeouts_();
  this->apply_reset_sources_();
  this->close_settings_();
}

void HAPanel::set_sound_on_press(bool on) {
  this->sound_on_press_ = on;
  this->staged_sound_on_press_ = on;
  this->sound_dirty_ = false;
  if (this->sound_switch_ != nullptr) {
    if (on)
      lv_obj_add_state(this->sound_switch_, LV_STATE_CHECKED);
    else
      lv_obj_remove_state(this->sound_switch_, LV_STATE_CHECKED);
  }
  // Restored-on at boot → warm the codec now so the first click is steady-gain.
  if (on)
    this->warm_up_speaker_();
}

void HAPanel::apply_sound_() {
  if (!this->sound_dirty_)
    return;
  this->sound_on_press_ = this->staged_sound_on_press_;
  if (this->sound_committer_)
    this->sound_committer_(this->sound_on_press_);
#ifdef USE_SPEAKER
  if (this->sound_on_press_) {
    // Turned on → warm the codec now so the first click is steady-gain.
    this->warm_up_speaker_();
  } else if (this->speaker_ != nullptr && !this->speaker_->is_stopped()) {
    // Turned off → release the continuously-running bus (timeout: never keeps
    // it alive otherwise). Re-enabling warms it again.
    this->speaker_->stop();
  }
#endif
  this->sound_dirty_ = false;
  ESP_LOGI(TAG, "sound-on-press applied: %d", (int) this->sound_on_press_);
}

void HAPanel::revert_sound_() {
  if (!this->sound_dirty_)
    return;
  this->staged_sound_on_press_ = this->sound_on_press_;
  if (this->sound_switch_ != nullptr) {
    if (this->sound_on_press_)
      lv_obj_add_state(this->sound_switch_, LV_STATE_CHECKED);
    else
      lv_obj_remove_state(this->sound_switch_, LV_STATE_CHECKED);
  }
  this->sound_dirty_ = false;
  ESP_LOGI(TAG, "sound-on-press reverted to %d", (int) this->sound_on_press_);
}

void HAPanel::on_sound_switch_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  self->staged_sound_on_press_ =
      lv_obj_has_state(self->sound_switch_, LV_STATE_CHECKED);
  self->sound_dirty_ = (self->staged_sound_on_press_ != self->sound_on_press_);
}

void HAPanel::play_click_() {
  // Committed setting only — staged edits don't take effect until Apply.
  if (!this->sound_on_press_)
    return;
  // Portability hook: a board can supply its own click backend (buzzer, haptic,
  // a different codec) via set_click_action. If set, it owns the sound; ha_panel
  // stays backend-agnostic and only decides *when* to click (tap, not swipe).
  if (this->click_action_) {
    this->click_action_();
    return;
  }
  // Default backend: the built-in i2s speaker (boards that called set_speaker).
  this->play_speaker_click_();
}

void HAPanel::play_speaker_click_() {
#ifdef USE_SPEAKER
  if (this->speaker_ == nullptr || this->click_pcm_.empty())
    return;
  // Queue the click into the continuously-running stream (speaker `timeout:
  // never`). The stream stays up and is fed silence between clicks, so there is
  // no per-tap start/stop — that start/stop transient was the uneven "pop", and
  // rapid taps just re-queue the same buffer back-to-back. The click's soft
  // character lives in the sample itself (see the PCM design in setup()), not in
  // any codec power-up ramp, so it sounds the same every press and ports to any
  // DAC. No finish() — that truncated the buffer mid-click.
  this->speaker_->play(this->click_pcm_.data(), this->click_pcm_.size());
#endif
}

void HAPanel::warm_up_speaker_() {
#ifdef USE_SPEAKER
  if (this->speaker_ == nullptr)
    return;
  // Feed ~64 ms of silence to start the stream now and let the ES8311 un-mute
  // ramp finish during this silence instead of during the user's first click.
  // With timeout:never the stream then stays up at steady gain, so press #1 is
  // identical to the rest.
  static const std::vector<uint8_t> silence(2048, 0);  // 1024 samples @ 16 kHz mono
  this->speaker_->play(silence.data(), silence.size());
#endif
}

void HAPanel::touch_pressed(int16_t x, int16_t y) {
  this->touch_active_ = true;
  this->touch_moved_ = false;
  this->touch_start_x_ = x;
  this->touch_start_y_ = y;
}

void HAPanel::touch_moved(int16_t x, int16_t y) {
  if (!this->touch_active_ || this->touch_moved_)
    return;
  // Past ~16 px from the press origin = a swipe/scroll, not a tap.
  const int16_t kTapSlopPx = 16;
  if (abs(x - this->touch_start_x_) > kTapSlopPx ||
      abs(y - this->touch_start_y_) > kTapSlopPx)
    this->touch_moved_ = true;
}

void HAPanel::touch_released() {
  if (!this->touch_active_)
    return;
  this->touch_active_ = false;
  // A tap (barely moved) plays the click; a swipe/scroll stays silent.
  if (!this->touch_moved_)
    this->play_click_();
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
  // the (full-screen) sheet is open over the tileview — so swiping pages no
  // longer needs the navigate-away revert. Just retitle the header.
  for (size_t pi = 0; pi < self->tile_objs_.size(); pi++) {
    if (self->tile_objs_[pi] != tile)
      continue;
    if (self->header_label_ != nullptr)
      lv_label_set_text(self->header_label_, self->pages_[pi].name.c_str());
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
    // E9: a chartable read-only entity (numeric sensor / binary_sensor) opens
    // the history sheet instead of the read-only no-op. Other read-only rows
    // (text sensors) still fall through to tap_entity_'s no-op.
    if (en.render_class == RenderClass::READ_ONLY_TEXT &&
        HAPanel::is_chartable_(en)) {
      self->open_history_(entity_idx);
      return;
    }
    // Summary rows (climate / media_player / number / select) have no inline
    // tap action — tap_entity_ would no-op. Open the detail modal directly so a
    // short tap, not only a long-press, brings up the control surface.
    if (en.render_class == RenderClass::SUMMARY_TEXT &&
        HAPanel::has_detail_(en.domain)) {
      self->open_detail_(entity_idx);
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
  if (col >= self->pages_.size())  // E1: picker lists pages only
    return;
  lv_tileview_set_tile_by_index(self->tileview_, (uint32_t) col, 0, LV_ANIM_ON);
  if (self->header_label_ != nullptr)
    lv_label_set_text(self->header_label_, self->pages_[col].name.c_str());
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
  // UE10: warn before committing a config that leaves the screen at full
  // brightness forever (dim + blank + sleep all off) on a burn-in-prone AMOLED.
  // Proceed → commit; Cancel → back to settings, nothing committed.
  if (self->is_amoled_ && self->staged_protection_all_off_()) {
    self->open_burnin_warning_();
    return;
  }
  self->commit_settings_();  // E1: Apply commits then closes the sheet.
}

void HAPanel::on_cancel_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  self->revert_brightness_();
  self->revert_sleep_();
  self->revert_sound_();
  self->revert_timeouts_();
  self->revert_reset_sources_();
  self->close_settings_();  // E1: Cancel reverts then closes the sheet.
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
    self->step_page_(-1);
}

void HAPanel::on_nav_right_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->step_page_(+1);
}

// E6: Home → first page.
void HAPanel::on_home_clicked_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->go_to_page_(0);
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

void HAPanel::on_timeout_slider_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  lv_obj_t *sl = lv_event_get_target_obj(e);
  // seconds = slider units * 5 (coarse step). Route to the matching staged value
  // + label by which of the three sliders fired.
  uint16_t secs = (uint16_t)(lv_slider_get_value(sl) * kTimeoutStepS);
  uint8_t idx;
  if (sl == self->dim_timeout_slider_) {
    self->staged_dim_timeout_ = secs;
    idx = 0;
  } else if (sl == self->blank_timeout_slider_) {
    self->staged_blank_timeout_ = secs;
    idx = 1;
  } else if (sl == self->sleep_timeout_slider_) {
    self->staged_sleep_timeout_ = secs;
    idx = 2;
  } else {
    return;
  }
  self->update_timeout_label_(idx);
  self->timeouts_dirty_ =
      (self->staged_dim_timeout_ != self->dim_timeout_) ||
      (self->staged_blank_timeout_ != self->blank_timeout_) ||
      (self->staged_sleep_timeout_ != self->sleep_timeout_);
}

void HAPanel::on_reset_touch_switch_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->reset_touch_switch_ == nullptr)
    return;
  self->staged_reset_on_touch_ =
      lv_obj_has_state(self->reset_touch_switch_, LV_STATE_CHECKED);
  self->reset_sources_dirty_ =
      (self->staged_reset_on_touch_ != self->reset_on_touch_) ||
      (self->staged_reset_on_motion_ != self->reset_on_motion_);
  self->update_timeouts_enabled_();  // both-off greys dim/blank
}

void HAPanel::on_reset_motion_switch_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->reset_motion_switch_ == nullptr)
    return;
  self->staged_reset_on_motion_ =
      lv_obj_has_state(self->reset_motion_switch_, LV_STATE_CHECKED);
  self->reset_sources_dirty_ =
      (self->staged_reset_on_touch_ != self->reset_on_touch_) ||
      (self->staged_reset_on_motion_ != self->reset_on_motion_);
  self->update_timeouts_enabled_();  // both-off greys dim/blank
}

void HAPanel::on_burnin_proceed_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  // User accepted the burn-in risk → close the warning and commit for real.
  self->close_confirm_();
  self->commit_settings_();
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

// Some HA climate integrations report hvac_modes as Python enum reprs, e.g.
// "<HVACMode.OFF: 'off'>" instead of the bare "off" — parse_ha_list_ keeps the
// interior quotes since they aren't the token's outer quotes. Pull the value
// out of the first single-quoted span; pass through anything already bare.
static std::string clean_hvac_mode_(const std::string &s) {
  size_t q1 = s.find('\'');
  if (q1 != std::string::npos) {
    size_t q2 = s.find('\'', q1 + 1);
    if (q2 != std::string::npos && q2 > q1 + 1)
      return s.substr(q1 + 1, q2 - q1 - 1);
  }
  return s;
}

// UE3: format a setpoint for the dial label — whole number when the step is an
// integer (no "77.0"), one decimal for fractional steps (°C 0.5).
static void fmt_setpoint_(char *buf, size_t n, float value, float step) {
  if (step >= 0.999f)
    snprintf(buf, n, "%.0f", value);
  else
    snprintf(buf, n, "%.1f", value);
}

// UE3: dual-setpoint HVAC modes use target_temp_low/high (two dials); single
// modes (heat/cool) use one `temperature` setpoint. auto is treated as dual per
// the "set a heat point and a cool point" expectation. off/dry/fan_only have no
// active setpoint but still show the single dial (greyed) so the modal isn't empty.
static bool climate_mode_is_dual_(const std::string &m) {
  return m == "auto" || m == "heat_cool";
}

// Dial tint by HVAC mode: heat = warm, cool = blue, off = grey, else teal.
static uint32_t climate_mode_color_(const std::string &m) {
  if (m == "off")
    return 0x888888;
  if (m.find("heat") != std::string::npos)
    return 0xFF7043;
  if (m.find("cool") != std::string::npos)
    return 0x4FC3F7;
  return 0x44CCDD;
}

// UE3: build one round setpoint dial (lv_arc) inside a transparent, centered,
// non-scrollable holder appended to `box`. Returns the arc; *out_label is its
// centered value label. Shared by the single + both dual dials.
static lv_obj_t *add_setpoint_dial(lv_obj_t *box, int rng_min, int rng_max,
                                   int rng_cur, float per_unit, uint32_t color,
                                   lv_event_cb_t cb, void *user_data,
                                   lv_obj_t **out_label, int dial_px) {
  lv_obj_t *holder = lv_obj_create(box);
  lv_obj_remove_style_all(holder);
  lv_obj_set_size(holder, LV_PCT(100), dial_px + 10);
  lv_obj_set_style_bg_opa(holder, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *arc = lv_arc_create(holder);
  lv_obj_set_size(arc, dial_px, dial_px);
  lv_obj_center(arc);
  lv_arc_set_range(arc, rng_min, rng_max);
  lv_arc_set_value(arc, rng_cur);
  lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(arc, lv_color_hex(color), LV_PART_KNOB);
  lv_obj_add_event_cb(arc, cb, LV_EVENT_VALUE_CHANGED, user_data);
  lv_obj_t *lbl = lv_label_create(arc);
  char buf[24];
  fmt_setpoint_(buf, sizeof(buf), (float) rng_cur * per_unit, per_unit);
  lv_label_set_text(lbl, buf);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
  lv_obj_center(lbl);
  *out_label = lbl;
  return arc;
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
  // Clickable so background taps are swallowed (don't fall through to the page
  // behind). No bg-tap-to-close: only Apply / Cancel dismiss the modal — a
  // stray tap on the side/bottom while adjusting a dial must not lose edits.
  lv_obj_add_flag(this->detail_modal_, LV_OBJ_FLAG_CLICKABLE);

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
  this->dw_temp_single_box_ = nullptr;
  this->dw_temp_dual_box_ = nullptr;
  this->dw_temp_low_slider_ = nullptr;
  this->dw_temp_low_label_ = nullptr;
  this->dw_temp_high_slider_ = nullptr;
  this->dw_temp_high_label_ = nullptr;
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
            "max_temp", "target_temp_step", "target_temp_low",
            "target_temp_high"};
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
  // E7: toggling power reveals/greys the brightness slider.
  lv_obj_add_event_cb(this->dw_light_switch_, &HAPanel::on_detail_light_switch_,
                      LV_EVENT_VALUE_CHANGED, this);

  // Brightness slider 0-100%. HA reports `brightness` 0-255; we display %.
  // E7: no fake 100% default. A present `brightness` (on, dimmable light) seeds
  // the slider with the real value; when absent — off light (HA omits it),
  // non-dimmable light (never reports it), or pre-arrival — the slider renders
  // disabled with a "—"/"Off" placeholder rather than a misleading number.
  int b_raw = this->get_attr_int_(entity_idx, "brightness", -1);
  this->dw_brightness_known_ = (b_raw >= 0);
  int cur_pct = this->dw_brightness_known_ ? (b_raw * 100 + 127) / 255 : 0;
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
  if (this->dw_brightness_known_) {
    snprintf(buf, sizeof(buf), "%d %%", cur_pct);
  } else {
    // Disable input + grey the slider, and show a non-numeric placeholder.
    lv_obj_add_state(this->dw_brightness_slider_, LV_STATE_DISABLED);
    lv_obj_clear_flag(this->dw_brightness_slider_, LV_OBJ_FLAG_CLICKABLE);
    // ASCII only — built-in montserrat_18 has no em-dash glyph (see E5 note).
    snprintf(buf, sizeof(buf), "%s", e.state == "off" ? "Off" : "--");
  }
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
  if (this->get_attr_(entity_idx, "hvac_modes", &modes_raw)) {
    this->dw_hvac_modes_ = parse_ha_list_(modes_raw);
    for (auto &m : this->dw_hvac_modes_)
      m = clean_hvac_mode_(m);
  }
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

  // Setpoint dial(s). Stored as int = temp / step so the arc can hit
  // non-integer setpoints without LVGL float ranges. heat/cool use one
  // `temperature` dial; auto/heat_cool use two — target_temp_low (heat point)
  // and target_temp_high (cool point). Both boxes are built; the mode-change
  // handler shows one and hides the other so switching mode swaps the dials live
  // (a hidden flex child takes no layout space).
  float min_t = this->get_attr_float_(entity_idx, "min_temp", 7.0f);
  float max_t = this->get_attr_float_(entity_idx, "max_temp", 35.0f);
  // Default to whole degrees when HA doesn't report a step (common on °F
  // thermostats); honor target_temp_step when present (e.g. 0.5 on °C units).
  float step = this->get_attr_float_(entity_idx, "target_temp_step", 1.0f);
  if (step < 0.1f) step = 0.1f;
  this->dw_temp_step_ = step;
  int scale = (int) std::round(1.0f / step);
  if (scale < 1) scale = 1;
  int rng_min = (int) std::round(min_t * scale);
  int rng_max = (int) std::round(max_t * scale);
  if (rng_max <= rng_min) rng_max = rng_min + 1;
  auto to_rng = [&](float t) {
    int r = (int) std::round(t * scale);
    if (r < rng_min) r = rng_min;
    if (r > rng_max) r = rng_max;
    return r;
  };
  float mid_t = (min_t + max_t) / 2.0f;
  float cur_target = this->get_attr_float_(entity_idx, "temperature", mid_t);
  float lo_t = this->get_attr_float_(entity_idx, "target_temp_low", NAN);
  float hi_t = this->get_attr_float_(entity_idx, "target_temp_high", NAN);
  // Seed the dual dials from the single setpoint when low/high aren't reported
  // (e.g. the entity is currently in heat/cool): a small spread around the
  // current target, clamped to range.
  float spread = (max_t - min_t) * 0.1f;
  if (spread < step) spread = step;
  if (std::isnan(lo_t)) lo_t = cur_target - spread;
  if (std::isnan(hi_t)) hi_t = cur_target + spread;
  if (hi_t < lo_t) hi_t = lo_t;

  // --- single-setpoint box (heat / cool / off) ---
  this->dw_temp_single_box_ = lv_obj_create(parent);
  lv_obj_remove_style_all(this->dw_temp_single_box_);
  lv_obj_set_size(this->dw_temp_single_box_, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(this->dw_temp_single_box_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_row(this->dw_temp_single_box_, 10, 0);
  lv_obj_set_flex_flow(this->dw_temp_single_box_, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(this->dw_temp_single_box_, LV_OBJ_FLAG_SCROLLABLE);
  add_section_label(this->dw_temp_single_box_, "Target", 0xFFFFFF);
  this->dw_temp_slider_ = add_setpoint_dial(
      this->dw_temp_single_box_, rng_min, rng_max, to_rng(cur_target), step,
      climate_mode_color_(e.state), &HAPanel::on_detail_temp_slider_, this,
      &this->dw_temp_label_, 180);

  // --- dual-setpoint box (auto / heat_cool): two dials SIDE BY SIDE so both
  // the heat point and cool point are visible at once. Stacking pushed the
  // lower dial below the fold and the arc traps vertical scroll, so the cool
  // dial was unreachable. Smaller 150px dials fit two-across in the 400px
  // content width with the header still visible. ---
  this->dw_temp_dual_box_ = lv_obj_create(parent);
  lv_obj_remove_style_all(this->dw_temp_dual_box_);
  lv_obj_set_size(this->dw_temp_dual_box_, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(this->dw_temp_dual_box_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_column(this->dw_temp_dual_box_, 6, 0);
  lv_obj_set_flex_flow(this->dw_temp_dual_box_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(this->dw_temp_dual_box_, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_clear_flag(this->dw_temp_dual_box_, LV_OBJ_FLAG_SCROLLABLE);
  auto add_dial_col = [&](const char *title, uint32_t color, int rng_cur,
                          lv_event_cb_t cb, lv_obj_t **out_label) {
    lv_obj_t *col = lv_obj_create(this->dw_temp_dual_box_);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 196, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_row(col, 4, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    add_section_label(col, title, color);
    return add_setpoint_dial(col, rng_min, rng_max, rng_cur, step, color, cb,
                             this, out_label, 150);
  };
  this->dw_temp_low_slider_ =
      add_dial_col("Heat to", 0xFF7043, to_rng(lo_t),
                   &HAPanel::on_detail_temp_low_slider_, &this->dw_temp_low_label_);
  this->dw_temp_high_slider_ =
      add_dial_col("Cool to", 0x4FC3F7, to_rng(hi_t),
                   &HAPanel::on_detail_temp_high_slider_, &this->dw_temp_high_label_);

  // Toggle which box is visible by mode; re-run on dropdown change. Base the
  // initial choice on the dropdown's selected mode (not raw e.state) so the
  // dials always agree with the dropdown even if hvac_modes lacks e.state.
  lv_obj_add_event_cb(this->dw_hvac_dropdown_,
                      &HAPanel::on_detail_hvac_mode_changed_,
                      LV_EVENT_VALUE_CHANGED, this);
  uint16_t sel = lv_dropdown_get_selected(this->dw_hvac_dropdown_);
  const std::string &cur_mode =
      sel < this->dw_hvac_modes_.size() ? this->dw_hvac_modes_[sel] : e.state;
  lv_obj_add_flag(climate_mode_is_dual_(cur_mode) ? this->dw_temp_single_box_
                                                  : this->dw_temp_dual_box_,
                  LV_OBJ_FLAG_HIDDEN);
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
  // UE3: round volume dial (lv_arc), 0-100, same VALUE_CHANGED handler + apply
  // path as the old slider. Holder centers the square arc in the flex column.
  lv_obj_t *vol_holder = lv_obj_create(parent);
  lv_obj_remove_style_all(vol_holder);
  lv_obj_set_size(vol_holder, LV_PCT(100), 190);
  lv_obj_set_style_bg_opa(vol_holder, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(vol_holder, LV_OBJ_FLAG_SCROLLABLE);
  this->dw_volume_slider_ = lv_arc_create(vol_holder);
  lv_obj_set_size(this->dw_volume_slider_, 180, 180);
  lv_obj_center(this->dw_volume_slider_);
  lv_arc_set_range(this->dw_volume_slider_, 0, 100);
  lv_arc_set_value(this->dw_volume_slider_, v100);
  lv_obj_set_style_arc_width(this->dw_volume_slider_, 14, LV_PART_MAIN);
  lv_obj_set_style_arc_width(this->dw_volume_slider_, 14, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(this->dw_volume_slider_, lv_color_hex(0x44CCDD),
                             LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(this->dw_volume_slider_, lv_color_hex(0x44CCDD),
                            LV_PART_KNOB);
  lv_obj_add_event_cb(this->dw_volume_slider_,
                      &HAPanel::on_detail_volume_slider_,
                      LV_EVENT_VALUE_CHANGED, this);
  this->dw_volume_label_ = lv_label_create(this->dw_volume_slider_);
  char buf[24];
  snprintf(buf, sizeof(buf), "%d %%", v100);
  lv_label_set_text(this->dw_volume_label_, buf);
  lv_obj_set_style_text_color(this->dw_volume_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(this->dw_volume_label_, &lv_font_montserrat_18, 0);
  lv_obj_center(this->dw_volume_label_);
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

// Maps a cover's Entity::state to a "Currently: …" line text + colour for the
// detail modal and confirm sheet. Mirrors the COVER_TEXT page-row cues
// (green = open, grey = closed, neutral for the transient/unknown states).
static void cover_state_line_(const std::string &state, const char **text,
                              uint32_t *color) {
  if (state == "open") {
    *text = "Currently: Open";
    *color = 0x66BB66;
  } else if (state == "closed") {
    *text = "Currently: Closed";
    *color = 0x888888;
  } else if (state == "opening") {
    *text = "Currently: Opening...";
    *color = 0xCCCCCC;
  } else if (state == "closing") {
    *text = "Currently: Closing...";
    *color = 0xCCCCCC;
  } else {
    *text = "Currently: Unknown";
    *color = 0xCC4444;
  }
}

void HAPanel::build_detail_cover_(lv_obj_t *parent, size_t entity_idx) {
  // Current state line (from Entity::state, no extra subscription). Replaces
  // the old bare "Position not reported" for position-less covers.
  const char *state_txt;
  uint32_t state_col;
  cover_state_line_(this->entities_[entity_idx].state, &state_txt, &state_col);
  add_section_label(parent, state_txt, state_col);

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
    // Position-less cover (e.g. ratgdo): transport buttons + state line above
    // are the whole interface. No slider, no misleading "not reported" note.
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
      // E7: only send brightness when the slider holds a real target. A
      // disabled placeholder (non-dimmable light, or value never arrived)
      // would otherwise push a misleading 0/100 % on a bare turn-on.
      if (this->dw_brightness_slider_ != nullptr && this->dw_brightness_known_) {
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
    // Send set_hvac_mode ONLY when the mode actually changed. A redundant mode
    // call makes many integrations re-read/reset the target temperature, which
    // races the set_temperature we send right after — the symptom was the new
    // setpoint "sticking" only ~half the time. sel_mode is still resolved (even
    // when unchanged) so the dual/single temperature branch below is correct.
    std::string sel_mode = e.state;
    if (this->dw_hvac_dropdown_ != nullptr && !this->dw_hvac_modes_.empty()) {
      uint16_t idx = lv_dropdown_get_selected(this->dw_hvac_dropdown_);
      if (idx < this->dw_hvac_modes_.size())
        sel_mode = this->dw_hvac_modes_[idx];
    }
    if (sel_mode != e.state) {
      std::map<std::string, std::string> hdata;
      hdata["entity_id"] = e.entity_id;
      hdata["hvac_mode"] = sel_mode;
      this->call_homeassistant_service("climate.set_hvac_mode", hdata);
      ESP_LOGI(TAG, "apply %s → climate.set_hvac_mode=%s", e.entity_id.c_str(),
               sel_mode.c_str());
    }
    // Dual modes (auto/heat_cool) send target_temp_low + target_temp_high; the
    // single `temperature` param is invalid for them. Single modes send
    // `temperature`. Match what the visible dial(s) edited.
    if (climate_mode_is_dual_(sel_mode) && this->dw_temp_low_slider_ != nullptr &&
        this->dw_temp_high_slider_ != nullptr) {
      int lo = lv_arc_get_value(this->dw_temp_low_slider_);
      int hi = lv_arc_get_value(this->dw_temp_high_slider_);
      char lob[24], hib[24];
      snprintf(lob, sizeof(lob), "%.1f", (float) lo * this->dw_temp_step_);
      snprintf(hib, sizeof(hib), "%.1f", (float) hi * this->dw_temp_step_);
      std::map<std::string, std::string> tdata;
      tdata["entity_id"] = e.entity_id;
      tdata["target_temp_low"] = lob;
      tdata["target_temp_high"] = hib;
      this->call_homeassistant_service("climate.set_temperature", tdata);
      ESP_LOGI(TAG, "apply %s → climate.set_temperature low=%s high=%s",
               e.entity_id.c_str(), lob, hib);
    } else if (this->dw_temp_slider_ != nullptr) {
      int v = lv_arc_get_value(this->dw_temp_slider_);
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
      int v = lv_arc_get_value(this->dw_volume_slider_);
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

void HAPanel::on_detail_light_switch_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->dw_light_switch_ == nullptr ||
      self->dw_brightness_slider_ == nullptr ||
      self->dw_brightness_label_ == nullptr)
    return;
  bool on = lv_obj_has_state(self->dw_light_switch_, LV_STATE_CHECKED);
  char buf[24];
  if (on) {
    // Reveal the slider as an editable target. If it didn't already hold a
    // real value, seed from the last-known cache, else a neutral 50 %.
    lv_obj_clear_state(self->dw_brightness_slider_, LV_STATE_DISABLED);
    lv_obj_add_flag(self->dw_brightness_slider_, LV_OBJ_FLAG_CLICKABLE);
    if (!self->dw_brightness_known_) {
      int seed = 50;
      size_t i = self->detail_entity_idx_;
      if (i < self->entities_.size() && self->entities_[i].last_bri_pct >= 0)
        seed = self->entities_[i].last_bri_pct;
      lv_slider_set_value(self->dw_brightness_slider_, seed, LV_ANIM_OFF);
      self->dw_brightness_known_ = true;
    }
    int v = lv_slider_get_value(self->dw_brightness_slider_);
    snprintf(buf, sizeof(buf), "%d %%", v);
  } else {
    // Power off — grey the slider back to a placeholder, drop the target.
    lv_obj_add_state(self->dw_brightness_slider_, LV_STATE_DISABLED);
    lv_obj_clear_flag(self->dw_brightness_slider_, LV_OBJ_FLAG_CLICKABLE);
    self->dw_brightness_known_ = false;
    snprintf(buf, sizeof(buf), "Off");
  }
  lv_label_set_text(self->dw_brightness_label_, buf);
}

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
  int v = lv_arc_get_value(self->dw_temp_slider_);
  char buf[24];
  fmt_setpoint_(buf, sizeof(buf), (float) v * self->dw_temp_step_,
                self->dw_temp_step_);
  lv_label_set_text(self->dw_temp_label_, buf);
}

// UE3 dual setpoint: heat-point dial. Clamp low <= high (push high up if the
// user drags past it) so the band stays valid.
void HAPanel::on_detail_temp_low_slider_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->dw_temp_low_slider_ == nullptr ||
      self->dw_temp_low_label_ == nullptr)
    return;
  int v = lv_arc_get_value(self->dw_temp_low_slider_);
  char buf[24];
  if (self->dw_temp_high_slider_ != nullptr &&
      v > lv_arc_get_value(self->dw_temp_high_slider_)) {
    lv_arc_set_value(self->dw_temp_high_slider_, v);
    if (self->dw_temp_high_label_ != nullptr) {
      fmt_setpoint_(buf, sizeof(buf), (float) v * self->dw_temp_step_,
                    self->dw_temp_step_);
      lv_label_set_text(self->dw_temp_high_label_, buf);
    }
  }
  fmt_setpoint_(buf, sizeof(buf), (float) v * self->dw_temp_step_,
                self->dw_temp_step_);
  lv_label_set_text(self->dw_temp_low_label_, buf);
}

// UE3 dual setpoint: cool-point dial. Clamp high >= low (pull low down if the
// user drags below it).
void HAPanel::on_detail_temp_high_slider_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->dw_temp_high_slider_ == nullptr ||
      self->dw_temp_high_label_ == nullptr)
    return;
  int v = lv_arc_get_value(self->dw_temp_high_slider_);
  char buf[24];
  if (self->dw_temp_low_slider_ != nullptr &&
      v < lv_arc_get_value(self->dw_temp_low_slider_)) {
    lv_arc_set_value(self->dw_temp_low_slider_, v);
    if (self->dw_temp_low_label_ != nullptr) {
      fmt_setpoint_(buf, sizeof(buf), (float) v * self->dw_temp_step_,
                    self->dw_temp_step_);
      lv_label_set_text(self->dw_temp_low_label_, buf);
    }
  }
  fmt_setpoint_(buf, sizeof(buf), (float) v * self->dw_temp_step_,
                self->dw_temp_step_);
  lv_label_set_text(self->dw_temp_high_label_, buf);
}

// UE3: HVAC mode dropdown changed — swap single vs dual setpoint dials and
// re-tint the single dial to the newly selected mode.
void HAPanel::on_detail_hvac_mode_changed_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr || self->dw_hvac_dropdown_ == nullptr ||
      self->dw_temp_single_box_ == nullptr ||
      self->dw_temp_dual_box_ == nullptr)
    return;
  uint16_t idx = lv_dropdown_get_selected(self->dw_hvac_dropdown_);
  if (idx >= self->dw_hvac_modes_.size())
    return;
  const std::string &mode = self->dw_hvac_modes_[idx];
  if (climate_mode_is_dual_(mode)) {
    lv_obj_add_flag(self->dw_temp_single_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(self->dw_temp_dual_box_, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(self->dw_temp_single_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(self->dw_temp_dual_box_, LV_OBJ_FLAG_HIDDEN);
    if (self->dw_temp_slider_ != nullptr) {
      uint32_t col = climate_mode_color_(mode);
      lv_obj_set_style_arc_color(self->dw_temp_slider_, lv_color_hex(col),
                                 LV_PART_INDICATOR);
      lv_obj_set_style_bg_color(self->dw_temp_slider_, lv_color_hex(col),
                                LV_PART_KNOB);
    }
  }
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
  int v = lv_arc_get_value(self->dw_volume_slider_);
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
    const char *state_txt;
    uint32_t state_col;
    cover_state_line_(e.state, &state_txt, &state_col);
    add_section_label(this->confirm_body_, state_txt, state_col);
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

void HAPanel::open_burnin_warning_() {
  // UE10: reuse the confirm overlay for the burn-in warning — warning text + a
  // single Proceed button. The sheet's own bottom Cancel (on_confirm_cancel_ →
  // close_confirm_) and bg-tap return to the still-open settings sheet beneath
  // without committing anything.
  if (this->confirm_sheet_ == nullptr) {
    this->commit_settings_();  // no overlay to show → don't block the user
    return;
  }
  lv_label_set_text(this->confirm_title_, "Burn-in warning");
  if (this->confirm_unavail_label_ != nullptr)
    lv_obj_add_flag(this->confirm_unavail_label_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(this->confirm_body_);

  lv_obj_t *msg = add_section_label(
      this->confirm_body_,
      "Dim, blank and sleep are all\noff. The screen will stay at\nfull "
      "brightness and can\npermanently burn in. Continue?",
      0xDDAA33);
  lv_obj_set_width(msg, 384);
  lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);

  add_confirm_button(this->confirm_body_, "Proceed anyway", 0x553A2A, 0xFFFFFF,
                     &HAPanel::on_burnin_proceed_, this, true);

  lv_obj_clear_flag(this->confirm_sheet_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(this->confirm_sheet_);
  this->confirm_open_ = true;
  ESP_LOGI(TAG, "burn-in warning shown");
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

// ---------- E9 read-only history chart sheet ----------

// Ring-buffer cap per chartable entity. ~240 samples × 8 B = ~1.9 KB each;
// only chartable read-only entities allocate one. HA pushes on change only, so
// this is "last 240 changes since boot", decimated to the chart width on draw.
static const size_t HISTORY_CAP = 240;
// UE7: deeper ring for `realtime: true` entities (scope mode). A high-rate feed
// at f Hz holds HISTORY_CAP_RT / f seconds; 600 backs the 30 s Live window up to
// 20 Hz. ~600 × 8 B = ~4.7 KB each — paid only by the handful of realtime
// entities, NOT the ~50 ordinary sensors (internal heap is tight; see 082f308).
static const size_t HISTORY_CAP_RT = 600;
// Window chip → seconds. Index matches history_window_idx_ (0 = 1h, …).
// UE7: index 3 = "Live" — a short scope window for watching high-rate sensors
// scroll in real time. 30 s decimates near 1:1 to MAX_CHART_POINTS, so the trace
// sweeps visibly. Backed by the realtime ring (HISTORY_CAP_RT) for those entities.
static const uint32_t HISTORY_WINDOW_S[4] = {3600u, 6u * 3600u, 24u * 3600u, 30u};
// Cap the points fed to lv_chart. ~200 across the 432 px chart ≈ 2 px/point — a
// smooth scope trace without sub-pixel waste. Long windows decimate down to it.
static const size_t MAX_CHART_POINTS = 200;
// Cap the points kept from a REST response. Bounds the internal-heap vector (a
// 24 h per-minute series is ~1440 pts). ≥ MAX_CHART_POINTS so the chart's own
// decimation still has every point it needs; kept at 300 to limit internal heap
// during the fetch (REST = non-realtime only). The chart never shows more than
// MAX_CHART_POINTS.
static const size_t SAMPLE_CAP = 300;
// UE7: fixed slot count for the Live roll-mode trace. The newest sample sits in
// the rightmost slot; the chart shows at most this many samples at a constant
// X scale (≈432 px / 60 ≈ 7 px/sample). Time span shown ≈ LIVE_CHART_POINTS /
// sample_rate seconds (60 ≈ 30 s at 2 Hz). Raise for a wider / smoother sweep.
static const size_t LIVE_CHART_POINTS = 60;

// Format an elapsed duration (in seconds) as a compact "-12s" / "-45m" /
// "-2.5h" axis label.
static void fmt_age_(uint32_t s, char *buf, size_t n) {
  if (s < 90u) {
    snprintf(buf, n, "-%us", (unsigned) s);
    return;
  }
  uint32_t m = (s + 30u) / 60u;
  if (m < 90u) {
    snprintf(buf, n, "-%um", (unsigned) m);
    return;
  }
  uint32_t h10 = (s * 10u + 1800u) / 3600u;  // tenths of an hour, rounded
  snprintf(buf, n, "-%u.%uh", (unsigned) (h10 / 10u), (unsigned) (h10 % 10u));
}

bool HAPanel::state_to_value_(const Entity &e, float *out) {
  const std::string &s = e.state;
  if (!e.has_state || s.empty() || s == "unavailable" || s == "unknown")
    return false;
  if (e.domain == "binary_sensor") {
    if (s == "on") { *out = 1.0f; return true; }
    if (s == "off") { *out = 0.0f; return true; }
    return false;
  }
  const char *c = s.c_str();
  char *end = nullptr;
  float v = strtof(c, &end);
  if (end == c)
    return false;
  *out = v;
  return true;
}

bool HAPanel::is_chartable_(const Entity &e) {
  if (e.domain == "binary_sensor")
    return true;
  if (e.domain != "sensor")
    return false;
  // A sensor is chartable if it's ever produced a numeric sample (history) or
  // its current state parses as a number. Covers a sensor that reads
  // "unavailable" at tap time but is normally numeric.
  if (!e.history.empty())
    return true;
  float v;
  return HAPanel::state_to_value_(e, &v);
}

void HAPanel::record_history_(size_t entity_idx) {
  if (entity_idx >= this->entities_.size())
    return;
  Entity &e = this->entities_[entity_idx];
  if (!HAPanel::is_chartable_(e))
    return;
  float v;
  if (!HAPanel::state_to_value_(e, &v))
    return;  // skip unavailable/unknown/non-numeric transients
  // Ring-buffer samples are stamped in uptime-seconds (millis()/1000).
  e.history.push_back({millis() / 1000u, v});
  // UE7: realtime entities keep a deeper ring so the short scope window stays
  // fully backed at high sample rates.
  const size_t cap = e.realtime ? HISTORY_CAP_RT : HISTORY_CAP;
  if (e.history.size() > cap)
    e.history.erase(e.history.begin());
}

void HAPanel::build_history_sheet_(lv_obj_t *scr) {
  // Full-screen overlay, built once, hidden — same recipe as detail_modal_.
  this->history_sheet_ = lv_obj_create(scr);
  lv_obj_remove_style_all(this->history_sheet_);
  lv_obj_set_size(this->history_sheet_, 480, 480);
  lv_obj_set_pos(this->history_sheet_, 0, 0);
  lv_obj_set_style_bg_color(this->history_sheet_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(this->history_sheet_, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(this->history_sheet_, 0, 0);
  lv_obj_set_style_border_width(this->history_sheet_, 0, 0);
  lv_obj_add_flag(this->history_sheet_, LV_OBJ_FLAG_HIDDEN);
  // CLICKABLE so backdrop taps are absorbed (not passed to the page beneath),
  // but no close-on-backdrop handler: only the ✕ button closes this sheet.
  lv_obj_add_flag(this->history_sheet_, LV_OBJ_FLAG_CLICKABLE);

  // Title (top-left), capped + ellipsised so it clears the close button.
  this->history_title_ = lv_label_create(this->history_sheet_);
  lv_label_set_text(this->history_title_, "");
  lv_obj_set_style_text_color(this->history_title_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(this->history_title_, &lv_font_montserrat_18, 0);
  lv_obj_set_width(this->history_title_, 380);
  lv_label_set_long_mode(this->history_title_, LV_LABEL_LONG_DOT);
  lv_obj_align(this->history_title_, LV_ALIGN_TOP_LEFT, 24, 16);

  // Close ✕ (top-right). 44 px corner inset clears the rounded bezel.
  lv_obj_t *close = lv_button_create(this->history_sheet_);
  lv_obj_set_size(close, 44, 36);
  lv_obj_set_style_bg_opa(close, LV_OPA_TRANSP, 0);
  lv_obj_set_style_bg_color(close, lv_color_hex(0x2E3640), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(close, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_radius(close, 8, 0);
  lv_obj_set_style_border_width(close, 0, 0);
  lv_obj_set_style_shadow_width(close, 0, 0);
  lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -40, 8);
  lv_obj_add_event_cb(close, &HAPanel::on_history_close_, LV_EVENT_CLICKED, this);
  lv_obj_t *clbl = lv_label_create(close);
  lv_label_set_text(clbl, LV_SYMBOL_CLOSE);
  lv_obj_set_style_text_color(clbl, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(clbl, &lv_font_montserrat_18, 0);
  lv_obj_center(clbl);

  // Current value, large.
  this->history_value_ = lv_label_create(this->history_sheet_);
  lv_label_set_text(this->history_value_, "");
  lv_obj_set_style_text_color(this->history_value_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(this->history_value_, &lv_font_montserrat_24, 0);
  lv_obj_align(this->history_value_, LV_ALIGN_TOP_LEFT, 24, 52);

  // Numeric chart (line). Hidden when the open entity is a binary_sensor.
  // UE4: shortened 230→176 and dropped to y=150 (bottom stays at 326) to free a
  // band above it for the top-right analog gauge.
  this->history_chart_ = lv_chart_create(this->history_sheet_);
  lv_obj_set_size(this->history_chart_, 432, 176);
  lv_obj_align(this->history_chart_, LV_ALIGN_TOP_MID, 0, 150);
  lv_obj_set_style_bg_color(this->history_chart_, lv_color_hex(0x111111), 0);
  lv_obj_set_style_bg_opa(this->history_chart_, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(this->history_chart_, 0, 0);
  lv_obj_set_style_radius(this->history_chart_, 8, 0);
  lv_obj_set_style_pad_all(this->history_chart_, 4, 0);
  lv_chart_set_type(this->history_chart_, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(this->history_chart_, 3, 0);
  lv_obj_set_style_line_color(this->history_chart_, lv_color_hex(0x333333),
                              LV_PART_MAIN);
  // 2 px line; hide the per-point dots for a clean trend line.
  lv_obj_set_style_line_width(this->history_chart_, 2, LV_PART_ITEMS);
  lv_obj_set_style_width(this->history_chart_, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(this->history_chart_, 0, LV_PART_INDICATOR);
  this->history_series_ = lv_chart_add_series(
      this->history_chart_, lv_color_hex(0x44CCDD), LV_CHART_AXIS_PRIMARY_Y);

  // Binary timeline strip (on/off bands). Same footprint as the chart; exactly
  // one of the two is visible per open. Children (bands) are rebuilt on redraw.
  this->history_strip_ = lv_obj_create(this->history_sheet_);
  lv_obj_remove_style_all(this->history_strip_);
  lv_obj_set_size(this->history_strip_, 432, 176);
  lv_obj_align(this->history_strip_, LV_ALIGN_TOP_MID, 0, 150);
  lv_obj_set_style_bg_color(this->history_strip_, lv_color_hex(0x111111), 0);
  lv_obj_set_style_bg_opa(this->history_strip_, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(this->history_strip_, 8, 0);
  lv_obj_set_style_pad_all(this->history_strip_, 0, 0);
  lv_obj_set_style_clip_corner(this->history_strip_, true, 0);
  lv_obj_clear_flag(this->history_strip_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(this->history_strip_, LV_OBJ_FLAG_HIDDEN);

  // UE6: real lv_spinner over the chart footprint, shown during the worker-task
  // backfill. The fetch no longer blocks the main loop, so lv_timer_handler keeps
  // firing and this self-animates (retiring the UE2 hand-rotated arc + its
  // spin_history_/lv_refr_now pumping). No knob, not clickable.
  this->history_spinner_ = lv_spinner_create(this->history_sheet_);
  lv_spinner_set_anim_duration(this->history_spinner_, 1500);  // 1.5 s/rev — match splash
  lv_spinner_set_arc_sweep(this->history_spinner_, 270);       // 270° arc — match splash
  lv_obj_set_size(this->history_spinner_, 60, 60);
  lv_obj_align(this->history_spinner_, LV_ALIGN_TOP_MID, 0, 208);
  lv_obj_remove_flag(this->history_spinner_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(this->history_spinner_, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(this->history_spinner_, lv_color_hex(0x222222),
                             LV_PART_MAIN);
  lv_obj_set_style_arc_width(this->history_spinner_, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(this->history_spinner_, lv_color_hex(0x44CCDD),
                             LV_PART_INDICATOR);
  lv_obj_add_flag(this->history_spinner_, LV_OBJ_FLAG_HIDDEN);

  // UE4: analog "now" gauge — a round lv_scale + line needle, top-right in the
  // band freed by the shortened chart, just under the ✕ button. It renders the
  // *current* value (same as history_value_) as a needle angle; the chart stays
  // the focus (gauge = now, chart = history). Tick labels are off: the numbers
  // live in history_value_ / history_range_label_, and rotated labels on a 96 px
  // dial would only clutter it. Hidden for binary_sensor and no-data (see
  // redraw_history_); shown only once numeric data is drawn.
  this->history_gauge_ = lv_scale_create(this->history_sheet_);
  lv_obj_set_size(this->history_gauge_, 96, 96);
  lv_obj_align(this->history_gauge_, LV_ALIGN_TOP_RIGHT, -24, 48);
  lv_obj_remove_flag(this->history_gauge_, LV_OBJ_FLAG_CLICKABLE);
  lv_scale_set_mode(this->history_gauge_, LV_SCALE_MODE_ROUND_INNER);
  lv_scale_set_label_show(this->history_gauge_, false);
  lv_scale_set_total_tick_count(this->history_gauge_, 21);
  lv_scale_set_major_tick_every(this->history_gauge_, 5);
  // 270° sweep starting at 7-8 o'clock (rotation 135) — the classic gauge layout.
  lv_scale_set_angle_range(this->history_gauge_, 270);
  lv_scale_set_rotation(this->history_gauge_, 135);
  // Main = the arc track; ITEMS = minor ticks; INDICATOR = major ticks.
  lv_obj_set_style_arc_width(this->history_gauge_, 3, LV_PART_MAIN);
  lv_obj_set_style_arc_color(this->history_gauge_, lv_color_hex(0x444444),
                             LV_PART_MAIN);
  lv_obj_set_style_line_color(this->history_gauge_, lv_color_hex(0x666666),
                              LV_PART_ITEMS);
  lv_obj_set_style_length(this->history_gauge_, 4, LV_PART_ITEMS);
  lv_obj_set_style_line_width(this->history_gauge_, 1, LV_PART_ITEMS);
  lv_obj_set_style_line_color(this->history_gauge_, lv_color_hex(0xAAAAAA),
                              LV_PART_INDICATOR);
  lv_obj_set_style_length(this->history_gauge_, 8, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(this->history_gauge_, 2, LV_PART_INDICATOR);

  // Needle: a line child of the scale; lv_scale_set_line_needle_value owns its
  // point array and re-aims it each redraw. Teal to match the chart series.
  this->history_gauge_needle_ = lv_line_create(this->history_gauge_);
  lv_obj_set_style_line_width(this->history_gauge_needle_, 3, 0);
  lv_obj_set_style_line_color(this->history_gauge_needle_,
                              lv_color_hex(0x44CCDD), 0);
  lv_obj_set_style_line_rounded(this->history_gauge_needle_, true, 0);
  lv_obj_add_flag(this->history_gauge_, LV_OBJ_FLAG_HIDDEN);

  // Bottom row under the chart: time span (left = oldest visible sample age,
  // right = "now") + centered value range. Time markers make the x-axis legible
  // and stop a data-starved window from looking like a populated one.
  this->history_time_left_ = lv_label_create(this->history_sheet_);
  lv_label_set_text(this->history_time_left_, "");
  lv_obj_set_style_text_color(this->history_time_left_, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(this->history_time_left_, &lv_font_montserrat_18, 0);
  lv_obj_align(this->history_time_left_, LV_ALIGN_TOP_LEFT, 24, 332);

  this->history_range_label_ = lv_label_create(this->history_sheet_);
  lv_label_set_text(this->history_range_label_, "");
  lv_obj_set_style_text_color(this->history_range_label_, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(this->history_range_label_, &lv_font_montserrat_18, 0);
  lv_obj_align(this->history_range_label_, LV_ALIGN_TOP_MID, 0, 332);

  this->history_time_right_ = lv_label_create(this->history_sheet_);
  lv_label_set_text(this->history_time_right_, "");
  lv_obj_set_style_text_color(this->history_time_right_, lv_color_hex(0x888888), 0);
  lv_obj_set_style_text_font(this->history_time_right_, &lv_font_montserrat_18, 0);
  lv_obj_align(this->history_time_right_, LV_ALIGN_TOP_RIGHT, -24, 332);

  // Window chips: 1h / 6h / 24h / Live segmented row.
  lv_obj_t *chips = lv_obj_create(this->history_sheet_);
  lv_obj_remove_style_all(chips);
  lv_obj_set_size(chips, 480, 56);
  lv_obj_set_pos(chips, 0, 372);
  lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_left(chips, 32, 0);
  lv_obj_set_style_pad_right(chips, 32, 0);
  lv_obj_set_style_pad_column(chips, 12, 0);
  lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(chips, LV_OBJ_FLAG_SCROLLABLE);

  const char *chip_text[4] = {"1h", "6h", "24h", "Live"};
  for (int i = 0; i < 4; i++) {
    lv_obj_t *chip = lv_button_create(chips);
    lv_obj_set_size(chip, 92, 44);
    lv_obj_set_style_bg_color(chip, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, 8, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_user_data(chip, (void *) (uintptr_t) i);
    lv_obj_add_event_cb(chip, &HAPanel::on_history_chip_, LV_EVENT_CLICKED, this);
    lv_obj_t *lbl = lv_label_create(chip);
    lv_label_set_text(lbl, chip_text[i]);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl);
    this->history_chips_[i] = chip;
  }
}

void HAPanel::open_history_(size_t entity_idx) {
  if (this->history_sheet_ == nullptr || entity_idx >= this->entities_.size())
    return;
  const Entity &e = this->entities_[entity_idx];
  this->history_entity_idx_ = entity_idx;
  // UE7: realtime entities open on the "Live" window (idx 3); everyone else
  // keeps the 1 h default (idx 0).
  this->history_window_idx_ = e.realtime ? 3 : 0;
  lv_label_set_text(this->history_title_, e.friendly_name.c_str());
  lv_obj_clear_flag(this->history_sheet_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(this->history_sheet_);
  this->history_open_ = true;
  // UE6: async for REST (spinner now, redraw when the worker finishes); the
  // no-REST path inside redraws synchronously.
  this->start_history_load_(entity_idx, this->history_window_idx_);
  ESP_LOGI(TAG, "history open: %s", e.entity_id.c_str());
}

void HAPanel::close_history_() {
  if (this->history_sheet_ == nullptr)
    return;
  lv_obj_add_flag(this->history_sheet_, LV_OBJ_FLAG_HIDDEN);
  this->history_open_ = false;
  ESP_LOGD(TAG, "history close");
}

void HAPanel::show_history_loading_() {
  // Loading state: spinner up over a cleared chart area. (montserrat_18 has no
  // ellipsis glyph, hence the literal "...".)
  lv_label_set_text(this->history_value_, "Loading...");
  lv_obj_add_flag(this->history_chart_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(this->history_strip_, LV_OBJ_FLAG_HIDDEN);
  if (this->history_gauge_ != nullptr)
    lv_obj_add_flag(this->history_gauge_, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(this->history_time_left_, "");
  lv_label_set_text(this->history_time_right_, "");
  lv_label_set_text(this->history_range_label_, "");
  if (this->history_spinner_ != nullptr) {
    lv_obj_clear_flag(this->history_spinner_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(this->history_spinner_);
  }
}

void HAPanel::start_history_load_(size_t entity_idx, uint8_t window_idx) {
  if (entity_idx >= this->entities_.size())
    return;
  // No REST deps, or a realtime entity that opts out of backfill → synchronous
  // ring-buffer copy + immediate redraw (no fetch, no spinner).
  if (!this->history_rest_enabled_() || this->entities_[entity_idx].realtime) {
    this->history_rest_mode_ = false;
    this->history_samples_ = this->entities_[entity_idx].history;
    this->redraw_history_();
    return;
  }
  // REST: show the spinner now and record the desired request.
  this->show_history_loading_();
  this->hist_want_entity_ = entity_idx;
  this->hist_want_window_ = window_idx;
  this->hist_seq_want_++;
  this->hist_want_pending_ = true;
  // Prefer the async worker (no UI freeze); fall back to a blocking fetch on the
  // main loop if the worker can't be created (heap too low for its stack).
  if (this->ensure_history_worker_())
    this->dispatch_history_fetch_();
  else
    this->run_history_fetch_sync_();
}

bool HAPanel::ensure_history_worker_() {
  if (this->hist_task_ != nullptr)
    return true;
  if (this->hist_req_sem_ == nullptr) {
    this->hist_req_sem_ = xSemaphoreCreateBinary();
    if (this->hist_req_sem_ == nullptr) {
      ESP_LOGW(TAG, "history: semaphore alloc failed — using sync fetch");
      return false;
    }
  }
  // 8 KB stack (core 0, prio 2): the same GET + parse already ran inside the main
  // loop task's stack in the pre-task path, so 8 KB is ample for LAN http + the
  // shallow ArduinoJson DOM. A https base URL (mbedTLS) would need far more.
  // Created lazily so its stack doesn't compete with the WiFi scan allocator at
  // boot. Persistent once up (no per-open create/delete churn).
  BaseType_t r = xTaskCreatePinnedToCore(&HAPanel::history_task_trampoline_,
                                         "ha_hist", 8192, this, 2,
                                         &this->hist_task_, 0);
  if (r != pdPASS) {
    this->hist_task_ = nullptr;
    ESP_LOGW(TAG, "history worker create failed — using sync fetch "
                  "(internal free=%u largest=%u)",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return false;
  }
  ESP_LOGI(TAG, "UE6: history worker task started on core 0");
  return true;
}

void HAPanel::run_history_fetch_sync_() {
  // Blocking fetch on the main loop (WDT backstop covers it) — the pre-UE6
  // behavior, used only when the worker task couldn't be created.
  std::string url;
  if (!this->build_history_url_(this->hist_want_entity_, this->hist_want_window_,
                                &url)) {
    this->hist_want_pending_ = false;
    this->history_rest_mode_ = false;
    if (this->hist_want_entity_ < this->entities_.size())
      this->history_samples_ = this->entities_[this->hist_want_entity_].history;
    if (this->history_open_ &&
        this->history_entity_idx_ == this->hist_want_entity_)
      this->redraw_history_();
    return;
  }
  this->hist_req_url_ = url;
  this->hist_req_is_binary_ =
      this->entities_[this->hist_want_entity_].domain == "binary_sensor";
  this->hist_want_pending_ = false;
  const bool ok = this->run_history_fetch_();  // blocks
  if (ok) {
    this->history_samples_.swap(this->hist_staging_);
    this->history_rest_mode_ = true;
  } else {
    this->history_rest_mode_ = false;
    if (this->hist_want_entity_ < this->entities_.size())
      this->history_samples_ = this->entities_[this->hist_want_entity_].history;
  }
  this->hist_staging_.clear();
  if (this->history_open_ &&
      this->history_entity_idx_ == this->hist_want_entity_)
    this->redraw_history_();
}

void HAPanel::dispatch_history_fetch_() {
  // Don't touch the request fields while the worker may be reading them.
  if (this->hist_fetch_state_.load(std::memory_order_acquire) == HIST_RUNNING)
    return;
  if (!this->hist_want_pending_)
    return;
  std::string url;
  if (!this->build_history_url_(this->hist_want_entity_, this->hist_want_window_,
                                &url)) {
    // Clock not valid yet / bad idx → fall back to the ring buffer synchronously.
    ESP_LOGW(TAG, "history: cannot build URL — ring-buffer fallback");
    this->hist_want_pending_ = false;
    this->history_rest_mode_ = false;
    if (this->hist_want_entity_ < this->entities_.size())
      this->history_samples_ = this->entities_[this->hist_want_entity_].history;
    if (this->history_open_ &&
        this->history_entity_idx_ == this->hist_want_entity_)
      this->redraw_history_();
    return;
  }
  this->hist_req_url_ = url;
  this->hist_req_is_binary_ =
      this->entities_[this->hist_want_entity_].domain == "binary_sensor";
  this->hist_req_seq_ = this->hist_seq_want_;
  this->hist_want_pending_ = false;
  // Publish the request, then wake the worker. The store(release) + semaphore
  // give ensure the worker sees the request fields above.
  this->hist_fetch_state_.store(HIST_RUNNING, std::memory_order_release);
  xSemaphoreGive(this->hist_req_sem_);
}

void HAPanel::poll_history_fetch_() {
  if (this->hist_req_sem_ == nullptr)
    return;  // no worker
  uint8_t s = this->hist_fetch_state_.load(std::memory_order_acquire);
  if (s != HIST_DONE_OK && s != HIST_DONE_FAIL)
    return;
  this->hist_fetch_state_.store(HIST_IDLE, std::memory_order_relaxed);
  const bool ok = (s == HIST_DONE_OK);
  // Only the newest desired request matters: a window switch mid-fetch bumped
  // hist_seq_want_ past the in-flight seq, so an older result is dropped.
  const bool current = (this->hist_req_seq_ == this->hist_seq_want_);
  if (current) {
    if (ok) {
      this->history_samples_.swap(this->hist_staging_);
      this->history_rest_mode_ = true;
    } else {
      ESP_LOGW(TAG, "history REST fetch failed — falling back to ring buffer");
      this->history_rest_mode_ = false;
      if (this->hist_want_entity_ < this->entities_.size())
        this->history_samples_ = this->entities_[this->hist_want_entity_].history;
    }
    if (this->history_open_ &&
        this->history_entity_idx_ == this->hist_want_entity_)
      this->redraw_history_();
  }
  this->hist_staging_.clear();
  // A newer request queued while the worker was busy → dispatch it now.
  if (this->hist_want_pending_)
    this->dispatch_history_fetch_();
}

void HAPanel::update_history_gauge_(float vmin, float vmax, float current) {
  if (this->history_gauge_ == nullptr || this->history_gauge_needle_ == nullptr)
    return;
  // Pad the data window's range so the needle floats inside the dial instead of
  // pinning to an end (the current value is itself part of [vmin,vmax]). A flat
  // series gets a synthetic span so the scale isn't degenerate.
  float span = vmax - vmin;
  float pad = span > 0.0f ? span * 0.15f
                          : (std::fabs(vmax) > 1.0f ? std::fabs(vmax) * 0.1f : 1.0f);
  float gmin = vmin - pad, gmax = vmax + pad;
  // Scale ×10 for one decimal of needle resolution. Labels are off, so the
  // scaled ints are never shown — they only set the needle's angular position.
  int32_t lo = (int32_t) std::lround(gmin * 10.0f);
  int32_t hi = (int32_t) std::lround(gmax * 10.0f);
  if (hi <= lo) hi = lo + 1;
  lv_scale_set_range(this->history_gauge_, lo, hi);
  int32_t nv = (int32_t) std::lround(current * 10.0f);
  if (nv < lo) nv = lo;
  if (nv > hi) nv = hi;
  lv_scale_set_line_needle_value(this->history_gauge_, this->history_gauge_needle_,
                                 34, nv);
  lv_obj_clear_flag(this->history_gauge_, LV_OBJ_FLAG_HIDDEN);
}

// UE7: roll-mode draw for the Live window. Pins the newest sample to the right
// edge at a fixed X scale; older samples step left one slot each; blank slots on
// the left until the trace fills. `now` is the per-mode clock from redraw_history_
// (uptime-seconds for the realtime ring, epoch for a REST-backed Live view).
void HAPanel::redraw_live_roll_(const Entity &e, uint32_t now) {
  const size_t N = LIVE_CHART_POINTS;
  const size_t total = this->history_samples_.size();
  const size_t k = total < N ? total : N;  // samples actually drawn
  if (k == 0) {
    lv_chart_set_point_count(this->history_chart_, 0);
    lv_chart_refresh(this->history_chart_);
    lv_label_set_text(this->history_value_, "No data yet");
    lv_label_set_text(this->history_time_left_, "");
    lv_label_set_text(this->history_time_right_, "");
    lv_label_set_text(this->history_range_label_, "");
    if (this->history_gauge_ != nullptr)
      lv_obj_add_flag(this->history_gauge_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  // The k most-recent samples are the tail of the ring.
  const size_t first = total - k;  // index of the oldest drawn sample
  float vmin = this->history_samples_[first].value;
  float vmax = vmin;
  for (size_t i = first; i < total; i++) {
    float v = this->history_samples_[i].value;
    if (v < vmin) vmin = v;
    if (v > vmax) vmax = v;
  }
  int32_t lo = (int32_t) std::floor(vmin * 10.0f);
  int32_t hi = (int32_t) std::ceil(vmax * 10.0f);
  if (lo == hi) { lo -= 10; hi += 10; }  // flat series → vertical room
  lv_chart_set_axis_range(this->history_chart_, LV_CHART_AXIS_PRIMARY_Y, lo, hi);

  // Fixed slot count. Left (N-k) slots blank, the k samples fill the right,
  // newest in the last slot → trace originates from the right and grows left.
  lv_chart_set_point_count(this->history_chart_, (uint32_t) N);
  for (size_t slot = 0; slot < N - k; slot++)
    lv_chart_set_series_value_by_id(this->history_chart_, this->history_series_,
                                    (uint32_t) slot, LV_CHART_POINT_NONE);
  for (size_t i = 0; i < k; i++)
    lv_chart_set_series_value_by_id(
        this->history_chart_, this->history_series_, (uint32_t) (N - k + i),
        (int32_t) std::lround(this->history_samples_[first + i].value * 10.0f));
  lv_chart_refresh(this->history_chart_);

  lv_label_set_text(this->history_value_, e.has_state ? e.state.c_str() : "...");
  char buf[24];
  fmt_age_(now - this->history_samples_[first].t_s, buf, sizeof(buf));
  lv_label_set_text(this->history_time_left_, buf);
  lv_label_set_text(this->history_time_right_, "now");
  char range[40];
  snprintf(range, sizeof(range), "%.1f - %.1f", vmin, vmax);
  lv_label_set_text(this->history_range_label_, range);

  float gnow;
  if (!HAPanel::state_to_value_(e, &gnow))
    gnow = this->history_samples_.back().value;
  this->update_history_gauge_(vmin, vmax, gnow);
}

void HAPanel::redraw_history_() {
  if (this->history_sheet_ == nullptr ||
      this->history_entity_idx_ >= this->entities_.size())
    return;
  // The fetch is done by the time we redraw — hide the loading spinner.
  if (this->history_spinner_ != nullptr)
    lv_obj_add_flag(this->history_spinner_, LV_OBJ_FLAG_HIDDEN);
  const Entity &e = this->entities_[this->history_entity_idx_];

  // Highlight the active window chip.
  for (int i = 0; i < 4; i++) {
    if (this->history_chips_[i] == nullptr)
      continue;
    lv_obj_set_style_bg_color(
        this->history_chips_[i],
        lv_color_hex(i == this->history_window_idx_ ? 0x3A4A6A : 0x1A1A1A), 0);
  }

  // Current value (raw state — we don't carry the unit).
  lv_label_set_text(this->history_value_, e.has_state ? e.state.c_str() : "...");

  // Timeline is in seconds. "now" matches the sample origin per mode: UTC epoch
  // for REST-backfilled data, uptime-seconds otherwise. This is what makes the
  // axis labels honest across windows (REST oldest ≈ full window span).
  uint32_t now;
  if (this->history_rest_mode_ && this->history_time_ != nullptr) {
    auto t = this->history_time_->utcnow();
    now = t.is_valid() ? (uint32_t) t.timestamp : millis() / 1000u;
  } else {
    now = millis() / 1000u;
  }
  const uint32_t window_s = HISTORY_WINDOW_S[this->history_window_idx_];
  const uint32_t cutoff = now > window_s ? now - window_s : 0u;
  const bool is_binary = e.domain == "binary_sensor";

  if (is_binary) {
    // ---- on/off band strip ----
    lv_obj_add_flag(this->history_chart_, LV_OBJ_FLAG_HIDDEN);
    if (this->history_gauge_ != nullptr)  // UE4: gauge is numeric-only
      lv_obj_add_flag(this->history_gauge_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(this->history_strip_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(this->history_strip_);
    // The strip's x-axis is genuinely time-proportional, so it spans the full
    // window (clamped to uptime when the device booted more recently).
    lv_label_set_text(this->history_range_label_, "");

    // Anchor state = the last sample at/before the cutoff, so the leading band
    // is coloured even if the last change predates the window.
    bool have_state = false;
    float cur_val = 0.0f;
    uint32_t seg_start = cutoff;
    const int32_t strip_w = 432;
    const int32_t strip_h = 176;  // UE4: matches the shortened chart footprint
    for (const auto &s : this->history_samples_) {
      if (s.t_s <= cutoff) {
        cur_val = s.value;
        have_state = true;
        continue;
      }
      // s.t_s in (cutoff, now]: close the band [seg_start, s.t_s) at cur_val.
      if (have_state) {
        float frac = (float) (s.t_s - seg_start) / (float) window_s;
        int32_t x0 = (int32_t) ((float) (seg_start - cutoff) / window_s * strip_w);
        int32_t w = (int32_t) (frac * strip_w);
        if (w > 0) {
          lv_obj_t *band = lv_obj_create(this->history_strip_);
          lv_obj_remove_style_all(band);
          lv_obj_set_size(band, w, strip_h);
          lv_obj_set_pos(band, x0, 0);
          lv_obj_set_style_bg_color(
              band, lv_color_hex(cur_val > 0.5f ? 0x66BB66 : 0x444444), 0);
          lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
        }
      }
      seg_start = s.t_s;
      cur_val = s.value;
      have_state = true;
    }
    // Final band from the last transition (or cutoff) up to now.
    if (have_state) {
      int32_t x0 = (int32_t) ((float) (seg_start - cutoff) / window_s * strip_w);
      int32_t w = strip_w - x0;
      if (w > 0) {
        lv_obj_t *band = lv_obj_create(this->history_strip_);
        lv_obj_remove_style_all(band);
        lv_obj_set_size(band, w, strip_h);
        lv_obj_set_pos(band, x0, 0);
        lv_obj_set_style_bg_color(
            band, lv_color_hex(cur_val > 0.5f ? 0x66BB66 : 0x444444), 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
      }
    } else {
      lv_obj_t *empty = lv_label_create(this->history_strip_);
      lv_label_set_text(empty, "No data yet");
      lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
      lv_obj_set_style_text_font(empty, &lv_font_montserrat_18, 0);
      lv_obj_center(empty);
    }
    if (have_state) {
      char buf[24];
      fmt_age_(now - cutoff, buf, sizeof(buf));
      lv_label_set_text(this->history_time_left_, buf);
      lv_label_set_text(this->history_time_right_, "now");
    } else {
      lv_label_set_text(this->history_time_left_, "");
      lv_label_set_text(this->history_time_right_, "");
    }
    return;
  }

  // ---- numeric line chart ----
  lv_obj_add_flag(this->history_strip_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(this->history_chart_, LV_OBJ_FLAG_HIDDEN);

  // UE7: Live = roll-mode scope. Fixed X scale: the newest sample is pinned to
  // the right edge, older samples fill leftward one slot each, and the unfilled
  // left slots stay blank until the trace grows into them. A fresh feed therefore
  // starts as a short segment on the right and extends left (no index-spread
  // "squish" that rescales as points accumulate). Sample-order based, so it is
  // immune to the ring's 1 s timestamp resolution.
  if (this->history_window_idx_ == 3) {
    this->redraw_live_roll_(e, now);
    return;
  }

  // Collect in-window values in order, tracking the oldest visible timestamp so
  // the left axis label reflects the real data span. The line chart spaces
  // points evenly (not time-proportionally), so labelling the actual span — not
  // the chosen window — is what keeps a data-starved 6h view honest.
  std::vector<float> vals;
  uint32_t oldest_t = now;
  bool have_anchor = false;
  float anchor_val = 0.0f;
  for (const auto &s : this->history_samples_) {
    if (s.t_s < cutoff) {
      // Last value *before* the window — carried forward as the line's starting
      // level (like the binary strip's anchor band).
      anchor_val = s.value;
      have_anchor = true;
      continue;
    }
    if (vals.empty())
      oldest_t = s.t_s;
    vals.push_back(s.value);
  }
  // Seed the line with the value as of the window start. HA's
  // significant_changes_only returns ~1 boundary point for a flat sensor, and its
  // timestamp sits right at the window edge; the redraw recomputes `cutoff` from
  // a slightly later `now` (the async fetch adds latency), so without this anchor
  // that lone sample drifts just outside the window and the chart intermittently
  // shows "No data yet" even though the value is known.
  if (have_anchor) {
    vals.insert(vals.begin(), anchor_val);
    oldest_t = cutoff;
  }
  if (vals.empty()) {
    lv_chart_set_point_count(this->history_chart_, 0);
    lv_chart_refresh(this->history_chart_);
    lv_label_set_text(this->history_value_, "No data yet");
    lv_label_set_text(this->history_time_left_, "");
    lv_label_set_text(this->history_time_right_, "");
    lv_label_set_text(this->history_range_label_, "");
    if (this->history_gauge_ != nullptr)  // UE4: no needle without data
      lv_obj_add_flag(this->history_gauge_, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  // A single sample can't draw a line — duplicate it into a flat two-point
  // segment so a steady sensor (e.g. pool temp pinned at 84°) renders a flat line.
  if (vals.size() == 1)
    vals.push_back(vals[0]);

  // Decimate to MAX_CHART_POINTS by striding so a 24 h window stays light.
  std::vector<float> pts;
  if (vals.size() > MAX_CHART_POINTS) {
    float stride = (float) vals.size() / (float) MAX_CHART_POINTS;
    for (size_t i = 0; i < MAX_CHART_POINTS; i++)
      pts.push_back(vals[(size_t) (i * stride)]);
  } else {
    pts = vals;
  }

  float vmin = pts[0], vmax = pts[0];
  for (float v : pts) {
    if (v < vmin) vmin = v;
    if (v > vmax) vmax = v;
  }
  // Chart stores int32 — scale ×10 to keep one decimal of precision.
  int32_t lo = (int32_t) std::floor(vmin * 10.0f);
  int32_t hi = (int32_t) std::ceil(vmax * 10.0f);
  if (lo == hi) { lo -= 10; hi += 10; }  // flat series → give it vertical room
  lv_chart_set_axis_range(this->history_chart_, LV_CHART_AXIS_PRIMARY_Y, lo, hi);
  lv_chart_set_point_count(this->history_chart_, (uint32_t) pts.size());
  for (size_t i = 0; i < pts.size(); i++) {
    lv_chart_set_series_value_by_id(this->history_chart_, this->history_series_,
                                    (uint32_t) i,
                                    (int32_t) std::lround(pts[i] * 10.0f));
  }
  lv_chart_refresh(this->history_chart_);

  // Time span (oldest visible → now) on the ends, value range centered.
  char buf[24];
  fmt_age_(now - oldest_t, buf, sizeof(buf));
  lv_label_set_text(this->history_time_left_, buf);
  lv_label_set_text(this->history_time_right_, "now");
  char range[40];
  snprintf(range, sizeof(range), "%.1f - %.1f", vmin, vmax);
  lv_label_set_text(this->history_range_label_, range);

  // UE4: aim the analog gauge at the current value (prefer the live state so it
  // matches history_value_; fall back to the freshest in-window sample), with
  // the dial scaled to this window's value range.
  float gnow;
  if (!HAPanel::state_to_value_(e, &gnow))
    gnow = vals.back();
  this->update_history_gauge_(vmin, vmax, gnow);
}

void HAPanel::on_history_close_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self != nullptr)
    self->close_history_();
}

void HAPanel::on_history_chip_(lv_event_t *e) {
  auto *self = static_cast<HAPanel *>(lv_event_get_user_data(e));
  if (self == nullptr)
    return;
  lv_obj_t *chip = lv_event_get_target_obj(e);
  size_t idx = (size_t) (uintptr_t) lv_obj_get_user_data(chip);
  if (idx > 3 || idx == self->history_window_idx_)
    return;  // re-tapping the active window would needlessly re-fetch
  self->history_window_idx_ = (uint8_t) idx;
  // UE6: REST mode dispatches a worker fetch for the new span (async, redraw on
  // completion); ring-buffer mode re-windows its copy and redraws synchronously.
  self->start_history_load_(self->history_entity_idx_, self->history_window_idx_);
}

// ---------- E9 REST history backfill ----------

bool HAPanel::history_rest_enabled_() const {
  return this->history_http_ != nullptr && this->history_time_ != nullptr &&
         !this->history_base_url_.empty() && !this->history_token_.empty();
}

bool HAPanel::build_history_url_(size_t entity_idx, uint8_t window_idx,
                                 std::string *out) {
  if (entity_idx >= this->entities_.size() || window_idx > 3)
    return false;
  auto t = this->history_time_->utcnow();
  if (!t.is_valid()) {
    ESP_LOGW(TAG, "history: time not valid yet — cannot build request");
    return false;
  }
  const int64_t now_epoch = (int64_t) t.timestamp;
  const uint32_t window_s = HISTORY_WINDOW_S[window_idx];
  const int64_t start_epoch = now_epoch - (int64_t) window_s;

  auto st = ESPTime::from_epoch_utc((time_t) start_epoch);
  // ISO-8601 UTC with the URL-significant chars pre-encoded (':'→%3A, '+'→%2B).
  char start_iso[48];
  snprintf(start_iso, sizeof(start_iso),
           "%04d-%02d-%02dT%02d%%3A%02d%%3A%02d%%2B00%%3A00", st.year,
           st.month, st.day_of_month, st.hour, st.minute, st.second);

  std::string base = this->history_base_url_;
  while (!base.empty() && base.back() == '/')
    base.pop_back();
  const Entity &e = this->entities_[entity_idx];
  *out = base + "/api/history/period/" + start_iso +
         "?filter_entity_id=" + e.entity_id +
         "&minimal_response&no_attributes&significant_changes_only";
  return true;
}

// UE6 worker entry. Persistent: blocks on the request semaphore, runs one fetch,
// publishes the result, repeats. Lives on core 0 (loopTask/LVGL are on core 1).
void HAPanel::history_task_trampoline_(void *param) {
  auto *self = static_cast<HAPanel *>(param);
  for (;;) {
    xSemaphoreTake(self->hist_req_sem_, portMAX_DELAY);
    bool ok = self->run_history_fetch_();
    // Release so the matching acquire in poll_history_fetch_() sees hist_staging_.
    self->hist_fetch_state_.store(ok ? HIST_DONE_OK : HIST_DONE_FAIL,
                                  std::memory_order_release);
  }
}

bool HAPanel::iso_to_epoch_(const char *s, int64_t *out) {
  int y, mo, d, h, mi, se;
  if (s == nullptr ||
      sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6)
    return false;
  // days_from_civil (Howard Hinnant) → days since 1970-01-01, UTC.
  int yy = y - (mo <= 2 ? 1 : 0);
  int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  int64_t yoe = yy - era * 400;
  int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;
  *out = days * 86400 + h * 3600 + mi * 60 + se;
  return true;
}

bool HAPanel::run_history_fetch_() {
  // WORKER THREAD (core 0). Reads only hist_req_url_ / hist_req_is_binary_ and
  // the immutable http client + token; writes only hist_staging_. No lv_* calls
  // and no entity-vector access — that's what makes running off the main loop
  // safe (LVGL is single-threaded). Result handed back via hist_fetch_state_.
  const std::string &url = this->hist_req_url_;
  const bool is_binary = this->hist_req_is_binary_;

  std::vector<http_request::Header> headers;
  http_request::Header auth;
  auth.name = "Authorization";
  auth.value = std::string("Bearer ") + this->history_token_;
  headers.push_back(auth);

  ESP_LOGI(TAG, "history GET %s", url.c_str());
  auto container = this->history_http_->get(url, headers);
  if (container == nullptr) {
    ESP_LOGW(TAG, "history: null response container");
    return false;
  }
  if (!http_request::is_success(container->status_code)) {
    ESP_LOGW(TAG, "history: HTTP %d", container->status_code);
    container->end();
    return false;
  }

  // Read the body fully into a PSRAM buffer. Internal heap fragments fast under
  // repeated fetches, so a big std::string there would (and did) abort with
  // bad_alloc; PSRAM has megabytes to spare. Bounded so a runaway response
  // falls back to the ring buffer instead of eating RAM.
  static const size_t BODY_CAP = 128u * 1024u;
  RAMAllocator<uint8_t> alloc(RAMAllocator<uint8_t>::ALLOC_EXTERNAL);
  uint8_t *body = alloc.allocate(BODY_CAP);
  if (body == nullptr) {
    ESP_LOGW(TAG, "history: could not allocate %u B body buffer",
             (unsigned) BODY_CAP);
    container->end();
    return false;
  }
  size_t blen = 0;
  uint32_t last = millis();
  bool read_ok = true;
  while (true) {
    int r = container->read(body + blen, BODY_CAP - blen);
    // Yield so core 0's idle task runs while we stream — keeps the task WDT happy
    // even though this task blocks on the socket. (The main loop on core 1 is
    // never blocked, which is the whole point of the worker.)
    yield();
    if (r > 0) {
      blen += (size_t) r;
      last = millis();
      if (blen >= BODY_CAP) {
        ESP_LOGW(TAG, "history: response exceeds %u B cap", (unsigned) BODY_CAP);
        read_ok = false;
        break;
      }
      continue;
    }
    if (r < 0) {
      ESP_LOGW(TAG, "history: read error %d", r);
      read_ok = false;
      break;
    }
    if (container->is_read_complete())
      break;
    if (millis() - last > 8000u) {
      ESP_LOGW(TAG, "history: read timeout");
      read_ok = false;
      break;
    }
    delay(1);
  }
  container->end();
  container.reset();  // free the HTTP/socket buffers before the JSON parse
  if (!read_ok) {
    alloc.deallocate(body, BODY_CAP);
    return false;
  }

  // Response: [ [ {state,last_changed}, ... ] ] — one inner array per entity.
  JsonDocument doc = json::parse_json(body, blen);
  alloc.deallocate(body, BODY_CAP);  // doc copied what it needs (PSRAM-backed)
  if (doc.isNull()) {
    ESP_LOGW(TAG, "history: JSON parse failed (%u B)", (unsigned) blen);
    return false;
  }
  JsonArray outer = doc.as<JsonArray>();
  if (outer.isNull() || outer.size() == 0) {
    ESP_LOGW(TAG, "history: empty result");
    return false;
  }
  JsonArray series = outer[0].as<JsonArray>();
  if (series.isNull())
    return false;

  // Decimate at ingestion. A 24 h window of a per-minute sensor is ~1440 points;
  // the staging vector lives on internal heap, and a vector that big forced a
  // contiguous realloc that abort()ed under fragmentation. The chart only draws
  // ~100 points anyway, so cap here. Keep every stride-th point (stride≈1 for
  // the small binary transition series, so no transitions are dropped).
  const size_t n = series.size();
  // ceil division so the kept count never exceeds SAMPLE_CAP.
  const size_t stride = n > SAMPLE_CAP ? (n + SAMPLE_CAP - 1) / SAMPLE_CAP : 1;
  this->hist_staging_.clear();
  this->hist_staging_.reserve((n > SAMPLE_CAP ? SAMPLE_CAP : n) + 4);
  size_t i = 0;
  for (JsonObject pt : series) {
    bool keep = (i % stride) == 0;
    i++;
    if (!keep)
      continue;
    const char *state = pt["state"] | "";
    if (state[0] == '\0')
      continue;
    float v;
    if (is_binary) {
      if (strcmp(state, "on") == 0)
        v = 1.0f;
      else if (strcmp(state, "off") == 0)
        v = 0.0f;
      else
        continue;
    } else {
      char *end = nullptr;
      v = strtof(state, &end);
      if (end == state)
        continue;
    }
    const char *ts = pt["last_changed"] | "";
    if (ts[0] == '\0')
      ts = pt["last_updated"] | "";
    int64_t ep;
    if (!HAPanel::iso_to_epoch_(ts, &ep))
      continue;
    // Store UTC epoch-seconds directly; redraw_history_ uses the live HA clock
    // as "now" in REST mode, so ages/spans are correct regardless of uptime.
    this->hist_staging_.push_back({(uint32_t) ep, v});
  }
  ESP_LOGI(TAG, "history: %u points (%u B)",
           (unsigned) this->hist_staging_.size(), (unsigned) blen);
  return !this->hist_staging_.empty();
}

}  // namespace ha_panel
}  // namespace esphome
