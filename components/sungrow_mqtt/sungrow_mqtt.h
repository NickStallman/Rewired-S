#pragma once
// sungrow_mqtt: per-device Home Assistant MQTT discovery for the connected Sungrow devices.
//
// One Modbus device (inverter / battery / meter / charger / ...) -> one separate HA device, via
// MQTT discovery to a LOCAL broker. Modelled on the winet-extractor project
// (home-assistant-repo/winet-extractor/src/{homeassistant,index}.ts): same topic layout, naming
// and payloads, plus our own additions (sw_version, via_device the dongle). States are RETAINED
// with per-signal payload dedup + a periodic full refresh; a device that goes offline keeps its
// retained configs and only flips availability=offline (NEVER auto-deleted).
//
// This is a SECOND esp_mqtt client alongside isolarcloud's cloud uplink (confirmed safe: esp-mqtt
// has no singletons, each client owns its task/context). The esp-mqtt pattern is borrowed from
// isolarcloud.cpp: per-instance esp_mqtt_client_handle_t; a MQTT_EVENT_ANY handler that runs on the
// mqtt task and only touches atomics / mutex-guarded queues; everything else happens in loop().
// Keep the buffers and outbox SMALL: a LAN broker needs no backlog.
//
// Data source: sungrow_datamodel::SignalStore (values) + discovery_meta.h (presentation, baked
// from register_meta.py at codegen). Device inventory + lifecycle: sungrow_modbus::ModbusMaster
// (MasterListener on_discovered / on_online_changed). Dongle SN (publisher root / via_device):
// sungrow_extflash. Writes: per-device number/select/switch discovery for the writable subset,
// commands routed through the sungrow_control gate (behind the writes_enabled switch, default OFF).

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <mqtt_client.h>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"

#include "esphome/components/sungrow_datamodel/sungrow_datamodel.h"
#include "esphome/components/sungrow_modbus/sungrow_modbus.h"

#include "discovery_meta.h"
#include "mqtt_settings.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#ifdef SUNGROW_MQTT_BIND_EXTFLASH
#include "esphome/components/sungrow_extflash/sungrow_extflash.h"
#endif
#ifdef SUNGROW_MQTT_BIND_CONTROL
#include "esphome/components/sungrow_control/sungrow_control.h"
#include "esphome/components/sungrow_control/write_entities.h"
#endif

namespace esphome {
namespace sungrow_mqtt {

using sungrow_datamodel::Family;
using sungrow_datamodel::SignalStore;
using sungrow_modbus::Device;
using sungrow_modbus::ModbusMaster;

/// DM serial sigid, shared by every family's `serial_number` row (register_meta 0x0002).
static constexpr uint16_t SIG_SERIAL = 0x0002;

/// Per Modbus device: its resolved HA identity and what discovery/state we have published.
/// Discovery is published only once the device is SETTLED (see device_settled_): nothing is ever
/// published under a provisional identity, so there is no retract/upgrade churn and no fallback.
struct DevicePub {
  uint8_t index{0};          ///< SignalStore / master device index
  uint8_t address{1};        ///< Modbus slave address
  Family family{Family::INVERTER};
  std::string model;         ///< DevInfo.model (device_slug + HA model)
  std::string serial;        ///< the inverter's own serial, polled from sigid 0x0002 (never a fallback)
  std::string device_slug;   ///< "<model>_<serial>" stripped to [A-Za-z0-9]; set only once settled
  bool serial_valid{false};  ///< sigid 0x0002 resolved to a non-empty serial
  bool known{false};         ///< on_discovered fired (DevInfo populated)
  bool online{false};
  bool first_round{false};    ///< the device's first full poll round completed (read set populated)
  bool discovery_done{false};     ///< the settled set has been published (this device is ours now)
  uint32_t offline_since_ms{0};   ///< 0 = online; else millis() when it went offline
  std::set<uint16_t> configured;  ///< read sigids whose discovery config has been published
  std::set<uint16_t> write_configured;  ///< write sigids (0xF1xx) whose control discovery is published
  bool problem_configured{false};  ///< the "Problem" binary_sensor discovery has been published
};

// The modbus master has a single MasterListener slot, which the SungrowModbus hub itself holds
// (sungrow_modbus.cpp sets master().set_listener(this)). sync_inventory_() therefore polls
// master().devices() every loop and detects discovered / online transitions against the cached
// DevicePub state.
class SungrowMqttComponent : public Component {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;

