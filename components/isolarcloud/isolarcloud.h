#pragma once
// isolarcloud: the iSolarCloud MQTT uplink, acting as the stock WiNet dongle.
//
// Protocol pieces live in host-testable files (topics, envelope, zcodec, inbound, commands,
// server_switch, rundata_format, scheduler, publish_queue, cloud_time); this class owns the IDF
// esp_mqtt client, persistence, entities and the glue. Protocol reference: SG-Docs
// reference/protocols/cloud-*.md; identity details in docs/cloud_identity.md.

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <mqtt_client.h>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"
#include "esphome/components/time/real_time_clock.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

#ifdef ISOLARCLOUD_BIND_EXTFLASH
#include "esphome/components/sungrow_extflash/sungrow_extflash.h"
#endif
#ifdef ISOLARCLOUD_BIND_DATAMODEL
#include "esphome/components/sungrow_datamodel/sungrow_datamodel.h"
#endif
#ifdef ISOLARCLOUD_BIND_MODBUS
#include "esphome/components/sungrow_modbus/sungrow_modbus.h"
#endif

#include "adapters.h"
#include "cloud_time.h"
#include "cloud_types.h"
#include "commands.h"
#include "envelope.h"
#include "inbound.h"
#include "interfaces.h"
#include "publish_queue.h"
#include "rundata_format.h"
#include "scheduler.h"
#include "server_switch.h"
#include "topics.h"

namespace esphome::isolarcloud {

/// CA file for the broker TLS chain on the stock filesystem.
static constexpr const char *STOCK_CA_FILE = "/usrdataout/tlsca/cafile";

#ifdef ISOLARCLOUD_BIND_EXTFLASH
/// IdentitySource over sungrow_extflash: only values read from the sigdata file (plus a plausible
/// SN) are reported, so extflash's stock defaults never override YAML or persisted values.
class ExtflashIdentitySource : public IdentitySource {
 public:
  explicit ExtflashIdentitySource(sungrow_extflash::SungrowExtFlash *flash) : flash_(flash) {}
  bool identity_ready() const override { return this->flash_->identity_loaded(); }
  uint32_t read_identity(CloudIdentity &out) const override {
    return this->identity_ready() ? map_extflash_identity(this->flash_->identity(), out) : 0;
  }
  /// Fires immediately when extflash already loaded (it reads synchronously in its setup()).
  void add_on_loaded_callback(std::function<void()> &&cb) override {
    this->flash_->add_on_identity_callback([cb = std::move(cb)](const sungrow_extflash::Identity &) { cb(); });
  }
  /// Files read through here (the broker CA) are a few KB; anything over 16 KB is refused.
  bool read_file(const std::string &path, std::vector<uint8_t> &out) override {
    return this->flash_->read_file(path, out, 16 * 1024);
  }

 protected:
  sungrow_extflash::SungrowExtFlash *flash_;
};
#endif

class IsolarcloudComponent : public Component, protected MqttPublisher {
 public:
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;

