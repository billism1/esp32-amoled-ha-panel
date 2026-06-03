// ha_panel.h — runtime model + state subscription + LVGL UI tree.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

// UE6: history backfill runs on a pinned FreeRTOS worker task.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esphome/core/component.h"
#include "esphome/core/string_ref.h"
#include "esphome/components/api/custom_api_device.h"
#include "esphome/components/font/font.h"
#include "esphome/components/http_request/http_request.h"
#include "esphome/components/time/real_time_clock.h"
#ifdef USE_SPEAKER
#include "esphome/components/speaker/speaker.h"
#endif

#include "lvgl.h"

namespace esphome {
namespace ha_panel {

// P7c: how an entity row paints itself + how a short-tap dispatches. Picked
// once at codegen time from the entity_id domain. A new HA domain that we
// don't recognise falls into READ_ONLY_TEXT — safe default, no crash.
// E8: per-entity render size. Picked at codegen from the YAML `size:` enum.
// SMALL is the historical fixed 60 px row — zero visual change for configs
// that omit `size:`. MEDIUM/LARGE scale the whole row (height, name font, icon
// glyph, right-side widget) via the row-metrics lookup in ha_panel.cpp.
enum class EntitySize : uint8_t {
  SMALL,
  MEDIUM,
  LARGE,
};

enum class RenderClass : uint8_t {
  BINARY_SWITCH,    // light/switch/fan/input_boolean → lv_switch indicator
  ACTION_ICON,      // scene/script/automation/button → LV_SYMBOL_PLAY badge
  LOCK_TEXT,        // lock → glyph + Locked/Unlocked text
  COVER_TEXT,       // cover → chevron + Open/Closed text
  SUMMARY_TEXT,     // climate/media_player/number/select → state summary (P7d adds modal)
  READ_ONLY_TEXT,   // sensor/binary_sensor/everything else → text badge
  // UE12: a synthetic "report" row whose right-side label holds a computed
  // aggregate (count / min / max / …) over the OTHER entities, not one HA
  // state. No HA subscription; painted by recompute_reports_, inert on tap.
  REPORT_TEXT,
};

// UE12: which aggregate a REPORT_TEXT row computes over the filtered entities.
// Mirrors the `type:` enum validated in __init__.py (REPORT_TYPES).
enum class ReportType : uint8_t {
  COUNT,    // N entities matching {domains, match_state}; show_total → "N / M"
  BOOLEAN,  // count collapsed to a flag: 0 → green check, else amber "N"
  OFFLINE,  // N matched entities that are unavailable / unknown
  SUM,      // sum of numeric states
  AVG,      // mean of numeric states
  MIN,      // smallest numeric state (show_source → prepend its friendly_name)
  MAX,      // largest numeric state
};

// UE12: which entities a report aggregates over. Mirrors REPORT_SCOPES in
// __init__.py.
enum class ReportScope : uint8_t {
  ALL,   // every entity on the panel, across all pages (default)
  PAGE,  // only entities on the same page as the report row
};

// UE12: the parsed `report:` spec carried by a synthetic report Entity. Empty
// `domains` = any domain; empty `match_state` = any state. `device_class` is
// matched against the entity's subscribed attrs — inert until UE7 subscribes
// it, so v1 reports filter on domains + match_state.
struct ReportSpec {
  ReportType type{ReportType::COUNT};
  ReportScope scope{ReportScope::ALL};
  std::vector<std::string> domains;
  std::vector<std::string> match_state;
  std::string device_class;
  std::string unit;          // suffix appended to numeric results ("°", "W", …)
  bool show_total{false};    // COUNT: render "matched / in-scope"
  bool show_source{false};   // MIN/MAX: prepend the extreme entity's name
};

// E9: one captured value in a read-only entity's history series. t_s is a
// timestamp in *seconds* whose origin depends on the source: uptime-seconds
// (millis()/1000) for ring-buffer samples, UTC epoch-seconds for REST-backfilled
// samples. redraw_history_ picks a matching "now" per mode, so only the relative
// (now - t_s) is ever used. value is the numeric reading, or 1/0 for binary.
struct HistorySample {
  uint32_t t_s;
  float value;
};

// UE13: per-row text-style bit flags (Entity::name_style), applied to the row's
// name/title label. Bold + italic swap to a baked Montserrat variant (LVGL has
// no built-in bold/italic + no synthetic weighting); underline is a runtime
// lv_text_decor (no font needed). bold|italic with no bold-italic font → bold.
static constexpr uint8_t STYLE_BOLD = 1 << 0;
static constexpr uint8_t STYLE_ITALIC = 1 << 1;
static constexpr uint8_t STYLE_UNDERLINE = 1 << 2;

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
  // E7: last non-null brightness seen for a light, as 0-100 %. -1 = never seen.
  // Off lights report `brightness = None`, so this retains the prior on-level
  // to seed the detail slider when the user toggles an off light back on.
  int last_bri_pct{-1};
  // P7e: optional "mdi:foo" override from YAML. Empty = resolve from domain.
  std::string icon_override;
  // P7f: short-tap opens a confirm sheet / detail modal instead of firing the
  // action immediately. Set at codegen; read in the row-click pre-flight.
  bool confirm{false};
  // E8: per-entity render size. Defaults to SMALL (today's look) so existing
  // configs render byte-for-byte unchanged.
  EntitySize size{EntitySize::SMALL};
  // UE7: realtime opt-in. When true the history sheet skips the REST backfill
  // for this entity (no /api/history/period fetch, no spinner) and plots purely
  // from the in-device ring buffer + on_state_ live-tail, defaulting to the
  // short "Live" window. For high-rate (e.g. 1 Hz MQTT) sensors that HA does not
  // record, where a REST fetch is wasted and the live stream is the data source.
  bool realtime{false};
  // UE13: STYLE_* bit flags for the row's name/title label. 0 = plain.
  uint8_t name_style{0};
  // E9: in-device history ring buffer. Only populated for chartable read-only
  // entities (numeric sensor / binary_sensor); empty for everything else. Fed
  // from on_state_ since boot, capped at HISTORY_CAP, wiped on reboot / sleep
  // wake. This is the no-token data source for the history chart sheet (REST
  // backfill is a deferred follow-up).
  std::vector<HistorySample> history;
  // Cached UTF-8 glyph for the icon column. v1 resolution (override → domain
  // default → fallback) never changes at runtime, so resolve once on first
  // render. The HA-`icon` attribute tier (P9 batched sensor) will invalidate
  // this when live icons land.
  mutable std::string icon_resolved_;
  mutable bool icon_cached_{false};
  // UE12: only meaningful when render_class == REPORT_TEXT. Empty/default on
  // every real entity (small fixed overhead — two empty vectors + strings).
  ReportSpec report;
};

// UE11: a page-picker count badge. Each page declares at most one, shown as
// icon+value to the right of the page name in the picker. Computed fresh on
// open over the page's OWN entities only (page-scoped). Mirrors BADGE_TYPES /
// BADGE_AGGS in __init__.py. The config-free types (lights_on … entities) ship
// in v1; the device_class / numeric types (open_doors … aqi) parse + evaluate
// but stay empty until UE7 subscribes device_class (gated, documented).
enum class BadgeType : uint8_t {
  NONE,            // no badge (default)
  LIGHTS_ON,       // light == on
  DEVICES_ON,      // switch / fan / input_boolean == on
  UNLOCKED,        // lock != locked
  OPEN_COVERS,     // cover != closed
  MEDIA_PLAYING,   // media_player == playing
  CLIMATE_ACTIVE,  // climate != off
  RUNNING,         // script / automation == on, timer == active
  OFFLINE,         // unavailable / unknown
  ENTITIES,        // total entities on the page (no state filter)
  OPEN_DOORS,      // binary_sensor door/window/garage_door == on  (needs UE7)
  MOTION,          // binary_sensor motion/occupancy/presence == on (needs UE7)
  LOW_BATTERY,     // battery device_class <= threshold (needs UE7)
  ALARM,           // smoke/moisture/co/gas/problem/safety == on → red dot (UE7)
  TEMPERATURE,     // agg of temperature sensors (needs UE7 device_class)
  HUMIDITY,        // avg of humidity sensors (needs UE7)
  POWER,           // sum of power sensors (needs UE7)
  CO2,             // avg of carbon_dioxide sensors (needs UE7)
  AQI,             // avg of aqi sensors (needs UE7)
  SEVERITY,        // composite: alarm → red, open_doors → amber, offline → amber
  IDLE,            // green check when nothing on / open
};

// UE11: numeric-aggregate selector for the temperature / co2 / aqi badge types.
enum class BadgeAgg : uint8_t { AVG, MIN, MAX, SUM };

struct PickerBadge {
  BadgeType type{BadgeType::NONE};
  BadgeAgg agg{BadgeAgg::AVG};
  int threshold{20};  // LOW_BATTERY: percent at/below which a battery counts
  std::string unit;   // numeric types: suffix override ("°", "%", "W", …)
};

struct Page {
  std::string name;
  std::vector<size_t> entity_indices;  // indexes into HAPanel::entities_
  // UE11: zero or more page-picker badges, shown stacked horizontally to the
  // right of the page name (name stays left-aligned). Empty = no badge.
  std::vector<PickerBadge> badges;
};

class HAPanel : public Component, public api::CustomAPIDevice {
 public:
  void setup() override;
  // UE6: polls the history worker task's completion flag (main thread).
  void loop() override;
  void dump_config() override;
  // Run after LVGL (PROCESSOR) is initialised so lv_scr_act() is valid.
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  // Codegen-time API.
  void add_page(const std::string &name);
  void add_entity(const std::string &entity_id, const std::string &friendly_name,
                  const std::string &icon_override = "", bool confirm = false,
                  EntitySize size = EntitySize::SMALL, bool realtime = false,
                  uint8_t name_style = 0);
  // UE12: append a synthetic report row to the current page. `domains_csv` and
  // `match_state_csv` are comma-joined lists (split with parse_ha_list_); the
  // codegen passes them flat to avoid emitting std::vector initializers.
  void add_report(const std::string &title, const std::string &type,
                  const std::string &domains_csv, const std::string &match_state_csv,
                  const std::string &device_class, const std::string &unit,
                  bool show_total, bool show_source, const std::string &scope,
                  const std::string &icon, EntitySize size, uint8_t name_style = 0);
  // UE11: append a picker badge to the most-recently-added page (call once per
  // badge in the page's `picker_badge:` list). `type` / `agg` are the validated
  // enum strings; threshold is for LOW_BATTERY; unit overrides a numeric badge's
  // suffix. Type "none" is ignored.
  void add_page_badge(const std::string &type, const std::string &agg,
                      int threshold, const std::string &unit);
  // P7e: MDI glyph font for the per-entity icon column. nullptr → icons off,
  // rows fall back to the pre-P7e name-at-left layout.
  void set_mdi_font(font::Font *f) { this->mdi_font_ = f; }
  // E8: larger MDI re-bakes for medium/large rows. nullptr → that size falls
  // back to the base 24 px font (icon just looks relatively smaller).
  void set_mdi_font_medium(font::Font *f) { this->mdi_font_med_ = f; }
  void set_mdi_font_large(font::Font *f) { this->mdi_font_lg_ = f; }
  // UE13: baked name-label font variants. size_idx 0/1/2 = small/medium/large;
  // variant_idx 0 = bold, 1 = italic. nullptr (unwired) → that style falls back
  // to the regular built-in montserrat for the size.
  void set_style_font(uint8_t size_idx, uint8_t variant_idx, font::Font *f) {
    if (size_idx < 3 && variant_idx < 2)
      this->style_fonts_[size_idx][variant_idx] = f;
  }
  // E9: REST history backfill wiring. All four must be set for backfill to run;
  // otherwise the chart falls back to the in-device ring buffer.
  void set_history_http(http_request::HttpRequestComponent *c) { this->history_http_ = c; }
  void set_history_time(time::RealTimeClock *t) { this->history_time_ = t; }
  void set_history_base_url(const std::string &u) { this->history_base_url_ = u; }
  void set_history_token(const std::string &t) { this->history_token_ = t; }

