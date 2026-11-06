#include "sungrow_mqtt.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_crt_bundle.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace sungrow_mqtt {

static const char *const TAG = "sungrow_mqtt";

// ---------------------------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------------------------

static void json_escape(std::string &out, const char *s) {
  for (const char *p = s; p != nullptr && *p != '\0'; ++p) {
    char c = *p;
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;  // UTF-8 bytes (e.g. °C) pass through
        }
    }
  }
}

/// sensor_slug / device_slug sanitiser: keep [A-Za-z0-9], everything else -> '_', collapsed and
/// trimmed. (The sensor_slug is normally pre-slugified at codegen; this guards the runtime
/// device_slug built from model + serial.)
static std::string slugify_alnum(const std::string &in, bool lower) {
  std::string out;
  out.reserve(in.size());
  bool last_us = true;  // trim leading
  for (char c : in) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (ok) {
      out += lower ? static_cast<char>(std::tolower(c)) : c;
      last_us = false;
    } else if (!last_us) {
      out += '_';
      last_us = true;
    }
  }
  while (!out.empty() && out.back() == '_')
    out.pop_back();
  return out;
}

/// winet-extractor-compatible part sanitiser: keep [A-Za-z0-9] only (non-alnum STRIPPED, not turned
/// into '_'), optional lower. Used to build the device slug from model/serial exactly like
/// winet-extractor, so "SG5.0RS" -> "SG50RS" and our discovery takes over its device.
static std::string alnum_only(const std::string &in, bool lower) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (ok)
      out += lower ? static_cast<char>(std::tolower(c)) : c;
  }
  return out;
}

static std::string fmt_number(double v) {
  if (std::isnan(v))
    return "null";
  char buf[32];
  // Whole numbers (counters, u32 codes such as module date codes) print exactly; others
  // keep 7 significant digits (the float scale makes later digits noise).
  if (std::fabs(v) < 1e15 && v == std::floor(v))
    snprintf(buf, sizeof(buf), "%.0f", v);
  else
    snprintf(buf, sizeof(buf), "%.7g", v);
  return buf;
}

// ---------------------------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------------------------

void SungrowMqttComponent::setup() {
  if (this->dm_ == nullptr || this->hub_ == nullptr) {
    ESP_LOGE(TAG, "sungrow_datamodel and sungrow_modbus are required");
    this->mark_failed();
    return;
  }
  this->cached_dongle_id_ = this->dongle_id_();
#ifdef USE_BINARY_SENSOR
  if (this->connected_sensor_ != nullptr)
    this->connected_sensor_->publish_initial_state(false);
#endif

  // One listener over the whole store: every changed signal is queued for a throttled publish.
  this->dm_->store().add_listener([this](const sungrow_datamodel::Signal &s) { this->on_signal_changed_(s); });
  this->listener_installed_ = true;
#ifdef SUNGROW_MQTT_BIND_CONTROL
  // A start/stop command's follow-up goes to that control's result topic as it progresses:
  // {"ok","status","state","final"}, state = the running-state code seen last.
  if (this->control_ != nullptr) {
    this->control_->core().add_expect_listener(
        [this](uint8_t device, const sungrow_control::ExpectStatus &st) { this->publish_expect_(device, st); });
  }
#endif

  // Lifecycle: the master's single MasterListener slot is owned by the hub, so we derive
  // discovered / online transitions by polling master().devices() in sync_inventory_() (see .h).

  // Broker settings: YAML wins; else the record stored from the web UI (runtime_settings); else
  // idle until the Settings page names a broker.
  MqttSettings stored;
  const bool have_stored = this->load_stored_settings_(&stored);
  this->source_ = resolve_mqtt_settings(this->yaml_, have_stored ? &stored : nullptr, &this->live_);
  this->adopt_live_();
  if (!this->is_configured()) {
    ESP_LOGI(TAG, "No broker configured%s", this->runtime_settings_ ? "; set one on the web UI's Settings page" : "");
    return;
  }
  this->start_client_();
}

bool SungrowMqttComponent::load_stored_settings_(MqttSettings *out) {
  if (!this->runtime_settings_)
    return false;
  this->settings_pref_ = global_preferences->make_preference<MqttSettingsPref>(fnv1_hash("sungrow_mqtt_settings"), true);
  MqttSettingsPref p{};
  if (!this->settings_pref_.load(&p))
    return false;
  if (!unpack_mqtt_settings(p, out)) {
    ESP_LOGW(TAG, "Stored MQTT settings unreadable; ignored");
    return false;
  }
  return true;
}

void SungrowMqttComponent::adopt_live_() {
  this->broker_ = this->live_.host;
  this->port_ = this->live_.port;
  this->username_ = this->live_.user;
  this->password_ = this->live_.pass;
  this->prefix_ = this->live_.prefix;
  this->tls_ = this->live_.tls;
}

bool SungrowMqttComponent::apply_settings(const MqttSettings &s, std::string *why) {
  if (!this->runtime_settings_) {
    if (why != nullptr)
      *why = "runtime settings are not enabled in this build";
    return false;
  }
  if (!this->yaml_.host.empty()) {
    if (why != nullptr)
      *why = "the broker is set in the configuration";
    return false;
  }
  if (!validate_mqtt_settings(s, why))
    return false;
  MqttSettingsPref p;
  pack_mqtt_settings(s, &p);
  if (!this->settings_pref_.save(&p)) {
    if (why != nullptr)
      *why = "could not store the settings";
    return false;
  }
  global_preferences->sync();

  this->stop_client_("settings changed");
  this->reset_publish_state_();
  this->last_error_.clear();
  this->source_ = resolve_mqtt_settings(this->yaml_, &s, &this->live_);
  this->adopt_live_();
  if (!this->is_configured()) {
    ESP_LOGI(TAG, "MQTT settings cleared; client idle");
    return true;
  }
  ESP_LOGI(TAG, "MQTT settings stored; reconnecting");
  this->start_client_();
  return true;
}

void SungrowMqttComponent::reset_publish_state_() {
  for (auto &d : this->pubs_) {
    d.discovery_done = false;
    d.configured.clear();
    d.problem_configured = false;
#ifdef SUNGROW_MQTT_BIND_CONTROL
    d.write_configured.clear();
#endif
  }
  {
    std::lock_guard<std::mutex> lk(this->dirty_mutex_);
    this->dirty_.clear();
    this->dirty_set_.clear();
  }
  this->last_state_ms_.clear();
  this->last_payload_.clear();
  this->last_value_.clear();
  this->refresh_.clear();
  this->last_full_refresh_ms_ = 0;
  this->first_settled_ms_ = 0;
  this->initial_publish_done_ = false;
  this->backfill_pending_ = false;
#ifdef SUNGROW_MQTT_BIND_CONTROL
  this->write_settle_.clear();
#endif
}

