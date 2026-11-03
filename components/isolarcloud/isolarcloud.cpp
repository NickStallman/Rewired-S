#include "isolarcloud.h"

#include <cinttypes>
#include <cstring>
#include <memory>
#include <sys/time.h>

#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_tls.h>
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif

#include "esphome/components/network/util.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "json_compat.h"

#if defined(USE_WINET_S) || defined(USE_WINET_S2)
#include "esphome/components/sungrow_board/sungrow_board.h"
#define ISOLARCLOUD_HAVE_BOARD
#endif

namespace esphome::isolarcloud {

static const char *const TAG = "isolarcloud";

static constexpr uint32_t CONNECT_TIMEOUT_MS = 30000;
static constexpr size_t RX_MAX_QUEUE = 8;

// ---------------------------------------------------------------------------------------------
// Persistence (ESPHome preferences / NVS). Holds the last effective identity so the uplink comes
// back with the same SN/broker if the identity source is unavailable, the result of a server
// switch (which must win over the read-only extflash copy) and cloud-set values (settimezone,
// 10384 rundata period).
// ---------------------------------------------------------------------------------------------
namespace {
constexpr uint32_t PERSIST_MAGIC = 0x49434C33;  // "ICL3"

struct PersistedServer {
  char host[65];
  uint16_t port;
  uint16_t ssl_flag;
  uint8_t key[16];
} __attribute__((packed));

/// Live-data nodes (SecondData::serialize()), NUL-terminated.
struct PersistedSecond {
  uint32_t magic;
  char text[3072];
};
static constexpr uint32_t SECOND_MAGIC = 0x53454331;  // "SEC1"
// devstatusdata / generaldata change poll period, and the all-device devstatusdata period
// (stock 0x1c1f compared with '>').
static constexpr uint32_t STATUS_POLL_S = 5;
static constexpr uint32_t STATUS_ALL_S = 7200;

struct PersistedIdentity {
  uint32_t magic;
  char sn[24];
  uint16_t encrypt, keynum, compress;
  PersistedServer server;
  PersistedServer old_server;
  uint16_t timezone_id;   // 0x107a
  char timezone[17];      // 0x1089
  uint16_t rundata_period_min;
  uint8_t silence_enable, silence_start, silence_end;
  uint32_t remote_ctl_mask;
  uint32_t source_mask;  // IdentityField bits that came from the identity source
  uint32_t cloud_mask;   // IdentityField bits set by cloud commands
  uint8_t server_from_switch;
  uint8_t switch_pending;
} __attribute__((packed));

void to_persisted(const ServerEndpoint &s, PersistedServer &p) {
  memset(&p, 0, sizeof(p));
  strncpy(p.host, s.host.c_str(), sizeof(p.host) - 1);
  p.port = s.port;
  p.ssl_flag = s.ssl_flag;
  memcpy(p.key, s.key.data(), sizeof(p.key));
}
void from_persisted(const PersistedServer &p, ServerEndpoint &s) {
  s.host.assign(p.host, strnlen(p.host, sizeof(p.host)));
  s.port = p.port;
  s.ssl_flag = p.ssl_flag;
  memcpy(s.key.data(), p.key, sizeof(p.key));
}

[[maybe_unused]] std::string key_printable(const MqttKey &k) {
  std::string s;
  for (uint8_t c : k) {
    if (c == 0)
      break;
    s += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '?';
  }
  return s;
}

constexpr uint32_t CLOUD_FIELDS = FIELD_TIMEZONE | FIELD_TIMEZONE_ID | FIELD_RUNDATA_PERIOD;
constexpr uint32_t SWITCH_FIELDS = FIELD_SERVER_ANY | FIELD_OLD_SERVER;
}  // namespace

// ---------------------------------------------------------------------------------------------
// setup / identity
// ---------------------------------------------------------------------------------------------
void IsolarcloudComponent::setup() {
  if (!this->formatter_)
    this->formatter_ = std::make_unique<BusinessFormatter>();
  this->boot_ms_ = millis();
  this->down_since_ms_ = this->boot_ms_;
  // Per-boot random rundata delay: 60..179 s.
  this->scheduler_ = CloudScheduler(stock_random_delay(esp_random()), this->timing_);
  this->pref_ = global_preferences->make_preference<PersistedIdentity>(fnv1_hash("isolarcloud_identity"), true);
  this->load_persisted_();
  this->second_pref_ = global_preferences->make_preference<PersistedSecond>(fnv1_hash("isolarcloud_second"), true);
  {
    auto p = std::make_unique<PersistedSecond>();
    if (this->second_pref_.load(p.get()) && p->magic == SECOND_MAGIC) {
      p->text[sizeof(p->text) - 1] = '\0';
      if (this->second_.deserialize(p->text) && this->second_.active())
        ESP_LOGI(TAG, "Live data restored: %u device node(s)", static_cast<unsigned>(this->second_.nodes().size()));
    }
  }
  this->install_default_handlers_();
  this->resolve_identity_("setup");
  if (this->resume_switch_)
    this->switch_.resume(millis());
  if (this->identity_source_ != nullptr)
    this->identity_source_->add_on_loaded_callback([this]() { this->on_identity_loaded(); });
  this->queue_.set_done_callback([this](const OutMessage &m, bool ok) {
    if (!ok)
      this->set_error_("no PUBACK for " + m.label + ", dropped");
  });
  this->next_attempt_ms_ = millis();
}

void IsolarcloudComponent::set_identity_source(IdentitySource *src) { this->identity_source_ = src; }

bool IsolarcloudComponent::has_error() const {
  if (!this->enabled_)
    return false;
  std::string why;
  if (!this->identity_usable_(&why) || this->last_connect_failed_)
    return true;
  return !this->session_up_ && millis() - this->down_since_ms_ > 60000;
}

void IsolarcloudComponent::on_identity_loaded() { this->resolve_identity_("identity source loaded"); }

void IsolarcloudComponent::load_persisted_() {
  PersistedIdentity p{};
  if (!this->pref_.load(&p) || p.magic != PERSIST_MAGIC)
    return;
  CloudIdentity &id = this->persisted_;
  id.sn.assign(p.sn, strnlen(p.sn, sizeof(p.sn)));
  id.encrypt = p.encrypt;
  id.keynum = p.keynum;
  id.compress = p.compress;
  from_persisted(p.server, id.server);
  from_persisted(p.old_server, id.old_server);
  id.timezone_id = p.timezone_id;
  id.timezone.assign(p.timezone, strnlen(p.timezone, sizeof(p.timezone)));
  id.rundata_period_min = p.rundata_period_min;
  id.silence_enable = p.silence_enable != 0;
  id.silence_start_hour = p.silence_start;
  id.silence_end_hour = p.silence_end;
  id.remote_ctl_mask = p.remote_ctl_mask;
  this->persisted_mask_ = p.source_mask;
  this->source_mask_ = p.source_mask;
  this->cloud_mask_ = p.cloud_mask & CLOUD_FIELDS;
  this->server_from_switch_ = p.server_from_switch != 0;
  this->resume_switch_ = p.switch_pending != 0;
}

void IsolarcloudComponent::save_persisted_() {
  PersistedIdentity p{};
  const CloudIdentity &id = this->identity_;
  p.magic = PERSIST_MAGIC;
  strncpy(p.sn, id.sn.c_str(), sizeof(p.sn) - 1);
  p.encrypt = id.encrypt;
  p.keynum = id.keynum;
  p.compress = id.compress;
  to_persisted(id.server, p.server);
  to_persisted(id.old_server, p.old_server);
  p.timezone_id = id.timezone_id;
  strncpy(p.timezone, id.timezone.c_str(), sizeof(p.timezone) - 1);
  p.rundata_period_min = id.rundata_period_min;
  p.silence_enable = id.silence_enable ? 1 : 0;
  p.silence_start = id.silence_start_hour;
  p.silence_end = id.silence_end_hour;
  p.remote_ctl_mask = id.remote_ctl_mask;
  p.source_mask = this->source_mask_;
  p.cloud_mask = this->cloud_mask_;
  p.server_from_switch = this->server_from_switch_ ? 1 : 0;
  p.switch_pending = this->switch_.confirming() ? 1 : 0;

  PersistedIdentity old{};
  if (this->pref_.load(&old) && memcmp(&old, &p, sizeof(p)) == 0)
    return;  // unchanged: don't wear the flash
  if (this->pref_.save(&p))
    global_preferences->sync();
  this->persisted_ = id;
  this->persisted_mask_ = this->source_mask_;
}

// Precedence (low -> high): defaults, persisted copy of earlier identity-source values, identity
// source (extflash), svr_convert result (beats the source's server fields), cloud-set values
// (settimezone / 10384), explicit YAML values. productType/productName: YAML, else the
// sungrow_board compile-time constants (the stored 0x0004 is not used on the cloud path). The
// remote-control mask default depends on the variant (WiNet-S: no Layout group), so it is applied
// once the product type is known, unless a layer set the mask.
void IsolarcloudComponent::resolve_identity_(const char *why) {
  CloudIdentity id;
  apply_identity_fields(id, this->persisted_, this->persisted_mask_ & ~(FIELD_PRODUCT_TYPE | FIELD_PRODUCT_NAME));
  if (this->identity_source_ != nullptr && this->identity_source_->identity_ready()) {
    CloudIdentity s = id;
    uint32_t mask = this->identity_source_->read_identity(s) & ~(FIELD_PRODUCT_TYPE | FIELD_PRODUCT_NAME);
    uint32_t skip = (this->server_from_switch_ ? SWITCH_FIELDS : 0) | this->cloud_mask_;
    apply_identity_fields(id, s, mask & ~skip);
    this->source_mask_ = mask;
  }
  const CloudIdentity &prev = this->identity_.sn.empty() ? this->persisted_ : this->identity_;
  if (this->server_from_switch_)
    apply_identity_fields(id, prev, SWITCH_FIELDS);
  apply_identity_fields(id, prev, this->cloud_mask_);
  apply_identity_fields(id, this->yaml_identity_, this->yaml_mask_);

#ifdef ISOLARCLOUD_HAVE_BOARD
  if (sungrow_board::global_board != nullptr) {
    if (!(this->yaml_mask_ & FIELD_PRODUCT_TYPE))
      id.product_type = sungrow_board::global_board->product_type();
    if (!(this->yaml_mask_ & FIELD_PRODUCT_NAME))
      id.product_name = sungrow_board::global_board->product_name();
    if (!this->firmware_version_from_yaml_)
      this->firmware_version_ = sungrow_board::global_board->firmware_version();
  }
#endif
  if (!((this->source_mask_ | this->persisted_mask_ | this->yaml_mask_) & FIELD_REMOTE_CTL_MASK))
    id.remote_ctl_mask = default_remote_ctl_mask(id.product_type);

  int32_t off = 0;
  if (!parse_cloud_timezone(id.timezone, off)) {
    ESP_LOGW(TAG, "Bad timezone '%s', using %s", id.timezone.c_str(), DEFAULT_TIMEZONE);
    id.timezone = DEFAULT_TIMEZONE;
    parse_cloud_timezone(id.timezone, off);
  }
  this->tz_offset_s_ = off;

  const bool reconnect = id.sn != this->identity_.sn || id.server != this->identity_.server ||
                         id.encrypt != this->identity_.encrypt || id.keynum != this->identity_.keynum ||
                         id.compress != this->identity_.compress ||
                         id.remote_ctl_mask != this->identity_.remote_ctl_mask;
  const bool tz_id_changed = id.timezone_id != this->identity_.timezone_id;
  this->identity_ = id;
  this->topics_ = TopicBuilder(id.sn, TopicEnum{id.encrypt, id.keynum, id.compress});
  this->refresh_logger_signals_();
  ESP_LOGD(TAG, "Identity resolved (%s): SN '%s', broker %s:%u ssl %u, tz %s (id %u)", why, id.sn.c_str(),
           id.server.host.c_str(), id.server.port, id.server.ssl_flag, id.timezone.c_str(), id.timezone_id);
  if (id.valid())
    this->save_persisted_();
  if (reconnect && this->client_ != nullptr)
    this->restart_requested_ = true;
  else if (tz_id_changed && this->session_up_)
    this->update_group_subscription_();
}

void IsolarcloudComponent::refresh_logger_signals_() {
  const CloudIdentity &id = this->identity_;
  LoggerSignals &l = this->logger_sigs_;
  // 0xF7 sigs the logger elements read (identity, fixed values per variant).
  l.set_str(0x0001, id.product_name);  // model
  l.set_str(0x0002, id.sn);
  l.set_u16(0x0004, id.product_type);  // also attribute point "-1"
  l.set_str(0x0005, id.product_name);
  // 0x0104 functionEnum: the variant's 16-byte capability bitmap. The cloud reads it from the
  // logger's rsp/propertydata and gates features such as Live data on it.
  const bool winet_s = is_winet_s(id.product_type);
  l.set_bytes(0x0104, function_enum_bytes(winet_s));
  if (this->formatter_) {
    this->formatter_->set_device_function_enum(function_enum_bytes(winet_s));
    this->formatter_->set_winet_s(winet_s);
  }
  if (!this->firmware_version_.empty())
    l.set_str(0x1000, this->firmware_version_);
  l.set_u32(0x101b, 86400);
  l.set_u16(0x001d, 152);
  l.set_u16(0x0025, winet_s ? 1068 : 1580);
  l.set_u32(0x6492, id.remote_ctl_mask);
  // Logger attribute points (rsp/propertydata) with fixed values.
  l.set_u32(0x6496, default_remote_ctl_mask(id.product_type));  // point 1246
  l.set_u16(0x1102, 3);      // point 29807
  l.set_u32(0x2061, 0);      // point 23248
  l.set_u16(0x2068, 0);      // point 23281
  l.set_u16(0x1391, 0);      // point 23274
  l.set_u16(0x1331, 1);      // point 23275
  if (!winet_s) {
    l.set_str(0x177b, "1-WINET-SV300.001.03.B001;2-32MB");  // point 23246
    l.set_str(0x2060, "2022-09-01 18:16:01");               // point 23249
  }
  this->refresh_logger_network_();
}

void IsolarcloudComponent::refresh_logger_network_() {
  // 0x1202 active link IP (point 23241, devinfo "5767"), 0x1203 Wi-Fi SSID (point 23242).
  char ip[network::IP_ADDRESS_BUFFER_SIZE] = "";
  for (const auto &a : network::get_ip_addresses()) {
    if (a.is_ip4() && a.is_set()) {
      a.str_to(ip);
      break;
    }
  }
  this->logger_sigs_.set_str(0x1202, ip);
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    char ssid[wifi::SSID_BUFFER_SIZE] = "";
    this->logger_sigs_.set_str(0x1203, wifi::global_wifi_component->wifi_ssid_to(ssid));
  }
#endif
}

