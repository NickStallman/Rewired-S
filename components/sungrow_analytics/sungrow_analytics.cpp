#include "sungrow_analytics.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/build_info_data.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#ifdef USE_ESP32
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <sdkconfig.h>
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_SUNGROW_DIAG
#include "../sungrow_board/diag.h"
#include "../sungrow_board/diag_journal.h"
#include "../sungrow_board/sungrow_board.h"
#endif
#ifdef USE_SUNGROW_NET_FALLBACK
#include "../sungrow_board/sungrow_board.h"
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CONTROL
#include "../sungrow_control/dsp_ranges.gen.h"
#endif

namespace esphome {
namespace sungrow_analytics {

static const char *const TAG = "analytics";

using sungrow_modbus::Device;
using sungrow_modbus::ModbusMaster;
namespace dm = sungrow_datamodel;

#ifndef ESPHOME_PROJECT_VERSION
#define ESPHOME_PROJECT_VERSION ""
#endif
/// The version the dongle presents to iSolarCloud, as the board resolves it (YAML, detected
/// from the stock slot, or the web UI override; cloud_version.h).
static const char *stock_version() {
  return sungrow_board::global_board != nullptr ? sungrow_board::global_board->firmware_version() : "";
}

static const char *board_name() {
#ifdef USE_WINET_S2
  return "winet_s2";
#else
  return "winet_s";
#endif
}

static const char *chip_name() {
#ifdef CONFIG_IDF_TARGET
  return CONFIG_IDF_TARGET;
#else
  return "host";
#endif
}

static const char *network_name() {
#if defined(USE_ETHERNET)
  return "ethernet";
#elif defined(USE_WIFI)
  return "wifi";
#else
  return "none";
#endif
}

static const char *child_kind_name(sungrow_modbus::ChildKind k) {
  switch (k) {
    case sungrow_modbus::ChildKind::SBR:
      return "SBR";
    case sungrow_modbus::ChildKind::CHARGER:
      return "charger";
    case sungrow_modbus::ChildKind::ENERGY_BRIDGE:
      return "energy_bridge";
    default:
      return "none";
  }
}

static uint32_t internal_heap_free() {
#ifdef USE_ESP32
  return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#else
  return 0;
#endif
}

static uint32_t internal_heap_min() {
#ifdef USE_ESP32
  return static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
#else
  return 0;
#endif
}

static uint32_t psram_total() {
#ifdef USE_ESP32
  return static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
#else
  return 0;
#endif
}

static uint32_t psram_free() {
#ifdef USE_ESP32
  return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#else
  return 0;
#endif
}

static int reset_reason_value() {
#ifdef USE_ESP32
  return static_cast<int>(esp_reset_reason());
#else
  return 0;
#endif
}

// ---- setup -----------------------------------------------------------------------------------------

void SungrowAnalytics::setup() {
  this->pref_ = global_preferences->make_preference<Pref>(fnv1_hash("sungrow_analytics"), true);
  if (!this->pref_.load(&this->pref_data_) || this->pref_data_.magic != PREF_MAGIC) {
    this->pref_data_ = Pref{};
    this->pref_data_.magic = PREF_MAGIC;
    random_bytes(this->pref_data_.fallback_id, sizeof(this->pref_data_.fallback_id));
    this->save_pref_();
  }
  this->cap_.window_start_epoch = this->pref_data_.window_start_epoch;
  this->cap_.uploads = this->pref_data_.uploads;

  uint32_t seed = 0;
  random_bytes(reinterpret_cast<uint8_t *>(&seed), sizeof(seed));
  this->schedule_.arm(seed, millis());

#ifdef USE_SUNGROW_DIAG
  {
    using namespace sungrow_board;
    const diag::Journal *prev = diag_previous_journal();
    const int reason = diag_reset_reason();
    this->boot_trigger_ = diag::reset_trigger(reason, prev);
    if (this->boot_trigger_ == nullptr && diag_coredump_reported())
      this->boot_trigger_ = "crash";
    if (prev != nullptr && prev->crash.magic == diag::CRASH_MAGIC)
      this->boot_crash_class_ = diag::crash_class(prev->crash);
    else
      this->boot_crash_class_ = reset_reason_code(reason);
  }
#endif

  if (this->hub_ != nullptr)
    this->hub_->add_on_unsupported_callback([this](const Device &d, const char *why) { this->on_unsupported_(d, why); });

#ifdef SUNGROW_ANALYTICS_BIND_EXTFLASH
  if (this->flash_ != nullptr) {
    this->flash_->add_on_identity_callback([this](const sungrow_extflash::Identity &id) {
      if (id.valid() && !id.sn.empty())
        this->set_distinct_from_sn_(id.sn);
      else
        this->use_fallback_id_();
    });
    return;
  }
#endif
  this->use_fallback_id_();
}

void SungrowAnalytics::use_fallback_id_() {
  if (!this->distinct_id_.empty())
    return;
  this->distinct_id_ = hex_lower(this->pref_data_.fallback_id, sizeof(this->pref_data_.fallback_id));
  ESP_LOGD(TAG, "Install id from the stored random id");
}

void SungrowAnalytics::set_distinct_from_sn_(const std::string &sn) {
  if (!this->distinct_id_.empty())
    return;
  this->dongle_sn_ = sn;
#ifdef USE_ESP32
  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t *>(sn.data()), sn.size());
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  this->distinct_id_ = hex_lower(digest, sizeof(digest));
  ESP_LOGD(TAG, "Install id from the dongle serial");
#else
  this->use_fallback_id_();
#endif
}

bool SungrowAnalytics::device_key_reported_(uint16_t key) const {
  const uint8_t n = std::min<uint8_t>(this->pref_data_.device_keys_n, DEVICE_KEYS_MAX);
  for (uint8_t i = 0; i < n; i++)
    if (this->pref_data_.device_keys[i] == key)
      return true;
  return false;
}

void SungrowAnalytics::remember_device_key_(uint16_t key) {
  if (this->device_key_reported_(key))
    return;
  uint8_t n = std::min<uint8_t>(this->pref_data_.device_keys_n, DEVICE_KEYS_MAX);
  if (n == DEVICE_KEYS_MAX) {  // oldest out
    for (size_t i = 1; i < DEVICE_KEYS_MAX; i++)
      this->pref_data_.device_keys[i - 1] = this->pref_data_.device_keys[i];
    n = DEVICE_KEYS_MAX - 1;
  }
  this->pref_data_.device_keys[n] = key;
  this->pref_data_.device_keys_n = n + 1;
  this->save_pref_();
}

void SungrowAnalytics::save_pref_() {
  this->pref_data_.window_start_epoch = this->cap_.window_start_epoch;
  this->pref_data_.uploads = this->cap_.uploads;
  this->pref_.save(&this->pref_data_);
  global_preferences->sync();
}

void SungrowAnalytics::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Usage reporting:\n"
                "  Reporting: %s\n"
                "  Endpoint: %s\n"
                "  Automatic uploads today: %u of %u",
                this->enabled_ ? "on" : "off (manual diagnostics upload only)", this->endpoint_.c_str(),
                static_cast<unsigned>(this->cap_.uploads), static_cast<unsigned>(UploadCap::MAX));
}