void SungrowMqttComponent::start_client_() {
  if (this->client_ != nullptr)
    return;

  this->will_topic_ = this->status_topic_();
  const std::string client_id = this->cached_dongle_id_ + "-ha";

  esp_mqtt_client_config_t cfg{};
  cfg.broker.address.hostname = this->broker_.c_str();
  cfg.broker.address.port = this->port_;
  cfg.broker.address.transport = this->tls_ ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
  if (this->tls_) {
    cfg.broker.verification.skip_cert_common_name_check = true;
    if (this->ca_certificate_ != nullptr) {
      cfg.broker.verification.certificate = this->ca_certificate_;
    } else {
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
      cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    }
  }
  cfg.credentials.client_id = client_id.c_str();
  if (!this->username_.empty())
    cfg.credentials.username = this->username_.c_str();
  if (!this->password_.empty())
    cfg.credentials.authentication.password = this->password_.c_str();
  cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  cfg.session.keepalive = 30;
  cfg.session.last_will.topic = this->will_topic_.c_str();
  cfg.session.last_will.msg = "offline";
  cfg.session.last_will.msg_len = 7;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  // LAN broker: let esp-mqtt reconnect. The outbox limit is the hard stop (enqueue fails above it);
  // loop() keeps the fill below OUTBOX_HIGH_WATER (outbox_has_room_) so bulk publishing never gets
  // there. 32 KB only bounds growth; it is not pre-allocated.
  cfg.network.disable_auto_reconnect = false;
  cfg.network.reconnect_timeout_ms = 5000;
  cfg.network.timeout_ms = 10000;
  cfg.buffer.size = 1024;       // inbound: our command payloads are tiny
  cfg.buffer.out_size = 2048;   // one discovery config (incl. enum maps) fits comfortably
  cfg.outbox.limit = 32 * 1024;

  this->client_ = esp_mqtt_client_init(&cfg);
  if (this->client_ == nullptr) {
    ESP_LOGE(TAG, "esp_mqtt_client_init failed");
    return;
  }
  esp_mqtt_client_register_event(this->client_, MQTT_EVENT_ANY, &SungrowMqttComponent::mqtt_event_handler_, this);
  ESP_LOGI(TAG, "Connecting to %s://%s:%u", this->tls_ ? "mqtts" : "mqtt", this->broker_.c_str(), this->port_);
  if (esp_mqtt_client_start(this->client_) != ESP_OK) {
    ESP_LOGE(TAG, "esp_mqtt_client_start failed");
    this->stop_client_("start failed");
  }
}

void SungrowMqttComponent::stop_client_(const char *why) {
  if (this->client_ == nullptr)
    return;
  ESP_LOGI(TAG, "Closing MQTT session: %s", why);
  esp_mqtt_client_handle_t c = this->client_;
  this->client_ = nullptr;
  esp_mqtt_client_destroy(c);
  this->connected_.store(false);
  this->session_up_ = false;
  this->connect_event_.store(false);
  this->disconnect_event_.store(false);
  this->ha_birth_event_.store(false);
#ifdef USE_BINARY_SENSOR
  if (this->connected_sensor_ != nullptr)
    this->connected_sensor_->publish_state(false);
#endif
  {
    std::lock_guard<std::mutex> lk(this->rx_mutex_);
    this->rx_queue_.clear();
  }
}

void SungrowMqttComponent::on_shutdown() {
  // Flip our own availability offline (retained), then tear down.
  if (this->client_ != nullptr && this->session_up_)
    this->publish_(this->status_topic_(), "offline", 1, true);
  this->stop_client_("shutdown");
}

// ---------------------------------------------------------------------------------------------
// mqtt task: only atomics + mutex-guarded queues here (borrowed from isolarcloud)
// ---------------------------------------------------------------------------------------------

void SungrowMqttComponent::mqtt_event_handler_(void *arg, esp_event_base_t /*base*/, int32_t /*id*/,
                                               void *event_data) {
  static_cast<SungrowMqttComponent *>(arg)->handle_event_(static_cast<esp_mqtt_event_handle_t>(event_data));
}

void SungrowMqttComponent::handle_event_(esp_mqtt_event_handle_t ev) {
  switch (ev->event_id) {
    case MQTT_EVENT_CONNECTED:
      this->connected_.store(true);
      this->connect_event_.store(true);
      break;
    case MQTT_EVENT_DISCONNECTED:
      this->connected_.store(false);
      this->disconnect_event_.store(true);
      break;
    case MQTT_EVENT_DATA: {
      // Small single-fragment payloads only (our command/status topics are tiny).
      if (ev->current_data_offset != 0)
        break;
      std::string topic(ev->topic, ev->topic_len);
      std::string data(ev->data, ev->data_len);
      if (topic == this->prefix_ + "/status") {
        if (data == "online")
          this->ha_birth_event_.store(true);
        break;
      }
      std::lock_guard<std::mutex> lk(this->rx_mutex_);
      if (this->rx_queue_.size() < RX_MAX_QUEUE)
        this->rx_queue_.push_back({std::move(topic), std::move(data)});
      break;
    }
    case MQTT_EVENT_ERROR:
      ESP_LOGW(TAG, "mqtt transport error");  // never logs credentials
      if (ev->error_handle != nullptr) {
        this->err_type_.store(static_cast<int>(ev->error_handle->error_type));
        this->err_connect_code_.store(static_cast<int>(ev->error_handle->connect_return_code));
        this->err_tls_.store(static_cast<int>(ev->error_handle->esp_tls_last_esp_err));
        this->err_errno_.store(ev->error_handle->esp_transport_sock_errno);
        this->error_event_.store(true);
      }
      break;
    default:
      break;
  }
}

int SungrowMqttComponent::publish_(const std::string &topic, const std::string &payload, int qos, bool retain) {
  if (this->client_ == nullptr)
    return -1;
  const char *data = payload.empty() ? "" : payload.c_str();
  const int id = esp_mqtt_client_enqueue(this->client_, topic.c_str(), data, static_cast<int>(payload.size()), qos,
                                         retain ? 1 : 0, true);
  if (id < 0) {
    // Outbox full / client down. Bulk publishers pause at OUTBOX_HIGH_WATER, so this is unexpected;
    // rate-limit the log and let the periodic full-state refresh re-publish later.
    const uint32_t now = millis();
    if (now - this->enqueue_fail_log_ms_ >= 5000) {
      this->enqueue_fail_log_ms_ = now;
      ESP_LOGW(TAG, "mqtt enqueue failed for %s (outbox full?)", topic.c_str());
    }
  }
  return id;
}

void SungrowMqttComponent::subscribe_(const std::string &topic, int qos) {
  if (this->client_ == nullptr)
    return;
  esp_mqtt_client_subscribe(this->client_, topic.c_str(), qos);
}

bool SungrowMqttComponent::outbox_has_room_() const {
  if (this->client_ == nullptr)
    return false;
  return esp_mqtt_client_get_outbox_size(this->client_) < static_cast<int>(OUTBOX_HIGH_WATER);
}

// ---------------------------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------------------------

/// Human-readable text for an esp-mqtt error (loop side; the numbers were captured on the mqtt task).
static std::string describe_mqtt_error(int type, int connect_code, int tls_err, int sock_errno) {
  char buf[96];
  if (type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
    const char *why = connect_code == MQTT_CONNECTION_REFUSE_PROTOCOL       ? "protocol version"
                      : connect_code == MQTT_CONNECTION_REFUSE_ID_REJECTED  ? "client id rejected"
                      : connect_code == MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE ? "server unavailable"
                      : connect_code == MQTT_CONNECTION_REFUSE_BAD_USERNAME ? "bad username or password"
                      : connect_code == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED ? "not authorised"
                                                                              : "refused";
    std::snprintf(buf, sizeof(buf), "connection refused: %s (code %d)", why, connect_code);
    return buf;
  }
  if (type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
    if (tls_err != 0)
      std::snprintf(buf, sizeof(buf), "transport error: %s", esp_err_to_name(tls_err));
    else if (sock_errno != 0)
      std::snprintf(buf, sizeof(buf), "transport error: %s (errno %d)", strerror(sock_errno), sock_errno);
    else
      std::snprintf(buf, sizeof(buf), "transport error (host unreachable or refused)");
    return buf;
  }
  std::snprintf(buf, sizeof(buf), "mqtt error type %d", type);
  return buf;
}