  // Programmatic action (tests / future automations).
  bool tap(size_t page_idx, size_t entity_idx);

  size_t num_pages() const { return this->pages_.size(); }

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
  // UE10: commit the persisted idle-timeout globals (dim / blank-total / sleep,
  // all seconds) on Apply, and seed the settings-sheet sliders from the
  // boot-restored globals. Same committer/seeder split as sleep.
  void set_timeouts_committer(std::function<void(uint16_t, uint16_t, uint16_t)> committer) {
    this->timeouts_committer_ = std::move(committer);
  }
  void set_timeout_settings(uint16_t dim, uint16_t blank_total, uint16_t sleep_s);
  // UE10: commit + seed the screen-reset-source toggles (touch / motion).
  void set_reset_sources_committer(std::function<void(bool, bool)> committer) {
    this->reset_sources_committer_ = std::move(committer);
  }
  void set_reset_sources(bool touch, bool motion);
  // UE10: per-board AMOLED flag. true → the settings sheet warns about burn-in
  // when all screen protection is disabled. Default false (safe for LCD/IPS).
  void set_is_amoled(bool amoled) { this->is_amoled_ = amoled; }
  void set_clock_text(const std::string &text);
  void set_api_connected(bool connected);
  // E2: Wi-Fi link state, fed by wifi on_connect/on_disconnect. ESPHome
  // auto-reconnects after a drop, so "disconnected" is rendered as "connecting".
  void set_wifi_connected(bool connected);
  // P7b: header status icons.
  void set_wifi_rssi(int rssi);
  void set_battery_voltage(float volts);