// ---- clock ---------------------------------------------------------------------------------------

bool SungrowAnalytics::time_valid_() const {
#ifdef USE_TIME
  return this->time_ != nullptr && this->time_->utcnow().is_valid();
#else
  return false;
#endif
}

std::string SungrowAnalytics::now_rfc3339_() const {
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    ESPTime t = this->time_->utcnow();
    if (t.is_valid()) {
      char buf[32];
      t.strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ");
      return buf;
    }
  }
#endif
  return "";
}

uint32_t SungrowAnalytics::now_epoch_() const {
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    ESPTime t = this->time_->utcnow();
    if (t.is_valid())
      return static_cast<uint32_t>(t.timestamp);
  }
#endif
  return 0;
}

// ---- loop ----------------------------------------------------------------------------------------

void SungrowAnalytics::loop() {
  const uint32_t now = millis();
  this->poll_jobs_(now);
  if (now - this->last_tick_ms_ < 1000)
    return;
  this->last_tick_ms_ = now;

  if (!network::is_connected()) {
    this->net_up_ms_ = 0;
    return;
  }
  if (this->net_up_ms_ == 0)
    this->net_up_ms_ = now;
  if (this->distinct_id_.empty())
    return;

  if (this->enabled_ && !this->key_rejected_ && !this->batch_job_) {
    if (this->schedule_.due(now, this->time_valid_()))
      this->send_ping_(now);
    else if (!this->errors_.empty() && now - this->last_error_send_ms_ >= ERROR_FLUSH_MS)
      this->send_errors_(now);
  }
  if (now - this->last_sample_ms_ >= SAMPLE_MS) {
    this->last_sample_ms_ = now;
    this->sample_(now);
  }
  this->check_boot_trigger_(now);
  this->check_device_trigger_(now);
  this->run_pending_bundle_(now);
}