void SungrowMqttComponent::loop() {
  if (this->error_event_.exchange(false)) {
    std::string err = describe_mqtt_error(this->err_type_.load(), this->err_connect_code_.load(),
                                          this->err_tls_.load(), this->err_errno_.load());
    if (err != this->last_error_)  // esp-mqtt retries every 5 s: log a change, not every attempt
      ESP_LOGW(TAG, "Broker %s", err.c_str());
    this->last_error_ = std::move(err);
  }
  if (this->connect_event_.exchange(false)) {
    this->session_up_ = true;
    this->last_error_.clear();
#ifdef USE_BINARY_SENSOR
    if (this->connected_sensor_ != nullptr)
      this->connected_sensor_->publish_state(true);
#endif
    ESP_LOGI(TAG, "Connected to broker");
    this->publish_(this->status_topic_(), "online", 1, true);  // our birth
    this->subscribe_(this->prefix_ + "/status", 0);            // HA birth
    this->subscribe_("sungrow-mqtt/" + this->dongle_id_() + "/+/+/set", 1);  // commands (writes)
    this->republish_all_(false);
  }
  if (this->disconnect_event_.exchange(false)) {
    this->session_up_ = false;
#ifdef USE_BINARY_SENSOR
    if (this->connected_sensor_ != nullptr)
      this->connected_sensor_->publish_state(false);
#endif
    ESP_LOGW(TAG, "Disconnected from broker");
  }
  if (!this->session_up_)
    return;

  if (this->ha_birth_event_.exchange(false)) {
    ESP_LOGI(TAG, "Home Assistant restarted, republishing discovery + state");
    this->republish_all_(true);
  }

  // drain inbound command messages
  for (;;) {
    Inbound msg;
    {
      std::lock_guard<std::mutex> lk(this->rx_mutex_);
      if (this->rx_queue_.empty())
        break;
      msg = std::move(this->rx_queue_.front());
      this->rx_queue_.pop_front();
    }
    this->handle_command_(msg.topic, msg.data);
  }

  this->sync_inventory_();

  const uint32_t now = millis();
  // Publish-only-when-settled gate. Nothing is published for a device until EVERYTHING its final
  // discovery set needs is in hand (device_settled_): discovered + online, first poll round done
  // (read set populated), the inverter's own serial (sigid 0x0002) valid, and its write profile
  // attached. There is no provisional identity and no dongle-SN fallback, so a device whose serial
  // never resolves is simply never published -- no ghost, no later retract/upgrade churn.
  this->publish_settled_();

  // Stream the settled devices' retained configs in, outbox-paced and bounded per loop.
  this->backfill_configs_();
#ifdef SUNGROW_MQTT_BIND_CONTROL
  this->backfill_write_configs_();  // per-device number/select/switch controls (writable subset)
#endif

  this->drain_dirty_();
#ifdef SUNGROW_MQTT_BIND_CONTROL
  this->drain_settle_();  // follow just-written entities until their read-back settles (bounded window)
#endif

  // Periodic full-state refresh (heartbeat): every full_refresh_ms_ re-publish every current state
  // (force, bypassing the payload dedup), spread over subsequent loops via refresh_ so one loop
  // never stalls. Held while a discovery back-fill is still streaming so the initial set goes out
  // once, in order, before any repeat.
  if (!this->backfill_pending_ && now - this->last_full_refresh_ms_ >= this->full_refresh_ms_) {
    this->last_full_refresh_ms_ = now;
    this->enqueue_full_refresh_();
  }
  this->drain_refresh_();

  // NO night retraction: a device that goes offline keeps all its retained discovery configs; only
  // its availability flips to "offline" (in sync_inventory_). We NEVER auto-delete a device that
  // legitimately powers down (the shed inverter is offline every night) -- deleting it churns HA
  // entity-ids and breaks dashboards + Riemann integral sources downstream.
}

// ---------------------------------------------------------------------------------------------
// inventory + lifecycle (polled from master().devices(); see the note in the header)
// ---------------------------------------------------------------------------------------------

DevicePub *SungrowMqttComponent::dev_for_index_(uint8_t index) {
  for (auto &d : this->pubs_)
    if (d.index == index)
      return &d;
  return nullptr;
}

void SungrowMqttComponent::sync_inventory_() {
  for (const Device &dev : this->hub_->master().devices()) {
    const uint8_t index = dev.config().index;
    DevicePub *p = this->dev_for_index_(index);
    if (p == nullptr) {
      this->pubs_.push_back(DevicePub{});
      p = &this->pubs_.back();
      p->index = index;
    }
    p->address = dev.config().address;
    if (dev.table() != nullptr)
      p->family = dev.table()->family;

    // discovered transition
    if (dev.discovered() && !p->known) {
      p->known = true;
      const char *m = dev.devinfo().model;
      if (m[0] != '\0')
        p->model = m;
      ESP_LOGI(TAG, "Device %u discovered: model '%s' family %s", p->index, p->model.c_str(),
               family_name(p->family));
    } else if (dev.discovered()) {
      const char *m = dev.devinfo().model;
      if (m[0] != '\0' && p->model != m)
        p->model = m;
    }
    // read set populated once the first full poll round has completed (resets on reconnect).
    p->first_round = dev.first_round_done();

    // online transition
    const bool online = dev.online();
    if (online != p->online) {
      p->online = online;
      p->offline_since_ms = online ? 0 : millis();
      this->set_availability_(*p, online);
      ESP_LOGD(TAG, "Device %u %s", p->index, online ? "online" : "offline");
    }
  }
}

// ---------------------------------------------------------------------------------------------
// identity / per-device publishing
// ---------------------------------------------------------------------------------------------

std::string SungrowMqttComponent::dongle_id_() const {
  std::string sn;
#ifdef SUNGROW_MQTT_BIND_EXTFLASH
  if (this->flash_ != nullptr && this->flash_->identity_loaded())
    sn = this->flash_->identity().sn;
#endif
  if (sn.empty())
    sn = "sungrow";
  return slugify_alnum(sn, true);
}

bool SungrowMqttComponent::device_settled_(const DevicePub &d) const {
  // Everything the final discovery set needs, and nothing published before all of it is true.
  if (!d.known || !d.online || !d.first_round)
    return false;
  if (!this->dm_->store().is_valid(d.index, SIG_SERIAL))
    return false;  // the device's own serial has not been read yet
#ifdef SUNGROW_MQTT_BIND_CONTROL
  // Write profiles exist for inverters only (sungrow_control auto_attach_profiles_: dev_type
  // 35 / 21 / 1); batteries, meters and chargers have no write entities, so nothing to wait for.
  if (this->control_ != nullptr && d.family == sungrow_datamodel::Family::INVERTER) {
    const sungrow_control::ControlCore::DeviceCfg *cfg = this->control_->core().device(d.index);
    if (cfg == nullptr || cfg->profile == nullptr)
      return false;  // its write profile (by dev_type) has not been attached yet
  }
#endif
  return true;
}

bool SungrowMqttComponent::resolve_serial_(DevicePub &d) {
  const SignalStore &store = this->dm_->store();
  const std::string serial = store.get_string(d.index, SIG_SERIAL);
  if (!store.is_valid(d.index, SIG_SERIAL) || serial.empty())
    return false;  // no fallback: a device without a real serial is simply never published
  d.serial = serial;
  d.serial_valid = true;
  const std::string model = d.model.empty() ? "sungrow" : d.model;
  // Match winet-extractor's device slug exactly: <model-alnum>_<serial-alnum> e.g. "SG50RS_A2291234567"
  // (non-alnum stripped, NOT replaced with '_'), so our discovery takes over the device HA already
  // learned from winet-extractor instead of creating a near-duplicate "SG5_0RS_...".
  d.device_slug = alnum_only(model, false) + "_" + alnum_only(serial, false);
  return true;
}

