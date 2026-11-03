#pragma once
// Usage reporting and the diagnostics report (docs/analytics.md).
//
// Usage: a `ping` with the firmware, board and feature facts plus one `device_registered` per
// connected device 60..120 s after boot and then daily; `error` events for notable faults, each
// key at most hourly. Sent as one PostHog-style batch to the collector.
// Diagnostics report: build, features, memory, every device with its identification reply,
// register block map and static register values, the reset journal and this boot's log tail,
// scrubbed of serials, the node name, the Wi-Fi name and addresses. Uploaded on request from
// the web UI (any time) and automatically on a crash-class reset, a runtime fault or a device
// discovery could not classify (only with `enabled`, at most three a day).
// Identity: the SHA-256 of the dongle serial number, or a random id kept in NVS.
// Transport: analytics_http (one worker task per upload, never the main loop).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"

#include "../sungrow_datamodel/sungrow_datamodel.h"
#include "../sungrow_modbus/sungrow_modbus.h"
#include "analytics_core.h"
#include "analytics_http.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef SUNGROW_ANALYTICS_BIND_EXTFLASH
#include "../sungrow_extflash/sungrow_extflash.h"
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CLOUD
#include "../isolarcloud/isolarcloud.h"
#endif
#ifdef SUNGROW_ANALYTICS_BIND_MQTT
#include "../sungrow_mqtt/sungrow_mqtt.h"
#endif
#ifdef SUNGROW_ANALYTICS_BIND_MODBUS_TCP
#include "../sungrow_modbus_tcp/sungrow_modbus_tcp.h"
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CONTROL
#include "../sungrow_control/sungrow_control.h"
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CAPTURE
#include "../sungrow_capture/sungrow_capture.h"
#endif

namespace esphome {
namespace sungrow_analytics {

enum class BundleState : uint8_t { IDLE, RUNNING, DONE, FAILED };

/// State of the most recent diagnostics upload (the web UI polls it).
struct BundleStatus {
  BundleState state{BundleState::IDLE};
  std::string id;       ///< the collector's reference for a DONE upload
  std::string error;    ///< short failure text for a FAILED upload
  std::string trigger;  ///< what started it ("manual", "crash", ...)
  uint32_t age_ms{0};   ///< since it started (RUNNING) or finished
};

class SungrowAnalytics : public Component {
 public:
  using BundleState = sungrow_analytics::BundleState;
  using BundleStatus = sungrow_analytics::BundleStatus;

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  // ---- configuration (generated code) --------------------------------------------------------
  /// Usage reporting and automatic uploads on/off; the YAML default, then the "Usage reporting"
  /// switch (ReportingSwitch) at runtime. Read live by the schedulers.
  void set_enabled(bool on) { this->enabled_ = on; }
  void set_endpoint(const char *url) { this->endpoint_ = url; }
  void set_api_key(const char *key) { this->api_key_ = key; }
  void set_modbus(sungrow_modbus::SungrowModbus *hub) { this->hub_ = hub; }
  void set_datamodel(sungrow_datamodel::SungrowDataModel *dm) { this->dm_ = dm; }
#ifdef USE_TIME
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
#endif
#ifdef SUNGROW_ANALYTICS_BIND_EXTFLASH
  void set_extflash(sungrow_extflash::SungrowExtFlash *f) { this->flash_ = f; }
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CLOUD
  void set_cloud(isolarcloud::IsolarcloudComponent *c) { this->cloud_ = c; }
#endif
#ifdef SUNGROW_ANALYTICS_BIND_MQTT
  void set_mqtt(sungrow_mqtt::SungrowMqttComponent *m) { this->mqtt_ = m; }
#endif
#ifdef SUNGROW_ANALYTICS_BIND_MODBUS_TCP
  void set_modbus_tcp(sungrow_modbus_tcp::SungrowModbusTcp *t) { this->mbtcp_ = t; }
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CONTROL
  void set_control(sungrow_control::SungrowControl *c) { this->control_ = c; }
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CAPTURE
  void set_capture(sungrow_capture::SungrowCapture *c) { this->capture_ = c; }
#endif

  // ---- API (web UI) --------------------------------------------------------------------------
  /// Usage reporting and automatic uploads are on (the YAML flag).
  bool enabled() const { return this->enabled_; }
  /// Upload the diagnostics report now. "manual" is accepted whatever the flag says; the
  /// automatic triggers need the flag and the daily cap. False (with `why`) when an upload is
  /// running, the cap is spent or the identity is not ready yet.
  bool request_bundle(const char *trigger, std::string *why);
  BundleStatus bundle_status() const;
  /// The scrubbed diagnostics report text.
  std::string build_bundle();
  /// Pseudonymous install id (hex SHA-256 of the dongle serial, or the stored random id).
  const std::string &distinct_id() const { return this->distinct_id_; }

 protected:
  static constexpr uint32_t SAMPLE_MS = 60000;
  static constexpr uint32_t ERROR_FLUSH_MS = 10000;
  static constexpr uint32_t BOOT_BUNDLE_DELAY_MS = 30000;
  static constexpr uint32_t OFFLINE_BUNDLE_MS = 10u * 60u * 1000u;
  static constexpr uint32_t DEVICE_BUNDLE_WAIT_MS = 5u * 60u * 1000u;
  static constexpr uint32_t PING_RETRY_MS = 3600000;
  static constexpr uint32_t HEAP_FLOOR = 24 * 1024;
  static constexpr uint8_t HEAP_SAMPLES = 3;
  static constexpr uint8_t CLOUD_SAMPLES = 15;
  static constexpr uint32_t MODBUS_BURST = 30;