void SungrowAnalytics::poll_jobs_(uint32_t now) {
  if (this->batch_job_) {
    const int st = this->batch_job_->state.load(std::memory_order_acquire);
    if (st >= AnalyticsJob::DONE) {
      if (st == AnalyticsJob::DONE) {
        if (this->batch_is_ping_)
          this->pings_sent_++;
        ESP_LOGD(TAG, "%s sent", this->batch_is_ping_ ? "Usage report" : "Error report");
      } else {
        ESP_LOGW(TAG, "%s not sent: %s", this->batch_is_ping_ ? "Usage report" : "Error report",
                 this->batch_job_->error);
        if (st == AnalyticsJob::UNAUTHORIZED) {
          this->key_rejected_ = true;
          ESP_LOGW(TAG, "The collector rejected the api key; reporting is off until the next boot");
        } else if (this->batch_is_ping_) {
          this->schedule_.next_due_ms = now + PING_RETRY_MS;
        }
      }
      this->batch_job_.reset();
    }
  }
  if (this->bundle_job_) {
    const int st = this->bundle_job_->state.load(std::memory_order_acquire);
    if (st >= AnalyticsJob::DONE) {
      this->bundle_stamp_ms_ = now;
      if (st == AnalyticsJob::DONE) {
        this->bundle_state_ = BundleState::DONE;
        this->bundle_id_ = this->bundle_job_->id;
        this->bundle_error_.clear();
        ESP_LOGI(TAG, "Diagnostics report sent (%s), reference %s", this->bundle_trigger_.c_str(),
                 this->bundle_id_.c_str());
      } else {
        this->bundle_state_ = BundleState::FAILED;
        this->bundle_error_ = this->bundle_job_->error;
        ESP_LOGW(TAG, "Diagnostics report not sent (%s): %s", this->bundle_trigger_.c_str(),
                 this->bundle_error_.c_str());
      }
      this->bundle_job_.reset();
    }
  }
}

// ---- usage batches -------------------------------------------------------------------------------

bool SungrowAnalytics::start_batch_(const std::string &body) {
  auto job = std::make_unique<AnalyticsJob>();
  job->url = this->endpoint_ + "/batch/";
  job->content_type = "application/json";
  job->body = body;
  if (!start_job(job.get())) {
    ESP_LOGW(TAG, "Upload task not started");
    return false;
  }
  this->batch_job_ = std::move(job);
  return true;
}

void SungrowAnalytics::send_ping_(uint32_t now) {
  if (this->pings_sent_ == 0 && this->boot_trigger_ != nullptr)
    this->report_error_(this->boot_trigger_, this->boot_crash_class_, "", now);
  PingProps p;
  this->fill_ping_(&p);
  std::vector<DeviceEvent> devs;
  this->fill_devices_(&devs);
  const std::string body = build_batch(this->api_key_.c_str(), this->distinct_id_, this->now_rfc3339_(), &p, devs,
                                       this->errors_);
  this->batch_is_ping_ = true;
  if (this->start_batch_(body)) {
    this->errors_.clear();
    this->last_error_send_ms_ = now;
    this->schedule_.sent(now);
    ESP_LOGD(TAG, "Usage report queued: %u devices, %u bytes", static_cast<unsigned>(devs.size()),
             static_cast<unsigned>(body.size()));
  } else {
    this->schedule_.next_due_ms = now + 60000;
  }
}

void SungrowAnalytics::send_errors_(uint32_t now) {
  const std::string body =
      build_batch(this->api_key_.c_str(), this->distinct_id_, this->now_rfc3339_(), nullptr, {}, this->errors_);
  this->batch_is_ping_ = false;
  if (this->start_batch_(body)) {
    this->errors_.clear();
    this->last_error_send_ms_ = now;
  }
}

void SungrowAnalytics::report_error_(const char *type, const std::string &error, const std::string &device,
                                     uint32_t now) {
  const std::string key = std::string(type) + '|' + error + '|' + device;
  const uint32_t count = this->throttle_.report(key, now);
  if (count == 0)
    return;
  if (this->errors_.size() >= 16)
    this->errors_.erase(this->errors_.begin());
  this->errors_.push_back({type, error, device, count});
  ESP_LOGD(TAG, "Error event %s: %s%s%s (x%u)", type, error.c_str(), device.empty() ? "" : " on ", device.c_str(),
           static_cast<unsigned>(count));
}