  // ---- configuration (codegen) ----
  // The YAML values land in yaml_; setup() resolves them against the stored runtime record into
  // the live broker_/port_/username_/password_/prefix_/tls_ (resolve_mqtt_settings).
  void set_broker(const char *host) { this->yaml_.host = host; }
  void set_port(uint16_t port) { this->yaml_.port = port; }
  void set_credentials(const char *user, const char *pass) {
    this->yaml_.user = user;
    this->yaml_.pass = pass;
  }
  void set_discovery_prefix(const char *p) { this->yaml_.prefix = p; }
  void set_tls(bool on) { this->yaml_.tls = on; }
  void set_runtime_settings(bool on) { this->runtime_settings_ = on; }
#ifdef USE_BINARY_SENSOR
  void set_connected_binary_sensor(binary_sensor::BinarySensor *s) { this->connected_sensor_ = s; }
#endif
  void set_ca_certificate(const char *pem) { this->ca_certificate_ = pem; }
  void set_sw_version(const char *v) { this->sw_version_ = v; }
  void set_state_throttle(uint32_t ms) { this->state_throttle_ms_ = ms; }
  void set_settle_grace(uint32_t ms) { this->settle_grace_ms_ = ms; }
  void set_deadband_scale(float s) { this->deadband_scale_ = s; }

  /// The broker session is up (set from the MQTT task; read from any task).
  bool is_connected() const { return this->connected_.load(); }

  // ---- runtime settings (web UI; loop task only) ----
  /// A broker is known (YAML or stored): the client runs. False = idle until one is set.
  bool is_configured() const { return !this->broker_.empty(); }
  bool runtime_settings() const { return this->runtime_settings_; }
  MqttSettingsSource settings_source() const { return this->source_; }
  /// The live settings (the password included; callers must not expose it).
  const MqttSettings &settings() const { return this->live_; }
  /// Short text of the last transport / CONNACK error, "" after a successful connect.
  const std::string &last_error() const { return this->last_error_; }
  /// Validate, store in NVS and apply `s`: the client is torn down, every device's discovery and
  /// state bookkeeping reset, and a new client started against the new broker (an empty host
  /// leaves it idle). False with `why` when the record is invalid or YAML names the broker.
  bool apply_settings(const MqttSettings &s, std::string *why);

  void set_datamodel(sungrow_datamodel::SungrowDataModel *dm) { this->dm_ = dm; }
  void set_modbus(sungrow_modbus::SungrowModbus *hub) { this->hub_ = hub; }
#ifdef SUNGROW_MQTT_BIND_EXTFLASH
  void set_extflash(sungrow_extflash::SungrowExtFlash *f) { this->flash_ = f; }
#endif
#ifdef SUNGROW_MQTT_BIND_CONTROL
  void set_control(sungrow_control::SungrowControl *c) { this->control_ = c; }
#endif