  // Touch click sound (settings-gated). speaker plays a short synthesized
  // click; committer persists the toggle global; set_sound_on_press seeds the
  // settings switch from the boot-restored value.
#ifdef USE_SPEAKER
  void set_speaker(speaker::Speaker *spk) { this->speaker_ = spk; }
#endif
  // Portability hook (Option C): override the click backend. If set, this is
  // called instead of the built-in speaker — wire it per board to a buzzer
  // (rtttl.play), a haptic pulse, or a different codec. ha_panel only decides
  // *when* to click (a tap, not a swipe); the board decides *how* it sounds.
  void set_click_action(std::function<void()> action) {
    this->click_action_ = std::move(action);
  }
  void set_sound_committer(std::function<void(bool)> committer) {
    this->sound_committer_ = std::move(committer);
  }
  void set_sound_on_press(bool on);

  // Touch gesture feed (from the board's touchscreen triggers). A press is a
  // "tap" only if the finger barely moved before release; a swipe/scroll moves
  // past the threshold and plays no click. touch_pressed marks the start,
  // touch_moved flags movement, touch_released decides + plays.
  void touch_pressed(int16_t x, int16_t y);
  void touch_moved(int16_t x, int16_t y);
  void touch_released();

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
  // E1: step the active page by ±1 with wrap-around (bottom-bar arrows).
  void step_page_(int delta);
  // E6: jump to a page by index (tile-set + header-update tail shared by
  // step_page_ and the bottom-bar Home button).
  void go_to_page_(size_t page_idx);
  // P7c: dispatches on Entity::render_class to update the right child widget.
  void rebuild_entity_row_(size_t entity_idx);
  // UE13: resolve the bold/italic baked font for a row's name label given its
  // style flags + size; returns nullptr to use the regular built-in (the
  // RowMetrics name_font). bold+italic with no bold-italic font → bold.
  const lv_font_t *resolve_name_font_(const Entity &e) const;
  // UE12: recompute every REPORT_TEXT row's value+colour and repaint its label.
  // Called once after build_ui_ and again from on_state_ on any state change.
  void recompute_reports_();
  // UE12: evaluate one report spec → display string + text colour. When
  // scope_indices is non-null the scan is limited to those entity indices
  // (page scope); null scans every entity (all-pages scope).
  void compute_report_(const ReportSpec &spec, const std::vector<size_t> *scope_indices,
                       std::string *out_text, uint32_t *out_color);
  // UE11: recompute every page's picker badge from current state and repaint
  // (icon + value + colour, hidden on 0/empty). Called from open_picker_ — the
  // picker is a transient modal, so there's no per-frame / on_state_ wiring.
  void update_picker_badges_();
  // UE11: evaluate one badge spec over a page's OWN entities (page-scoped).
  // Returns false → hide (nothing in scope to monitor); otherwise fills the mdi
  // icon name (empty = value only), the value string (empty = icon-only dot),
  // and the text colour — a DIM grey when monitored-but-quiet (the badge stays
  // visible to signal "watching this"), the semantic colour when notable.
  bool eval_picker_badge_(size_t page_idx, const PickerBadge &spec,
                          std::string *icon, std::string *value, uint32_t *color);
  void open_picker_();
  void close_picker_();
  void update_status_dot_();
  void update_splash_status_();
  // E5: a splash init-stage's indicator handles — an amber dot that blinks
  // while the stage is in progress, swapped for a green check when it is done.
  struct SplashStage {
    lv_obj_t *dot{nullptr};      // amber dim dot shown while the stage is queued
    lv_obj_t *spinner{nullptr};  // UE2: animated spinner shown while stage active
    lv_obj_t *check{nullptr};  // green LV_SYMBOL_OK shown when stage done
  };
  // E5: build one splash init-stage row (phrase + amber dot + hidden green
  // check) at vertical offset y; returns the stage's indicator handles.
  SplashStage build_splash_stage_(lv_obj_t *parent, const char *text, lv_coord_t y);
  // E5: drive a stage's indicator — green check when done, else amber dot
  // (blinking when active, dim-steady when not yet reached).
  void update_splash_stage_(const SplashStage &st, bool done, bool active);
  void update_wifi_icon_();
  void update_battery_icon_();
  // E2: start/stop the shared blink timer based on whether any indicator is in
  // a pending (amber) state. Called whenever wifi/api connection state changes.
  void update_blink_timer_();
  static void blink_timer_cb_(lv_timer_t *t);
  // Fires once ~7 s after boot. If the splash stages still haven't passed (api
  // not connected), show the "taking longer than expected" popup and drop the
  // splash so the app loads while reconnection keeps running in the background.
  static void splash_timeout_cb_(lv_timer_t *t);
  void show_splash_timeout_popup_();
  void dismiss_splash_timeout_popup_();
  static void on_splash_timeout_dismiss_(lv_event_t *e);
  bool is_settings_active_() const;
  // P7b: stage / commit / revert brightness slider edits.
  void apply_brightness_();
  void revert_brightness_();
  // P8: stage / commit / revert sleep toggle + mode dropdown edits. Same
  // dirty-flag + revert-on-navigate-away recipe as brightness.
  void apply_sleep_();
  void revert_sleep_();
  // Grey out the mode dropdown when the master toggle is off (mode irrelevant).
  // UE10: also greys the sleep-timeout slider (same trigger).
  void update_sleep_mode_enabled_();
  // UE10: stage / commit / revert the three idle-timeout sliders + the two
  // reset-source switches (same dirty-flag recipe as sleep). apply_timeouts_
  // clamps the dim ≤ blank ≤ sleep ordering before committing.
  void apply_timeouts_();
  void revert_timeouts_();
  void apply_reset_sources_();
  void revert_reset_sources_();
  // UE10: refresh a timeout slider's seconds label ("Never" at 0). idx selects
  // 0=dim, 1=blank, 2=sleep.
  void update_timeout_label_(uint8_t idx);
  // UE10: grey the dim + blank sliders when neither reset source is staged on —
  // dim/blank are suppressed in that case (an unrecoverable screen otherwise).
  void update_timeouts_enabled_();
  // UE10: true when the staged settings leave NO screen protection (dim off,
  // blank off, sleep off) — the burn-in-warning trigger on an AMOLED board.
  bool staged_protection_all_off_() const;
  // UE10: run every apply_* and close the settings sheet. Called directly on
  // Apply, or after the user accepts the burn-in warning.
  void commit_settings_();
  // UE10: raise the burn-in confirm overlay (reuses confirm_sheet_) before
  // committing an all-protection-off config on an AMOLED board.
  void open_burnin_warning_();
  // Touch click sound: stage / commit / revert the toggle (same dirty-flag
  // recipe as sleep). play_click_ pushes the precomputed PCM to the speaker.
  void apply_sound_();
  void revert_sound_();
  // play_click_ = the gate + backend dispatch (override hook, else built-in
  // speaker). play_speaker_click_ = the built-in i2s-speaker backend.
  void play_click_();
  void play_speaker_click_();
  // Play a brief silence to spend the codec's un-mute ramp up front, so the
  // first audible click is at steady gain like every later one (no uniquely
  // soft first press). Called when the sound setting turns on.
  void warm_up_speaker_();

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