void SungrowMqttComponent::publish_settled_() {
  size_t settled = 0, settling = 0;
  for (const auto &d : this->pubs_) {
    if (d.discovery_done)
      continue;
    if (this->device_settled_(d))
      settled++;
    else if (d.known && d.online)
      settling++;  // discovered, answering, first round / serial / profile still pending
  }
  if (settled == 0)
    return;
  if (!this->initial_publish_done_) {
    // Boot: hold the first settled device at least settle_grace_ms_, and while another discovered
    // online device is still settling (an inverter's first round of ~70 reads takes longer than a
    // battery's 10), bounded by SETTLE_MAX_HOLD_MS, so the whole bus is published in the same pass.
    const uint32_t now = millis();
    if (this->first_settled_ms_ == 0)
      this->first_settled_ms_ = now;
    const uint32_t held = now - this->first_settled_ms_;
    if (held < this->settle_grace_ms_ || (settling > 0 && held < SETTLE_MAX_HOLD_MS))
      return;
    this->initial_publish_done_ = true;
    ESP_LOGI(TAG, "Publishing %u settled device(s) in one pass after %u ms%s", static_cast<unsigned>(settled),
             static_cast<unsigned>(held), settling > 0 ? " (hold limit reached)" : "");
  }
  for (auto &d : this->pubs_) {
    if (d.discovery_done || !this->device_settled_(d))
      continue;
    this->resolve_serial_(d);  // settled implies 0x0002 is valid; this fixes the real slug
    this->publish_device_(d);  // the complete set streams in from here (backfill_configs_)
  }
}

void SungrowMqttComponent::publish_device_(DevicePub &d) {
  if (!this->session_up_ || d.device_slug.empty())
    return;
  // The device is settled: its slug is final and will never change, so there is no retract/upgrade.
  // Publish availability now; the complete read + write discovery set (each config carrying the HA
  // device block) streams in via backfill_configs_() / backfill_write_configs_(), device by device,
  // outbox-paced. Each state follows its config (then on change via drain_dirty_).
  this->set_availability_(d, d.online);
  d.discovery_done = true;
  this->backfill_pending_ = true;
  ESP_LOGI(TAG, "Publishing settled device %s (serial %s)", d.device_slug.c_str(), d.serial.c_str());
}

void SungrowMqttComponent::backfill_configs_() {
  if (!this->backfill_pending_)
    return;  // nothing unconfigured anywhere: skip the store scan
  const SignalStore &store = this->dm_->store();
  size_t budget = CONFIG_PER_LOOP;
  bool paused = false;     // per-loop cap or outbox high-water reached: resume next loop
  bool remaining = false;  // an unconfigured valid signal is still waiting
  for (auto &d : this->pubs_) {
    if (!d.discovery_done || d.device_slug.empty())
      continue;
    // Every valid signal of this device with HA metadata that has no retained config yet gets its
    // config followed by its current state, in store order, so the set goes out exactly once.
    // A signal that becomes valid later (drain_dirty_ sets backfill_pending_) is added the same way.
    store.for_each([&](const sungrow_datamodel::Signal &s) {
      if (s.device != d.index || !s.valid || d.configured.count(s.sigid))
        return;
      const SensorMeta *m = lookup_meta(d.family, s.sigid);
      if (m == nullptr)
        return;
      if (paused || budget == 0 || !this->outbox_has_room_()) {
        paused = remaining = true;
        return;
      }
      if (this->publish_signal_config_(d, *m)) {
        // The high-water mark leaves ample outbox room, so the state enqueue cannot fail here.
        this->publish_signal_state_(d, *m, /*force=*/true);
        budget--;
      } else {
        paused = remaining = true;
      }
    });
    // The Problem binary_sensor follows once the fault count is in the store.
    if (has_problem_sensor_(d.family) && !d.problem_configured &&
        store.is_valid(d.index, sungrow_datamodel::SIG_ACTIVE_FAULT_COUNT)) {
      if (paused || budget == 0 || !this->outbox_has_room_() || !this->publish_problem_config_(d)) {
        paused = remaining = true;
      } else {
        this->publish_problem_state_(d, /*force=*/true);
        budget--;
      }
    }
  }
  this->backfill_pending_ = remaining;
}

bool SungrowMqttComponent::publish_signal_config_(DevicePub &d, const SensorMeta &m) {
  const std::string topic = this->config_topic_(d.device_slug, m.slug);
  const std::string payload = this->build_config_json_(d, m);
  if (this->publish_(topic, payload, 1, true) < 0)
    return false;
  d.configured.insert(m.sigid);
  return true;
}

bool SungrowMqttComponent::publish_state_payload_(uint32_t key, const std::string &topic,
                                                  const std::string &payload, bool force) {
  if (!force) {
    auto it = this->last_payload_.find(key);
    if (it != this->last_payload_.end() && it->second == payload)
      return false;  // identical rounded payload -> skip (dedup; kills the ~1 s republish spam)
  }
  // States are RETAINED at QoS 1: a consumer connecting after the last change still gets the current
  // value, AND delivery is reliable. QoS 0 was lossy -- an enqueue could "succeed" (so last_payload_
  // was updated here) while the message was dropped before reaching the broker; every later deduped
  // publish was then wrongly skipped until the 60s force-refresh, leaving the broker/HA stale for up
  // to ~30s (control read-backs, total_yield, operating_status). QoS 1 keeps last_payload_ in sync
  // with what the broker actually holds, so the dedup is safe.
  if (this->publish_(topic, payload, 1, true) < 0)
    return false;  // enqueue failed (logged in publish_); leave the cache so we retry on refresh
  this->last_payload_[key] = payload;
  return true;
}

bool SungrowMqttComponent::publish_signal_state_(const DevicePub &d, const SensorMeta &m, bool force) {
  const SignalStore &store = this->dm_->store();
  const uint32_t key = (static_cast<uint32_t>(d.index) << 16) | m.sigid;
  if (!store.is_valid(d.index, m.sigid)) {
    this->last_payload_.erase(key);  // force a republish when it next becomes valid
    this->last_value_.erase(key);
    return false;
  }
  std::string payload;
  bool numeric = false;
  double v = 0.0;
  if (m.enum_map != nullptr && !m.is_string) {
    // enum read: publish the DECODED TEXT (fall back to the raw integer if the code is unmapped).
    uint64_t raw = 0;
    const char *text = store.get_bits(d.index, m.sigid, &raw) ? enum_lookup(m.enum_map, static_cast<uint16_t>(raw))
                                                              : nullptr;
    payload = build_state_json_str_(text != nullptr ? std::string(text) : std::to_string(static_cast<int64_t>(raw)));
  } else if (m.is_string) {
    payload = build_state_json_str_(store.get_string(d.index, m.sigid));
  } else {
    float mult;
    const char *unit = normalise_unit(m.unit, &mult);
    v = store.get_number(d.index, m.sigid) * m.multiplier;
    numeric = true;
    // Deadband: an analog goes out only once it has moved at least m.deadband (scaled) from the
    // value last published for it. The forced 60 s refresh bypasses this, so a drift inside the
    // band reaches the broker within a minute. Keeps HA from writing a states row per jitter.
    const double band = static_cast<double>(m.deadband) * this->deadband_scale_;
    if (!force && band > 0.0) {
      auto it = this->last_value_.find(key);
      if (it != this->last_value_.end() && std::fabs(v - it->second) < band)
        return false;
    }
    payload = build_state_json_(v, unit);
  }
  if (!this->publish_state_payload_(key, this->state_topic_(d.device_slug, m.slug), payload, force))
    return false;
  if (numeric)
    this->last_value_[key] = v;
  return true;
}

void SungrowMqttComponent::set_availability_(DevicePub &d, bool online) {
  if (!this->session_up_ || d.device_slug.empty())
    return;
  this->publish_(this->availability_topic_(d.device_slug), online ? "online" : "offline", 1, true);
}