bool IsolarcloudComponent::identity_usable_(std::string *why) const {
  if (this->identity_.sn.empty()) {
    *why = this->identity_source_ != nullptr ? "waiting for identity source (no SN)" : "no SN configured";
    return false;
  }
  if (this->identity_.product_type == 0) {
    *why = "productType is 0 (set sungrow_board product_type)";
    return false;
  }
  if (this->identity_.server.host.empty() || this->identity_.server.port == 0) {
    *why = "no broker host/port";
    return false;
  }
  if (this->identity_.server.transport() == Transport::INVALID) {
    // ssl flag other than 0/2: the link never connects.
    *why = "protocol not found (ssl flag " + std::to_string(this->identity_.server.ssl_flag) + ")";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// enable / server switch
// ---------------------------------------------------------------------------------------------
void IsolarcloudComponent::set_enabled(bool enabled) {
  if (enabled == this->enabled_)
    return;
  this->enabled_ = enabled;
  ESP_LOGI(TAG, "Uplink %s", enabled ? "enabled" : "disabled");
  if (!enabled)
    this->stop_client_("uplink disabled");
  this->last_connect_failed_ = false;
  this->down_since_ms_ = millis();
  this->backoff_.reset();
  this->next_attempt_ms_ = millis();
}

void IsolarcloudComponent::set_writes_enabled(bool enabled) {
  if (enabled == this->writes_enabled_)
    return;
  this->writes_enabled_ = enabled;
  ESP_LOGI(TAG, "Cloud writes %s", enabled ? "enabled" : "disabled (read-only)");
}

bool IsolarcloudComponent::request_server_switch(const ServerEndpoint &next) {
  if (!this->switch_.request(this->identity_, next, millis())) {
    ESP_LOGI(TAG, "svr_convert to %s:%u ignored: host/port/key unchanged", next.host.c_str(), next.port);
    return false;
  }
  ESP_LOGW(TAG, "svr_convert requested: %s:%u ssl %u -> %s:%u ssl %u", this->identity_.server.host.c_str(),
           this->identity_.server.port, this->identity_.server.ssl_flag, next.host.c_str(), next.port, next.ssl_flag);
  return true;
}

// ---------------------------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------------------------
void IsolarcloudComponent::loop() {
  const uint32_t now = millis();

  // Errors reported by the mqtt task.
  {
    std::lock_guard<std::mutex> lock(this->err_mutex_);
    if (!this->pending_error_.empty()) {
      std::string e = std::move(this->pending_error_);
      this->pending_error_.clear();
      this->last_error_ = e;
      this->diag_dirty_ = true;
      ESP_LOGW(TAG, "%s", e.c_str());
    }
  }

  // Connection edges.
  if (this->connect_event_.exchange(false)) {
    this->session_up_ = true;
    this->last_connect_failed_ = false;
    this->on_connected_();
  }
  if (this->disconnect_event_.exchange(false) && this->client_ != nullptr && !this->connected_.load()) {
    this->stop_client_(this->session_up_ ? "disconnected" : "connect failed");  // resets backoff if it was up
  }
  if (this->client_ != nullptr && !this->session_up_ && now - this->client_started_ms_ > CONNECT_TIMEOUT_MS) {
    this->set_error_("connect timeout");
    this->stop_client_("connect timeout");
  }
  if (this->restart_requested_) {
    this->restart_requested_ = false;
    this->stop_client_("identity changed");
    this->next_attempt_ms_ = now;
  }

  // Inbound messages + PUBACKs (the MQTT pump and inbound keep running in the silence window).
  std::vector<int> acks;
  for (int i = 0; i < 2; i++) {
    Inbound msg;
    {
      std::lock_guard<std::mutex> lock(this->rx_mutex_);
      if (i == 0)
        acks.swap(this->pubacks_);
      if (this->rx_queue_.empty())
        break;
      msg = std::move(this->rx_queue_.front());
      this->rx_queue_.pop_front();
    }
    this->process_inbound_(msg);
  }
  for (int id : acks)
    this->queue_.on_puback(id, now);
  // The portal's online flag follows beatdata: beat again when the reported device set changes (a
  // battery identified after the connect-time beat, a device going on/offline), not only every
  // 1800 s; at most every 30 s.
  if (this->session_up_ && this->client_ != nullptr && now - this->beat_check_ms_ >= 5000) {
    this->beat_check_ms_ = now;
    std::vector<TelemetryDevice> devs;
    this->list_devices_(devs);
    if (this->beat_signature_(devs) != this->beat_sig_ && now - this->last_beat_ms_ >= 30000)
      this->publish_beat_();
  }
  if (this->session_up_ && this->client_ != nullptr)
    this->queue_.tick(*this, now);

  // Server switch apply / confirm / rollback (1 Hz).
  if (now - this->last_switch_check_ms_ >= 1000) {
    this->last_switch_check_ms_ = now;
    if (this->switch_.pending()) {
      switch (this->switch_.update(this->identity_, this->session_up_, now)) {
        case ServerSwitch::Action::APPLY:
          ESP_LOGW(TAG, "into mqtt svr convert: %s:%u ssl %u", this->identity_.server.host.c_str(),
                   this->identity_.server.port, this->identity_.server.ssl_flag);
          this->server_from_switch_ = true;
          this->save_persisted_();
          this->stop_client_("server switch");
          this->backoff_.reset();
          this->next_attempt_ms_ = now;
          break;
        case ServerSwitch::Action::CONFIRMED:
          // The commit has copied the new settings into the backup, so devinfo names the new
          // broker in both 38338 and 38337.
          ESP_LOGI(TAG, "mqtt connected after svr_convert: new server confirmed");
          this->save_persisted_();
          this->publish_devinfo_(this->identity_.server, this->identity_.old_server);
          break;
        case ServerSwitch::Action::ROLLBACK:
          ESP_LOGW(TAG, "svr back to old svr due to new svr connect error: %s:%u",
                   this->identity_.server.host.c_str(), this->identity_.server.port);
          this->set_error_("server switch failed, rolled back");
          // server_from_switch_ stays set: the persisted server (now the old one) is authoritative,
          // which also preserves an earlier confirmed switch.
          this->save_persisted_();
          this->stop_client_("server switch rollback");
          this->backoff_.reset();
          this->next_attempt_ms_ = now;
          break;
        case ServerSwitch::Action::STAY:
          ESP_LOGW(TAG, "remote control is change, not back to old server");
          this->save_persisted_();
          break;
        case ServerSwitch::Action::NONE:
          break;
      }
    }
  }

  // Connect when allowed (checked at most every 500 ms).
  if (this->client_ == nullptr && now - this->last_connect_check_ms_ >= 500) {
    this->last_connect_check_ms_ = now;
    std::string why;
    if (!this->can_connect_(&why)) {
      if (why != this->wait_reason_) {
        ESP_LOGI(TAG, "Not connecting: %s", why.c_str());
        this->wait_reason_ = why;
      }
    } else if (destroy_pending_.load() != 0) {
      why = "previous session still closing";
      if (why != this->wait_reason_) {
        ESP_LOGD(TAG, "Not connecting: %s", why.c_str());
        this->wait_reason_ = why;
      }
    } else if (static_cast<int32_t>(now - this->next_attempt_ms_) >= 0) {
      this->wait_reason_.clear();
      this->start_client_();
    }
  }

  // Scheduler: 1 tick per second.
  if (now - this->last_tick_ms_ >= 1000) {
    this->last_tick_ms_ = now;
    this->scheduler_tick_();
    // The per-timezone broadcast topic is re-subscribed every 4 h (also after a settime dropped it).
    if (this->session_up_ && this->client_ != nullptr && now - this->group_sub_ms_ >= GROUP_TOPIC_REFRESH_S * 1000) {
      this->group_topic_.clear();
      this->update_group_subscription_();
    }
  }

  const uint32_t overflows = json_overflow_count().load(std::memory_order_relaxed);
  if (overflows != this->json_overflows_seen_) {
    ESP_LOGE(TAG, "%u cloud JSON document(s) ran out of memory and were not sent",
             static_cast<unsigned>(overflows - this->json_overflows_seen_));
    this->json_overflows_seen_ = overflows;
    this->set_error_("JSON document out of memory");
  }
  if (this->diag_dirty_)
    this->publish_diag_();
}

bool IsolarcloudComponent::can_connect_(std::string *why) {
  if (!this->enabled_) {
    *why = "uplink disabled";
    return false;
  }
  if (!this->identity_usable_(why))
    return false;
  if (!network::is_connected()) {
    *why = "network down";
    return false;
  }
  // sendTime (and the will) need wall-clock time.
  CivilTime t;
  if (!this->local_now_(t)) {
    *why = "waiting for valid time (SNTP)";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// time
// ---------------------------------------------------------------------------------------------
bool IsolarcloudComponent::local_now_(CivilTime &out) const {
  if (this->time_ == nullptr)
    return false;
  ESPTime u = this->time_->utcnow();
  if (!u.is_valid())
    return false;
  const int64_t epoch = static_cast<int64_t>(u.timestamp);
  // Local time = UTC + the 0x1089 offset; send_time_utc is an escape hatch.
  out = this->send_time_utc_ ? civil_from_epoch(epoch) : local_from_utc(epoch, this->tz_offset_s_);
  return clock_plausible(out);
}

bool IsolarcloudComponent::now_send_time_(std::string &out) const {
  CivilTime t;
  if (!this->local_now_(t))
    return false;
  out = format_cloud_time(t);
  return true;
}

bool IsolarcloudComponent::in_silence_(const CivilTime &local) const {
  const CloudIdentity &id = this->identity_;
  return in_silence_window(id.silence_enable, id.silence_start_hour, id.silence_end_hour,
                           static_cast<uint8_t>(local.hour));
}

// ---------------------------------------------------------------------------------------------
// MQTT client
// ---------------------------------------------------------------------------------------------
void IsolarcloudComponent::start_client_() {
  const CloudIdentity &id = this->identity_;
  const bool tls = id.server.use_tls();

  // Will: {"…","businessData":[{"devCode":247,"devType":22,"beat":0}]} on pub/willdata, QoS1 retain1.
  std::string send_time;
  this->now_send_time_(send_time);
  this->will_topic_ = this->topics_.will();
  std::string will_json = build_will_json(this->public_data_(send_time),
                                          make_message_id(id.sn, send_time, this->msg_counter_.next()));
  if (will_json.empty() || !encode_payload(will_json, id.compress, this->will_payload_)) {
    this->set_error_("will compress failed");
    this->next_attempt_ms_ = millis() + this->backoff_.next();
    return;
  }
  if (this->will_payload_.size() > MAX_WILL_PAYLOAD) {
    ESP_LOGW(TAG, "will payload is %u B (> %u B)", static_cast<unsigned>(this->will_payload_.size()),
             static_cast<unsigned>(MAX_WILL_PAYLOAD));
  }
  this->host_ = id.server.host;

  esp_mqtt_client_config_t cfg{};
  cfg.broker.address.hostname = this->host_.c_str();
  cfg.broker.address.port = id.server.port;
  cfg.broker.address.transport = tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
  if (tls) {
    // The chain is verified; the host name is not checked and no SNI is sent.
    // Trust store: YAML ca_certificate > ext-flash cafile > IDF bundle.
    if (this->verify_server_) {
      cfg.broker.verification.skip_cert_common_name_check = true;
      if (this->ca_certificate_ != nullptr) {
        cfg.broker.verification.certificate = this->ca_certificate_;
      } else {
        if (this->ca_buf_.empty() && this->identity_source_ != nullptr) {
          // Read straight into the buffer the TLS config points at (no intermediate copy).
          if (this->identity_source_->read_file(STOCK_CA_FILE, this->ca_buf_) && !this->ca_buf_.empty()) {
            static const char PEM_TAG[] = "-----BEGIN";
            const size_t file_size = this->ca_buf_.size();
            this->ca_is_der_ = std::search(this->ca_buf_.begin(), this->ca_buf_.end(), PEM_TAG,
                                           PEM_TAG + sizeof(PEM_TAG) - 1) == this->ca_buf_.end();
            if (!this->ca_is_der_)
              this->ca_buf_.push_back('\0');  // mbedTLS parses PEM as a NUL-terminated string
            ESP_LOGI(TAG, "Broker CA from %s (%u B, %s)", STOCK_CA_FILE, static_cast<unsigned>(file_size),
                     this->ca_is_der_ ? "DER" : "PEM");
          } else {
            this->ca_buf_.clear();
          }
        }
        if (!this->ca_buf_.empty()) {
          cfg.broker.verification.certificate = reinterpret_cast<const char *>(this->ca_buf_.data());
          // PEM: length including the NUL; DER: exact length.
          cfg.broker.verification.certificate_len = this->ca_buf_.size();
        } else {
#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
          ESP_LOGW(TAG, "No CA (%s missing, no ca_certificate): falling back to the IDF bundle", STOCK_CA_FILE);
          cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif
        }
      }
    } else {
      cfg.broker.verification.skip_cert_common_name_check = true;
    }
  }
  cfg.credentials.client_id = id.sn.c_str();
  cfg.credentials.username = this->username_.c_str();
  cfg.credentials.authentication.password = this->password_.c_str();
  cfg.session.protocol_ver = MQTT_PROTOCOL_V_3_1_1;
  cfg.session.keepalive = MQTT_KEEPALIVE_S;
  cfg.session.disable_clean_session = false;
  cfg.session.last_will.topic = this->will_topic_.c_str();
  cfg.session.last_will.msg = reinterpret_cast<const char *>(this->will_payload_.data());
  cfg.session.last_will.msg_len = static_cast<int>(this->will_payload_.size());
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.network.disable_auto_reconnect = true;  // we reconnect with our own backoff
  // With auto-reconnect off this only sets how long the esp-mqtt task sleeps between checks of its
  // run flag after a failed connect (half of it). esp_mqtt_client_stop() waits for that check; the
  // 10 s default made it block 5 s, which tripped the 5 s task watchdog.
  cfg.network.reconnect_timeout_ms = 1000;
  cfg.network.timeout_ms = 15000;
  cfg.buffer.size = 4096;
  cfg.buffer.out_size = 4096;
  cfg.outbox.limit = 64 * 1024;

  this->client_ = esp_mqtt_client_init(&cfg);
  if (this->client_ == nullptr) {
    this->set_error_("esp_mqtt_client_init failed");
    this->next_attempt_ms_ = millis() + this->backoff_.next();
    return;
  }
  esp_mqtt_client_register_event(this->client_, MQTT_EVENT_ANY, &IsolarcloudComponent::mqtt_event_handler_, this);
  this->active_client_.store(this->client_);
  this->connected_.store(false);
  this->connect_event_.store(false);
  this->disconnect_event_.store(false);
  this->session_up_ = false;
  this->client_started_ms_ = millis();
  ESP_LOGI(TAG, "Connecting to %s://%s:%u as '%s'", tls ? "mqtts" : "mqtt", this->host_.c_str(), id.server.port,
           id.sn.c_str());
  if (esp_mqtt_client_start(this->client_) != ESP_OK) {
    this->set_error_("esp_mqtt_client_start failed");
    this->stop_client_("start failed");
  }
}

void IsolarcloudComponent::stop_client_(const char *why) {
  if (this->client_ == nullptr)
    return;
  ESP_LOGI(TAG, "Closing MQTT session: %s", why);
  esp_mqtt_client_handle_t c = this->client_;
  this->client_ = nullptr;
  this->active_client_.store(nullptr);
  // esp_mqtt_client_destroy() waits for the esp-mqtt task to stop, which can take seconds while it
  // is inside a TLS connect: do it on a short-lived task so the loop (and the task watchdog) never
  // waits on it.
  // destroy_pending_ holds start_client_() off until it is done: two live clients would hold two
  // sets of TLS and MQTT buffers at once.
  destroy_pending_.fetch_add(1);
  if (xTaskCreate(&IsolarcloudComponent::destroy_task_, "cloud_mqtt_del", 4096, c, 5, nullptr) != pdPASS) {
    esp_mqtt_client_destroy(c);
    destroy_pending_.fetch_sub(1);
  }
  const bool was_up = this->session_up_;
  if (was_up)
    this->down_since_ms_ = millis();
  else if (strcmp(why, "connect failed") == 0 || strcmp(why, "connect timeout") == 0 || strcmp(why, "start failed") == 0) {
    this->last_connect_failed_ = true;
    this->connect_failures_++;
  }
  this->connected_.store(false);
  this->session_up_ = false;
  this->diag_dirty_ = true;
  this->group_topic_.clear();
  this->queue_.clear();  // nothing queued before the drop is sent after a reconnect
  {
    std::lock_guard<std::mutex> lock(this->rx_mutex_);
    this->rx_queue_.clear();
    this->pubacks_.clear();
  }
  if (was_up)
    this->backoff_.reset();
  // Jitter (0-25 %) so a fleet doesn't reconnect in lockstep.
  uint32_t d = this->backoff_.next();
  d += esp_random() % (d / 4 + 1);
  this->next_attempt_ms_ = millis() + d;
  ESP_LOGD(TAG, "Next connect attempt in %" PRIu32 " ms", d);
}

void IsolarcloudComponent::on_shutdown() { this->stop_client_("shutdown"); }

std::atomic<uint8_t> IsolarcloudComponent::destroy_pending_{0};

void IsolarcloudComponent::destroy_task_(void *client) {
  esp_mqtt_client_destroy(static_cast<esp_mqtt_client_handle_t>(client));
  destroy_pending_.fetch_sub(1);
  vTaskDelete(nullptr);
}

void IsolarcloudComponent::mqtt_event_handler_(void *arg, esp_event_base_t /*base*/, int32_t /*event_id*/,
                                               void *event_data) {
  auto *self = static_cast<IsolarcloudComponent *>(arg);
  auto *ev = static_cast<esp_mqtt_event_handle_t>(event_data);
  if (ev->client != self->active_client_.load())
    return;  // a client that is being closed
  self->handle_event_(ev);
}

// Runs on the esp-mqtt task: only touch atomics and mutex-protected queues here.
void IsolarcloudComponent::handle_event_(esp_mqtt_event_handle_t ev) {
  switch (ev->event_id) {
    case MQTT_EVENT_CONNECTED:
      this->connected_.store(true);
      this->connect_event_.store(true);
      break;
    case MQTT_EVENT_DISCONNECTED:
      this->connected_.store(false);
      this->disconnect_event_.store(true);
      break;
    case MQTT_EVENT_PUBLISHED: {
      std::lock_guard<std::mutex> lock(this->rx_mutex_);
      if (this->pubacks_.size() < 64)
        this->pubacks_.push_back(ev->msg_id);
      break;
    }
    case MQTT_EVENT_ERROR: {
      char buf[128];
      const esp_mqtt_error_codes_t *e = ev->error_handle;
      if (e == nullptr) {
        snprintf(buf, sizeof(buf), "mqtt error");
      } else if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        snprintf(buf, sizeof(buf), "CONNACK refused, code %d", static_cast<int>(e->connect_return_code));
      } else if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        // esp-tls stores the mbedtls error already negated (esp_tls_mbedtls.c captures -ret), so
        // esp_tls_stack_err is positive: print it as mbedtls writes its codes, -0x2700 etc.
        snprintf(buf, sizeof(buf), "transport error: esp 0x%x tls -0x%04x sock errno %d",
                 static_cast<unsigned>(e->esp_tls_last_esp_err), static_cast<unsigned>(e->esp_tls_stack_err),
                 e->esp_transport_sock_errno);
      } else {
        snprintf(buf, sizeof(buf), "mqtt error type %d", static_cast<int>(e->error_type));
      }
      std::lock_guard<std::mutex> lock(this->err_mutex_);
      this->pending_error_ = buf;
      break;
    }
    case MQTT_EVENT_DATA: {
      // Large messages arrive in fragments (offset > 0 carries no topic). Stock drops > 4096 B.
      if (ev->current_data_offset == 0) {
        this->rx_partial_.topic.assign(ev->topic, ev->topic_len);
        this->rx_partial_.data.clear();
        this->rx_partial_drop_ = ev->total_data_len > static_cast<int>(MAX_INBOUND_PAYLOAD);
        if (!this->rx_partial_drop_)
          this->rx_partial_.data.reserve(ev->total_data_len);
      }
      if (!this->rx_partial_drop_)
        this->rx_partial_.data.append(ev->data, ev->data_len);
      if (ev->current_data_offset + ev->data_len >= ev->total_data_len) {
        std::lock_guard<std::mutex> lock(this->rx_mutex_);
        if (!this->rx_partial_drop_ && this->rx_queue_.size() < RX_MAX_QUEUE)
          this->rx_queue_.push_back(std::move(this->rx_partial_));
        this->rx_partial_ = Inbound{};
      }
      break;
    }
    default:
      break;
  }
}

int IsolarcloudComponent::publish(const std::string &topic, const std::vector<uint8_t> &payload, int qos, bool retain) {
  if (this->client_ == nullptr)
    return -1;
  // len 0 means "strlen(data)" to esp-mqtt: an empty payload must point at "".
  const char *data = payload.empty() ? "" : reinterpret_cast<const char *>(payload.data());
  return esp_mqtt_client_enqueue(this->client_, topic.c_str(), data, static_cast<int>(payload.size()), qos,
                                 retain ? 1 : 0, true);
}

void IsolarcloudComponent::on_connected_() {
  ESP_LOGI(TAG, "Connected to %s:%u", this->host_.c_str(), this->identity_.server.port);
  this->backoff_.reset();
  this->diag_dirty_ = true;
  this->queue_.clear();
  // QoS 1, own-compress variants only, gated by the remote-control mask.
  for (const auto &t : this->topics_.subscriptions(this->identity_.remote_ctl_mask)) {
    if (esp_mqtt_client_subscribe(this->client_, t.c_str(), 1) < 0) {
      ESP_LOGW(TAG, "subscribe failed: %s", t.c_str());
    } else {
      ESP_LOGV(TAG, "subscribed %s", t.c_str());
    }
  }
  this->group_topic_.clear();
  this->update_group_subscription_();
  // CONNACK burst, in order: beatdata (beat:1, overrides the retained will's beat:0), generaldata
  // of the first online device that has some, then the attribute dump (rsp/propertydata, carrying
  // each device's functionEnum, which gates cloud features such as Live data) for the logger and
  // every online device. On a reconnect also devstatusdata of every online device. devinfo goes
  // out on every CONNACK on the WiNet-S and on reconnects on the S2 (its first one follows the
  // first-connect rundata). The first-connect rundata burst (~30 s later, once per boot) is up to
  // the scheduler. The clock is valid here (can_connect_ gates on it), so enqueue won't fail.
  this->publish_beat_();
  std::vector<TelemetryDevice> devs;
  this->list_devices_(devs);
  for (const auto &d : devs) {
    if (d.online && d.dev_type != 0 && this->publish_generaldata_(d))
      break;
  }
  const std::string attr_topic = this->topics_.rsp("propertydata");
  this->publish_propertydata_(logger_device(), attr_topic, UNSOLICITED_BATCH_ID);
  for (const auto &d : devs) {
    if (d.online && d.dev_type != 0)
      this->publish_propertydata_(d, attr_topic, UNSOLICITED_BATCH_ID);
  }
  if (this->connected_before_) {
    for (const auto &d : devs) {
      if (d.online && d.dev_type != 0)
        this->publish_devstatus_(d);
    }
  }
  const bool winet_s = is_winet_s(this->identity_.product_type);
  if (winet_s || this->connected_before_)
    this->publish_devinfo_(this->identity_.server, this->identity_.old_server);
  this->connected_before_ = true;
}

void IsolarcloudComponent::update_group_subscription_() {
  if (this->client_ == nullptr || !this->session_up_)
    return;
  if (this->group_topic_ == "-")
    return;  // dropped after a settime: back at the 4 h refresh or the next (re)connect
  const std::string want = TopicBuilder::group_cmd(this->identity_.timezone_id);
  if (want == this->group_topic_)
    return;
  if (!this->group_topic_.empty())
    esp_mqtt_client_unsubscribe(this->client_, this->group_topic_.c_str());
  this->group_sub_ms_ = millis();
  if (esp_mqtt_client_subscribe(this->client_, want.c_str(), 1) < 0) {
    ESP_LOGW(TAG, "subscribe failed: %s", want.c_str());
    this->group_topic_.clear();
    return;
  }
  ESP_LOGD(TAG, "timezone broadcast topic: %s", want.c_str());
  this->group_topic_ = want;
}

// ---------------------------------------------------------------------------------------------
// outbound
// ---------------------------------------------------------------------------------------------
PublicData IsolarcloudComponent::public_data_(const std::string &send_time) const {
  PublicData pd;
  pd.product_type = this->identity_.product_type;
  pd.product_name = this->identity_.product_name;
  pd.sn = this->identity_.sn;
  pd.send_time = send_time;
  return pd;
}

bool IsolarcloudComponent::enqueue_(const std::string &topic, const std::string &business_json, uint16_t compress,
                                    const char *label) {
  if (this->client_ == nullptr || !this->session_up_ || topic.empty() || business_json.empty())
    return false;
  std::string send_time;
  if (!this->now_send_time_(send_time)) {
    this->set_error_("get cur timestamp string fail");
    return false;
  }
  std::string mid = make_message_id(this->identity_.sn, send_time, this->msg_counter_.next());
  OutMessage m;
  m.topic = topic;
  m.label = label;
  m.qos = 1;  // every publish: QoS 1, retain 0
  m.droppable = std::strncmp(label, "rundata", 7) == 0;  // the next period replaces it
  size_t bytes = 0;
  const std::vector<std::string> envelopes = build_envelopes(this->public_data_(send_time), mid, business_json);
  if (envelopes.empty()) {
    this->set_error_(std::string("envelope JSON out of memory (") + label + ")");
    return false;
  }
  for (const auto &json : envelopes) {
    std::vector<uint8_t> wire;
    if (!encode_payload(json, compress, wire)) {
      this->set_error_(std::string("mqtt root compress failed (") + label + ")");
      return false;
    }
    bytes += wire.size();
    m.chunks.push_back(std::move(wire));
  }
  const size_t parts = m.chunks.size();
  if (!this->queue_.push(std::move(m))) {
    ESP_LOGW(TAG, "Publish queue full (%u messages, %u B): %s dropped", static_cast<unsigned>(this->queue_.size()),
             static_cast<unsigned>(this->queue_.bytes()), label);
    this->set_error_(std::string("publish queue full (") + label + ")");
    return false;
  }
  if (this->queue_.last_push_evicted() != 0)
    ESP_LOGW(TAG, "Publish queue over %u B: dropped %u older message(s) for %s (%u evicted in total)",
             static_cast<unsigned>(PublishQueue::DEFAULT_MAX_BYTES),
             static_cast<unsigned>(this->queue_.last_push_evicted()), label,
             static_cast<unsigned>(this->queue_.evicted()));
  this->publish_count_++;
  this->last_publish_ = send_time + " " + label;
  this->diag_dirty_ = true;
  ESP_LOGD(TAG, "Queued %s: %u part(s), %u B (JSON %u B)", label, static_cast<unsigned>(parts),
           static_cast<unsigned>(bytes), static_cast<unsigned>(business_json.size()));
  return true;
}

bool IsolarcloudComponent::publish_business(const std::string &family, const std::string &business_json) {
  return this->enqueue_(this->topics_.pub(family), business_json, this->identity_.compress, family.c_str());
}

void IsolarcloudComponent::list_devices_(std::vector<TelemetryDevice> &out) const {
  out.clear();
  if (this->telemetry_ != nullptr)
    this->telemetry_->list_devices(out);
}

void IsolarcloudComponent::scheduler_tick_() {
  SchedulerInput in;
  in.mono_s = (millis() - this->boot_ms_) / 1000;
  in.clock_valid = this->local_now_(in.local);
  in.link_up = this->session_up_ && this->client_ != nullptr;
  in.silenced = in.clock_valid && this->in_silence_(in.local);
  in.period_min = this->identity_.rundata_period_min;
  SchedulerActions a = this->scheduler_.tick(in);
  // Live data: every local 10 s boundary, after the first-connect rundata, outside the silence
  // window, while the live-data enable is set. The first tick inside each 10 s window publishes,
  // so a late loop pass does not skip a slot.
  if (in.clock_valid && in.link_up && !in.silenced && this->scheduler_.first_connect_done() &&
      this->second_.active()) {
    const int64_t slot = epoch_from_civil(in.local) / 10;
    if (slot != this->last_second_slot_) {
      this->last_second_slot_ = slot;
      CivilTime t = in.local;
      t.second -= t.second % 10;
      this->publish_second_(t);
    }
  }
  if (in.clock_valid && in.link_up && !in.silenced && this->scheduler_.first_connect_done())
    this->poll_status_(in.mono_s);
  if (!a.any())
    return;
  if (a.rundata) {
    this->publish_rundata_(a.data_time);
    if (a.first_connect) {
      ESP_LOGV(TAG, "faultdata after first connect: not implemented");
    }
  }
  if (a.devinfo)
    this->publish_devinfo_(this->identity_.server, this->identity_.old_server);
  if (a.propertydata_all || a.logger_attr) {
    const std::string topic = this->topics_.rsp("propertydata");
    this->publish_propertydata_(logger_device(), topic, UNSOLICITED_BATCH_ID);
    if (a.propertydata_all) {
      std::vector<TelemetryDevice> devs;
      this->list_devices_(devs);
      for (const auto &d : devs) {
        if (d.online && d.dev_type != 0)
          this->publish_propertydata_(d, topic, UNSOLICITED_BATCH_ID);
      }
    }
  }
  if (a.beat)
    this->publish_beat_();
}

void IsolarcloudComponent::publish_rundata_(const CivilTime &data_time) {
  ESP_LOGD(TAG, "loop task stack: %u B free at least", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  const std::string dt = format_cloud_time(data_time);
  const std::string topic = this->topics_.pub("rundata/5min");
  // One message per device (the next one follows the previous one's PUBACK): the logger first.
  this->enqueue_(topic, this->formatter_->rundata(logger_device(), &this->logger_sigs_, dt), this->identity_.compress,
                 "rundata 247");
  std::vector<TelemetryDevice> devs;
  this->list_devices_(devs);
  for (const auto &d : devs) {
    if (!d.online || d.dev_type == 0)
      continue;  // "DEV %d OFFLINE." / no table
    std::string label = "rundata " + std::to_string(d.dev_code);
    this->enqueue_(topic, this->formatter_->rundata(d, this->telemetry_, dt), this->identity_.compress,
                   label.c_str());
  }
}

void IsolarcloudComponent::publish_second_(const CivilTime &data_time) {
  // Skip a slot while the previous live-data messages are still queued (slow link).
  if (this->queue_.size() > 8) {
    ESP_LOGD(TAG, "live data: %u messages still queued, slot skipped", static_cast<unsigned>(this->queue_.size()));
    return;
  }
  const std::string dt = format_cloud_time(data_time);
  const std::string topic = this->topics_.pub("rundata/second");
  std::vector<TelemetryDevice> devs;
  this->list_devices_(devs);
  for (const auto &d : devs) {
    if (d.dev_code == LOGGER_DEV_CODE || !d.online || d.dev_type == 0)
      continue;
    const SecondNode *n = this->second_.node(static_cast<uint16_t>(d.dev_code));
    if (n == nullptr || !n->enabled)
      continue;
    std::string body = this->formatter_->rundata_second(d, this->telemetry_, dt, n->point_ids);
    if (body.empty())
      continue;
    std::string label = "second " + std::to_string(d.dev_code);
    this->enqueue_(topic, body, this->identity_.compress, label.c_str());
  }
}

void IsolarcloudComponent::save_second_() {
  auto p = std::make_unique<PersistedSecond>();
  memset(p.get(), 0, sizeof(PersistedSecond));
  p->magic = SECOND_MAGIC;
  const std::string s = this->second_.serialize();
  if (s.size() >= sizeof(p->text)) {
    ESP_LOGW(TAG, "live data: %u B of point lists do not fit the saved copy; not kept across reboots",
             static_cast<unsigned>(s.size()));
    p->text[0] = '\0';
  } else {
    memcpy(p->text, s.data(), s.size());
  }
  this->second_pref_.save(p.get());
}

void IsolarcloudComponent::publish_devstatus_(const TelemetryDevice &d) {
  std::string dt;
  if (!this->now_send_time_(dt))
    return;
  struct timeval tv {};
  gettimeofday(&tv, nullptr);
  char ms[8];
  snprintf(ms, sizeof(ms), "%03d", static_cast<int>(tv.tv_usec / 1000) % 1000);
  const std::string body = this->formatter_->devstatus(d, this->telemetry_, dt, dt + ms);
  if (body.empty())
    return;
  const std::string label = "devstatusdata " + std::to_string(d.dev_code);
  if (this->enqueue_(this->topics_.pub("devstatusdata"), body, this->identity_.compress, label.c_str()))
    this->status_sent_[d.dev_code] = this->formatter_->point_signature(d, this->telemetry_, MQTT_MASK_STATUS);
}

bool IsolarcloudComponent::publish_generaldata_(const TelemetryDevice &d) {
  std::string dt;
  if (!this->now_send_time_(dt))
    return false;
  const std::string body = this->formatter_->generaldata(d, this->telemetry_, dt);
  if (body.empty())
    return false;
  const std::string label = "generaldata " + std::to_string(d.dev_code);
  if (!this->enqueue_(this->topics_.pub("generaldata"), body, this->identity_.compress, label.c_str()))
    return false;
  this->general_sent_[d.dev_code] = this->formatter_->point_signature(d, this->telemetry_, MQTT_MASK_GENERAL);
  return true;
}

void IsolarcloudComponent::poll_status_(uint32_t mono_s) {
  if (this->status_poll_s_ != 0 && mono_s < this->status_poll_s_ + STATUS_POLL_S)
    return;
  this->status_poll_s_ = mono_s;
  const bool all = mono_s >= this->status_all_due_s_;
  if (all)
    this->status_all_due_s_ = mono_s + STATUS_ALL_S;
  std::vector<TelemetryDevice> devs;
  this->list_devices_(devs);
  for (const auto &d : devs) {
    if (!d.online || d.dev_type == 0 || d.dev_code == LOGGER_DEV_CODE)
      continue;
    const std::string st = this->formatter_->point_signature(d, this->telemetry_, MQTT_MASK_STATUS);
    auto it = this->status_sent_.find(d.dev_code);
    if (all || it == this->status_sent_.end() || it->second != st)
      this->publish_devstatus_(d);
    const std::string gen = this->formatter_->point_signature(d, this->telemetry_, MQTT_MASK_GENERAL);
    auto git = this->general_sent_.find(d.dev_code);
    if (!gen.empty() && (git == this->general_sent_.end() || git->second != gen))
      this->publish_generaldata_(d);
  }
}

void IsolarcloudComponent::publish_devinfo_(const ServerEndpoint &server, const ServerEndpoint &old_server) {
  DevinfoContext ctx;
  if (!this->now_send_time_(ctx.data_time))
    return;
  ctx.server = server;
  ctx.old_server = old_server;
  this->refresh_logger_network_();
  TelemetrySignal ip;
  if (this->logger_sigs_.get_signal(0, 0x1202, ip))
    ctx.sig_1202 = ip.str;
  this->publish_business("devinfo", this->formatter_->devinfo(ctx));
}

void IsolarcloudComponent::publish_propertydata_(const TelemetryDevice &d, const std::string &topic,
                                                 const std::string &batch_id) {
  std::string dt;
  if (!this->now_send_time_(dt))
    return;
  const bool logger = d.dev_code == LOGGER_DEV_CODE;
  if (logger)
    this->refresh_logger_network_();
  const TelemetrySource *src = logger ? static_cast<const TelemetrySource *>(&this->logger_sigs_) : this->telemetry_;
  std::string label = "propertydata " + std::to_string(d.dev_code);
  this->enqueue_(topic, this->formatter_->propertydata(d, src, dt, batch_id), this->identity_.compress, label.c_str());
}

std::string IsolarcloudComponent::beat_signature_(const std::vector<TelemetryDevice> &devs) const {
  std::string sig;
  for (const auto &d : devs) {
    if (d.dev_code == LOGGER_DEV_CODE || d.dev_type == 0 || d.dev_code == 0)
      continue;
    sig += std::to_string(d.dev_code) + ":" + std::to_string(d.dev_type) + (d.online ? ":1;" : ":0;");
  }
  return sig;
}

void IsolarcloudComponent::publish_beat_() {
  std::vector<TelemetryDevice> devs;
  this->list_devices_(devs);
  this->beat_sig_ = this->beat_signature_(devs);
  this->last_beat_ms_ = millis();
  this->publish_business("beatdata", this->formatter_->beat(devs));
}

// ---------------------------------------------------------------------------------------------
// inbound
// ---------------------------------------------------------------------------------------------
namespace {
struct CmdLabel {
  const char *code;
  const char *label;
  bool is_read;  // poll-style read; excluded from "last control command"
};
// cmdType -> short name for the diagnostic text sensors. Substring match like CommandRouter::find.
constexpr CmdLabel CMD_LABELS[] = {
    {"10325", "getparam", true},      {"10303", "setparam", false},   {"10384", "logger-set", false},
    {"10324", "settimezone", false},  {"10307", "settime", false},    {"10308", "upgrade", false},
    {"10376", "upgrade", false},      {"10310", "propertydata", true}, {"10451", "sp600-set", false},
    {"10453", "sp600-read", true},    {"10421", "live-data", true},
};
const CmdLabel *cmd_label(const std::string &cmd_type) {
  if (cmd_type.empty())
    return nullptr;
  for (const auto &e : CMD_LABELS) {
    if (cmd_type.find(e.code) != std::string::npos)
      return &e;
  }
  return nullptr;
}
}  // namespace

void IsolarcloudComponent::process_inbound_(const Inbound &msg) {
  InboundCommand cmd;
  InboundStatus st = decode_inbound(msg.topic, reinterpret_cast<const uint8_t *>(msg.data.data()), msg.data.size(),
                                    this->identity_.sn, cmd);
  if (st == InboundStatus::EMPTY) {
    ESP_LOGV(TAG, "empty message on '%s' (retained clear)", msg.topic.c_str());
    return;
  }
  if (st == InboundStatus::BRANCH_A) {
    ESP_LOGW(TAG, "Dropping non-cmd message on '%s' (%u B)", msg.topic.c_str(), static_cast<unsigned>(msg.data.size()));
    return;
  }
  if (st != InboundStatus::OK) {
    ESP_LOGW(TAG, "Inbound on '%s' dropped: %s", msg.topic.c_str(), inbound_status_str(st));
    if (st == InboundStatus::INFLATE_FAILED)
      this->set_error_("mqtt get data uncompress failed");
    return;
  }
  if (this->dedupe_.check_and_add(cmd)) {
    ESP_LOGI(TAG, "recv same cmd: batchId %s", cmd.batch_id.c_str());
    return;
  }
  ESP_LOGI(TAG, "Command cmdType '%s' batchId '%s' on %s", cmd.cmd_type.c_str(), cmd.batch_id.c_str(),
           cmd.topic.family.empty() ? "(timezone broadcast)" : cmd.topic.family.c_str());
  {
    // Diagnostic text sensors: "<cmdType> <label> [batchId=<id>]". Unknown cmdTypes fall back to
    // the topic family; the control variant skips read/poll commands so writes stay visible.
    const CmdLabel *lbl = cmd_label(cmd.cmd_type);
    std::string desc = cmd.cmd_type + " ";
    if (lbl != nullptr)
      desc += lbl->label;
    else
      desc += cmd.topic.family.empty() ? "?" : cmd.topic.family;
    if (cmd.has_batch_id && !cmd.batch_id.empty())
      desc += " batchId=" + cmd.batch_id;
    this->last_command_ = desc;
    if (lbl == nullptr || !lbl->is_read)
      this->last_control_command_ = std::move(desc);
    this->diag_dirty_ = true;
  }
  ESP_LOGV(TAG, "  %s", cmd.json.c_str());
  if (!remote_ctl_allows(this->identity_.remote_ctl_mask, cmd.cmd_type)) {
    ESP_LOGW(TAG, "  cmdType %s blocked by remote-control mask 0x%" PRIx32, cmd.cmd_type.c_str(),
             this->identity_.remote_ctl_mask);
  } else if (!this->router_.dispatch(cmd)) {
    ESP_LOGI(TAG, "  no handler for cmdType %s (no reply)", cmd.cmd_type.c_str());
  }
  ESP_LOGD(TAG, "  loop task stack: %u B free at least", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  // Always clear the retained command: empty payload, QoS 1, retain 1, to the incoming topic.
  OutMessage clear;
  clear.topic = cmd.raw_topic;
  clear.chunks.emplace_back();
  clear.label = "retained clear";
  clear.qos = 1;
  clear.retain = true;
  if (this->session_up_)
    this->queue_.push(std::move(clear));
}

void IsolarcloudComponent::send_reply_(const InboundCommand &cmd, const std::string &business) {
  const std::string topic = reply_topic(this->topics_, cmd);
  if (topic.empty()) {
    ESP_LOGD(TAG, "  no reply topic (timezone broadcast)");
    return;
  }
  this->enqueue_(topic, business, reply_compress(this->topics_, cmd), "reply");
}

void IsolarcloudComponent::install_default_handlers_() {
  // Reads (getparam, attribute dumps, live data), logger points we support, time/timezone, and
  // device setting writes (setparam) while the "Cloud writes" switch is on. Upgrade commands
  // (10308/10376) and the other families have no handler: no reply.
  this->router_.on_cmd_type("10325", [this](const InboundCommand &c) { this->handle_getparam_(c); });
  this->router_.on_cmd_type("10384", [this](const InboundCommand &c) { this->handle_logger_param_set_(c); });
  this->router_.on_cmd_type("10324", [this](const InboundCommand &c) { this->handle_settimezone_(c); });
  this->router_.on_cmd_type("10307", [this](const InboundCommand &c) { this->handle_settime_(c); });
  this->router_.on_cmd_type("10310", [this](const InboundCommand &c) { this->handle_propertydata_(c); });
  this->router_.on_cmd_type("10421", [this](const InboundCommand &c) { this->handle_second_(c); });
  this->router_.on_cmd_type("10303", [this](const InboundCommand &c) { this->handle_setparam_(c); });
}

namespace {
struct GetParamJob {
  InboundCommand cmd;
  GetParamRequest req;
  std::vector<GetParamResult> results;
  size_t next{0};
};
}  // namespace

void IsolarcloudComponent::handle_getparam_(const InboundCommand &cmd) {
  auto job = std::make_shared<GetParamJob>();
  std::string err;
  if (!parse_getparam(cmd.business_json, job->req, err)) {
    ESP_LOGW(TAG, "  getparam: %s (no reply)", err.c_str());
    return;
  }
  job->cmd = cmd;
  bool online = false;
  uint8_t address = 0;
  std::vector<TelemetryDevice> devs;
  this->list_devices_(devs);
  for (const auto &d : devs) {
    if (d.dev_code == job->req.dev_code && d.online) {
      online = true;
      address = d.address;
    }
  }
  std::string dt;
  this->now_send_time_(dt);
  if (!online || !this->param_reader_) {
    // Offline / logger / no Modbus binding: result 0, no data.
    this->send_reply_(cmd, build_getparam_reply(cmd, dt, job->req, 0, {}));
    return;
  }
  // Points are read one after another; the reply goes out after the last one. The pending
  // completion owns the step function (weak self-reference: no cycle).
  auto step = std::make_shared<std::function<void()>>();
  std::weak_ptr<std::function<void()>> wstep = step;
  *step = [this, job, wstep, address]() {
    while (job->next < job->req.points.size()) {
      const GetParamPoint &p = job->req.points[job->next];
      ParamReadRequest r;
      r.addr = address;
      r.fn = job->req.fn();
      r.reg = p.reg;
      r.count = p.count;
      const size_t i = job->next++;
      auto self = wstep.lock();
      bool queued = this->param_reader_(r, [this, job, self, i](bool ok, const uint8_t *regs, uint16_t n) {
        GetParamResult &res = job->results[i];
        if (ok && regs != nullptr) {
          res.read_value = getparam_read_value(job->req.points[i].data_type, regs, n);
          res.read_result = "1";
        }
        if (self)
          (*self)();
      });
      if (queued)
        return;  // continue from the completion
    }
    std::string dt2;
    this->now_send_time_(dt2);
    this->send_reply_(job->cmd, build_getparam_reply(job->cmd, dt2, job->req, 1, job->results));
  };
  for (const auto &p : job->req.points) {
    GetParamResult r;
    r.key = p.key;
    r.modbus_address = p.modbus_address;
    job->results.push_back(r);
  }
  (*step)();
}

namespace {
struct SetParamJob {
  InboundCommand cmd;
  SetParamRequest req;
  std::vector<GetParamResult> results;
  size_t next{0};
};
}  // namespace

void IsolarcloudComponent::handle_setparam_(const InboundCommand &cmd) {
  {
    // devCode 247 sets a logger point; the reply has the 10384 shape with cmdType "10303".
    LoggerParamSet lp;
    std::string lerr;
    if (parse_logger_param_set(cmd.business_json, lp, lerr)) {
      this->handle_logger_param_set_(cmd);
      return;
    }
  }
  auto job = std::make_shared<SetParamJob>();
  std::string err;
  if (!parse_setparam(cmd.business_json, job->req, err)) {
    ESP_LOGW(TAG, "  setparam: %s (no reply)", err.c_str());
    return;
  }
  job->cmd = cmd;
  for (const auto &p : job->req.points) {
    GetParamResult r;
    r.key = p.key;
    r.modbus_address = p.modbus_address;
    r.read_value = p.echo;
    r.read_result = "0";
    job->results.push_back(r);
  }
  bool online = false;
  uint8_t address = 0;
  std::vector<TelemetryDevice> devs;
  this->list_devices_(devs);
  for (const auto &d : devs) {
    if (d.dev_code == job->req.dev_code && d.dev_code != LOGGER_DEV_CODE && d.online) {
      online = true;
      address = d.address;
    }
  }
  const char *refuse = nullptr;
  if (!this->writes_enabled_)
    refuse = "cloud writes are off (read-only)";
  else if (job->req.mode == 1)
    refuse = "mode 1 (private FC 0x76) is not supported";
  else if (!online || !this->param_writer_)
    refuse = "device offline";
  if (refuse != nullptr) {
    ESP_LOGW(TAG, "  setparam to devCode %" PRId64 " refused: %s", job->req.dev_code, refuse);
    std::string dt;
    this->now_send_time_(dt);
    this->send_reply_(cmd, build_setparam_reply(cmd, dt, job->req, job->results));
    return;
  }
  // Points are written one after another in request order; the reply follows the last one.
  auto step = std::make_shared<std::function<void()>>();
  std::weak_ptr<std::function<void()>> wstep = step;
  *step = [this, job, wstep, address]() {
    while (job->next < job->req.points.size()) {
      const size_t i = job->next++;
      const SetParamPoint &p = job->req.points[i];
      if (!p.encodable) {
        ESP_LOGW(TAG, "  setparam %s: dataType %u value '%s' not writable", p.modbus_address.c_str(), p.data_type,
                 p.data_value.c_str());
        continue;
      }
      if (const char *range = setparam_protected_range(p.reg, p.values.size())) {
        ESP_LOGW(TAG, "  setparam reg %u (%u register(s)) refused: %s", p.reg, static_cast<unsigned>(p.values.size()),
                 range);
        continue;
      }
      ParamWriteRequest w;
      w.addr = address;
      w.reg = p.reg;
      w.values = p.values;
      auto self = wstep.lock();
      bool queued = this->param_writer_(w, [this, job, self, i](bool ok, uint8_t exception) {
        const SetParamPoint &pt = job->req.points[i];
        job->results[i].read_result = ok ? "1" : (exception != 0 ? "0" : "4");
        ESP_LOGI(TAG, "  setparam devCode %" PRId64 " reg %u = %s (dataType %u): %s", job->req.dev_code, pt.reg,
                 pt.echo.c_str(), pt.data_type, ok ? "written" : (exception != 0 ? "exception" : "no reply"));
        if (self)
          (*self)();
      });
      if (queued)
        return;  // continue from the completion
      ESP_LOGW(TAG, "  setparam reg %u: write not queued", p.reg);
    }
    std::string dt;
    this->now_send_time_(dt);
    this->send_reply_(job->cmd, build_setparam_reply(job->cmd, dt, job->req, job->results));
  };
  ESP_LOGI(TAG, "  setparam: %u point(s) to devCode %" PRId64 " (Modbus %u)",
           static_cast<unsigned>(job->req.points.size()), job->req.dev_code, address);
  (*step)();
}

void IsolarcloudComponent::handle_second_(const InboundCommand &cmd) {
  SecondRequest req;
  std::string err;
  if (!parse_second_request(cmd.business_json, req, err)) {
    ESP_LOGW(TAG, "  live data: %s (no reply)", err.c_str());
    return;
  }
  const SecondData::Outcome o = this->second_.apply(req);
  std::string codes;
  for (int64_t c : req.dev_codes)
    codes += (codes.empty() ? "" : ",") + std::to_string(c);
  size_t points = 0;
  for (const auto &kv : req.data)
    points += kv.second.size();
  ESP_LOGI(TAG, "  live data: devSwitch %" PRId64 " devCodes [%s], %u point(s) -> result %d, %s", req.dev_switch,
           codes.c_str(), static_cast<unsigned>(points), o.result, this->second_.active() ? "on" : "off");
  this->save_second_();
  this->last_second_slot_ = -1;
  std::string dt;
  this->now_send_time_(dt);
  this->send_reply_(cmd, build_second_reply(cmd, dt, req, o));
}

void IsolarcloudComponent::handle_logger_param_set_(const InboundCommand &cmd) {
  LoggerParamSet p;
  std::string err;
  if (!parse_logger_param_set(cmd.business_json, p, err)) {
    ESP_LOGW(TAG, "  10384: %s (no reply)", err.c_str());
    return;
  }
  std::string dt;
  this->now_send_time_(dt);
  // readValue echoes the request's dataValue; readResult 1 applied, 5 refused.
  int result = 5;
  const std::string &value = p.data_value;
  if (p.point == "4") {
    // Point 4 -> 0x1015 rundata period (minutes; 0 = 5).
    char *end = nullptr;
    long v = strtol(p.data_value.c_str(), &end, 10);
    if (end != p.data_value.c_str() && *end == '\0' && v >= 0 && v <= 1440) {
      if (this->yaml_mask_ & FIELD_RUNDATA_PERIOD)
        ESP_LOGW(TAG, "  rundata period %ld from the cloud overridden by YAML", v);
      this->identity_.rundata_period_min = static_cast<uint16_t>(v);
      this->cloud_mask_ |= FIELD_RUNDATA_PERIOD;
      this->resolve_identity_("cloud rundata period");
      result = 1;
    }
  } else if (p.point == "10457") {
    ServerEndpoint ep;
    if (parse_server_switch(p, ep)) {
      this->request_server_switch(ep);
      result = 1;
    }
  } else {
    ESP_LOGI(TAG, "  10384 point %s not supported", p.point.c_str());
  }
  this->send_reply_(cmd, build_logger_param_reply(cmd, dt, p, value, result));
}

void IsolarcloudComponent::handle_settimezone_(const InboundCommand &cmd) {
  SetTimezone tz;
  if (!parse_settimezone(cmd.business_json, tz)) {
    ESP_LOGW(TAG, "  settimezone: bad timezone/timezoneId (no reply)");
    return;
  }
  ESP_LOGI(TAG, "  settimezone: %s, timezoneId %u", tz.timezone.c_str(), tz.timezone_id);
  this->identity_.timezone_id = tz.timezone_id;
  this->cloud_mask_ |= FIELD_TIMEZONE_ID;
  if (this->apply_cloud_timezone_) {
    this->identity_.timezone = tz.timezone;
    this->cloud_mask_ |= FIELD_TIMEZONE;
  } else {
    ESP_LOGI(TAG, "  mqtt set timezone, but apply_cloud_timezone is off: keeping %s", this->identity_.timezone.c_str());
  }
  if (this->yaml_mask_ & (FIELD_TIMEZONE | FIELD_TIMEZONE_ID))
    ESP_LOGW(TAG, "  YAML timezone/timezone_id win over the cloud's");
  this->resolve_identity_("settimezone");  // persists
  // The broadcast topic cmd/<timezoneId>/enum_0_0_0 is subscribed again right away.
  this->group_topic_.clear();
  this->update_group_subscription_();
  if (!cmd.has_batch_id) {
    ESP_LOGD(TAG, "  settimezone without batchId: zone applied, no reply");
    return;
  }
  std::string dt;
  this->now_send_time_(dt);
  this->send_reply_(cmd, build_reply_business(cmd, dt, 1));
}

void IsolarcloudComponent::handle_settime_(const InboundCommand &cmd) {
  CivilTime t;
  if (!parse_settime(cmd.business_json, t)) {
    ESP_LOGW(TAG, "  settime: bad dataTime");
    return;
  }
  if (this->cloud_settime_) {
    // dataTime is local time in our zone (stock writes it into the RTC sig 0x1080).
    struct timeval tv {};
    tv.tv_sec = static_cast<time_t>(epoch_from_civil(t) - this->tz_offset_s_);
    if (settimeofday(&tv, nullptr) == 0) {
      ESP_LOGI(TAG, "  settime: clock set to %s local", format_cloud_time(t).c_str());
    } else {
      ESP_LOGW(TAG, "  settime: settimeofday failed");
      return;
    }
  } else {
    ESP_LOGI(TAG, "  settime ignored (cloud_settime: false)");
  }
  // The broadcast topic is dropped after a time set, until the 4 h refresh or the next
  // (re)connect.
  if (this->client_ != nullptr && !this->group_topic_.empty() && this->group_topic_ != "-") {
    esp_mqtt_client_unsubscribe(this->client_, this->group_topic_.c_str());
    ESP_LOGD(TAG, "  unsubscribed %s until the 4 h refresh or a reconnect", this->group_topic_.c_str());
    this->group_topic_ = "-";  // not re-subscribed on a timezone change
  }
}

void IsolarcloudComponent::handle_propertydata_(const InboundCommand &cmd) {
  const std::string topic = reply_topic(this->topics_, cmd);
  if (topic.empty())
    return;
  // One attribute element, for the first online device of devCodeArr, with the request's
  // batchId. A request without batchId or devCodeArr gets no reply.
  std::vector<uint16_t> want;
  if (!cmd.has_batch_id || !parse_dev_code_arr(cmd.business_json, want)) {
    ESP_LOGW(TAG, "  10310: batchId or devCodeArr missing (no reply)");
    return;
  }
  std::vector<TelemetryDevice> devs;
  this->list_devices_(devs);
  devs.insert(devs.begin(), logger_device());
  for (uint16_t code : want) {
    for (const auto &d : devs) {
      if (d.dev_code != code)
        continue;
      if (!d.online || d.dev_type == 0) {
        ESP_LOGI(TAG, "  10310: devCode %u offline", code);
        break;
      }
      this->publish_propertydata_(d, topic, cmd.batch_id);
      return;
    }
  }
}

// ---------------------------------------------------------------------------------------------
// diagnostics
// ---------------------------------------------------------------------------------------------
void IsolarcloudComponent::set_error_(const std::string &err) {
  this->last_error_ = err;
  this->diag_dirty_ = true;
  ESP_LOGW(TAG, "%s", err.c_str());
}

void IsolarcloudComponent::publish_diag_() {
  this->diag_dirty_ = false;
#ifdef USE_BINARY_SENSOR
  if (this->connected_sensor_ != nullptr)
    this->connected_sensor_->publish_state(this->session_up_);
#endif
#ifdef USE_SENSOR
  if (this->publish_count_sensor_ != nullptr)
    this->publish_count_sensor_->publish_state(static_cast<float>(this->publish_count_));
#endif
#ifdef USE_TEXT_SENSOR
  if (this->last_publish_sensor_ != nullptr && !this->last_publish_.empty() &&
      this->last_publish_sensor_->get_state() != this->last_publish_)
    this->last_publish_sensor_->publish_state(this->last_publish_);
  if (this->last_error_sensor_ != nullptr && !this->last_error_.empty() &&
      this->last_error_sensor_->get_state() != this->last_error_)
    this->last_error_sensor_->publish_state(this->last_error_);
  if (this->last_command_sensor_ != nullptr && !this->last_command_.empty() &&
      this->last_command_sensor_->get_state() != this->last_command_)
    this->last_command_sensor_->publish_state(this->last_command_);
  if (this->last_control_command_sensor_ != nullptr && !this->last_control_command_.empty() &&
      this->last_control_command_sensor_->get_state() != this->last_control_command_)
    this->last_control_command_sensor_->publish_state(this->last_control_command_);
#endif
}

void IsolarcloudComponent::dump_config() {
  const CloudIdentity &id = this->identity_;
  ESP_LOGCONFIG(TAG,
                "iSolarCloud uplink:\n"
                "  SN (clientId): '%s'\n"
                "  Product: %s (type %u)\n"
                "  Broker: %s:%u (ssl flag %u -> %s, verify %s%s)\n"
                "  Old broker: %s:%u ssl %u\n"
                "  Username: %s\n"
                "  Topic enum: enum_%u_%u_%u, remote-control mask 0x%" PRIx32 "\n"
                "  Key (0x1013): %s\n"
                "  Timezone: %s (%s), timezoneId %u, apply cloud zone %s, cloud settime %s\n"
                "  Rundata every %u min (+%" PRIu32 " s), beat every %" PRIu32 " s, logger attrs every %" PRIu32
                " s, devinfo on connect %s\n"
                "  Silence window: %s %02u-%02u h\n"
                "  Identity source: %s, telemetry: %s, getparam reader: %s\n"
                "  Server switch: %s%s",
                id.sn.c_str(), id.product_name.c_str(), id.product_type, id.server.host.c_str(), id.server.port,
                id.server.ssl_flag,
                id.server.transport() == Transport::TLS   ? "TLS"
                : id.server.transport() == Transport::TCP ? "TCP"
                                                          : "invalid",
                YESNO(this->verify_server_), this->ca_certificate_ != nullptr ? " (custom CA)" : "",
                id.old_server.host.c_str(), id.old_server.port, id.old_server.ssl_flag, this->username_.c_str(),
                id.encrypt, id.keynum, id.compress, id.remote_ctl_mask, key_printable(id.server.key).c_str(),
                id.timezone.c_str(), this->send_time_utc_ ? "sendTime in UTC" : "local", id.timezone_id,
                YESNO(this->apply_cloud_timezone_), YESNO(this->cloud_settime_), id.rundata_period_min,
                this->scheduler_.random_delay(), this->timing_.beat_s, this->timing_.logger_attr_s,
                YESNO(this->timing_.devinfo_on_connect),
                id.silence_enable ? "on" : "off", id.silence_start_hour, id.silence_end_hour,
                this->identity_source_ != nullptr ? "bound" : "none", this->telemetry_ != nullptr ? "bound" : "none",
                this->param_reader_ ? "bound" : "none", this->switch_.pending() ? "pending" : "idle",
                this->server_from_switch_ ? " (server from svr_convert)" : "");
  if (this->wait_reason_.size()) {
    ESP_LOGCONFIG(TAG, "  Waiting: %s", this->wait_reason_.c_str());
  }
}

// ---------------------------------------------------------------------------------------------
// switch
// ---------------------------------------------------------------------------------------------
#ifdef USE_SWITCH
void UplinkSwitch::setup() {
  bool initial = this->get_initial_state_with_restore_mode().value_or(true);
  this->write_state(initial);
}
void UplinkSwitch::dump_config() { LOG_SWITCH("", "iSolarCloud uplink switch", this); }
void UplinkSwitch::write_state(bool state) {
  this->parent_->set_enabled(state);
  this->publish_state(state);
}
void WritesSwitch::setup() {
  bool initial = this->get_initial_state_with_restore_mode().value_or(false);
  this->write_state(initial);
}
void WritesSwitch::dump_config() { LOG_SWITCH("", "iSolarCloud writes switch", this); }
void WritesSwitch::write_state(bool state) {
  this->parent_->set_writes_enabled(state);
  this->publish_state(state);
}
#endif

}  // namespace esphome::isolarcloud
