#pragma once
// iSolarCloud uplink: identity, constants and topic family tables. Host-buildable (no ESPHome
// headers). Protocol reference: SG-Docs reference/protocols/cloud-mqtt.md and cloud-messages.md.

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace esphome::isolarcloud {

// ---- stock defaults (device 0xF7 sig table, cloud_identity.md) -------------------------------
static constexpr const char *DEFAULT_BROKER_HOST = "iot.isolarcloud.com";  // sig 0x1071
static constexpr uint16_t DEFAULT_BROKER_PORT = 16668;                     // sig 0x1072
static constexpr uint16_t DEFAULT_SSL_FLAG = 2;                            // sig 0x0023
static constexpr uint16_t DEFAULT_ENCRYPT = 0;                             // sig 0x0020
static constexpr uint16_t DEFAULT_KEYNUM = 0;                              // sig 0x0021
static constexpr uint16_t DEFAULT_COMPRESS = 1;                            // sig 0x0022 (zlib on)
/// sig 0x107a: timezoneId, the %d in the per-timezone broadcast topic cmd/<id>/enum_0_0_0.
/// Written by the settimezone command (10324).
static constexpr uint16_t DEFAULT_TIMEZONE_ID = 8;
/// sig 0x1089: timezone string as the cloud writes it ("UTC+10:00" = 10 h ahead of UTC).
/// Local time = UTC + this offset.
static constexpr const char *DEFAULT_TIMEZONE = "UTC+8:00";
static constexpr uint16_t DEFAULT_RUNDATA_PERIOD_MIN = 5;                  // sig 0x1015, minutes
/// Remote-control mask (WiNet-S2 sig 0x6492, WiNet-S sig 0x5232). The WiNet-S default has no
/// Layout group (bit 4).
static constexpr uint32_t DEFAULT_REMOTE_CTL_MASK = 0x4FF;
static constexpr uint32_t DEFAULT_REMOTE_CTL_MASK_WINET_S = 0x4EF;
/// publicData productType of the WiNet-S ("1813"); the WiNet-S2 is 0x720 ("1824").
static constexpr uint16_t PRODUCT_TYPE_WINET_S = 0x715;
inline bool is_winet_s(uint16_t product_type) { return product_type == PRODUCT_TYPE_WINET_S; }
inline uint32_t default_remote_ctl_mask(uint16_t product_type) {
  return is_winet_s(product_type) ? DEFAULT_REMOTE_CTL_MASK_WINET_S : DEFAULT_REMOTE_CTL_MASK;
}
/// sig 0x1013 "mqtt_pubkey" (AES-128). Only an unused binary command framing reads it; it is
/// carried so it survives a server switch and is reported in devinfo.
static constexpr const char *DEFAULT_MQTT_KEY = "Sun#0*2Pow925Mdk";
/// Broker port 19999 forces plain TCP whatever the ssl flag says.
static constexpr uint16_t PLAINTEXT_FORCED_PORT = 19999;

// Dongle's own identity in businessData (the will's {"devCode":247,"devType":22}).
static constexpr uint16_t LOGGER_DEV_CODE = 247;  // 0xF7: the dongle itself
static constexpr uint16_t LOGGER_DEV_TYPE = 22;

/// functionEnum capability bitmap sent in rsp/propertydata, 16 bytes, a per-variant constant. The
/// logger's is 0xF7 sig 0x104; a device's is sig 0x2191 (bytes 0-7) + 0x219d (bytes 8-15), with
/// the same bytes. Only the tables listed in table_has_function_enum() carry it.
static constexpr size_t FUNCTION_ENUM_LEN = 16;
static constexpr uint8_t FUNCTION_ENUM_S2[FUNCTION_ENUM_LEN] = {0x07, 0x10, 0x00, 0x00, 0xd7, 0xf1, 0xc4, 0x27,
                                                                0xb0, 0x65, 0x20, 0x00, 0x00, 0x10, 0x06, 0x00};
static constexpr uint8_t FUNCTION_ENUM_S[FUNCTION_ENUM_LEN] = {0x07, 0x00, 0x00, 0x00, 0xd7, 0xf1, 0x44, 0x21,
                                                               0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00};
/// Cloud devType (subordinate logger) whose functionEnum is the logger's 0x104.
static constexpr uint16_t FUNCTION_ENUM_LOGGER_DEV_TYPE = 46;
/// SBR battery (cloud devType 43): bit 108 follows bit 4 of the first word of its 8-byte sig
/// 0x202c block, cleared when unreadable.
static constexpr uint16_t FUNCTION_ENUM_SBR_DEV_TYPE = 43;
static constexpr uint16_t FUNCTION_ENUM_SBR_BIT = 108;
inline std::string function_enum_bytes(bool winet_s) {
  return std::string(reinterpret_cast<const char *>(winet_s ? FUNCTION_ENUM_S : FUNCTION_ENUM_S2), FUNCTION_ENUM_LEN);
}
/// Device tables that carry the functionEnum sigs 0x2191/0x219d: string inverters (0x01, 0x15),
/// hybrids (0x23), SBR batteries (0x2c), EV chargers (0x2e) and the Energy Bridge (0x5c). Other
/// tables (meters, ...) have none: their bitmap is all zeros on the WiNet-S2 and the WiNet-S
/// sends no propertydata for them.
inline bool table_has_function_enum(uint16_t table_type) {
  return table_type == 0x01 || table_type == 0x15 || table_type == 0x23 || table_type == 0x2c ||
         table_type == 0x2e || table_type == 0x5c;
}

// MQTT session: 3.1.1, clean session, keepalive 120 s.
static constexpr uint16_t MQTT_KEEPALIVE_S = 120;
/// Outbound businessData is split into 2560-byte string slices.
static constexpr size_t CHUNK_SIZE = 2560;
/// Inbound payloads (raw and inflated) above 4096 bytes are dropped.
static constexpr size_t MAX_INBOUND_PAYLOAD = 4096;
/// The will length travels in one byte of the CONNECT packet.
static constexpr size_t MAX_WILL_PAYLOAD = 255;
/// Batch id of unsolicited attribute/devinfo publishes.
static constexpr const char *UNSOLICITED_BATCH_ID = "999999999999999";
/// The per-timezone broadcast topic is re-subscribed this often while connected.
static constexpr uint32_t GROUP_TOPIC_REFRESH_S = 14400;

// ---- topic families ------------------------------------------------------------------------
static constexpr const char *const CMD_FAMILIES[] = {
    "settimezone", "propertydata", "setparam",         "iv",            "upgrade",       "second",       "generalcmd",
    "getparam",    "logexport",    "apidata",          "structurefileset", "parammultiset", "parammultiget", "fcas",
};
static constexpr const char *const RSP_FAMILIES[] = {
    "setparam", "structurefileset", "apidata", "propertydata", "upgrade",
};
static constexpr const char *const PUB_FAMILIES[] = {
    "rundata/history", "rundata/5min", "rundata/second", "generaldata", "faultmergedata", "beatdata", "faultdata",
    "faultdata/history", "devstatusdata", "devinfo", "fcas/second", "willdata", "devpointdata",
};
static constexpr size_t NUM_CMD_FAMILIES = sizeof(CMD_FAMILIES) / sizeof(CMD_FAMILIES[0]);
static constexpr size_t NUM_RSP_FAMILIES = sizeof(RSP_FAMILIES) / sizeof(RSP_FAMILIES[0]);
static constexpr size_t NUM_PUB_FAMILIES = sizeof(PUB_FAMILIES) / sizeof(PUB_FAMILIES[0]);

inline bool family_in(const char *const *list, size_t n, const std::string &family) {
  for (size_t i = 0; i < n; i++) {
    if (family == list[i])
      return true;
  }
  return false;
}
inline bool is_rsp_family(const std::string &f) { return family_in(RSP_FAMILIES, NUM_RSP_FAMILIES, f); }
inline bool is_cmd_family(const std::string &f) { return family_in(CMD_FAMILIES, NUM_CMD_FAMILIES, f); }

using MqttKey = std::array<uint8_t, 16>;

inline MqttKey key_from_string(const char *s) {
  MqttKey k{};
  size_t n = s != nullptr ? strnlen(s, k.size()) : 0;
  memcpy(k.data(), s, n);
  return k;
}

enum class Transport : uint8_t { TCP, TLS, INVALID };

/// One broker endpoint (sigs 0x1071/0x1072/0x1013/0x0023, or the "old" backup 0x1957-0x1959/0x0024).
struct ServerEndpoint {
  std::string host{DEFAULT_BROKER_HOST};
  uint16_t port{DEFAULT_BROKER_PORT};
  MqttKey key{key_from_string(DEFAULT_MQTT_KEY)};
  uint16_t ssl_flag{DEFAULT_SSL_FLAG};

  /// ssl flag 0 = plain TCP, 2 = TLS, anything else (incl. 1) = "protocol not found": the link
  /// never connects. Port 19999 forces plain TCP.
  Transport transport() const {
    if (this->ssl_flag == 0 || this->port == PLAINTEXT_FORCED_PORT)
      return Transport::TCP;
    return this->ssl_flag == 2 ? Transport::TLS : Transport::INVALID;
  }
  bool use_tls() const { return this->transport() == Transport::TLS; }
  bool operator==(const ServerEndpoint &o) const {
    return host == o.host && port == o.port && key == o.key && ssl_flag == o.ssl_flag;
  }
  bool operator!=(const ServerEndpoint &o) const { return !(*this == o); }
};

/// Everything the uplink needs to act as the stock dongle. Populated from (lowest to highest
/// precedence): defaults -> persisted copy -> identity source (extflash) -> persisted server
/// switch result -> explicit YAML values. See IsolarcloudComponent::resolve_identity_().
struct CloudIdentity {
  std::string sn;                       // 0x0002: MQTT clientId and topic %s
  uint16_t product_type{0};             // 0x0004 (S2 1824). From sungrow_board unless overridden
  std::string product_name;             // 0x0005 ("WiNet-S2")
  uint16_t encrypt{DEFAULT_ENCRYPT};    // 0x0020 enum #1
  uint16_t keynum{DEFAULT_KEYNUM};      // 0x0021 enum #2
  uint16_t compress{DEFAULT_COMPRESS};  // 0x0022 enum #3
  ServerEndpoint server;                // 0x1071/0x1072/0x1013/0x0023
  ServerEndpoint old_server;            // 0x1957/0x1958/0x1959/0x0024 (rollback target)
  uint16_t timezone_id{DEFAULT_TIMEZONE_ID};               // 0x107a
  std::string timezone{DEFAULT_TIMEZONE};                  // 0x1089
  uint16_t rundata_period_min{DEFAULT_RUNDATA_PERIOD_MIN};  // 0x1015
  bool silence_enable{false};                              // 0x1017
  uint8_t silence_start_hour{0};                           // 0x1018
  uint8_t silence_end_hour{0};                             // 0x1019
  uint32_t remote_ctl_mask{DEFAULT_REMOTE_CTL_MASK};       // 0x6492 (WiNet-S: 0x5232, default 0x4EF)

  bool valid() const { return !this->sn.empty(); }
};

/// Bitmask of CloudIdentity fields; used to record which ones the YAML set explicitly and which
/// ones an identity source provided.
enum IdentityField : uint32_t {
  FIELD_SN = 1u << 0,
  FIELD_PRODUCT_TYPE = 1u << 1,
  FIELD_PRODUCT_NAME = 1u << 2,
  FIELD_ENCRYPT = 1u << 3,
  FIELD_KEYNUM = 1u << 4,
  FIELD_COMPRESS = 1u << 5,
  FIELD_SERVER_HOST = 1u << 6,
  FIELD_SERVER_PORT = 1u << 7,
  FIELD_SERVER_KEY = 1u << 8,
  FIELD_SERVER_SSL = 1u << 9,
  FIELD_OLD_SERVER = 1u << 10,
  FIELD_TIMEZONE_ID = 1u << 11,
  FIELD_RUNDATA_PERIOD = 1u << 12,
  FIELD_SILENCE = 1u << 13,
  FIELD_REMOTE_CTL_MASK = 1u << 14,
  FIELD_TIMEZONE = 1u << 15,
  FIELD_ALL = (1u << 16) - 1,
};
static constexpr uint32_t FIELD_SERVER_ANY = FIELD_SERVER_HOST | FIELD_SERVER_PORT | FIELD_SERVER_KEY | FIELD_SERVER_SSL;

/// Copy the fields named in `mask` from `src` into `dst`.
void apply_identity_fields(CloudIdentity &dst, const CloudIdentity &src, uint32_t mask);

/// Silence window (sigs 0x1017/0x1018/0x1019): every scheduled publish is suppressed while the
/// local hour is in [start, end). Wraps past midnight when start > end; start == end (with enable
/// on) = silenced all day. No 0..23 clamp (values are validated where they enter: YAML / extflash).
inline bool in_silence_window(bool enable, uint8_t start_hour, uint8_t end_hour, uint8_t hour) {
  if (!enable)
    return false;
  if (start_hour == end_hour)
    return true;
  if (start_hour < end_hour)
    return hour >= start_hour && hour < end_hour;
  return hour >= start_hour || hour < end_hour;
}

/// Device table type (0x23 hybrid, 0x15 string inverter, 0x2c SBR battery, 0x12 meter, ...;
/// TelemetryDevice::table_type) -> businessData devType the cloud expects. 0 = unknown table type.
uint16_t cloud_dev_type_for_table(uint16_t table_type);

// ---- remote-control groups (subscriptions and command dispatch) --------------------------------
/// Group g is subscribed iff (mask >> 1 & 1) && (mask >> bit & 1); a cmdType listed in a group is
/// dispatched only if (mask >> bit & 1). cmdTypes in no group are always allowed.
struct RemoteCtlGroup {
  uint8_t bit;
  const char *const *families;  ///< cmd/<SN>/<family> topics (nullptr-terminated)
  const char *const *cmd_types;  ///< nullptr-terminated
};
const RemoteCtlGroup *remote_ctl_groups(size_t *count);
/// True when the mask allows dispatching `cmd_type` (first group listing it decides).
bool remote_ctl_allows(uint32_t mask, const std::string &cmd_type);
/// True when the group bit is subscribed under `mask` (master bit 1 and the group bit).
inline bool remote_ctl_group_on(uint32_t mask, uint8_t bit) { return ((mask >> 1) & 1) && ((mask >> bit) & 1); }

/// Exponential reconnect backoff: initial, x2 per failure, capped. Deterministic (the component
/// adds jitter) so it can be host-tested.
class Backoff {
 public:
  Backoff(uint32_t initial_ms, uint32_t max_ms) : initial_ms_(initial_ms), max_ms_(max_ms), cur_ms_(initial_ms) {}
  /// Delay before the next attempt; grows the following one.
  uint32_t next() {
    uint32_t d = this->cur_ms_;
    this->cur_ms_ = this->cur_ms_ >= this->max_ms_ / 2 ? this->max_ms_ : this->cur_ms_ * 2;
    return d;
  }
  void reset() { this->cur_ms_ = this->initial_ms_; }

 protected:
  uint32_t initial_ms_;
  uint32_t max_ms_;
  uint32_t cur_ms_;
};

}  // namespace esphome::isolarcloud