void SungrowAnalytics::fill_ping_(PingProps *p) const {
  p->version = ESPHOME_PROJECT_VERSION;
  p->esphome_version = ESPHOME_VERSION;
  p->stock_version = stock_version();
  p->board = board_name();
  p->chip = chip_name();
  p->network = network_name();
  p->uptime_s = millis() / 1000;
  p->reset_reason = reset_reason_code(reset_reason_value());
#ifdef SUNGROW_ANALYTICS_BIND_CLOUD
  p->cloud_uplink = this->cloud_ != nullptr && this->cloud_->is_enabled();
#endif
#ifdef SUNGROW_ANALYTICS_BIND_MQTT
  p->mqtt = this->mqtt_ != nullptr;
#endif
#ifdef SUNGROW_ANALYTICS_BIND_MODBUS_TCP
  p->modbus_tcp = this->mbtcp_ != nullptr && this->mbtcp_->is_enabled();
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CONTROL
  p->control_writes = this->control_ != nullptr && this->control_->is_enabled();
#endif
  p->psram_kb = psram_total() / 1024;
  p->heap_min_kb = internal_heap_min() / 1024;
}

std::string SungrowAnalytics::signal_text_(uint8_t device, uint16_t sigid) const {
  if (this->dm_ == nullptr)
    return "";
  const dm::Signal *s = this->dm_->store().get(device, sigid);
  if (s == nullptr || !s->valid)
    return "";
  if (s->kind == dm::ValueKind::STRING)
    return s->str();
  if (s->kind == dm::ValueKind::ARRAY)
    return "[" + std::to_string(s->len) + " bytes]";
  const double v = s->number();
  if (std::isnan(v)) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%" PRIX64, static_cast<uint64_t>(s->bits()));
    return buf;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.10g", v);
  return buf;
}

void SungrowAnalytics::fill_devices_(std::vector<DeviceEvent> *out) const {
  if (this->hub_ == nullptr)
    return;
  for (const Device &d : this->hub_->master().devices()) {
    if (d.abandoned() || !d.identified())
      continue;
    DeviceEvent e;
    const sungrow_modbus::DevInfo &di = d.devinfo();
    e.model = di.model;
    e.role = d.table() != nullptr ? role_name(d.table()->family) : "other";
    e.dev_type = di.dev_type;
    e.protocol = di.protocol;
    e.dev_code = di.dev_code;
    e.inv_type_known = d.inv_type_known();
    e.inv_type = d.inv_type();
    if (d.table() != nullptr) {
      const VersionSig *vs = nullptr;
      const size_t n = version_sigids(d.table()->family, &vs);
      for (size_t i = 0; i < n; i++) {
        std::string v = this->signal_text_(d.config().index, vs[i].sigid);
        if (!v.empty())
          e.versions.emplace_back(vs[i].name, v);
      }
    }
    out->push_back(std::move(e));
  }
}

// ---- triggers ------------------------------------------------------------------------------------

void SungrowAnalytics::on_unsupported_(const Device &dev, const char *why) {
  const uint32_t now = millis();
  const sungrow_modbus::DevInfo &di = dev.devinfo();
  char detail[96];
  std::snprintf(detail, sizeof(detail), "%s:%u/%u/%u", why, di.dev_type, di.protocol, di.dev_code);
  std::string err = detail;
  if (dev.inv_type_known()) {
    std::snprintf(detail, sizeof(detail), "/0x%04X", dev.inv_type());
    err += detail;
  }
  this->report_error_("unsupported_device", err, di.model, now);
  this->unsupported_.push_back({dev.config().index, di.dev_code, now, false});
}