  /// NVS record: the fallback id, the daily upload count and the device codes a `device`
  /// report was already uploaded for (one upload per unrecognised device code per install).
  static constexpr size_t DEVICE_KEYS_MAX = 8;
  struct Pref {
    uint32_t magic;
    uint8_t fallback_id[32];
    uint32_t window_start_epoch;
    uint8_t uploads;
    uint8_t device_keys_n;
    uint8_t pad[2];
    uint16_t device_keys[DEVICE_KEYS_MAX];
  };
  static constexpr uint32_t PREF_MAGIC = 0x414E4C32u;  // "ANL2"
  bool device_key_reported_(uint16_t key) const;
  void remember_device_key_(uint16_t key);

  struct DevState {
    bool was_online{false};
    uint32_t offline_since_ms{0};
    bool offline_reported{false};
    uint32_t last_fail_total{0};
    bool fails_primed{false};  ///< last_fail_total holds a sample taken after the first poll round
    bool burst_reported{false};
    bool catalogue_checked{false};
  };
  struct Unsupported {
    uint8_t index;
    uint16_t key;  ///< devinfo device code (the per-install upload memory is keyed on it)
    uint32_t at_ms;
    bool bundled;
  };

  void use_fallback_id_();
  void set_distinct_from_sn_(const std::string &sn);
  void save_pref_();
  bool time_valid_() const;
  std::string now_rfc3339_() const;
  uint32_t now_epoch_() const;
  void poll_jobs_(uint32_t now);
  void send_ping_(uint32_t now);
  void send_errors_(uint32_t now);
  bool start_batch_(const std::string &body);
  void report_error_(const char *type, const std::string &error, const std::string &device, uint32_t now);
  void sample_(uint32_t now);
  void check_boot_trigger_(uint32_t now);
  void check_device_trigger_(uint32_t now);
  void run_pending_bundle_(uint32_t now);
  bool request_auto_bundle_(const char *trigger, uint32_t now);
  bool start_bundle_(const char *trigger, uint32_t now, std::string *why);
  void on_unsupported_(const sungrow_modbus::Device &dev, const char *why);
  void fill_ping_(PingProps *p) const;
  void fill_devices_(std::vector<DeviceEvent> *out) const;
  void fill_bundle_device_(const sungrow_modbus::Device &d, uint32_t now, BundleDevice *out) const;
  void fill_scrubber_();
  std::string signal_text_(uint8_t device, uint16_t sigid) const;

  bool enabled_{true};
  std::string endpoint_{"https://posthog.nickstallman.net"};
  std::string api_key_;
  sungrow_modbus::SungrowModbus *hub_{nullptr};
  sungrow_datamodel::SungrowDataModel *dm_{nullptr};
#ifdef USE_TIME
  time::RealTimeClock *time_{nullptr};
#endif
#ifdef SUNGROW_ANALYTICS_BIND_EXTFLASH
  sungrow_extflash::SungrowExtFlash *flash_{nullptr};
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CLOUD
  isolarcloud::IsolarcloudComponent *cloud_{nullptr};
#endif
#ifdef SUNGROW_ANALYTICS_BIND_MQTT
  sungrow_mqtt::SungrowMqttComponent *mqtt_{nullptr};
#endif
#ifdef SUNGROW_ANALYTICS_BIND_MODBUS_TCP
  sungrow_modbus_tcp::SungrowModbusTcp *mbtcp_{nullptr};
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CONTROL
  sungrow_control::SungrowControl *control_{nullptr};
#endif
#ifdef SUNGROW_ANALYTICS_BIND_CAPTURE
  sungrow_capture::SungrowCapture *capture_{nullptr};
#endif

  ESPPreferenceObject pref_;
  Pref pref_data_{};
  std::string distinct_id_;
  std::string dongle_sn_;
  Scrubber scrubber_;
  ErrorThrottle throttle_;
  Schedule schedule_;
  UploadCap cap_;
  std::vector<ErrorEvent> errors_;
  std::vector<DevState> dev_state_;
  std::vector<Unsupported> unsupported_;
  std::unique_ptr<AnalyticsJob> batch_job_;
  std::unique_ptr<AnalyticsJob> bundle_job_;
  bool batch_is_ping_{false};
  bool key_rejected_{false};
  uint32_t last_tick_ms_{0};
  uint32_t last_sample_ms_{0};
  uint32_t last_error_send_ms_{0};
  uint32_t net_up_ms_{0};
  uint32_t pings_sent_{0};
  const char *boot_trigger_{nullptr};
  std::string boot_crash_class_;
  bool boot_bundle_done_{false};
  bool cloud_reported_{false};
  uint8_t cloud_bad_samples_{0};
  bool heap_reported_{false};
  uint8_t heap_bad_samples_{0};
  bool device_bundle_done_{false};
  std::string pending_trigger_;
  // Status of the latest upload.
  BundleState bundle_state_{BundleState::IDLE};
  std::string bundle_id_;
  std::string bundle_error_;
  std::string bundle_trigger_;
  uint32_t bundle_stamp_ms_{0};
};

#ifdef USE_SWITCH
/// "Usage reporting" (restored, default on): turns the pings, error events and automatic uploads
/// off without a rebuild; manual Send diagnostics keeps working. Pattern of isolarcloud's UplinkSwitch.
class ReportingSwitch : public switch_::Switch, public Component, public Parented<SungrowAnalytics> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void write_state(bool state) override;
};
#endif

}  // namespace sungrow_analytics
}  // namespace esphome
