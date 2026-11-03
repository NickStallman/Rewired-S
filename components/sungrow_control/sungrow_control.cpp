#include "sungrow_control.h"

#include <cinttypes>
#include <cmath>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace sungrow_control {

static const char *const TAG = "sungrow_control";
static constexpr uint32_t STATS_INTERVAL_MS = 30000;
static constexpr uint32_t CLOCK_CHECK_MS = 10000;
static constexpr uint32_t CLOCK_PERIOD_MS = 24UL * 3600UL * 1000UL;
static constexpr uint32_t CLOCK_RETRY_MS = 10UL * 60UL * 1000UL;

#ifdef USE_SWITCH
void WritesEnabledSwitch::write_state(bool state) {
  this->parent_->set_enabled(state);
  this->publish_state(state);
}
#endif

void SungrowControl::add_device(uint8_t index, const char *profile, bool readback, bool reassert_power_limit) {
  ControlCore::DeviceCfg c;
  c.index = index;
  c.profile = find_profile(profile);
  if (c.profile != nullptr)
    c.rules = profile_rules(*c.profile);
  c.readback = readback;
  c.reassert_power_limit = reassert_power_limit;
  this->cfgs_.push_back(std::move(c));
}

void SungrowControl::add_rule(uint16_t address, uint16_t count, int32_t min, int32_t max, bool is_signed) {
  if (this->cfgs_.empty())
    return;
  Rule r;
  r.address = address;
  r.count = count;
  r.min = min;
  r.max = max;
  r.type = is_signed ? sungrow_datamodel::TYPE_S16 : sungrow_datamodel::TYPE_U16;
  r.key = "yaml";
  this->cfgs_.back().rules.push_back(std::move(r));
}

void SungrowControl::set_enabled(bool on) {
  this->core_.set_enabled(on);
#ifdef USE_SWITCH
  if (this->enable_switch_ != nullptr && this->enable_switch_->state != on)
    this->enable_switch_->publish_state(on);
#endif
}

void SungrowControl::setup() {
  this->core_.set_master(&this->hub_->master());
  this->core_.set_store(&this->dm_->store());
  this->core_.set_clock([]() { return millis(); });
  this->core_.set_log([](int level, const char *msg) { esp_log_printf_(level, TAG, __LINE__, "%s", msg); });
  this->core_.set_result_listener([this](uint8_t, const char *text, bool ok) {
    (void) ok;
#ifdef USE_TEXT_SENSOR
    if (this->last_write_sensor_ != nullptr)
      this->last_write_sensor_->publish_state(text);
#else
    (void) text;
#endif
  });
  for (auto &c : this->cfgs_)
    this->core_.add_device(c);
  this->cfgs_.clear();
  bool on = false;  // nothing writes until the user turns it on
#ifdef USE_SWITCH
  if (this->enable_switch_ != nullptr) {
    auto initial = this->enable_switch_->get_initial_state_with_restore_mode();
    on = initial.has_value() && *initial;
    this->enable_switch_->publish_state(on);
  }
#endif
  this->core_.set_enabled(on);
}

void SungrowControl::auto_attach_profiles_() {
  if (this->hub_ == nullptr)
    return;
  for (const auto &dev : this->hub_->master().devices()) {
    const uint8_t index = dev.config().index;
    const uint8_t protocol = dev.devinfo().protocol;
    if (this->core_.device(index) != nullptr) {
      // Configured by YAML or an earlier pass: follow the device's protocol type.
      if (protocol != 0)
        this->core_.bind_protocol(index, protocol);
      continue;
    }
    if (!dev.discovered())
      continue;
    const uint8_t dt = dev.devinfo().dev_type;
    const char *profile = nullptr;
    if (dt == 35)
      profile = "hybrid";
    else if (dt == 21 || dt == 1)
      profile = "string";
    else
      continue;  // unknown dev_type: no profile, so every write to it stays NOT_ALLOWED
    ControlCore::DeviceCfg c;
    c.index = index;
    c.profile = find_profile(profile);
    c.protocol = protocol;
    if (c.profile != nullptr)
      c.rules = profile_rules(*c.profile, protocol);
    c.readback = true;
    this->core_.add_device(std::move(c));
    ESP_LOGI(TAG, "Auto-attached '%s' control profile to device %u (dev_type %u, protocol %u)", profile, index, dt,
             protocol);
  }
}

void SungrowControl::loop() {
  this->auto_attach_profiles_();
  this->core_.tick();
  const uint32_t now = millis();
#ifdef USE_TIME
  this->clock_sync_tick_(now);
#endif
  if (now - this->last_stats_ >= STATS_INTERVAL_MS) {
    this->last_stats_ = now;
#ifdef USE_SENSOR
    if (this->ok_sensor_ != nullptr)
      this->ok_sensor_->publish_state(static_cast<float>(this->core_.writes_ok()));
    if (this->failed_sensor_ != nullptr)
      this->failed_sensor_->publish_state(
          static_cast<float>(this->core_.writes_failed() + this->core_.writes_rejected()));
#endif
  }
}

