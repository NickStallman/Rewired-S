#pragma once
// Entity classes for the sensor / text_sensor / binary_sensor platforms. Header-only and
// guarded, so the component builds whichever entity domains the YAML actually uses.
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "esphome/core/defines.h"
#include "sungrow_datamodel.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

namespace esphome {
namespace sungrow_datamodel {

#ifdef USE_SENSOR
class DmSensor : public sensor::Sensor, public DmBinding {
 public:
  /// Extra factor on top of the table scale (unit normalisation, e.g. kvar -> var = 1000).
  void set_multiplier(float m) { this->multiplier_ = m; }

 protected:
  void dm_publish(const SignalStore &store) override {
    float fm;
    const Signal *s = this->pick_(store, &fm);
    double v = s != nullptr ? s->number() : NAN;
    // A fallback's multiplier already includes its own unit normalisation.
    double m = (s != nullptr && s->sigid != this->sigid_) ? fm : this->multiplier_;
    this->publish_state(std::isnan(v) ? NAN : static_cast<float>(v * m));
  }
  float multiplier_{1.0f};
};
#endif

#ifdef USE_TEXT_SENSOR
class DmTextSensor : public text_sensor::TextSensor, public DmBinding {
 public:
  void add_mapping(uint32_t code, const char *text) { this->map_.emplace_back(code, text); }
  void set_decimals(int8_t d) { this->decimals_ = d; }

 protected:
  void dm_publish(const SignalStore &store) override {
    const Signal *s = this->pick_(store);
    if (s == nullptr || !s->valid) {
      // ESPHome text sensors have no "unavailable" state: an invalid signal publishes "".
      this->publish_state("");
      return;
    }
    if (s->kind == ValueKind::STRING) {
      this->publish_state(s->str());
      return;
    }
    if (s->kind == ValueKind::SCALAR && !this->map_.empty()) {
      uint32_t code = static_cast<uint32_t>(s->bits());
      for (auto &kv : this->map_) {
        if (kv.first == code) {
          this->publish_state(kv.second);
          return;
        }
      }
      char buf[16];
      std::snprintf(buf, sizeof(buf), "0x%04X", (unsigned) code);
      this->publish_state(buf);
      return;
    }
    double v = s->number();
    if (std::isnan(v)) {
      this->publish_state("");
      return;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", this->decimals_, v);
    this->publish_state(buf);
  }
  std::vector<std::pair<uint32_t, const char *>> map_;
  int8_t decimals_{0};
};
#endif

#ifdef USE_BINARY_SENSOR
class DmBinarySensor : public binary_sensor::BinarySensor, public DmBinding {
 public:
  /// Bit index of a status/bitfield word, or -1 for "value != 0".
  void set_bit(int8_t bit) { this->bit_ = bit; }

 protected:
  void dm_publish(const SignalStore &store) override {
    const Signal *s = this->pick_(store);
    if (s == nullptr || !s->valid || s->kind != ValueKind::SCALAR) {
      this->invalidate_state();
      return;
    }
    uint64_t bits = s->bits();
    this->publish_state(this->bit_ < 0 ? bits != 0 : ((bits >> this->bit_) & 1ULL) != 0);
  }
  int8_t bit_{-1};
};
#endif

}  // namespace sungrow_datamodel
}  // namespace esphome