  // E9 read-only history chart sheet. Built once at setup, hidden; same overlay
  // recipe as detail_modal_ / settings_sheet_. open_history_ snapshots the
  // entity, redraw_history_ rebuilds the chart/timeline for the current window.
  void build_history_sheet_(lv_obj_t *scr);
  void open_history_(size_t entity_idx);
  void close_history_();
  void redraw_history_();
  // UE7: roll-mode draw for the Live window — newest sample pinned to the right
  // at a fixed X scale, blank slots padding the left until the trace fills.
  void redraw_live_roll_(const Entity &e, uint32_t now);
  // UE4: point the analog gauge's needle at `current`, scaling its range from the
  // window's [vmin,vmax] (padded so the needle isn't pinned) and revealing it.
  void update_history_gauge_(float vmin, float vmax, float current);
  // E9: append the current state to an entity's history ring buffer. No-op for
  // non-chartable entities. Called from on_state_ on every state change.
  void record_history_(size_t entity_idx);
  // E9: chartable = numeric sensor (state parses as a number) or binary_sensor.
  // Other read-only rows (text sensors) stay inert on tap.
  static bool is_chartable_(const Entity &e);
  // E9: parse a state string into a chartable value. Returns false for
  // non-numeric / unavailable / unknown so the sample is skipped. binary_sensor
  // maps on→1, off→0; everything else uses strtof.
  static bool state_to_value_(const Entity &e, float *out);
  // E9: true when all four REST-backfill dependencies are wired.
  bool history_rest_enabled_() const;
  // UE6: begin loading the open sheet's samples for `window_idx`. REST path is
  // async — shows the spinner and hands a request to the worker task (redraw
  // deferred to poll_history_fetch_); the no-REST path copies the ring buffer
  // and redraws synchronously. Replaces the old blocking load_history_samples_.
  void start_history_load_(size_t entity_idx, uint8_t window_idx);
  // UE6: paint the loading state (spinner up, chart/strip/gauge hidden).
  void show_history_loading_();
  // UE6: lazily create the worker task (+ semaphore) on first use, NOT at boot —
  // its internal-RAM stack must not starve the WiFi scan allocator during the
  // memory-tight boot/connect window. Returns false if creation fails (low heap).
  bool ensure_history_worker_();
  // UE6: synchronous fallback when the worker can't be created — runs the fetch
  // on the main loop (blocks; WDT backstop covers it), same as the pre-task path.
  void run_history_fetch_sync_();
  // UE6: (main) dispatch the latest desired request to the worker, or defer if a
  // fetch is already in flight (poll re-dispatches on completion).
  void dispatch_history_fetch_();
  // UE6: (main, from loop()) consume a finished fetch — swap staging in / fall
  // back to the ring buffer, redraw if still relevant, re-dispatch if superseded.
  void poll_history_fetch_();
  // UE6: (main) build the /api/history/period URL for the window. False if the
  // HA clock isn't valid yet (caller falls back to the ring buffer).
  bool build_history_url_(size_t entity_idx, uint8_t window_idx, std::string *out);
  // UE6: (WORKER THREAD) GET hist_req_url_ + parse into hist_staging_. Touches no
  // lv_* and no entity state — only the socket, a PSRAM buffer, and hist_staging_.
  bool run_history_fetch_();
  // UE6: worker task entry — waits on hist_req_sem_, runs run_history_fetch_,
  // publishes the result via hist_fetch_state_ (release).
  static void history_task_trampoline_(void *param);
  // E9: parse a leading ISO-8601 "YYYY-MM-DDTHH:MM:SS" into a UTC epoch.
  static bool iso_to_epoch_(const char *s, int64_t *out);

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
  static void on_nav_left_(lv_event_t *e);
  static void on_nav_right_(lv_event_t *e);
  static void on_gear_clicked_(lv_event_t *e);
  // E6: bottom-bar Home button → jump to the first page.
  static void on_home_clicked_(lv_event_t *e);
  // P8 sleep controls.
  static void on_sleep_switch_(lv_event_t *e);
  static void on_sleep_mode_dropdown_(lv_event_t *e);
  // UE10 settings controls.
  static void on_timeout_slider_(lv_event_t *e);
  static void on_reset_touch_switch_(lv_event_t *e);
  static void on_reset_motion_switch_(lv_event_t *e);
  static void on_burnin_proceed_(lv_event_t *e);
  // Touch click sound toggle.
  static void on_sound_switch_(lv_event_t *e);
  static void on_detail_apply_clicked_(lv_event_t *e);
  static void on_detail_cancel_clicked_(lv_event_t *e);
  static void on_detail_bg_clicked_(lv_event_t *e);
  // E7: power switch in the light modal — toggling on reveals/enables the
  // brightness slider (seeded from last-known), toggling off greys it.
  static void on_detail_light_switch_(lv_event_t *e);
  // Live label updates as detail-modal sliders move.
  static void on_detail_brightness_slider_(lv_event_t *e);
  static void on_detail_ct_slider_(lv_event_t *e);
  static void on_detail_temp_slider_(lv_event_t *e);
  static void on_detail_temp_low_slider_(lv_event_t *e);
  static void on_detail_temp_high_slider_(lv_event_t *e);
  static void on_detail_hvac_mode_changed_(lv_event_t *e);
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
  // E9 history-sheet trampolines. close/bg dismiss; chip selects the window
  // (1h/6h/24h) from the chip's user_data and redraws.
  static void on_history_close_(lv_event_t *e);
  static void on_history_chip_(lv_event_t *e);