 protected:
  // connection (mqtt task vs loop() split mirrors isolarcloud)
  void start_client_();
  void stop_client_(const char *why);
  static void mqtt_event_handler_(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
  void handle_event_(esp_mqtt_event_handle_t event);
  int publish_(const std::string &topic, const std::string &payload, int qos, bool retain);
  void subscribe_(const std::string &topic, int qos);
  /// Back-pressure for every bulk publisher: true while the esp-mqtt outbox holds less than
  /// OUTBOX_HIGH_WATER bytes. esp-mqtt drains about one QoS-1 message per broker round trip, far
  /// slower than loop() can enqueue, so each drain stops for the loop once the mark is reached and
  /// resumes next loop; nothing is dropped and the hard outbox limit is never hit.
  bool outbox_has_room_() const;

  // identity / topics
  std::string dongle_id_() const;  ///< slug of the dongle SN (publisher root + via_device)
  std::string status_topic_() const { return "sungrow-mqtt/" + this->dongle_id_() + "/status"; }
  std::string availability_topic_(const std::string &device_slug) const {
    return "sungrow-mqtt/" + this->dongle_id_() + "/" + device_slug + "/availability";
  }
  std::string state_topic_(const std::string &device_slug, const char *slug) const {
    return this->prefix_ + "/sensor/" + device_slug + "/" + slug + "/state";
  }
  std::string config_topic_(const std::string &device_slug, const char *slug) const {
    return this->prefix_ + "/sensor/" + device_slug + "/" + slug + "/config";
  }
  std::string command_topic_(const std::string &device_slug, const char *slug) const {
    return "sungrow-mqtt/" + this->dongle_id_() + "/" + device_slug + "/" + slug + "/set";
  }
  std::string result_topic_(const std::string &device_slug, const char *slug) const {
    return "sungrow-mqtt/" + this->dongle_id_() + "/" + device_slug + "/" + slug + "/result";
  }
  /// Writable-entity discovery/state live under the entity's own HA component (number/select/switch).
  std::string write_config_topic_(const char *comp, const std::string &device_slug, const char *slug) const {
    return this->prefix_ + "/" + comp + "/" + device_slug + "/" + slug + "/config";
  }
  std::string write_state_topic_(const char *comp, const std::string &device_slug, const char *slug) const {
    return this->prefix_ + "/" + comp + "/" + device_slug + "/" + slug + "/state";
  }
  /// The per-device "Problem" binary_sensor (inverter and battery families): `leaf` = config | state.
  std::string problem_topic_(const std::string &device_slug, const char *leaf) const {
    return this->prefix_ + "/binary_sensor/" + device_slug + "/problem/" + leaf;
  }

  // lifecycle / publishing (loop side)
  DevicePub *dev_for_index_(uint8_t index);
  void sync_inventory_();                  ///< pull master().devices() into pubs_
  /// True once EVERYTHING needed to publish the device's complete, final discovery set is in hand:
  /// discovered + online, its first poll round done (read set populated), serial (0x0002) valid,
  /// and (when control is bound) its write profile attached. Nothing is published before this.
  bool device_settled_(const DevicePub &d) const;
  bool resolve_serial_(DevicePub &d);      ///< fill serial/device_slug from valid 0x0002; false otherwise
  /// Boot gate: the devices of one bus settle within seconds of each other, so the first settled
  /// device waits at least settle_grace_ms_, and longer (up to SETTLE_MAX_HOLD_MS) while a discovered
  /// online device is still settling; then every settled device is published in ONE pass (device by
  /// device, configs then states, outbox-paced). A device that settles later is published on its own.
  void publish_settled_();
  void publish_device_(DevicePub &d);      ///< settled: publish availability, then stream the complete
                                           ///< read + write discovery set in via back-fill (no churn)
  void backfill_configs_();                ///< config+state any still-unconfigured valid signals,
                                           ///< bounded per loop so a single loop never stalls
  bool publish_signal_config_(DevicePub &d, const SensorMeta &m);
  /// Publish a read signal's state (RETAINED). force bypasses the per-signal payload dedup. Returns
  /// true if something was actually sent (false on dedup-skip, invalid value, or enqueue failure).
  bool publish_signal_state_(const DevicePub &d, const SensorMeta &m, bool force);
  /// RETAINED state publish with per-(device,sigid) payload dedup; force bypasses the dedup.
  bool publish_state_payload_(uint32_t key, const std::string &topic, const std::string &payload, bool force);
  void set_availability_(DevicePub &d, bool online);
  void republish_all_(bool include_states);
  void enqueue_full_refresh_();            ///< queue every current state for a forced heartbeat re-publish
  void drain_refresh_();                   ///< drain that heartbeat queue, bounded per loop
  void on_signal_changed_(const sungrow_datamodel::Signal &s);  ///< SignalStore listener (loop ctx)
  void drain_dirty_();                     ///< publish queued state changes, bounded per loop

  /// "Problem" binary_sensor: ON while the device reports active faults (0xF060 > 0); its JSON
  /// attributes carry the count, the codes and names split from the 0xF061 text and the first
  /// fault (0xF065). Published with the device's discovery set, on a change of any of the three
  /// signals and in the full refresh; same payload dedup as the sensors (key PROBLEM_KEY).
  static bool has_problem_sensor_(Family f) { return f == Family::INVERTER || f == Family::BATTERY; }
  bool publish_problem_config_(DevicePub &d);
  bool publish_problem_state_(const DevicePub &d, bool force);
  std::string build_problem_config_json_(const DevicePub &d) const;
  std::string build_problem_state_json_(const DevicePub &d) const;

  // JSON builders (hand-built, small; avoids a heap-heavy JSON lib on the LAN path)
  std::string build_config_json_(const DevicePub &d, const SensorMeta &m) const;
  void append_device_block_(std::string &j, const DevicePub &d) const;  ///< shared HA device{} + availability
  static std::string build_state_json_(double value, const char *unit);
  static std::string build_state_json_str_(const std::string &value);

  // writes
  void subscribe_commands_(const DevicePub &d);
  void handle_command_(const std::string &topic, const std::string &payload);
#ifdef SUNGROW_MQTT_BIND_CONTROL
  const char *write_component_(sungrow_control::WriteKind k) const;
  const sungrow_control::WriteEntity *write_entities_for_(const DevicePub &d, uint8_t *count) const;
  void backfill_write_configs_();  ///< publish control (number/select/switch) discovery, bounded
  bool publish_write_config_(DevicePub &d, const sungrow_control::WriteEntity &e);
  void publish_write_state_(const DevicePub &d, const sungrow_control::WriteEntity &e, bool force);
  std::string build_write_config_json_(const DevicePub &d, const sungrow_control::WriteEntity &e) const;
  /// After an accepted write, briefly watch its read-back sigid (note_write_settle_) and re-publish
  /// the entity state (deduped) each loop until it reflects the new value (drain_settle_) -- so HA
  /// follows the store within ~1s regardless of whether the verify-read or the 10s block-poll lands
  /// it, without relying on the change-notify (which can be dropped) or write_configured timing.
  void note_write_settle_(uint8_t index, uint16_t sigid);
  void drain_settle_();
  /// Start/stop follow-up on the control's result topic: {"ok","status","state","final"}.
  void publish_expect_(uint8_t index, const sungrow_control::ExpectStatus &st);
#endif

  /// Forget everything published so the next session re-publishes every device from scratch
  /// (settings change: the new broker has seen nothing).
  void reset_publish_state_();
  /// Copy `live_` into the fields the client and topic builders read.
  void adopt_live_();
  bool load_stored_settings_(MqttSettings *out);

  // --- config
  MqttSettings yaml_;   ///< as configured
  MqttSettings live_;   ///< what applies (resolve_mqtt_settings)
  MqttSettingsSource source_{MqttSettingsSource::NONE};
  bool runtime_settings_{false};
  ESPPreferenceObject settings_pref_;
  std::string broker_;
  uint16_t port_{1883};
  std::string username_;
  std::string password_;
  std::string prefix_{"homeassistant"};
  bool tls_{false};
  const char *ca_certificate_{nullptr};
  std::string sw_version_;
  uint32_t state_throttle_ms_{1000};
  uint32_t full_refresh_ms_{60000};            ///< periodic forced re-publish of every current state
  uint32_t settle_grace_ms_{5000};             ///< minimum hold after the first settled device before the one-pass publish
  float deadband_scale_{1.0f};                 ///< multiplies SensorMeta::deadband; 0 = publish every change

  // --- bindings
  sungrow_datamodel::SungrowDataModel *dm_{nullptr};
  sungrow_modbus::SungrowModbus *hub_{nullptr};
#ifdef SUNGROW_MQTT_BIND_EXTFLASH
  sungrow_extflash::SungrowExtFlash *flash_{nullptr};
#endif
#ifdef SUNGROW_MQTT_BIND_CONTROL
  sungrow_control::SungrowControl *control_{nullptr};
#endif

  // --- state
  esp_mqtt_client_handle_t client_{nullptr};
  std::string will_topic_;
  bool session_up_{false};  ///< loop-side view of connected_
  std::string cached_dongle_id_;

  std::vector<DevicePub> pubs_;  ///< one per master device (index == pubs_ position by sync)

#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
#endif

  // events from the mqtt task -> loop()
  std::atomic<bool> connected_{false};
  std::atomic<bool> connect_event_{false};
  std::atomic<bool> disconnect_event_{false};
  // MQTT_EVENT_ERROR, captured on the mqtt task as plain integers; loop() turns them into
  // last_error_ (the same atomic/loop split as the connect event; never a string across tasks).
  std::atomic<bool> error_event_{false};
  std::atomic<int> err_type_{0};
  std::atomic<int> err_connect_code_{0};
  std::atomic<int> err_tls_{0};
  std::atomic<int> err_errno_{0};
  std::string last_error_;
  std::atomic<bool> ha_birth_event_{false};  ///< `<prefix>/status` == "online" seen
  std::mutex rx_mutex_;
  struct Inbound { std::string topic, data; };
  std::deque<Inbound> rx_queue_;  ///< inbound command messages (writes); bounded

  // dirty state queue (SignalStore change -> publish in loop, throttled)
  std::mutex dirty_mutex_;
  std::deque<uint32_t> dirty_;        ///< packed (device<<16 | sigid)
  std::set<uint32_t> dirty_set_;      ///< dedup membership
  std::map<uint32_t, uint32_t> last_state_ms_;  ///< per-signal last publish (throttle)
  std::map<uint32_t, std::string> last_payload_;  ///< per-signal last published state payload (dedup)
  std::map<uint32_t, double> last_value_;      ///< per-signal last published numeric value (deadband)
  std::deque<uint32_t> refresh_;      ///< periodic forced-refresh queue (device<<16 | sigid)
  uint32_t last_full_refresh_ms_{0};
  uint32_t enqueue_fail_log_ms_{0};   ///< rate-limit the enqueue-failure warning
  uint32_t first_settled_ms_{0};      ///< millis() when the first device settled (boot gate start); 0 = none yet
  bool initial_publish_done_{false};  ///< the boot one-pass publish has happened
  bool backfill_pending_{false};      ///< some published device still has unconfigured valid signals

#ifdef SUNGROW_MQTT_BIND_CONTROL
  // Post-write "settle watch": loop-thread only (handle_command_ + drain_settle_ both run in loop()).
  struct WriteSettle {
    uint8_t index;
    uint16_t sigid;
    uint32_t deadline_ms;
  };
  std::vector<WriteSettle> write_settle_;
  uint32_t last_settle_ms_{0};  ///< throttle drain_settle_ re-checks (ms)
#endif

  bool listener_installed_{false};

  /// Sigid slot of the Problem sensor in the refresh queue and the payload-dedup map (no signal
  /// uses it).
  static constexpr uint16_t PROBLEM_KEY = 0xFFFF;
  static constexpr size_t RX_MAX_QUEUE = 16;
  static constexpr size_t DIRTY_MAX = 512;
  static constexpr size_t DRAIN_PER_LOOP = 8;  ///< bound work per loop, keep the bus quiet
  static constexpr size_t CONFIG_PER_LOOP = 4;  ///< bound retained config publishes per loop (keeps a loop under ~50 ms)
  static constexpr size_t REFRESH_PER_LOOP = 4;  ///< bound heartbeat state re-publishes per loop
  static constexpr size_t OUTBOX_HIGH_WATER = 12 * 1024;  ///< pause bulk publishing above this outbox fill
  static constexpr uint32_t SETTLE_MAX_HOLD_MS = 30000;   ///< longest boot hold for devices still settling
  static constexpr uint32_t SETTLE_WINDOW_MS = 20000;  ///< watch a just-written entity this long (covers the 10s block-poll)
  static constexpr size_t SETTLE_MAX = 32;       ///< bound concurrent settle watches
};

}  // namespace sungrow_mqtt
}  // namespace esphome
