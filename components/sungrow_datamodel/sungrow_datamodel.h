#pragma once
// ESPHome-facing hub: owns the SignalStore and drives entity publishing.
// The core (dm_types/decoder/signal_store/request_builder/derived/register_tables) has no
// ESPHome dependency; only this file and dm_entities.h include ESPHome headers.
#include <vector>

#include "esphome/core/component.h"
#include "signal_store.h"

namespace esphome {

namespace sungrow_datamodel {

class SungrowDataModel;

/// Glue between one store signal and one ESPHome entity. Publishes on change (throttled to
/// at most once per `throttle` ms) and optionally re-publishes every `force_interval` ms.
class DmBinding {
 public:
  virtual ~DmBinding() = default;
  void set_signal(uint8_t device, uint16_t sigid) {
    this->device_ = device;
    this->sigid_ = sigid;
  }
  /// Fallback signal (key list): published when the primary and earlier fallbacks are
  /// invalid. `multiplier` converts its value into the primary's unit (sensors only).
  void add_fallback(uint16_t sigid, float multiplier) { this->fallbacks_.push_back({sigid, multiplier}); }
  void set_throttle(uint32_t ms) { this->throttle_ms_ = ms; }
  void set_force_interval(uint32_t ms) { this->force_ms_ = ms; }
  /// Subscribe to the parent's store (called from generated code).
  void attach(SungrowDataModel *parent);
  /// Called by the hub's loop.
  void dm_tick(uint32_t now);

  uint8_t get_device() const { return this->device_; }
  uint16_t get_sigid() const { return this->sigid_; }

 protected:
  /// Publish the current store value (NaN / invalidate when the signal is invalid).
  virtual void dm_publish(const SignalStore &store) = 0;
  /// Signal to publish: the primary or the first valid fallback; the primary if none is valid
  /// (nullptr if never stored). *fallback_mult = the fallback's multiplier, 1 for the primary.
  const Signal *pick_(const SignalStore &store, float *fallback_mult = nullptr) const;

  struct Fallback {
    uint16_t sigid;
    float multiplier;
  };
  std::vector<Fallback> fallbacks_;
  SungrowDataModel *parent_{nullptr};
  uint8_t device_{0};
  uint16_t sigid_{0};
  bool dirty_{false};
  bool published_{false};
  uint32_t throttle_ms_{1000};
  uint32_t force_ms_{0};
  uint32_t last_publish_{0};
};

class SungrowDataModel : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // Before the Modbus master / cloud / TCP slave, which use the store.
  float get_setup_priority() const override { return setup_priority::BUS + 1.0f; }

  SignalStore &store() { return this->store_; }
  const SignalStore &store() const { return this->store_; }
  void register_binding(DmBinding *b) { this->bindings_.push_back(b); }

 protected:
  SignalStore store_;
  std::vector<DmBinding *> bindings_;
  uint32_t last_tick_{0};
};

}  // namespace sungrow_datamodel
}  // namespace esphome