  std::vector<Page> pages_;
  std::vector<Entity> entities_;

  // LVGL refs. nullptr until build_ui_ runs.
  lv_obj_t *header_label_{nullptr};
  lv_obj_t *clock_label_{nullptr};
  lv_obj_t *status_dot_{nullptr};
  // Shown in place of status_dot_ while the HA link is being established (wifi
  // up, api down) — same style as the boot-splash spinner. Replaces the old
  // amber blink.
  lv_obj_t *status_spinner_{nullptr};
  lv_obj_t *wifi_icon_{nullptr};
  lv_obj_t *battery_icon_{nullptr};
  lv_obj_t *tileview_{nullptr};
  lv_obj_t *picker_{nullptr};
  // UE11: per-page picker badge groups [page_idx][badge_idx]. Each group is a
  // flex row holding an mdi icon label (child 0) + a montserrat value label
  // (child 1); a group hides itself when its value is 0/empty (and collapses out
  // of the right-aligned bar's flex layout). Repainted by update_picker_badges_.
  std::vector<std::vector<lv_obj_t *>> picker_badges_;
  lv_obj_t *splash_{nullptr};
  // E5: splash shows one row per init stage (Wi-Fi, then HA API). Each row is a
  // phrase label with a status indicator to its right (after the "..."): an
  // amber dot that blinks while that stage is in progress, swapped for a green
  // checkmark once the stage completes. Built once; update_splash_status_
  // drives the states off wifi_connected_ / api_connected_. Add more stages by
  // adding SplashStage members + build/update calls.
  SplashStage splash_wifi_stage_{};
  SplashStage splash_ha_stage_{};
  // Popup raised by splash_timeout_cb_ when the splash stages stall past ~7 s,
  // explaining the slow stage while the app loads anyway. splash_timeout_label_
  // holds the stage-specific message; the timer is one-shot, nulled after it
  // fires or once the API connects first.
  lv_obj_t *splash_timeout_popup_{nullptr};
  lv_obj_t *splash_timeout_label_{nullptr};
  lv_timer_t *splash_timeout_timer_{nullptr};
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
  // UE10 settings-sheet idle-timeout sliders + their live seconds labels.
  lv_obj_t *dim_timeout_slider_{nullptr};
  lv_obj_t *dim_timeout_label_{nullptr};
  lv_obj_t *blank_timeout_slider_{nullptr};
  lv_obj_t *blank_timeout_label_{nullptr};
  lv_obj_t *sleep_timeout_slider_{nullptr};
  lv_obj_t *sleep_timeout_label_{nullptr};
  // UE10 reset-source switches.
  lv_obj_t *reset_touch_switch_{nullptr};
  lv_obj_t *reset_motion_switch_{nullptr};
  std::vector<lv_obj_t *> tile_objs_;            // [page_idx]
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
  // UE5: glowing status LED for binary_sensor rows (right slot). nullptr for
  // every other entity. Colour + brightness driven by rebuild_entity_row_.
  std::vector<lv_obj_t *> leds_by_entity_;

