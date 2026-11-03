#pragma once
// ESPHome glue for sungrow_control: the `writes_enabled` switch, select / number entities that
// write allow-listed holding registers, the read-back poller, the write action and stats.
// Safety logic and value mapping live in the host-tested core (control_core.h).
#include <functional>
#include <string>
#include <tuple>
#include <vector>

#include "esphome/components/sungrow_datamodel/sungrow_datamodel.h"
#include "esphome/components/sungrow_modbus/sungrow_modbus.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#include "control_core.h"
#include "esphome/components/sungrow_datamodel/dm_entities.h"

#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome {
namespace sungrow_control {

class SungrowControl;

#ifdef USE_SWITCH
/// Write enable for MQTT, the Modbus-TCP :502 server, the HA action and the native entities.
/// Web-interface writes are authorised by the login (and confirm token) and do not use it.
/// Restored, default OFF (RESTORE_DEFAULT_OFF). Stable id: sungrow_writes_enabled.
class WritesEnabledSwitch : public switch_::Switch, public Parented<SungrowControl> {
 protected:
  void write_state(bool state) override;
};
#endif

class SungrowControl : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the RS485 hub (DATA) so the master exists.
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

  // ---- configuration (generated code) ------------------------------------------------------
  void set_hub(sungrow_modbus::SungrowModbus *hub) { this->hub_ = hub; }
  void set_datamodel(sungrow_datamodel::SungrowDataModel *dm) { this->dm_ = dm; }
  void set_default_verify(bool v) { this->default_verify_ = v; }
#ifdef USE_TIME
  /// Keep the inverters' clocks set from this time source (local time): once it is valid after
  /// boot, then every 24 h per inverter; a failed write is retried after 10 min.
  void set_clock_source(time::RealTimeClock *t) { this->clock_source_ = t; }
#endif
  /// Add a device with a profile ("hybrid" / "string" / "none").
  void add_device(uint8_t index, const char *profile, bool readback, bool reassert_power_limit = true);
  /// YAML allow-list extra for the device added last (raw u16/s16 range).
  void add_rule(uint16_t address, uint16_t count, int32_t min, int32_t max, bool is_signed);
#ifdef USE_SWITCH
  void set_enable_switch(WritesEnabledSwitch *s) { this->enable_switch_ = s; }
#endif
#ifdef USE_SENSOR
  void set_writes_ok_sensor(sensor::Sensor *s) { this->ok_sensor_ = s; }
  void set_writes_failed_sensor(sensor::Sensor *s) { this->failed_sensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_last_write_sensor(text_sensor::TextSensor *s) { this->last_write_sensor_ = s; }
#endif

  // ---- API ---------------------------------------------------------------------------------
  void set_enabled(bool on);
  bool is_enabled() const { return this->core_.enabled(); }
  bool default_verify() const { return this->default_verify_; }
  ControlCore &core() { return this->core_; }
  sungrow_datamodel::SungrowDataModel *datamodel() { return this->dm_; }
  /// Gated write (all normal write paths end here). See ControlCore::write().
  Verdict write(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count, bool verify,
                const char *source, ControlCore::Callback cb, sungrow_modbus::WriteOptions opt = {}) {
    return this->core_.write(device, start, values, count, verify, source, std::move(cb), opt);
  }
  /// Called after a Setup / Installer frame starting at the device's country register was
  /// written OK (sungrow_modbus_tcp binds its on_country_written here).
  void set_country_written_cb(std::function<void(uint16_t)> cb) { this->core_.set_country_written_cb(std::move(cb)); }
  /// Outcome of the most recent write (set right before an action's next step runs).
  bool last_ok() const { return this->last_ok_; }
  const std::string &last_message() const { return this->last_message_; }
  void set_last(bool ok, const std::string &msg) {
    this->last_ok_ = ok;
    this->last_message_ = msg;
  }

 protected:
  /// Attach a curated control profile to each discovered device without one by its DevInfo
  /// dev_type (35 -> hybrid, 21/1 -> string), with the addresses of its protocol type, and bind
  /// YAML-configured profiles to the protocol type once the device knows it. Idempotent.
  void auto_attach_profiles_();
#ifdef USE_TIME
  void clock_sync_tick_(uint32_t now_ms);
  time::RealTimeClock *clock_source_{nullptr};
  uint32_t last_clock_check_{0};
  std::vector<std::pair<uint8_t, uint32_t>> clock_due_;  ///< device index -> millis of the next sync
#endif

  sungrow_modbus::SungrowModbus *hub_{nullptr};
  sungrow_datamodel::SungrowDataModel *dm_{nullptr};
  ControlCore core_;
  std::vector<ControlCore::DeviceCfg> cfgs_;
  bool default_verify_{true};
  bool last_ok_{false};
  std::string last_message_;
  uint32_t last_stats_{0};
#ifdef USE_SWITCH
  WritesEnabledSwitch *enable_switch_{nullptr};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *ok_sensor_{nullptr};
  sensor::Sensor *failed_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *last_write_sensor_{nullptr};
#endif
};

/// Base of the select / number entities: one allow-listed register of one device, state from
/// the read-back signal in the store (never optimistic).
class ControlEntity : public sungrow_datamodel::DmBinding {
 public:
  /// (Not Parented<>: DmBinding already has a parent_, the data model.)
  void set_parent(SungrowControl *c) { this->ctl_ = c; }
  void set_register(uint16_t address, uint8_t type, int32_t min, int32_t max) {
    this->address_ = address;
    this->type_ = type;
    this->min_ = min;
    this->max_ = max;
  }
  void set_verify(bool v) { this->verify_ = v; }

