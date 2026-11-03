#pragma once
// Publish scheduler, a 1 Hz tick (SG-Docs reference/protocols/cloud-messages.md, cadence).
// Host-buildable: time and link state are inputs, the result says what to publish this tick.
//
// Every tick (1 s):
//   1. (the caller keeps the MQTT pump and inbound commands running regardless)
//   2. clock not plausible (year 1971..2069) or inside the silence window -> nothing else this
//      tick; counters do not advance.
//   3. link up and the post-connect counter (30) not yet expired -> count down, nothing else.
//   4. first connect of this boot: rundata with dataTime = now (faultdata is not implemented),
//      devinfo on the next tick (only with devinfo_on_connect, the S2 order; the WiNet-S sends
//      devinfo at every CONNACK instead, from the component's connect handler), propertydata of
//      every online device 180 ticks later.
//   5. rundata/5min: when local minute % period == 0 (period = sig 0x1015, 0 -> 5, forced to 5 for
//      the first 1800 s of a boot), snapshot dataTime (seconds = 0) and start a per-boot random
//      60..179 s countdown; send when it expires.
//   6. logger attributes (rsp/propertydata) every 86400 s, beatdata every 1800 s, both counted from
//      the first tick of the boot. The beat at CONNACK is sent by the connect handler, not here.

#include <cstdint>

#include "cloud_time.h"

namespace esphome::isolarcloud {

struct SchedulerInput {
  uint32_t mono_s{0};        ///< seconds since boot (monotonic)
  bool clock_valid{false};   ///< wall clock set (and plausible)
  CivilTime local;           ///< local time (UTC + 0x1089 offset)
  bool link_up{false};       ///< MQTT session up
  bool silenced{false};      ///< in_silence_window(...) for local.hour
  uint16_t period_min{5};    ///< sig 0x1015
};

struct SchedulerActions {
  bool rundata{false};            ///< send rundata for every online device + the logger
  bool first_connect{false};      ///< this rundata is the first-connect burst
  CivilTime data_time;            ///< dataTime for the rundata
  bool devinfo{false};            ///< pub/devinfo (logger)
  bool propertydata_all{false};   ///< rsp/propertydata for every online device + logger
  bool logger_attr{false};        ///< rsp/propertydata for the logger (24 h)
  bool beat{false};               ///< pub/beatdata
  bool any() const { return rundata || devinfo || propertydata_all || logger_attr || beat; }
};

struct SchedulerTiming {
  uint32_t post_connect_s{30};      ///< hold after the first CONNACK
  uint32_t attr_delay_s{180};       ///< propertydata of every device after the first connect
  uint32_t beat_s{1800};            ///< beatdata period
  uint32_t logger_attr_s{86400};    ///< logger propertydata period
  uint32_t forced_period_s{1800};   ///< period forced to 5 min for this long after boot
  bool devinfo_on_connect{true};    ///< devinfo after the first-connect rundata (S2; the WiNet-S
                                    ///< sends it at CONNACK instead)
};

class CloudScheduler {
 public:
  /// `random_delay_s` is the per-boot countdown (60..179 s).
  explicit CloudScheduler(uint32_t random_delay_s = 60, SchedulerTiming timing = {})
      : delay_s_(random_delay_s), t_(timing) {}
  void set_random_delay(uint32_t s) { this->delay_s_ = s; }
  void set_timing(const SchedulerTiming &t) { this->t_ = t; }
  uint32_t random_delay() const { return this->delay_s_; }

  /// Call once per second.
  SchedulerActions tick(const SchedulerInput &in);

  /// The effective rundata period in minutes for this tick.
  uint16_t effective_period(const SchedulerInput &in) const;
  bool first_connect_done() const { return this->first_done_; }

 protected:
  uint32_t delay_s_;
  SchedulerTiming t_;
  bool started_{false};
  uint32_t post_connect_left_{0};
  bool first_done_{false};
  bool devinfo_pending_{false};
  uint32_t attr_left_{0};
  // rundata boundary
  int64_t last_boundary_{-1};  ///< epoch minute of the last trigger
  bool countdown_active_{false};
  uint32_t countdown_left_{0};
  CivilTime snapshot_;
  // interval timers (mono seconds)
  uint32_t last_beat_s_{0};
  uint32_t last_logger_attr_s_{0};
};

/// Per-boot random delay: 60..179 s.
inline uint32_t stock_random_delay(uint32_t rnd) { return rnd % 120 + 60; }

}  // namespace esphome::isolarcloud