void SungrowAnalytics::sample_(uint32_t now) {
  if (this->hub_ == nullptr)
    return;
  ModbusMaster &m = this->hub_->master();
  const auto &devs = m.devices();
  if (this->dev_state_.size() < devs.size())
    this->dev_state_.resize(devs.size());
  bool any_online = false;
  for (const Device &d : devs)
    any_online |= d.online();
  for (size_t i = 0; i < devs.size(); i++) {
    const Device &d = devs[i];
    DevState &s = this->dev_state_[i];
    if (d.abandoned())
      continue;
    const std::string model = d.devinfo().model;
    // Offline for 10 min after having been online, while another device still answers (a dark
    // inverter at night takes every device with it and is not a fault).
    if (d.online()) {
      s.was_online = true;
      s.offline_since_ms = 0;
    } else if (s.was_online) {
      if (s.offline_since_ms == 0)
        s.offline_since_ms = now;
      else if (!s.offline_reported && any_online && now - s.offline_since_ms >= OFFLINE_BUNDLE_MS) {
        s.offline_reported = true;
        this->report_error_("modbus_offline", "offline 10 min while others answer", model, now);
        this->request_auto_bundle_("offline", now);
      }
    }
    // A burst of timeouts / exceptions within one sample interval, counted only after the first
    // poll round (its unserved blocks are expected, not a fault).
    const sungrow_modbus::LinkStats &st = d.stats();
    const uint32_t fails = st.timeouts + st.exceptions;
    // A counter reset (device reconnect) lowers the total; treat that as zero new fails, not a
    // huge unsigned wrap. The next line re-primes last_fail_total to the lower value.
    const uint32_t new_fails = fails >= s.last_fail_total ? fails - s.last_fail_total : 0;
    if (d.online() && d.first_round_done() && s.fails_primed && new_fails >= MODBUS_BURST && !s.burst_reported) {
      s.burst_reported = true;
      this->report_error_("modbus_burst", std::to_string(new_fails) + " failed reads in 60 s", model, now);
      this->request_auto_bundle_("modbus", now);
    }
    if (d.first_round_done()) {
      s.last_fail_total = fails;
      s.fails_primed = true;
    }
#ifdef SUNGROW_ANALYTICS_BIND_CONTROL
    // An inverter with no power-stage range table (neither its code nor its model string is listed).
    if (!s.catalogue_checked && d.discovered() && d.table() != nullptr && d.table()->family == dm::Family::INVERTER) {
      s.catalogue_checked = true;
      if (sungrow_control::dsp_model_for(d.devinfo().dev_code, d.devinfo().model) == nullptr) {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "uncatalogued:%u/%u/%u", d.devinfo().dev_type,
                      d.devinfo().protocol, d.devinfo().dev_code);
        this->report_error_("unsupported_device", detail, model, now);
        this->unsupported_.push_back({d.config().index, d.devinfo().dev_code, now, false});
      }
    }
#endif
  }
#ifdef SUNGROW_ANALYTICS_BIND_CLOUD
  if (this->cloud_ != nullptr && !this->cloud_reported_) {
    if (this->cloud_->is_enabled() && !this->cloud_->is_connected() && this->cloud_->has_error()) {
      if (++this->cloud_bad_samples_ >= CLOUD_SAMPLES) {
        this->cloud_reported_ = true;
        this->report_error_("cloud_connect", "not connected for 15 min, " +
                                                 std::to_string(this->cloud_->connect_failures()) + " failed connects",
                            "", now);
        this->request_auto_bundle_("cloud", now);
      }
    } else {
      this->cloud_bad_samples_ = 0;
    }
  }
#endif
  if (!this->heap_reported_) {
    if (internal_heap_free() < HEAP_FLOOR) {
      if (++this->heap_bad_samples_ >= HEAP_SAMPLES) {
        this->heap_reported_ = true;
        this->report_error_("heap_low", std::to_string(internal_heap_free() / 1024) + " KB internal heap free", "",
                            now);
        this->request_auto_bundle_("heap", now);
      }
    } else {
      this->heap_bad_samples_ = 0;
    }
  }
}

void SungrowAnalytics::check_boot_trigger_(uint32_t now) {
  if (this->boot_bundle_done_ || this->boot_trigger_ == nullptr)
    return;
  if (now - this->net_up_ms_ < BOOT_BUNDLE_DELAY_MS)
    return;
  this->boot_bundle_done_ = true;
  this->request_auto_bundle_(this->boot_trigger_, now);
}

void SungrowAnalytics::check_device_trigger_(uint32_t now) {
  if (this->device_bundle_done_ || this->hub_ == nullptr)
    return;
  const auto &devs = this->hub_->master().devices();
  for (Unsupported &u : this->unsupported_) {
    if (u.bundled)
      continue;
    if (this->device_key_reported_(u.key)) {
      u.bundled = true;  // this install already uploaded a report for that device code
      continue;
    }
    const bool settled = u.index < devs.size() && devs[u.index].first_round_done();
    if (!settled && now - u.at_ms < DEVICE_BUNDLE_WAIT_MS)
      continue;
    u.bundled = true;
    this->device_bundle_done_ = true;
    if (this->request_auto_bundle_("device", now))
      this->remember_device_key_(u.key);
    return;
  }
}

bool SungrowAnalytics::request_auto_bundle_(const char *trigger, uint32_t now) {
  if (!this->enabled_ || this->key_rejected_)
    return false;
  if (!this->cap_.allow(this->now_epoch_())) {
    ESP_LOGI(TAG, "Diagnostics upload (%s) skipped: %u today already", trigger, static_cast<unsigned>(this->cap_.uploads));
    return false;
  }
  if (!this->pending_trigger_.empty())
    return false;
  this->pending_trigger_ = trigger;
  this->run_pending_bundle_(now);
  return true;
}