  // P7e: MDI glyph font for the icon column. nullptr → icons disabled.
  font::Font *mdi_font_{nullptr};
  // E8: re-baked MDI fonts for medium (36 px) / large (48 px) rows. nullptr →
  // fall back to mdi_font_ for that size.
  font::Font *mdi_font_med_{nullptr};
  font::Font *mdi_font_lg_{nullptr};
  // UE13: baked name-label style fonts, [size_idx][variant_idx] where size is
  // 0/1/2 = small/medium/large and variant is 0 = bold, 1 = italic. nullptr =
  // unwired → fall back to the regular built-in for that size.
  font::Font *style_fonts_[3][2]{};

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
  // UE10 idle timeouts (seconds). *_committed mirror the persisted globals;
  // staged_* track the live sliders; timeouts_dirty_ flips on edit. blank is
  // stored as TOTAL no-input time (not additive on dim). Defaults match the
  // YAML substitution seeds so a fresh boot is unchanged until the user edits.
  std::function<void(uint16_t, uint16_t, uint16_t)> timeouts_committer_;
  uint16_t dim_timeout_{15};
  uint16_t blank_timeout_{45};
  uint16_t sleep_timeout_{60};
  uint16_t staged_dim_timeout_{15};
  uint16_t staged_blank_timeout_{45};
  uint16_t staged_sleep_timeout_{60};
  bool timeouts_dirty_{false};
  // UE10 screen-reset sources (touch / motion), persisted + staged like sleep.
  std::function<void(bool, bool)> reset_sources_committer_;
  bool reset_on_touch_{true};
  bool reset_on_motion_{true};
  bool staged_reset_on_touch_{true};
  bool staged_reset_on_motion_{true};
  bool reset_sources_dirty_{false};
  // UE10 per-board AMOLED flag (set from the board's ${is_amoled} substitution).
  // Gates the burn-in warning. Default false = no warning (safe for LCD/IPS).
  bool is_amoled_{false};
  // Touch click sound. sound_on_press_ mirrors the persisted global;
  // staged_* tracks the switch; sound_dirty_ flips on edit, cleared on
  // Apply / revert. Default off.
  lv_obj_t *sound_switch_{nullptr};
  std::function<void(bool)> sound_committer_;
  bool sound_on_press_{false};
  bool staged_sound_on_press_{false};
  bool sound_dirty_{false};
  // Option C: optional board-supplied click backend. nullptr → use built-in
  // speaker (if any). Set via set_click_action.
  std::function<void()> click_action_;
#ifdef USE_SPEAKER
  // Speaker + precomputed click PCM (16-bit mono @ 16 kHz), built once in
  // setup() so a tap just re-queues the same buffer.
  speaker::Speaker *speaker_{nullptr};
  std::vector<uint8_t> click_pcm_;
#endif
  // Tap-vs-swipe tracking. touch_active_ guards a press in progress;
  // touch_moved_ trips once the finger leaves a small radius of the start.
  bool touch_active_{false};
  bool touch_moved_{false};
  int16_t touch_start_x_{0};
  int16_t touch_start_y_{0};
  // active_brightness_ = persisted (last committed) value, mirrors the YAML
  // global. staged_brightness_ = current slider position (live preview).
  // brightness_dirty_ flips true on first slider drag, false on Apply/Cancel.
  uint8_t active_brightness_{0xD0};
  uint8_t staged_brightness_{0xD0};
  bool brightness_dirty_{false};
  bool api_connected_{false};
  // E2: Wi-Fi link state. Starts false — boot shows the amber "connecting"
  // indicator until wifi on_connect fires.
  bool wifi_connected_{false};
  // E2: shared 500 ms blink. blink_on_ toggles each tick; pending (amber)
  // indicators read it for their lit/dim phase. Timer is created only while at
  // least one indicator is pending and deleted when all states are stable.
  bool blink_on_{true};
  lv_timer_t *blink_timer_{nullptr};
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
  // E7: true once the brightness slider holds a real target (a cached HA
  // value, or a value the user set by enabling the slider). When false the
  // slider is a disabled placeholder and apply_detail_ must NOT send
  // brightness_pct — otherwise an untouched placeholder would push 0/100 %.
  bool dw_brightness_known_{false};
  lv_obj_t *dw_ct_slider_{nullptr};
  lv_obj_t *dw_ct_label_{nullptr};
  lv_obj_t *dw_temp_slider_{nullptr};   // climate single setpoint arc (heat/cool)
  lv_obj_t *dw_temp_label_{nullptr};
  // UE3: dual-setpoint dials for auto/heat_cool (low = heat point, high = cool
  // point). single_box xor dual_box is shown per selected mode; the hidden one
  // takes no flex layout space. All three arcs share dw_temp_step_ scaling.
  lv_obj_t *dw_temp_single_box_{nullptr};
  lv_obj_t *dw_temp_dual_box_{nullptr};
  lv_obj_t *dw_temp_low_slider_{nullptr};
  lv_obj_t *dw_temp_low_label_{nullptr};
  lv_obj_t *dw_temp_high_slider_{nullptr};
  lv_obj_t *dw_temp_high_label_{nullptr};
  float dw_temp_step_{0.1f};
  lv_obj_t *dw_hvac_dropdown_{nullptr};
  std::vector<std::string> dw_hvac_modes_;
  lv_obj_t *dw_volume_slider_{nullptr};  // media volume arc (UE3), 0-100
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