  // ---- configuration (codegen) ----
  // Explicit YAML identity values (each also marks the field as "set by YAML" = highest precedence).
  void set_sn(const char *v) { this->yaml_identity_.sn = v; this->yaml_mask_ |= FIELD_SN; }
  void set_product_type(uint16_t v) { this->yaml_identity_.product_type = v; this->yaml_mask_ |= FIELD_PRODUCT_TYPE; }
  void set_product_name(const char *v) { this->yaml_identity_.product_name = v; this->yaml_mask_ |= FIELD_PRODUCT_NAME; }
  void set_encrypt(uint16_t v) { this->yaml_identity_.encrypt = v; this->yaml_mask_ |= FIELD_ENCRYPT; }
  void set_keynum(uint16_t v) { this->yaml_identity_.keynum = v; this->yaml_mask_ |= FIELD_KEYNUM; }
  void set_compress(uint16_t v) { this->yaml_identity_.compress = v; this->yaml_mask_ |= FIELD_COMPRESS; }
  void set_broker_host(const char *v) { this->yaml_identity_.server.host = v; this->yaml_mask_ |= FIELD_SERVER_HOST; }
  void set_broker_port(uint16_t v) { this->yaml_identity_.server.port = v; this->yaml_mask_ |= FIELD_SERVER_PORT; }
  void set_ssl_flag(uint16_t v) { this->yaml_identity_.server.ssl_flag = v; this->yaml_mask_ |= FIELD_SERVER_SSL; }
  void set_mqtt_key(const char *v) {
    this->yaml_identity_.server.key = key_from_string(v);
    this->yaml_mask_ |= FIELD_SERVER_KEY;
  }
  void set_timezone_id(uint16_t v) { this->yaml_identity_.timezone_id = v; this->yaml_mask_ |= FIELD_TIMEZONE_ID; }
  /// Cloud-style timezone string ("UTC+10:00" = 10 h ahead of UTC), sig 0x1089.
  void set_timezone(const char *v) { this->yaml_identity_.timezone = v; this->yaml_mask_ |= FIELD_TIMEZONE; }
  void set_rundata_period_min(uint16_t v) {
    this->yaml_identity_.rundata_period_min = v;
    this->yaml_mask_ |= FIELD_RUNDATA_PERIOD;
  }
  void set_silence(bool enable, uint8_t start_hour, uint8_t end_hour) {
    this->yaml_identity_.silence_enable = enable;
    this->yaml_identity_.silence_start_hour = start_hour;
    this->yaml_identity_.silence_end_hour = end_hour;
    this->yaml_mask_ |= FIELD_SILENCE;
  }
  void set_remote_ctl_mask(uint32_t v) {
    this->yaml_identity_.remote_ctl_mask = v;
    this->yaml_mask_ |= FIELD_REMOTE_CTL_MASK;
  }
  void set_credentials(const char *username, const char *password) {
    this->username_ = username;
    this->password_ = password;
  }
  void set_verify_server(bool v) { this->verify_server_ = v; }
  void set_ca_certificate(const char *pem) { this->ca_certificate_ = pem; }
  void set_time(time::RealTimeClock *t) { this->time_ = t; }
  void set_beat_interval(uint32_t ms) { this->timing_.beat_s = std::max<uint32_t>(1, ms / 1000); }
  void set_logger_attr_interval(uint32_t ms) { this->timing_.logger_attr_s = std::max<uint32_t>(1, ms / 1000); }
  /// devinfo after the first-connect rundata of a boot (stock S2 yes, WiNet-S no; YAML `auto`
  /// is resolved per sungrow_board variant). Devinfo on server-switch confirm is unconditional.
  void set_devinfo_on_connect(bool v) { this->timing_.devinfo_on_connect = v; }
  void set_send_time_utc(bool v) { this->send_time_utc_ = v; }
  /// settimezone (10324): apply the cloud's zone to our local time (stock: only with time source
  /// 3 = MQTT, whose default is open: C33). The timezoneId is persisted either way.
  void set_apply_cloud_timezone(bool v) { this->apply_cloud_timezone_ = v; }
  /// settime (10307, timezone broadcast): set the system clock from the cloud.
  void set_cloud_settime(bool v) { this->cloud_settime_ = v; }
  void set_ack_timing(uint32_t timeout_ms, uint8_t retries) { this->queue_.configure(timeout_ms, retries); }
  /// YAML firmware_version: fixed for the life of the build. "" (not set) = the board's live
  /// value (sungrow_board, re-read on every resolve so a web UI override applies at once).
  void set_firmware_version(const char *v) {
    this->firmware_version_ = v;
    this->firmware_version_from_yaml_ = v != nullptr && v[0] != '\0';
  }
  /// Re-resolve the identity (the board's cloud version changed from the web UI). Loop task only.
  void reload_identity(const char *why) { this->resolve_identity_(why); }
  void set_reconnect_backoff(uint32_t min_ms, uint32_t max_ms) { this->backoff_ = Backoff(min_ms, max_ms); }
  void set_server_switch_timing(uint32_t confirm_ms, uint32_t timeout_ms, uint32_t delay_ms) {
    this->switch_ = ServerSwitch(confirm_ms, timeout_ms, delay_ms);
  }
#ifdef USE_BINARY_SENSOR
  void set_connected_binary_sensor(binary_sensor::BinarySensor *s) { this->connected_sensor_ = s; }
#endif
#ifdef USE_SENSOR
  void set_publish_count_sensor(sensor::Sensor *s) { this->publish_count_sensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_last_publish_text_sensor(text_sensor::TextSensor *s) { this->last_publish_sensor_ = s; }
  void set_last_error_text_sensor(text_sensor::TextSensor *s) { this->last_error_sensor_ = s; }
  void set_last_command_text_sensor(text_sensor::TextSensor *s) { this->last_command_sensor_ = s; }
  void set_last_control_command_text_sensor(text_sensor::TextSensor *s) { this->last_control_command_sensor_ = s; }
#endif

  // ---- runtime control ----
  /// Uplink on/off (the `uplink` switch). Off disconnects and stays off.
  void set_enabled(bool enabled);
  /// Cloud write commands (setparam 10303 to devices). Off: they are answered as failed and
  /// nothing is written. Independent of sungrow_control's writes_enabled.
  void set_writes_enabled(bool enabled);
  bool writes_enabled() const { return this->writes_enabled_; }
  bool is_enabled() const { return this->enabled_; }
  bool is_connected() const { return this->connected_.load(); }
  /// For status LEDs: enabled and (not connected for > 60 s, or the last connect attempt failed,
  /// or the identity is unusable).
  bool has_error() const;
  const std::string &last_error() const { return this->last_error_; }
  /// Connect attempts that failed or timed out since boot.
  uint32_t connect_failures() const { return this->connect_failures_; }
  const CloudIdentity &identity() const { return this->identity_; }

  // ---- binding hooks (interfaces.h); the bind_* helpers below are what codegen calls ----
  void set_identity_source(IdentitySource *src);
  /// Re-read the identity source (registered through IdentitySource::add_on_loaded_callback).
  void on_identity_loaded();
  void set_telemetry_source(TelemetrySource *src) { this->telemetry_ = src; }
  void set_param_reader(ParamReader reader) { this->param_reader_ = std::move(reader); }
  void set_param_writer(ParamWriter writer) { this->param_writer_ = std::move(writer); }
#ifdef ISOLARCLOUD_BIND_EXTFLASH
  /// YAML `identity_source: <sungrow_extflash id>`.
  void bind_extflash(sungrow_extflash::SungrowExtFlash *flash) {
    this->owned_identity_ = std::make_unique<ExtflashIdentitySource>(flash);
    this->set_identity_source(this->owned_identity_.get());
  }
#endif
#ifdef ISOLARCLOUD_BIND_DATAMODEL
  /// rundata/propertydata from the SignalStore.
  void bind_datamodel(sungrow_datamodel::SungrowDataModel *dm) {
    this->owned_telemetry_ = std::make_unique<StoreTelemetrySource>(&dm->store());
    this->set_telemetry_source(this->owned_telemetry_.get());
  }
#endif
#ifdef ISOLARCLOUD_BIND_MODBUS
  /// Device list (address, table type, link state) for telemetry + getparam via ad-hoc reads.
  /// Call after bind_datamodel().
  void bind_modbus(sungrow_modbus::SungrowModbus *hub) {
#ifdef ISOLARCLOUD_BIND_DATAMODEL
    if (this->owned_telemetry_)
      this->owned_telemetry_->set_master(&hub->master());
#endif
    this->set_param_reader(make_modbus_param_reader(
        [hub](uint8_t addr, uint8_t fn, uint16_t start, uint16_t count, sungrow_modbus::AdhocCallback cb) {
          return hub->read_registers(addr, fn, start, count, std::move(cb));
        }));
    this->set_param_writer(make_modbus_param_writer(
        [hub](uint8_t addr, uint16_t start, const uint16_t *values, uint16_t count,
              const sungrow_modbus::WriteOptions &opt, sungrow_modbus::WriteCallback cb) {
          return hub->submit_write(addr, start, values, count, opt, std::move(cb));
        }));
  }
#endif
  /// Replace the businessData builders.
  void set_formatter(std::unique_ptr<BusinessFormatter> f) { this->formatter_ = std::move(f); }
  /// Extra command handlers (by cmdType, checked after the built-in ones); see CommandRouter.
  CommandRouter &router() { return this->router_; }

  /// Start a broker migration (svr_convert, also triggered by cmd 10384 point 10457). Applied after
  /// the switch delay, confirmed or rolled back automatically. False if host/port/key are unchanged.
  bool request_server_switch(const ServerEndpoint &next);

  /// Queue a printed businessData array on pub/<family> (chunked + compressed as configured).
  bool publish_business(const std::string &family, const std::string &business_json);

 protected:
  struct Inbound {
    std::string topic;
    std::string data;
  };

  // identity / persistence
  void resolve_identity_(const char *why);
  void load_persisted_();
  void save_persisted_();
  bool identity_usable_(std::string *why) const;
  void refresh_logger_signals_();
  void refresh_logger_network_();

  // connection
  bool can_connect_(std::string *why);
  void start_client_();
  void stop_client_(const char *why);
  void on_connected_();
  void update_group_subscription_();
  static void destroy_task_(void *client);
  /// Clients handed to destroy_task_ that are not destroyed yet.
  static std::atomic<uint8_t> destroy_pending_;
  static void mqtt_event_handler_(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
  void handle_event_(esp_mqtt_event_handle_t event);
  int publish(const std::string &topic, const std::vector<uint8_t> &payload, int qos, bool retain) override;

  // time
  bool local_now_(CivilTime &out) const;
  bool now_send_time_(std::string &out) const;
  bool in_silence_(const CivilTime &local) const;

  // outbound
  PublicData public_data_(const std::string &send_time) const;
  bool enqueue_(const std::string &topic, const std::string &business_json, uint16_t compress, const char *label);
  void scheduler_tick_();
  void list_devices_(std::vector<TelemetryDevice> &out) const;
  void publish_rundata_(const CivilTime &data_time);
  /// Live data (10421): one rundata/second message per enabled online device.
  void publish_second_(const CivilTime &data_time);
  void save_second_();
  void publish_devinfo_(const ServerEndpoint &server, const ServerEndpoint &old_server);
  void publish_propertydata_(const TelemetryDevice &d, const std::string &topic, const std::string &batch_id);
  /// Devices a beat reports, as "code:type:online;" (changes when a device appears or flips).
  std::string beat_signature_(const std::vector<TelemetryDevice> &devs) const;
  void publish_beat_();
  /// pub/devstatusdata for one device; remembers what was sent for the change poll.
  void publish_devstatus_(const TelemetryDevice &d);
  /// pub/generaldata for one device; false when it has nothing to send.
  bool publish_generaldata_(const TelemetryDevice &d);
  /// Status / general data change poll (stock scheduler item 10): every STATUS_POLL_S, a device
  /// whose points differ from the last sent copy is published; all devices every 7200 s.
  void poll_status_(uint32_t mono_s);

  // inbound
  void process_inbound_(const Inbound &msg);
  void install_default_handlers_();
  void send_reply_(const InboundCommand &cmd, const std::string &business);
  void handle_getparam_(const InboundCommand &cmd);
  void handle_second_(const InboundCommand &cmd);
  void handle_setparam_(const InboundCommand &cmd);
  void handle_logger_param_set_(const InboundCommand &cmd);
  void handle_settimezone_(const InboundCommand &cmd);
  void handle_settime_(const InboundCommand &cmd);
  void handle_propertydata_(const InboundCommand &cmd);

  // diagnostics
  void set_error_(const std::string &err);
  void publish_diag_();

  // --- config
  CloudIdentity yaml_identity_;
  uint32_t yaml_mask_{0};
  std::string username_;
  std::string password_;
  bool verify_server_{true};
  const char *ca_certificate_{nullptr};
  time::RealTimeClock *time_{nullptr};
  SchedulerTiming timing_;
  bool send_time_utc_{false};
  bool apply_cloud_timezone_{true};
  bool cloud_settime_{true};
  std::string firmware_version_;
  bool firmware_version_from_yaml_{false};

  // --- bindings
#ifdef ISOLARCLOUD_BIND_EXTFLASH
  std::unique_ptr<ExtflashIdentitySource> owned_identity_;
#endif
#ifdef ISOLARCLOUD_BIND_DATAMODEL
  std::unique_ptr<StoreTelemetrySource> owned_telemetry_;
#endif
  IdentitySource *identity_source_{nullptr};
  TelemetrySource *telemetry_{nullptr};
  ParamReader param_reader_;
  ParamWriter param_writer_;
  std::unique_ptr<BusinessFormatter> formatter_;
  CommandRouter router_;
  LoggerSignals logger_sigs_;

  // --- state
  CloudIdentity identity_;
  int32_t tz_offset_s_{8 * 3600};
  /// Persisted cache: values that came from the identity source (`persisted_mask_`), the svr_convert
  /// result (server_from_switch_) and cloud-set fields (`cloud_mask_`: settimezone / 10384). YAML
  /// values are never restored from it, so removing a YAML override takes effect on the next boot.
  CloudIdentity persisted_;
  uint32_t persisted_mask_{0};
  uint32_t source_mask_{0};
  uint32_t cloud_mask_{0};
  bool server_from_switch_{false};
  bool resume_switch_{false};
  ESPPreferenceObject pref_;
  // Live data (10421): node table + enable, kept across reboots like stock's secondcofig file.
  SecondData second_;
  ESPPreferenceObject second_pref_;
  int64_t last_second_slot_{-1};  ///< local epoch / 10 of the last live-data publish
  bool connected_before_{false};  ///< a CONNACK was seen this boot (S2 devinfo on reconnect)
  // devstatusdata / generaldata: last sent point signature per devCode (change detection).
  std::map<uint32_t, std::string> status_sent_;
  std::map<uint32_t, std::string> general_sent_;
  uint32_t status_poll_s_{0};    ///< mono second of the last change poll
  uint32_t status_all_due_s_{0};  ///< mono second of the next all-device devstatusdata
  TopicBuilder topics_;
  MessageCounter msg_counter_;
  CommandDeduper dedupe_;
  ServerSwitch switch_;
  Backoff backoff_{5000, 300000};
  CloudScheduler scheduler_;
  PublishQueue queue_;
  bool enabled_{true};
  bool writes_enabled_{false};
  bool restart_requested_{false};

  esp_mqtt_client_handle_t client_{nullptr};
  /// The client whose events count (read on the esp-mqtt task): events of a client that is being
  /// destroyed in the background are ignored.
  std::string beat_sig_;          ///< device set of the last beat sent
  uint32_t beat_check_ms_{0};
  uint32_t last_beat_ms_{0};
  std::atomic<esp_mqtt_client_handle_t> active_client_{nullptr};
  std::string will_topic_;
  std::vector<uint8_t> will_payload_;
  std::string host_;
  std::vector<uint8_t> ca_buf_;  ///< CA from the ext flash (PEM with a NUL appended, or DER)
  bool ca_is_der_{false};
  uint32_t json_overflows_seen_{0};  ///< json_overflow_count() already reported
  std::atomic<bool> connected_{false};
  std::atomic<bool> connect_event_{false};
  std::atomic<bool> disconnect_event_{false};
  bool session_up_{false};  // loop-side view of connected_
  uint32_t client_started_ms_{0};
  uint32_t down_since_ms_{0};        ///< when the session was last not up
  bool last_connect_failed_{false};
  uint32_t connect_failures_{0};
  uint32_t next_attempt_ms_{0};
  std::string wait_reason_;
  std::string group_topic_;      ///< currently subscribed cmd/<tz>/enum_0_0_0 ("" = none, "-" = dropped after a settime)
  uint32_t group_sub_ms_{0};     ///< when the broadcast topic was last (re)subscribed

  std::mutex rx_mutex_;
  std::deque<Inbound> rx_queue_;
  std::vector<int> pubacks_;  // msg_ids from MQTT_EVENT_PUBLISHED (guarded by rx_mutex_)
  Inbound rx_partial_;        // mqtt task only
  bool rx_partial_drop_{false};
  std::mutex err_mutex_;
  std::string pending_error_;  // set from the mqtt task

  uint32_t last_tick_ms_{0};
  uint32_t boot_ms_{0};
  uint32_t last_switch_check_ms_{0};
  uint32_t last_connect_check_ms_{0};
  uint32_t publish_count_{0};
  std::string last_publish_;
  std::string last_error_;
  std::string last_command_;          // every inbound cloud command
  std::string last_control_command_;  // write/control commands only (not getparam-style polls)
  bool diag_dirty_{true};

#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
#endif
#ifdef USE_SENSOR
  sensor::Sensor *publish_count_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *last_publish_sensor_{nullptr};
  text_sensor::TextSensor *last_error_sensor_{nullptr};
  text_sensor::TextSensor *last_command_sensor_{nullptr};
  text_sensor::TextSensor *last_control_command_sensor_{nullptr};
#endif
};

#ifdef USE_SWITCH
/// Uplink enable switch (default on, restored).
class UplinkSwitch : public switch_::Switch, public Component, public Parented<IsolarcloudComponent> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void write_state(bool state) override;
};

/// Cloud write switch (default off, restored): lets iSolarCloud change device settings.
class WritesSwitch : public switch_::Switch, public Component, public Parented<IsolarcloudComponent> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void write_state(bool state) override;
};
#endif

/// isolarcloud.server_switch action (manual broker migration, mostly for local testing).
template<typename... Ts> class ServerSwitchAction : public Action<Ts...>, public Parented<IsolarcloudComponent> {
 public:
  TEMPLATABLE_VALUE(std::string, host)
  TEMPLATABLE_VALUE(uint16_t, port)
  TEMPLATABLE_VALUE(uint16_t, ssl_flag)
  TEMPLATABLE_VALUE(std::string, mqtt_key)

  void play(const Ts &...x) override {
    ServerEndpoint ep = this->parent_->identity().server;
    ep.host = this->host_.value(x...);
    if (this->port_.has_value())
      ep.port = this->port_.value(x...);
    if (this->ssl_flag_.has_value())
      ep.ssl_flag = this->ssl_flag_.value(x...);
    if (this->mqtt_key_.has_value())
      ep.key = key_from_string(this->mqtt_key_.value(x...).c_str());
    this->parent_->request_server_switch(ep);
  }
};

}  // namespace esphome::isolarcloud