void SungrowMqttComponent::enqueue_full_refresh_() {
  if (!this->refresh_.empty())
    return;  // previous refresh still draining; skip this round
  for (auto &d : this->pubs_) {
    if (!d.discovery_done || d.device_slug.empty())
      continue;
    for (uint16_t sigid : d.configured)
      this->refresh_.push_back((static_cast<uint32_t>(d.index) << 16) | sigid);
    if (d.problem_configured)
      this->refresh_.push_back((static_cast<uint32_t>(d.index) << 16) | PROBLEM_KEY);
#ifdef SUNGROW_MQTT_BIND_CONTROL
    for (uint16_t sigid : d.write_configured)
      this->refresh_.push_back((static_cast<uint32_t>(d.index) << 16) | sigid);
#endif
  }
}

void SungrowMqttComponent::drain_refresh_() {
  for (size_t i = 0; i < REFRESH_PER_LOOP; i++) {
    if (this->refresh_.empty() || !this->outbox_has_room_())
      return;  // the queue keeps its order; resume next loop
    const uint32_t key = this->refresh_.front();
    this->refresh_.pop_front();
    const uint8_t index = static_cast<uint8_t>(key >> 16);
    const uint16_t sigid = static_cast<uint16_t>(key & 0xFFFF);
    DevicePub *d = this->dev_for_index_(index);
    if (d == nullptr || !d->discovery_done || d->device_slug.empty())
      continue;
    if (sigid == PROBLEM_KEY) {
      this->publish_problem_state_(*d, /*force=*/true);
      continue;
    }
    if (const SensorMeta *m = lookup_meta(d->family, sigid)) {
      this->publish_signal_state_(*d, *m, /*force=*/true);
      continue;
    }
#ifdef SUNGROW_MQTT_BIND_CONTROL
    uint8_t wn = 0;
    const sungrow_control::WriteEntity *ents = this->write_entities_for_(*d, &wn);
    for (uint8_t j = 0; j < wn; j++)
      if (ents[j].sigid == sigid) {
        this->publish_write_state_(*d, ents[j], /*force=*/true);
        break;
      }
#endif
  }
}

void SungrowMqttComponent::republish_all_(bool include_states) {
  // Broker reconnect / HA birth: re-publish ONLY the settled set. Each already-published device
  // keeps its final slug and clears `configured` so backfill re-sends each retained config (and a
  // current state) over the following loops, outbox-paced, overwriting in place with no retract.
  // Devices not yet settled are left alone and published later by the normal loop gate.
  (void) include_states;
  this->publish_(this->status_topic_(), "online", 1, true);
  this->last_payload_.clear();  // drop the state dedup cache so every current state re-publishes
  this->last_value_.clear();
  this->refresh_.clear();
  for (auto &d : this->pubs_) {
    if (!d.discovery_done || d.device_slug.empty())
      continue;
    d.configured.clear();
    d.problem_configured = false;
#ifdef SUNGROW_MQTT_BIND_CONTROL
    d.write_configured.clear();  // backfill_write_configs_() re-sends control discovery too
#endif
    this->publish_device_(d);
  }
}

// ---------------------------------------------------------------------------------------------
// state change queue
// ---------------------------------------------------------------------------------------------

void SungrowMqttComponent::on_signal_changed_(const sungrow_datamodel::Signal &s) {
  const uint32_t key = (static_cast<uint32_t>(s.device) << 16) | s.sigid;
  std::lock_guard<std::mutex> lk(this->dirty_mutex_);
  if (this->dirty_set_.count(key))
    return;
  if (this->dirty_.size() >= DIRTY_MAX)
    return;  // drop: the next full poll round re-sends it
  this->dirty_.push_back(key);
  this->dirty_set_.insert(key);
}

void SungrowMqttComponent::drain_dirty_() {
  const uint32_t now = millis();
  for (size_t i = 0; i < DRAIN_PER_LOOP; i++) {
    if (!this->outbox_has_room_())
      return;  // keys stay queued in order; resume next loop
    uint32_t key;
    {
      std::lock_guard<std::mutex> lk(this->dirty_mutex_);
      if (this->dirty_.empty())
        return;
      key = this->dirty_.front();
      this->dirty_.pop_front();
      this->dirty_set_.erase(key);
    }
    const uint8_t index = static_cast<uint8_t>(key >> 16);
    const uint16_t sigid = static_cast<uint16_t>(key & 0xFFFF);
    DevicePub *d = this->dev_for_index_(index);
    if (d == nullptr || !d->known || d->device_slug.empty())
      continue;
    // The Problem sensor is derived from three signals; any of them changing re-evaluates it
    // (its own payload dedup drops a no-op). The signal itself is then published as usual.
    if (d->problem_configured && (sigid == sungrow_datamodel::SIG_ACTIVE_FAULT_COUNT ||
                                  sigid == sungrow_datamodel::SIG_ACTIVE_FAULT_CODES ||
                                  sigid == sungrow_datamodel::SIG_FIRST_FAULT))
      this->publish_problem_state_(*d, /*force=*/false);
    const SensorMeta *m = lookup_meta(d->family, sigid);
    if (m == nullptr) {
#ifdef SUNGROW_MQTT_BIND_CONTROL
      // Writable read-back signals (0xF1xx) are not in the read metadata; publish their control
      // state so HA number/select/switch entities track the inverter's real value on change.
      uint8_t wn = 0;
      const sungrow_control::WriteEntity *ents = this->write_entities_for_(*d, &wn);
      for (uint8_t i = 0; i < wn; i++)
        if (ents[i].sigid == sigid) {
          if (d->write_configured.count(sigid)) {
            this->publish_write_state_(*d, ents[i], /*force=*/false);
          } else if (d->discovery_done && this->dm_->store().is_valid(index, sigid) &&
                     this->publish_write_config_(*d, ents[i])) {
            this->publish_write_state_(*d, ents[i], /*force=*/true);  // read-back now valid
          }
          break;
        }
#endif
      continue;
    }
    if (!d->configured.count(sigid)) {
      // No retained config yet: backfill_configs_ publishes the config and then the state, in
      // order with the rest of the device's set. Only a valid signal is worth a scan (a change
      // event also fires when a value is cleared, e.g. the empty SBR module slots).
      if (d->discovery_done && this->dm_->store().is_valid(index, sigid))
        this->backfill_pending_ = true;
      continue;
    }
    // throttle per signal (rate-limits rapidly-changing analogs; the payload dedup below drops the
    // identical-value republishes regardless).
    auto it = this->last_state_ms_.find(key);
    if (it != this->last_state_ms_.end() && now - it->second < this->state_throttle_ms_) {
      // re-queue for later rather than dropping
      std::lock_guard<std::mutex> lk(this->dirty_mutex_);
      if (!this->dirty_set_.count(key) && this->dirty_.size() < DIRTY_MAX) {
        this->dirty_.push_back(key);
        this->dirty_set_.insert(key);
      }
      continue;
    }
    if (this->publish_signal_state_(*d, *m, /*force=*/false))
      this->last_state_ms_[key] = now;  // only start the throttle window once something was sent
  }
}

// ---------------------------------------------------------------------------------------------
// JSON builders
// ---------------------------------------------------------------------------------------------