  // E9 history chart sheet. Built once, hidden. Numeric sensors render in
  // history_chart_ (line); binary_sensors in history_strip_ (on/off bands) —
  // exactly one is visible per open. history_window_idx_ selects 1h/6h/24h/Live.
  lv_obj_t *history_sheet_{nullptr};
  lv_obj_t *history_title_{nullptr};
  lv_obj_t *history_value_{nullptr};
  lv_obj_t *history_chart_{nullptr};
  lv_chart_series_t *history_series_{nullptr};
  lv_obj_t *history_strip_{nullptr};
  // UE4: analog "now" gauge for numeric sensors — an lv_scale (round) + line
  // needle, top-right above the chart. Same value the big label shows; range
  // tracks the window's padded min/max. Hidden for binary_sensor and no-data.
  lv_obj_t *history_gauge_{nullptr};
  lv_obj_t *history_gauge_needle_{nullptr};
  // Loading indicator over the chart during the REST backfill. UE6 made this a
  // real lv_spinner: the fetch now runs on a worker task, so the main loop keeps
  // ticking lv_timer_handler and the spinner self-animates (the old UE2 hand-
  // rotated arc was a workaround for the loop being blocked, now retired).
  lv_obj_t *history_spinner_{nullptr};
  // Bottom row under the chart: left/right are the time span of the *displayed
  // data* (oldest visible sample's age → "now"), so a window with no older data
  // reads honestly instead of looking identical at every chip. Center shows the
  // numeric value range (min–max); empty for binary_sensor.
  lv_obj_t *history_time_left_{nullptr};
  lv_obj_t *history_time_right_{nullptr};
  lv_obj_t *history_range_label_{nullptr};
  lv_obj_t *history_chips_[4]{nullptr, nullptr, nullptr, nullptr};
  size_t history_entity_idx_{0};
  bool history_open_{false};
  uint8_t history_window_idx_{0};  // 0 = 1h, 1 = 6h, 2 = 24h, 3 = Live
  // E9: working sample set for the open sheet — REST-backfilled points or a copy
  // of the entity's ring buffer. redraw_history_ reads this; the live tail
  // appends to it. history_rest_mode_ gates whether a window change re-fetches.
  std::vector<HistorySample> history_samples_;
  bool history_rest_mode_{false};