void SungrowAnalytics::run_pending_bundle_(uint32_t now) {
  if (this->pending_trigger_.empty() || this->bundle_job_)
    return;
  const std::string trigger = this->pending_trigger_;
  this->pending_trigger_.clear();
  if (!this->cap_.allow(this->now_epoch_()))
    return;
  std::string why;
  if (this->start_bundle_(trigger.c_str(), now, &why)) {
    this->cap_.record(this->now_epoch_());
    this->save_pref_();
  } else {
    ESP_LOGW(TAG, "Diagnostics upload (%s) not started: %s", trigger.c_str(), why.c_str());
  }
}

bool SungrowAnalytics::request_bundle(const char *trigger, std::string *why) {
  const uint32_t now = millis();
  if (!trigger_known(trigger)) {
    if (why != nullptr)
      *why = "unknown trigger";
    return false;
  }
  if (std::strcmp(trigger, "manual") != 0)
    return this->request_auto_bundle_(trigger, now);
  return this->start_bundle_(trigger, now, why);
}

bool SungrowAnalytics::start_bundle_(const char *trigger, uint32_t now, std::string *why) {
  auto fail = [&](const char *w) {
    if (why != nullptr)
      *why = w;
    return false;
  };
  if (this->bundle_job_)
    return fail("an upload is running");
  if (this->distinct_id_.empty())
    return fail("identity not ready");
  if (!network::is_connected())
    return fail("no network");
  auto job = std::make_unique<AnalyticsJob>();
  job->url = this->endpoint_ + "/diag/";
  job->content_type = "text/plain; charset=utf-8";
  job->headers = {{"X-Api-Key", this->api_key_},
                  {"X-Distinct-Id", this->distinct_id_},
                  {"X-Trigger", trigger},
                  {"X-Version", ESPHOME_PROJECT_VERSION}};
  job->body = this->build_bundle();
  if (!start_job(job.get()))
    return fail("upload task not started");
  this->bundle_job_ = std::move(job);
  this->bundle_state_ = BundleState::RUNNING;
  this->bundle_trigger_ = trigger;
  this->bundle_id_.clear();
  this->bundle_error_.clear();
  this->bundle_stamp_ms_ = now;
  ESP_LOGI(TAG, "Diagnostics report upload started (%s), %u bytes", trigger,
           static_cast<unsigned>(this->bundle_job_->body.size()));
  return true;
}

BundleStatus SungrowAnalytics::bundle_status() const {
  BundleStatus s;
  s.state = this->bundle_state_;
  s.id = this->bundle_id_;
  s.error = this->bundle_error_;
  s.trigger = this->bundle_trigger_;
  s.age_ms = this->bundle_state_ == BundleState::IDLE ? 0 : millis() - this->bundle_stamp_ms_;
  return s;
}

// ---- the report ----------------------------------------------------------------------------------

void SungrowAnalytics::fill_scrubber_() {
  this->scrubber_ = Scrubber{};
  if (!this->dongle_sn_.empty())
    this->scrubber_.add(this->dongle_sn_, "<sn>");
  if (this->hub_ != nullptr) {
    for (const Device &d : this->hub_->master().devices()) {
      const std::string sn = this->signal_text_(d.config().index, 0x0002);
      if (!sn.empty())
        this->scrubber_.add(sn, "<serial>");
    }
  }
  this->scrubber_.add(App.get_name().c_str(), "<host>");
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    char ssid[wifi::SSID_BUFFER_SIZE] = "";
    wifi::global_wifi_component->wifi_ssid_to(ssid);
    this->scrubber_.add(ssid, "<ssid>");
    this->scrubber_.add(wifi::global_wifi_component->get_sta().get_password().str(), "<psk>");
  }
#endif
  if (this->mqtt_ != nullptr)
    this->scrubber_.add(this->mqtt_->settings().pass, "<mqtt-password>");
}