 protected:
  void write_raw_(uint16_t raw, const char *source);
  /// Re-publish the store value (after a rejected write, so HA shows the real state again).
  void republish_() { this->dirty_ = true; }
  SungrowControl *ctl_{nullptr};
  uint16_t address_{0};
  uint8_t type_{sungrow_datamodel::TYPE_U16};
  int32_t min_{0};
  int32_t max_{0xFFFF};
  bool verify_{true};
};

#ifdef USE_SELECT
class ControlSelect : public select::Select, public ControlEntity {
 public:
  void add_option(uint16_t raw, const char *text) { this->opts_.push_back({raw, text}); }

 protected:
  void control(size_t index) override;
  void dm_publish(const sungrow_datamodel::SignalStore &store) override;
  std::vector<Option> opts_;
};
#endif

#ifdef USE_NUMBER
class ControlNumber : public number::Number, public ControlEntity {
 public:
  void set_scale(float s) { this->scale_ = s; }

 protected:
  void control(float value) override;
  void dm_publish(const sungrow_datamodel::SignalStore &store) override;
  float scale_{1.0f};
};
#endif

#ifdef USE_TEXT_SENSOR
/// Read-back text: mkaiser strings; "unknown" while the value is not known (offline / never
/// read), which HA consumers such as the battery controller treat as "Modbus dead" (an empty
/// string would look like a wrong mode instead).
class ControlTextSensor : public sungrow_datamodel::DmTextSensor {
 protected:
  void dm_publish(const sungrow_datamodel::SignalStore &store) override {
    const sungrow_datamodel::Signal *s = this->pick_(store);
    if (s == nullptr || !s->valid) {
      this->publish_state("unknown");
      return;
    }
    sungrow_datamodel::DmTextSensor::dm_publish(store);
  }
};
#endif

/// `sungrow_control.write_registers` action: completes when the write is done (or rejected);
/// the outcome is in last_ok() / last_message() for a following `api.respond`.
template<typename... Ts> class WriteRegistersAction : public Action<Ts...>, public Parented<SungrowControl> {
 public:
  TEMPLATABLE_VALUE(int, device)
  TEMPLATABLE_VALUE(int, address)
  TEMPLATABLE_VALUE(std::vector<int32_t>, values)
  TEMPLATABLE_VALUE(bool, verify)

  void play_complex(const Ts &...x) override {
    this->num_running_++;
    const int dev = this->device_.value(x...);
    const int addr = this->address_.value(x...);
    std::vector<int32_t> in = this->values_.value(x...);
    const bool verify = this->verify_.has_value() ? this->verify_.value(x...) : this->parent_->default_verify();
    std::vector<uint16_t> vals;
    bool bad = dev < 0 || dev > 255 || addr < 1 || addr > 0xFFFF || in.empty() ||
               in.size() > sungrow_modbus::MAX_WRITE_REGS;
    for (int32_t v : in) {
      if (v < -32768 || v > 0xFFFF)
        bad = true;
      vals.push_back(static_cast<uint16_t>(v & 0xFFFF));
    }
    if (bad) {
      this->parent_->set_last(false, "invalid arguments (device 0-255, address 1-65535, 1-123 values -32768..65535)");
      this->play_next_(x...);
      return;
    }
    auto args = std::make_tuple(x...);
    auto *parent = this->parent_;
    Verdict v = parent->write(static_cast<uint8_t>(dev), static_cast<uint16_t>(addr), vals.data(),
                              static_cast<uint16_t>(vals.size()), verify, "api",
                              [this, parent, args](const WriteResult &r) {
                                parent->set_last(r.status == WriteStatus::OK,
                                                 r.status == WriteStatus::OK
                                                     ? std::string("ok")
                                                     : std::string(sungrow_modbus::write_status_name(r.status)));
                                if (this->num_running_ > 0)
                                  this->play_next_tuple_(args);
                              });
    if (v != Verdict::OK) {
      this->parent_->set_last(false, verdict_name(v));
      this->play_next_(x...);
    }
  }
  void play(const Ts &...x) override { /* play_complex */ }
};

}  // namespace sungrow_control
}  // namespace esphome