#ifdef USE_TIME
void SungrowControl::clock_sync_tick_(uint32_t now_ms) {
  if (this->clock_source_ == nullptr || this->hub_ == nullptr || now_ms - this->last_clock_check_ < CLOCK_CHECK_MS)
    return;
  this->last_clock_check_ = now_ms;
  const ESPTime t = this->clock_source_->now();
  if (!t.is_valid())
    return;
  for (const auto &d : this->hub_->master().devices()) {
    const uint8_t idx = d.config().index;
    const ControlCore::DeviceCfg *cfg = this->core_.device(idx);
    if (cfg == nullptr || cfg->profile == nullptr || !d.online())
      continue;
    std::pair<uint8_t, uint32_t> *due = nullptr;
    for (auto &e : this->clock_due_)
      if (e.first == idx)
        due = &e;
    if (due == nullptr) {
      this->clock_due_.emplace_back(idx, now_ms);
      due = &this->clock_due_.back();
    } else if (static_cast<int32_t>(now_ms - due->second) < 0) {
      continue;
    }
    due->second = now_ms + CLOCK_RETRY_MS;  // until the result arrives
    const uint16_t f[6] = {t.year, t.month, t.day_of_month, t.hour, t.minute, t.second};
    const Verdict v = this->core_.write_clock(idx, f, "clock sync", [this, idx, f](const WriteResult &r) {
      const bool ok = r.status == sungrow_modbus::WriteStatus::OK;
      for (auto &e : this->clock_due_)
        if (e.first == idx)
          e.second = millis() + (ok ? CLOCK_PERIOD_MS : CLOCK_RETRY_MS);
      if (ok) {
        ESP_LOGI(TAG, "dev %u clock set to %04u-%02u-%02u %02u:%02u:%02u", idx, f[0], f[1], f[2], f[3], f[4], f[5]);
      } else {
        ESP_LOGW(TAG, "dev %u clock write failed: %s (exception %u); retry in 10 min", idx,
                 sungrow_modbus::write_status_name(r.status), r.exception);
      }
    });
    if (v != Verdict::OK)
      ESP_LOGW(TAG, "dev %u clock write not sent: %s; retry in 10 min", idx, verdict_name(v));
  }
}
#endif

void SungrowControl::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sungrow control (register writes):\n"
                "  Writes enabled: %s\n"
                "  Verify by default: %s",
                YESNO(this->core_.enabled()), YESNO(this->default_verify_));
}

// ---- entities ---------------------------------------------------------------------------------

void ControlEntity::write_raw_(uint16_t raw, const char *source) {
  // A read-back that differs from the written value (VERIFY_MISMATCH, e.g. a forced command or
  // power outside Forced mode) is logged; the entity shows the inverter's value.
  // The profile rule's address on this device's protocol type (e.g. start/stop 5006 / 13000 / 30045).
  const uint16_t addr = this->ctl_->core().address_for_sigid(this->device_, this->sigid_, this->address_);
  Verdict v = this->ctl_->write(this->device_, addr, &raw, 1, this->verify_, source,
                                   [this](const WriteResult &) { this->republish_(); });
  if (v != Verdict::OK)
    this->republish_();  // show the inverter's value again
}

#ifdef USE_SELECT
void ControlSelect::control(size_t index) {
  const char *text = this->option_at(index);
  uint16_t raw;
  if (text == nullptr || !option_to_raw(this->opts_, text, &raw)) {
    ESP_LOGW(TAG, "%s: unknown option", this->get_name().c_str());
    return;
  }
  this->write_raw_(raw, "select");
}

void ControlSelect::dm_publish(const sungrow_datamodel::SignalStore &store) {
  const sungrow_datamodel::Signal *s = this->pick_(store);
  if (s == nullptr || !s->valid)
    return;  // a select cannot be "unknown"; the read-back text sensor shows it
  const char *text = raw_to_option(this->opts_, static_cast<uint16_t>(s->bits()));
  if (text == nullptr) {
    ESP_LOGD(TAG, "%s: inverter value 0x%04X has no option", this->get_name().c_str(), (unsigned) s->bits());
    return;
  }
  if (this->index_of(text).has_value())
    this->publish_state(text);
}
#endif

#ifdef USE_NUMBER
void ControlNumber::control(float value) {
  uint16_t raw;
  if (!number_to_raw(value, this->scale_, this->type_, this->min_, this->max_, &raw)) {
    ESP_LOGW(TAG, "%s: %.3f out of range", this->get_name().c_str(), value);
    this->republish_();
    return;
  }
  this->write_raw_(raw, "number");
}

void ControlNumber::dm_publish(const sungrow_datamodel::SignalStore &store) {
  const sungrow_datamodel::Signal *s = this->pick_(store);
  this->publish_state(s != nullptr && s->valid ? static_cast<float>(s->number()) : NAN);
}
#endif

}  // namespace sungrow_control
}  // namespace esphome