void SungrowAnalytics::fill_bundle_device_(const Device &d, uint32_t now, BundleDevice *out) const {
  const sungrow_modbus::DevInfo &di = d.devinfo();
  out->index = d.config().index;
  out->address = d.config().address;
  out->child_kind = child_kind_name(d.config().child_kind);
  out->parent = d.config().parent;
  out->state = d.state() == Device::State::RUN ? "RUN" : "DISCOVER";
  out->dev_code = di.dev_code;
  out->model = di.model;
  out->proto_code = di.proto_code;
  out->tag = di.tag;
  out->protocol = di.protocol;
  out->dev_type = di.dev_type;
  out->dev_type_source = d.dev_type_source();
  out->inv_type_known = d.inv_type_known();
  out->inv_type = d.inv_type();
  out->table = d.table_name();
  out->family = d.table() != nullptr ? role_name(d.table()->family) : "";
  out->fallback_used = d.fallback_used();
  out->xml_family = sungrow_modbus::xml_family_name(d.xml_family());
  if (const sungrow_modbus::FamilyDescriptor *fd = d.descriptor(); fd != nullptr) {
    out->has_descriptor = true;
    out->country_reg = fd->country_reg;
    out->grid_reg = fd->grid_reg;
    out->power_switch_reg = fd->power_switch_reg;
  }
  out->identified = d.identified();
  out->discovered = d.discovered();
  out->online = d.online();
  out->abandoned = d.abandoned();
  out->first_round_done = d.first_round_done();
  const Device::IdentRaw ir = d.ident_raw();
  if (ir.nregs > 0) {
    out->ident_raw.assign(ir.data, ir.data + 2u * ir.nregs);
    this->scrubber_.apply_bytes(out->ident_raw.data(), out->ident_raw.size());
    out->ident_start = ir.start;
    out->ident_fn = ir.fn;
  }
  const sungrow_modbus::LinkStats &st = d.stats();
  out->sent = st.sent;
  out->received = st.received;
  out->timeouts = st.timeouts;
  out->crc_errors = st.crc_errors;
  out->exceptions = st.exceptions;
  out->loss_pct = st.loss_pct();
  const dm::RegisterTable *tab = d.table();
  for (const Device::Job &j : d.jobs()) {
    BundleJob bj;
    bj.fn = j.req.fn;
    bj.start = j.req.start;
    bj.count = j.req.count;
    bj.tier = sungrow_modbus::poll_tier_name(j.tier);
    bj.effective_tier = sungrow_modbus::poll_tier_name(j.effective_tier());
    bj.pending = j.pending;
    bj.unserved = j.unserved;
    bj.last_exc = j.last_exc;
    bj.exc_count = j.exc_count;
    bj.last_ok_age_s = j.last_ok != 0 ? static_cast<int32_t>((now - j.last_ok) / 1000) : -1;
    out->jobs.push_back(bj);
    // Static-tier rows carry the identity and capability registers.
    if (tab == nullptr || j.tier != sungrow_modbus::PollTier::STATIC || out->static_values.size() >= 160)
      continue;
    for (uint16_t r = j.req.first_row; r <= j.req.last_row && r < tab->count; r++) {
      const dm::RegisterRow &row = tab->rows[r];
      if (row.sigid == 0 || row.key == dm::NO_KEY)
        continue;
      std::string v = this->signal_text_(d.config().index, row.sigid);
      if (v.empty())
        continue;
      out->static_values.emplace_back(dm::kRowKeys[row.key], std::move(v));
      if (out->static_values.size() >= 160)
        break;
    }
  }
  if (tab != nullptr) {
    const VersionSig *vs = nullptr;
    const size_t n = version_sigids(tab->family, &vs);
    for (size_t i = 0; i < n; i++) {
      std::string v = this->signal_text_(d.config().index, vs[i].sigid);
      if (!v.empty())
        out->versions.emplace_back(vs[i].name, std::move(v));
    }
  }
}

