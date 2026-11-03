#include "scheduler.h"

namespace esphome::isolarcloud {

uint16_t CloudScheduler::effective_period(const SchedulerInput &in) const {
  if (in.period_min == 0 || in.mono_s < this->t_.forced_period_s)
    return 5;
  return in.period_min;
}

SchedulerActions CloudScheduler::tick(const SchedulerInput &in) {
  SchedulerActions a;
  if (!this->started_) {
    // Interval timers start at the first tick of the boot; the post-connect hold is armed once.
    this->started_ = true;
    this->last_beat_s_ = in.mono_s;
    this->last_logger_attr_s_ = in.mono_s;
    this->post_connect_left_ = this->t_.post_connect_s;
  }
  // 2. no valid clock or silenced: nothing (the caller still pumps MQTT / inbound).
  if (!in.clock_valid || !clock_plausible(in.local) || in.silenced)
    return a;
  // 3. first-connect burst held back after CONNACK.
  if (in.link_up && this->post_connect_left_ > 0) {
    this->post_connect_left_--;
    return a;
  }
  // 4. first connect of the boot.
  if (in.link_up && !this->first_done_) {
    this->first_done_ = true;
    a.rundata = true;
    a.first_connect = true;
    a.data_time = in.local;
    this->devinfo_pending_ = this->t_.devinfo_on_connect;  // next tick
    this->attr_left_ = this->t_.attr_delay_s;
  } else if (this->devinfo_pending_ && in.link_up) {
    this->devinfo_pending_ = false;
    a.devinfo = true;
  }
  if (this->attr_left_ > 0 && !a.first_connect) {
    if (--this->attr_left_ == 0)
      a.propertydata_all = true;
  }

  // 5. rundata on the wall clock.
  const uint16_t period = this->effective_period(in);
  CivilTime minute_start = in.local;
  minute_start.second = 0;
  const int64_t epoch_min = epoch_from_civil(minute_start) / 60;
  if (in.local.minute % period == 0 && epoch_min != this->last_boundary_) {
    this->last_boundary_ = epoch_min;
    if (this->countdown_active_ && !a.rundata) {
      // The next boundary came before the countdown ran out (period 1 with a delay > 60 s): the
      // pending snapshot is sent now rather than lost.
      a.rundata = true;
      a.data_time = this->snapshot_;
    }
    this->snapshot_ = minute_start;
    this->countdown_active_ = true;
    this->countdown_left_ = this->delay_s_;
  } else if (this->countdown_active_) {
    if (this->countdown_left_ > 0)
      this->countdown_left_--;
    if (this->countdown_left_ == 0) {
      this->countdown_active_ = false;
      if (in.link_up && !a.rundata) {
        a.rundata = true;
        a.data_time = this->snapshot_;
      }
    }
  }

  // 6. long-period timers (not gated on the link; the publish itself needs it).
  if (in.mono_s - this->last_logger_attr_s_ >= this->t_.logger_attr_s) {
    this->last_logger_attr_s_ = in.mono_s;
    a.logger_attr = in.link_up;
  }
  if (in.mono_s - this->last_beat_s_ >= this->t_.beat_s) {
    this->last_beat_s_ = in.mono_s;
    a.beat = in.link_up;
  }
  if (!in.link_up) {
    a.rundata = a.devinfo = a.propertydata_all = false;
  }
  return a;
}

}  // namespace esphome::isolarcloud