std::string SungrowMqttComponent::build_config_json_(const DevicePub &d, const SensorMeta &m) const {
  const std::string uid = slugify_alnum(d.device_slug + "_" + m.slug, true);
  // An enum read is presented as TEXT (its value is the decoded label), like a string signal.
  const bool text_like = m.is_string || m.enum_map != nullptr;
  float mult;
  const char *unit = text_like ? "" : normalise_unit(m.unit, &mult);

  std::string j = "{";
  j += "\"name\":\"";
  json_escape(j, m.name);
  j += "\",";
  j += "\"state_topic\":\"" + this->state_topic_(d.device_slug, m.slug) + "\",";
  j += "\"unique_id\":\"" + uid + "\",";
  if (m.diagnostic)  // group==static -> HA puts it under the device's Diagnostic section
    j += "\"entity_category\":\"diagnostic\",";

  // value_template (mirror winet homeassistant.ts)
  if (text_like) {
    j += "\"value_template\":\"{{ value_json.value }}\",";
    j += "\"encoding\":\"utf-8\",";
    // A text value with a class: "timestamp" (ISO 8601 "Last update").
    if (m.is_string && m.device_class[0] != '\0')
      j += std::string("\"device_class\":\"") + m.device_class + "\",";
  } else if (m.precision == 0) {
    j += "\"value_template\":\"{{ value_json.value | int }}\",";
  } else {
    j += "\"value_template\":\"{{ value_json.value | float }}\",";
  }

  // numeric presentation
  if (!text_like) {
    if (unit[0] != '\0') {
      j += "\"unit_of_measurement\":\"";
      json_escape(j, unit);
      j += "\",";
    }
    // state_class: register_meta is authoritative; fall back to the unit table.
    const char *sc = (m.state_class[0] != '\0') ? m.state_class : state_class_for_unit(unit);
    if (sc[0] != '\0')
      j += std::string("\"state_class\":\"") + sc + "\",";
    // device_class: register_meta, then winet fallbacks (% -> battery for soc, power_factor).
    const char *dc = m.device_class;
    if (dc[0] == '\0') {
      if (std::strcmp(unit, "%") == 0) {
        const std::string sl = m.slug;
        if (sl.find("soc") != std::string::npos || sl.find("battery_level") != std::string::npos)
          dc = "battery";
      } else {
        dc = device_class_for_unit(unit);
      }
    }
    if (dc[0] == '\0' && unit[0] == '\0') {
      const std::string sl = m.slug;
      if (sl.size() >= 12 && sl.compare(sl.size() - 12, 12, "power_factor") == 0)
        dc = "power_factor";
    }
    if (dc[0] != '\0')
      j += std::string("\"device_class\":\"") + dc + "\",";
    if (m.precision >= 0)
      j += "\"suggested_display_precision\":" + std::to_string(static_cast<int>(m.precision)) + ",";
  }

  this->append_device_block_(j, d);
  j += "}";
  return j;
}

void SungrowMqttComponent::append_device_block_(std::string &j, const DevicePub &d) const {
  // availability: our own status + this device's availability (both must be online)
  j += "\"availability\":[{\"topic\":\"" + this->status_topic_() + "\"},";
  j += "{\"topic\":\"" + this->availability_topic_(d.device_slug) + "\"}],";
  j += "\"availability_mode\":\"all\",";

  // per-device HA device block (winet DeviceInfo + our sw_version / via_device). Keyed on the
  // inverter's own identity so read sensors and write controls land on the SAME HA device.
  const std::string model = d.model.empty() ? "Sungrow" : d.model;
  j += "\"device\":{";
  j += "\"identifiers\":[\"" + d.device_slug + "\"],";
  j += "\"name\":\"";
  json_escape(j, model.c_str());
  j += " ";
  json_escape(j, d.serial.c_str());
  j += "\",";
  j += "\"model\":\"";
  json_escape(j, model.c_str());
  j += "\",";
  j += "\"manufacturer\":\"Sungrow\",";
  j += "\"serial_number\":\"";
  json_escape(j, d.serial.c_str());
  j += "\"";
  if (!this->sw_version_.empty()) {
    j += ",\"sw_version\":\"";
    json_escape(j, this->sw_version_.c_str());
    j += "\"";
  }
  j += ",\"via_device\":\"" + this->dongle_id_() + "\"";
  j += "}";
}

std::string SungrowMqttComponent::build_state_json_(double value, const char *unit) {
  std::string j = "{\"value\":" + fmt_number(value) + ",\"unit_of_measurement\":\"";
  json_escape(j, unit);
  j += "\"}";
  return j;
}

std::string SungrowMqttComponent::build_state_json_str_(const std::string &value) {
  std::string j = "{\"value\":\"";
  json_escape(j, value.c_str());
  j += "\"}";
  return j;
}

// ---------------------------------------------------------------------------------------------
// "Problem" binary_sensor
// ---------------------------------------------------------------------------------------------

bool SungrowMqttComponent::publish_problem_config_(DevicePub &d) {
  if (this->publish_(this->problem_topic_(d.device_slug, "config"), this->build_problem_config_json_(d), 1, true) < 0)
    return false;
  d.problem_configured = true;
  return true;
}

bool SungrowMqttComponent::publish_problem_state_(const DevicePub &d, bool force) {
  const uint32_t key = (static_cast<uint32_t>(d.index) << 16) | PROBLEM_KEY;
  if (!this->dm_->store().is_valid(d.index, sungrow_datamodel::SIG_ACTIVE_FAULT_COUNT)) {
    this->last_payload_.erase(key);  // republish when the fault list is decoded again
    return false;
  }
  return this->publish_state_payload_(key, this->problem_topic_(d.device_slug, "state"),
                                      this->build_problem_state_json_(d), force);
}

std::string SungrowMqttComponent::build_problem_config_json_(const DevicePub &d) const {
  const std::string topic = this->problem_topic_(d.device_slug, "state");
  std::string j = "{\"name\":\"Problem\",";
  j += "\"device_class\":\"problem\",";
  j += "\"unique_id\":\"" + slugify_alnum(d.device_slug + "_problem", true) + "\",";
  j += "\"state_topic\":\"" + topic + "\",";
  j += "\"value_template\":\"{{ value_json.value }}\",";
  j += "\"json_attributes_topic\":\"" + topic + "\",";
  j += "\"json_attributes_template\":\"{{ value_json.attributes | tojson }}\",";
  this->append_device_block_(j, d);
  j += "}";
  return j;
}

// {"value":"ON","attributes":{"count":2,"codes":[39,933],"names":["Low insulation resistance",
// "alarm"],"first":"39 Low insulation resistance"}}. The codes and names come from the 0xF061 text
// ("39 Low insulation resistance, 933 alarm"): entries split on ", ", each a code followed by its
// name; "No active faults" yields empty lists.
std::string SungrowMqttComponent::build_problem_state_json_(const DevicePub &d) const {
  const SignalStore &store = this->dm_->store();
  const int count = static_cast<int>(store.get_number(d.index, sungrow_datamodel::SIG_ACTIVE_FAULT_COUNT));
  std::string codes, names;
  if (count > 0 && store.is_valid(d.index, sungrow_datamodel::SIG_ACTIVE_FAULT_CODES)) {
    const std::string text = store.get_string(d.index, sungrow_datamodel::SIG_ACTIVE_FAULT_CODES);
    size_t pos = 0;
    while (pos < text.size()) {
      size_t end = text.find(", ", pos);
      if (end == std::string::npos)
        end = text.size();
      const std::string entry = text.substr(pos, end - pos);
      pos = end + 2;
      char *rest = nullptr;
      const long code = std::strtol(entry.c_str(), &rest, 10);
      if (rest == entry.c_str())
        continue;  // no leading code
      while (*rest == ' ')
        rest++;
      if (!codes.empty()) {
        codes += ',';
        names += ',';
      }
      codes += std::to_string(code);
      names += '"';
      json_escape(names, rest);
      names += '"';
    }
  }
  std::string j = "{\"value\":\"";
  j += count > 0 ? "ON" : "OFF";
  j += "\",\"attributes\":{\"count\":" + std::to_string(count);
  j += ",\"codes\":[" + codes + "],\"names\":[" + names + "],\"first\":\"";
  if (count > 0 && store.is_valid(d.index, sungrow_datamodel::SIG_FIRST_FAULT))
    json_escape(j, store.get_string(d.index, sungrow_datamodel::SIG_FIRST_FAULT).c_str());
  j += "\"}}";
  return j;
}