std::string SungrowAnalytics::build_bundle() {
  const uint32_t now = millis();
  this->fill_scrubber_();
  std::string out;
  out.reserve(16384);

  BundleBuild b;
  b.project_version = ESPHOME_PROJECT_VERSION;
  b.esphome_version = ESPHOME_VERSION;
#ifdef USE_ESP32
  b.idf_version = esp_get_idf_version();
#endif
  char build_id[64] = "";
#ifdef USE_SUNGROW_DIAG
  std::snprintf(build_id, sizeof(build_id), "%s", sungrow_board::diag_build());
  b.boot_count = sungrow_board::diag_boot_count();
  b.last_reset = sungrow_board::diag_last_reset();
#else
  std::snprintf(build_id, sizeof(build_id), "0x%08" PRIx32 " %s", ESPHOME_CONFIG_HASH, ESPHOME_BUILD_TIME_STR);
#endif
  b.build_id = build_id;
  b.board = board_name();
  b.chip = chip_name();
  b.network = network_name();
  b.stock_version = stock_version();
#ifdef USE_ESP32
  {
    uint32_t flash = 0;
    if (esp_flash_get_size(nullptr, &flash) == ESP_OK)
      b.flash_kb = flash / 1024;
  }
#endif
  b.psram_kb = psram_total() / 1024;
  b.uptime_s = now / 1000;
  b.reset_reason = reset_reason_code(reset_reason_value());
  write_headline(out, b);
  write_build(out, b);

  BundleFeatures f;
  f.analytics_enabled = this->enabled_;
#ifdef SUNGROW_ANALYTICS_BIND_CLOUD
  if (this->cloud_ != nullptr) {
    f.cloud_present = true;
    f.cloud_enabled = this->cloud_->is_enabled();
    f.cloud_connected = this->cloud_->is_connected();
    f.cloud_error = this->cloud_->has_error();
    f.cloud_failures = this->cloud_->connect_failures();
  }
#endif
#ifdef SUNGROW_ANALYTICS_BIND_MQTT
  if (this->mqtt_ != nullptr) {
    f.mqtt_present = true;
    f.mqtt_connected = this->mqtt_->is_connected();
  }
#endif
#ifdef SUNGROW_ANALYTICS_BIND_MODBUS_TCP
  if (this->mbtcp_ != nullptr) {
    f.mbtcp_present = true;
    f.mbtcp_enabled = this->mbtcp_->is_enabled();
    const auto &s = this->mbtcp_->stats();
    f.mbtcp_requests = s.requests;
    f.mbtcp_exceptions = s.exceptions;
    f.mbtcp_dropped = s.dropped;
    f.mbtcp_rejected = s.rejected;
  }
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CONTROL
  if (this->control_ != nullptr) {
    f.control_present = true;
    f.control_enabled = this->control_->is_enabled();
    if (this->hub_ != nullptr) {
      const sungrow_modbus::WriteStats &w = this->hub_->master().write_stats();
      f.writes_ok = w.ok;
      f.writes_failed = w.failed;
      f.writes_rejected = w.rejected;
      f.writes_mismatch = w.verify_mismatch;
    }
  }
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CAPTURE
  if (this->capture_ != nullptr) {
    f.capture_present = true;
    f.capture_running = this->capture_->core().running();
    f.capture_kind = f.capture_running ? sungrow_capture::kind_name(this->capture_->core().running_kind()) : "";
  }
#endif
#ifdef USE_SUNGROW_NET_FALLBACK
  std::string net_fallback_line;
  if (sungrow_board::global_board != nullptr) {
    net_fallback_line = sungrow_board::global_board->network_fallback_status();
    f.net_fallback = net_fallback_line.c_str();
  }
#endif
  write_features(out, f);

  BundleMemory mem;
  mem.heap_free = internal_heap_free();
  mem.heap_min = internal_heap_min();
  mem.psram_total = psram_total();
  mem.psram_free = psram_free();
  write_memory(out, mem);

  if (this->hub_ != nullptr) {
    const auto &devs = this->hub_->master().devices();
    size_t n = 0;
    for (const Device &d : devs)
      n += !d.abandoned();
    write_line(out, "== Devices (%u) ==", static_cast<unsigned>(n));
    for (const Device &d : devs) {
      if (d.abandoned())
        continue;
      BundleDevice bd;
      this->fill_bundle_device_(d, now, &bd);
      write_device_support(out, bd);
    }
  } else {
    out += "== Devices (0) ==\n";
  }

#ifdef USE_SUNGROW_DIAG
  out += "== Reset diagnostics ==\n";
  out += sungrow_board::diag_report();
  out += "\n--- log tail of this boot ---\n";
  out += sungrow_board::diag_ring_text();
#else
  out += "== Reset diagnostics ==\n(not built)\n";
#endif
  return this->scrubber_.apply(out);
}

// ---- "Usage reporting" switch ----------------------------------------------------------------------
#ifdef USE_SWITCH
void ReportingSwitch::setup() {
  // Restored state when there is one; else the `enabled` YAML option (the analytics substitution).
  optional<bool> restored;
  if (this->restore_mode & switch_::RESTORE_MODE_PERSISTENT_MASK)
    restored = this->get_initial_state();
  else
    restored = this->get_initial_state_with_restore_mode();
  this->write_state(restored.value_or(this->parent_->enabled()));
}
void ReportingSwitch::dump_config() { LOG_SWITCH("", "Usage reporting switch", this); }
void ReportingSwitch::write_state(bool state) {
  this->parent_->set_enabled(state);
  this->publish_state(state);
}
#endif

}  // namespace sungrow_analytics
}  // namespace esphome
