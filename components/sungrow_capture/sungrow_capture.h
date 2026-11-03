#pragma once
// ESPHome glue for the diagnostic captures (capture_core.h): I-V string scan, grid-waveform
// recording and fault-recording export, started from the web UI (sungrow_web /api/capture*),
// results kept in RAM (PSRAM) until reboot and downloaded raw for analysis. Not exposed to
// MQTT / Home Assistant.
//
// The inverter writes go through ControlCore::write_diag (its own fixed allow-list, no
// writes-enabled switch: the web confirm token authorises a capture).
#include <memory>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "../sungrow_control/sungrow_control.h"
#include "../sungrow_datamodel/sungrow_datamodel.h"
#include "../sungrow_modbus/sungrow_modbus.h"
#include "capture_core.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif

namespace esphome {
namespace sungrow_capture {

class SungrowCapture : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the hub (DATA) and sungrow_control (DATA - 1).
  float get_setup_priority() const override { return setup_priority::DATA - 2.0f; }

  void set_modbus(sungrow_modbus::SungrowModbus *m) { this->modbus_ = m; }
  void set_control(sungrow_control::SungrowControl *c) { this->control_ = c; }
  void set_datamodel(sungrow_datamodel::SungrowDataModel *dm) { this->dm_ = dm; }
#ifdef USE_TIME
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
#endif

  /// Main loop only. `device` is the SignalStore device index of an inverter.
  bool start_iv(uint8_t device, std::string *why);
  bool start_grid(uint8_t device, uint8_t cycles, std::string *why);
  bool start_fault(uint8_t device, std::string *why);
  void cancel() { this->core_.cancel(); }
  /// {"available":true,"running":B[,"device":D,"kind":K,"progress":P,"elapsed_ms":T,"step":S
  ///  [,"scan_ms":N,"iv_fault":F,"iv_fault_text":"..."|null (I-V only; null while F is 0)]],
  ///  "iv_starts":N,"generation":N,
  ///  "devices":[{"device":D,"online":B,"iv":B,"grid":B,"fault":B,"mppt":N,"model":"..."}],
  ///  "results":[{"id":N,"kind":K,"ok":B,"size":N,"cycles":N,"records":N,"wall_start":T,
  ///              "duration_ms":T,"result":"...","file_name":"...","inverter":"..."}]}
  /// with K = "iv" | "grid" | "fault"; results newest first.
  std::string status_json();
  std::shared_ptr<const Capture> find(uint32_t id) const { return this->core_.find(id); }
  CaptureCore &core() { return this->core_; }

 protected:
  bool resolve_(uint8_t device, uint8_t *addr, std::string *label, std::string *why);
  sungrow_modbus::SungrowModbus *modbus_{nullptr};
  sungrow_control::SungrowControl *control_{nullptr};
  sungrow_datamodel::SungrowDataModel *dm_{nullptr};
#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif
  CaptureCore core_;
  uint8_t core_device_{0};  ///< store index of the inverter the running capture writes to
  HighFrequencyLoopRequester fast_loop_;
};

}  // namespace sungrow_capture
}  // namespace esphome