// ---------------------------------------------------------------------------------------------
// writes: per-device number / select / switch discovery + command handling
// ---------------------------------------------------------------------------------------------

void SungrowMqttComponent::subscribe_commands_(const DevicePub & /*d*/) {
  // The single wildcard `sungrow-mqtt/<id>/+/+/set` subscribed on connect covers every device,
  // so there is nothing per-device to do. Kept as a seam for a later per-entity subscribe.
}

#ifdef SUNGROW_MQTT_BIND_CONTROL

const char *SungrowMqttComponent::write_component_(sungrow_control::WriteKind k) const {
  switch (k) {
    case sungrow_control::WriteKind::NUMBER: return "number";
    case sungrow_control::WriteKind::SELECT: return "select";
    case sungrow_control::WriteKind::SWITCH: return "switch";
  }
  return "number";
}

const sungrow_control::WriteEntity *SungrowMqttComponent::write_entities_for_(const DevicePub &d,
                                                                              uint8_t *count) const {
  *count = 0;
  if (this->control_ == nullptr)
    return nullptr;
  const sungrow_control::ControlCore::DeviceCfg *cfg = this->control_->core().device(d.index);
  if (cfg == nullptr || cfg->profile == nullptr)
    return nullptr;
  return sungrow_control::writable_entities(cfg->profile->profile, count);
}

std::string SungrowMqttComponent::build_write_config_json_(const DevicePub &d,
                                                           const sungrow_control::WriteEntity &e) const {
  using sungrow_control::WriteKind;
  const std::string uid = slugify_alnum(d.device_slug + "_" + e.key, true);

  std::string j = "{";
  j += "\"name\":\"";
  json_escape(j, e.name);
  j += "\",";
  j += "\"unique_id\":\"" + uid + "\",";
  j += "\"state_topic\":\"" + this->write_state_topic_(this->write_component_(e.kind), d.device_slug, e.key) + "\",";
  j += "\"command_topic\":\"" + this->command_topic_(d.device_slug, e.key) + "\",";
  j += "\"value_template\":\"{{ value_json.value }}\",";
  j += "\"entity_category\":\"config\",";
  j += "\"optimistic\":false,";

  if (e.kind == WriteKind::NUMBER) {
    j += "\"min\":" + fmt_number(e.number_min) + ",";
    j += "\"max\":" + fmt_number(e.number_max) + ",";
    j += "\"step\":" + fmt_number(e.step) + ",";
    j += "\"mode\":\"box\",";
    if (e.unit[0] != '\0') {
      j += "\"unit_of_measurement\":\"";
      json_escape(j, e.unit);
      j += "\",";
    }
  } else if (e.kind == WriteKind::SELECT) {
    j += "\"options\":[";
    for (uint8_t i = 0; i < e.noptions; i++) {
      if (i)
        j += ",";
      j += "\"";
      json_escape(j, e.options[i].text);
      j += "\"";
    }
    j += "],";
  } else {  // SWITCH: options[0] = on, options[1] = off
    const char *on_txt = e.noptions > 0 ? e.options[0].text : "Enabled";
    const char *off_txt = e.noptions > 1 ? e.options[1].text : "Disabled";
    j += "\"payload_on\":\"";
    json_escape(j, on_txt);
    j += "\",\"payload_off\":\"";
    json_escape(j, off_txt);
    j += "\",\"state_on\":\"";
    json_escape(j, on_txt);
    j += "\",\"state_off\":\"";
    json_escape(j, off_txt);
    j += "\",";
  }

  this->append_device_block_(j, d);
  j += "}";
  return j;
}

bool SungrowMqttComponent::publish_write_config_(DevicePub &d, const sungrow_control::WriteEntity &e) {
  const std::string topic = this->write_config_topic_(this->write_component_(e.kind), d.device_slug, e.key);
  const std::string payload = this->build_write_config_json_(d, e);
  if (this->publish_(topic, payload, 1, true) < 0)
    return false;
  d.write_configured.insert(e.sigid);
  return true;
}

void SungrowMqttComponent::publish_write_state_(const DevicePub &d, const sungrow_control::WriteEntity &e,
                                                bool force) {
  using sungrow_control::WriteKind;
  const SignalStore &store = this->dm_->store();
  const uint32_t key = (static_cast<uint32_t>(d.index) << 16) | e.sigid;
  if (!store.is_valid(d.index, e.sigid)) {
    this->last_payload_.erase(key);  // no read-back yet; force a republish when it next becomes valid
    return;
  }
  std::string payload;
  if (e.kind == WriteKind::NUMBER) {
    payload = build_state_json_(store.get_number(d.index, e.sigid), e.unit);
  } else {
    uint64_t raw = 0;
    if (!store.get_bits(d.index, e.sigid, &raw))
      return;
    std::vector<sungrow_control::Option> opts;
    for (uint8_t i = 0; i < e.noptions; i++)
      opts.push_back({e.options[i].raw, e.options[i].text});
    const char *text = sungrow_control::raw_to_option(opts, static_cast<uint16_t>(raw));
    if (text == nullptr)
      return;  // inverter holds a code with no option text (e.g. EMS MicroGrid): do not publish
    payload = build_state_json_str_(text);
  }
  this->publish_state_payload_(key, this->write_state_topic_(this->write_component_(e.kind), d.device_slug, e.key),
                               payload, force);
}

void SungrowMqttComponent::backfill_write_configs_() {
  if (this->control_ == nullptr)
    return;
  size_t budget = CONFIG_PER_LOOP;
  for (auto &d : this->pubs_) {
    if (budget == 0)
      break;
    if (!d.discovery_done || d.device_slug.empty())
      continue;
    uint8_t n = 0;
    const sungrow_control::WriteEntity *ents = this->write_entities_for_(d, &n);
    for (uint8_t i = 0; i < n && budget > 0; i++) {
      if (d.write_configured.count(ents[i].sigid))
        continue;
      // Only controls the device supports: published once its read-back has been valid (an
      // unsupported register answers the 0xFFFF sentinel, e.g. 33149/33150 on the SH10RS).
      if (!this->dm_->store().is_valid(d.index, ents[i].sigid))
        continue;
      // Battery controls stay out while the inverter reports "No battery".
      if (ents[i].requires_battery && !sungrow_datamodel::has_battery(this->dm_->store(), d.index))
        continue;
      if (!this->outbox_has_room_())
        return;  // resume next loop
      if (this->publish_write_config_(d, ents[i])) {
        this->publish_write_state_(d, ents[i], /*force=*/true);
        budget--;
      }
    }
  }
}

void SungrowMqttComponent::publish_expect_(uint8_t index, const sungrow_control::ExpectStatus &st) {
  if (!this->session_up_)
    return;
  for (const auto &d : this->pubs_) {
    if (d.index != index || d.device_slug.empty())
      continue;
    std::string j = std::string("{\"ok\":") + (!st.final || st.ok ? "true" : "false") + ",\"status\":\"";
    json_escape(j, st.text.c_str());
    j += "\",\"state\":";
    if (st.state_valid) {
      char code[12];
      std::snprintf(code, sizeof(code), "\"0x%04X\"", st.state);
      j += code;
    } else {
      j += "null";
    }
    j += st.final ? ",\"final\":true}" : ",\"final\":false}";
    this->publish_(this->result_topic_(d.device_slug, st.key), j, 0, false);
    return;
  }
}

void SungrowMqttComponent::note_write_settle_(uint8_t index, uint16_t sigid) {
  const uint32_t deadline = millis() + SETTLE_WINDOW_MS;
  for (auto &w : this->write_settle_)
    if (w.index == index && w.sigid == sigid) {
      w.deadline_ms = deadline;  // repeat write to the same entity: just extend the window
      return;
    }
  if (this->write_settle_.size() >= SETTLE_MAX)
    return;  // bounded; the 60s full refresh is the backstop
  this->write_settle_.push_back({index, sigid, deadline});
}