  // E9 REST backfill dependencies (all four required; unset → ring-buffer only).
  http_request::HttpRequestComponent *history_http_{nullptr};
  time::RealTimeClock *history_time_{nullptr};
  std::string history_base_url_;
  std::string history_token_;

  // UE6: history fetch worker. The blocking GET + JSON parse run here (core 0)
  // so they never stall loopTask/LVGL (core 1) — the stall tripped the task WDT
  // on 6h/24h windows. Handoff: main fills the request fields + gives
  // hist_req_sem_; worker fills hist_staging_ and stores hist_fetch_state_ with
  // release; loop() loads it with acquire (the barrier), then swaps staging in.
  // The worker touches ONLY hist_req_url_ / hist_req_is_binary_ / hist_staging_
  // and the http client — never lv_* or the entity vector.
  enum : uint8_t { HIST_IDLE = 0, HIST_RUNNING, HIST_DONE_OK, HIST_DONE_FAIL };
  TaskHandle_t hist_task_{nullptr};
  SemaphoreHandle_t hist_req_sem_{nullptr};
  std::atomic<uint8_t> hist_fetch_state_{HIST_IDLE};
  // Request (main → worker): written only when no fetch is in flight.
  std::string hist_req_url_;
  bool hist_req_is_binary_{false};
  uint32_t hist_req_seq_{0};  // seq of the dispatched (in-flight) request
  // Result (worker → main): valid only after HIST_DONE_OK.
  std::vector<HistorySample> hist_staging_;
  // Latest desired request (main-only). Bumped per user action; supersedes an
  // in-flight fetch on rapid window switches so only the newest result is drawn.
  uint32_t hist_seq_want_{0};
  size_t hist_want_entity_{0};
  uint8_t hist_want_window_{0};
  bool hist_want_pending_{false};  // a desired fetch not yet handed to the worker
};

}  // namespace ha_panel
}  // namespace esphome