void SungrowMqttComponent::drain_settle_() {
  if (this->write_settle_.empty() || !this->outbox_has_room_())
    return;
  const uint32_t now = millis();
  if (now - this->last_settle_ms_ < 250)
    return;  // re-check at most ~4x/s; the store readback cadence is far slower
  this->last_settle_ms_ = now;
  for (size_t i = 0; i < this->write_settle_.size();) {
    WriteSettle &w = this->write_settle_[i];
    DevicePub *d = this->dev_for_index_(w.index);
    bool drop = static_cast<int32_t>(now - w.deadline_ms) >= 0 || d == nullptr || !d->discovery_done ||
                d->device_slug.empty();
    if (!drop) {
      // Re-publish the entity state deduped: emits exactly once, the moment the store (via the
      // verify-read or the 10s block-poll) reflects the new value -- no spam while unchanged.
      uint8_t n = 0;
      const sungrow_control::WriteEntity *ents = this->write_entities_for_(*d, &n);
      for (uint8_t j = 0; j < n; j++)
        if (ents[j].sigid == w.sigid) {
          this->publish_write_state_(*d, ents[j], /*force=*/false);
          break;
        }
    }
    if (drop) {
      this->write_settle_[i] = this->write_settle_.back();
      this->write_settle_.pop_back();
    } else {
      i++;
    }
  }
}

void SungrowMqttComponent::handle_command_(const std::string &topic, const std::string &payload) {
  // topic = sungrow-mqtt/<dongle>/<device_slug>/<key>/set
  size_t p2 = topic.find('/');
  size_t p3 = p2 == std::string::npos ? p2 : topic.find('/', p2 + 1);
  size_t p4 = p3 == std::string::npos ? p3 : topic.find('/', p3 + 1);
  size_t p5 = p4 == std::string::npos ? p4 : topic.find('/', p4 + 1);
  if (p5 == std::string::npos || topic.compare(p5 + 1, std::string::npos, "set") != 0) {
    ESP_LOGW(TAG, "Command on %s ignored: unexpected topic shape", topic.c_str());
    return;
  }
  const std::string device_slug = topic.substr(p3 + 1, p4 - p3 - 1);
  const std::string key = topic.substr(p4 + 1, p5 - p4 - 1);

  DevicePub *d = nullptr;
  for (auto &dp : this->pubs_)
    if (dp.device_slug == device_slug) {
      d = &dp;
      break;
    }
  if (d == nullptr) {
    ESP_LOGW(TAG, "Command on %s ignored: unknown device '%s'", topic.c_str(), device_slug.c_str());
    return;
  }

  uint8_t n = 0;
  const sungrow_control::WriteEntity *ents = this->write_entities_for_(*d, &n);
  const sungrow_control::WriteEntity *e = nullptr;
  for (uint8_t i = 0; i < n; i++)
    if (key == ents[i].key) {
      e = &ents[i];
      break;
    }
  if (e == nullptr) {
    ESP_LOGW(TAG, "Command on %s ignored: '%s' is not a writable entity of this device", topic.c_str(),
             key.c_str());
    return;
  }

  // Convert the HA payload to a raw register value (the authoritative allow-list / value-set check
  // and the writes_enabled gate are re-applied inside SungrowControl::write()).
  uint16_t raw = 0;
  bool converted;
  if (e->kind == sungrow_control::WriteKind::NUMBER) {
    char *end = nullptr;
    const double val = std::strtod(payload.c_str(), &end);
    converted = end != payload.c_str() &&
                sungrow_control::number_to_raw(static_cast<float>(val), e->scale, e->type, e->raw_min, e->raw_max,
                                               &raw);
  } else {
    std::vector<sungrow_control::Option> opts;
    for (uint8_t i = 0; i < e->noptions; i++)
      opts.push_back({e->options[i].raw, e->options[i].text});
    // trim a trailing CR/LF/space a broker client may append
    std::string text = payload;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
      text.pop_back();
    converted = sungrow_control::option_to_raw(opts, text, &raw);
  }

  const std::string result_topic = this->result_topic_(device_slug, e->key);
  if (!converted) {
    ESP_LOGW(TAG, "Command on %s: payload '%s' not convertible", topic.c_str(), payload.c_str());
    this->publish_(result_topic, "{\"ok\":false,\"status\":\"bad payload\"}", 0, false);
    this->publish_write_state_(*d, *e, /*force=*/true);  // re-assert the inverter's value so HA snaps back
    return;
  }

  const bool verify = this->control_->default_verify();
  const std::string source = "mqtt " + device_slug + "/" + key;
  // The entity's register may depend on the device's protocol (start/stop: 5006 / 13000 / 30045).
  const uint16_t address = this->control_->core().address_for_sigid(d->index, e->sigid, e->address);
  sungrow_control::Verdict v = this->control_->write(
      d->index, address, &raw, 1, verify, source.c_str(),
      [this, result_topic](const sungrow_modbus::WriteResult &r) {
        const bool ok = r.status == sungrow_modbus::WriteStatus::OK;
        std::string j = std::string("{\"ok\":") + (ok ? "true" : "false") + ",\"status\":\"";
        json_escape(j, sungrow_modbus::write_status_name(r.status));
        j += "\"}";
        this->publish_(result_topic, j, 0, false);
      });
  if (v != sungrow_control::Verdict::OK) {
    std::string j = std::string("{\"ok\":false,\"status\":\"") + sungrow_control::verdict_name(v) + "\"}";
    this->publish_(result_topic, j, 0, false);
    this->publish_write_state_(*d, *e, /*force=*/true);  // rejected: HA snaps back to the real value
    return;
  }
  // Accepted: watch this entity's read-back sigid and re-publish its state (deduped) until it
  // reflects the new value, so HA follows the store within ~1s without relying on the change-notify.
  this->note_write_settle_(d->index, e->sigid);
}

#else  // !SUNGROW_MQTT_BIND_CONTROL

void SungrowMqttComponent::handle_command_(const std::string &topic, const std::string & /*payload*/) {
  ESP_LOGW(TAG, "Command on %s ignored: no sungrow_control bound", topic.c_str());
}

#endif  // SUNGROW_MQTT_BIND_CONTROL

// ---------------------------------------------------------------------------------------------

void SungrowMqttComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Sungrow MQTT (per-device HA discovery):");
  if (this->is_configured()) {
    ESP_LOGCONFIG(TAG, "  Broker: %s://%s:%u (%s)", this->tls_ ? "mqtts" : "mqtt", this->broker_.c_str(), this->port_,
                  mqtt_settings_source_name(this->source_));
  } else {
    ESP_LOGCONFIG(TAG, "  Broker: not configured%s", this->runtime_settings_ ? " (web UI Settings page)" : "");
  }
  ESP_LOGCONFIG(TAG, "  Discovery prefix: %s", this->prefix_.c_str());
  ESP_LOGCONFIG(TAG, "  Credentials: %s", this->username_.empty() ? "(none)" : "(set)");  // never log values
  ESP_LOGCONFIG(TAG, "  Dongle id (via_device / root): %s", this->cached_dongle_id_.c_str());
  ESP_LOGCONFIG(TAG, "  State throttle: %u ms", static_cast<unsigned>(this->state_throttle_ms_));
  ESP_LOGCONFIG(TAG, "  Deadband scale: %.2f (0 = publish every change)", this->deadband_scale_);
#ifdef SUNGROW_MQTT_BIND_CONTROL
  ESP_LOGCONFIG(TAG, "  Writes: control bound (number/select/switch discovery; gated by writes_enabled)");
#else
  ESP_LOGCONFIG(TAG, "  Writes: not configured");
#endif
}

}  // namespace sungrow_mqtt
}  // namespace esphome
