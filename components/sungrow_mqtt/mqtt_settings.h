#pragma once
// Runtime MQTT broker settings for sungrow_mqtt (pre-built images, docs/prebuilt.md).
//
// Header-only and free of ESPHome / IDF includes so tests/settings builds it on the host. The
// component stores one MqttSettingsPref record in NVS (pref key fnv1_hash("sungrow_mqtt_settings"))
// and the web UI edits it through GET/POST /api/mqtt_settings. YAML always wins: a `broker` in
// the configuration makes the stored record irrelevant (resolve_mqtt_settings).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome {
namespace sungrow_mqtt {

/// The NVS record. Fixed-size char arrays (NUL-terminated) so the whole struct round-trips through
/// ESPPreferenceObject; the magic distinguishes it from an older or foreign record.
struct MqttSettingsPref {
  uint32_t magic;
  char host[128];
  uint16_t port;
  char user[64];
  char pass[128];
  char prefix[32];
  uint8_t tls;
  uint8_t reserved;
};
static constexpr uint32_t MQTT_SETTINGS_MAGIC = 0x3254514Du;  // "MQT2": host 127, user 63, pass 127 characters

/// The settings as the component uses them. An empty host means "not configured" (client idle).
struct MqttSettings {
  std::string host;
  uint16_t port{1883};
  std::string user;
  std::string pass;
  std::string prefix{"homeassistant"};
  bool tls{false};
};

enum class MqttSettingsSource : uint8_t { NONE, YAML, RUNTIME };

inline const char *mqtt_settings_source_name(MqttSettingsSource s) {
  switch (s) {
    case MqttSettingsSource::YAML:
      return "yaml";
    case MqttSettingsSource::RUNTIME:
      return "runtime";
    default:
      return "none";
  }
}

namespace mqtt_settings_detail {
inline bool printable_ascii(const std::string &s) {
  for (unsigned char c : s)
    if (c < 0x21 || c > 0x7e)
      return false;
  return true;
}
inline bool printable_ascii_or_space(const std::string &s) {
  for (unsigned char c : s)
    if (c < 0x20 || c > 0x7e)
      return false;
  return true;
}
inline void copy_field(char *dst, size_t cap, const std::string &src) {
  const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}
inline bool field_ok(const char *field, size_t cap) { return std::memchr(field, '\0', cap) != nullptr; }
}  // namespace mqtt_settings_detail

/// Validate a settings record the web UI sent. An empty host is accepted (= MQTT off); everything
/// else must fit the NVS record and be plain ASCII (the broker URL and topics are built from it).
/// `why` receives a short reason on failure.
inline bool validate_mqtt_settings(const MqttSettings &s, std::string *why) {
  using namespace mqtt_settings_detail;
  auto fail = [&](const char *msg) {
    if (why != nullptr)
      *why = msg;
    return false;
  };
  if (s.host.size() >= sizeof(MqttSettingsPref::host))
    return fail("host too long (127 characters at most)");
  if (!printable_ascii(s.host))
    return fail("host must be a host name or IP address without spaces");
  if (s.host.find("://") != std::string::npos)
    return fail("host is a name or address, not a URL");
  if (s.port == 0)
    return fail("port must be 1-65535");
  if (s.user.size() >= sizeof(MqttSettingsPref::user))
    return fail("username too long (63 characters at most)");
  if (!printable_ascii_or_space(s.user))
    return fail("username must be printable ASCII");
  if (s.pass.size() >= sizeof(MqttSettingsPref::pass))
    return fail("password too long (127 characters at most)");
  if (!printable_ascii_or_space(s.pass))
    return fail("password must be printable ASCII");
  if (s.prefix.empty() || s.prefix.size() >= sizeof(MqttSettingsPref::prefix))
    return fail("discovery prefix must be 1-31 characters");
  for (unsigned char c : s.prefix) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                    c == '-' || c == '/';
    if (!ok)
      return fail("discovery prefix: letters, digits, '_', '-' and '/' only");
  }
  if (s.prefix.front() == '/' || s.prefix.back() == '/')
    return fail("discovery prefix must not start or end with '/'");
  return true;
}

/// Pack validated settings into the NVS record.
inline void pack_mqtt_settings(const MqttSettings &s, MqttSettingsPref *p) {
  using namespace mqtt_settings_detail;
  std::memset(p, 0, sizeof(*p));
  p->magic = MQTT_SETTINGS_MAGIC;
  copy_field(p->host, sizeof(p->host), s.host);
  p->port = s.port;
  copy_field(p->user, sizeof(p->user), s.user);
  copy_field(p->pass, sizeof(p->pass), s.pass);
  copy_field(p->prefix, sizeof(p->prefix), s.prefix);
  p->tls = s.tls ? 1 : 0;
}

/// Unpack a loaded record; false when the magic or a field is not usable (the record is ignored).
inline bool unpack_mqtt_settings(const MqttSettingsPref &p, MqttSettings *s) {
  using namespace mqtt_settings_detail;
  if (p.magic != MQTT_SETTINGS_MAGIC)
    return false;
  if (!field_ok(p.host, sizeof(p.host)) || !field_ok(p.user, sizeof(p.user)) || !field_ok(p.pass, sizeof(p.pass)) ||
      !field_ok(p.prefix, sizeof(p.prefix)))
    return false;
  s->host = p.host;
  s->port = p.port == 0 ? 1883 : p.port;
  s->user = p.user;
  s->pass = p.pass;
  s->prefix = p.prefix[0] != '\0' ? p.prefix : "homeassistant";
  s->tls = p.tls != 0;
  return validate_mqtt_settings(*s, nullptr);
}

/// Which settings apply: YAML when the configuration names a broker, else the stored record when
/// it holds a host, else none (`out` then carries the defaults with an empty host).
inline MqttSettingsSource resolve_mqtt_settings(const MqttSettings &yaml, const MqttSettings *stored,
                                                MqttSettings *out) {
  if (!yaml.host.empty()) {
    *out = yaml;
    return MqttSettingsSource::YAML;
  }
  if (stored != nullptr && !stored->host.empty()) {
    *out = *stored;
    return MqttSettingsSource::RUNTIME;
  }
  *out = MqttSettings{};
  return MqttSettingsSource::NONE;
}

namespace mqtt_settings_detail {
inline void json_str(std::string &o, const std::string &s) {
  o += '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += static_cast<char>(c);
    } else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof(b), "\\u%04x", c);
      o += b;
    } else {
      o += static_cast<char>(c);
    }
  }
  o += '"';
}
}  // namespace mqtt_settings_detail

/// What the web UI sees. The password NEVER appears; `has_password` says whether one is stored.
/// `available` = runtime settings are compiled in; `managed_by_yaml` = the configuration names a
/// broker, so the form is read-only.
inline std::string mqtt_settings_json(const MqttSettings &s, MqttSettingsSource source, bool available,
                                      bool connected, const std::string &last_error) {
  using mqtt_settings_detail::json_str;
  std::string j = "{\"available\":";
  j += available ? "true" : "false";
  j += ",\"source\":\"";
  j += mqtt_settings_source_name(source);
  j += "\",\"managed_by_yaml\":";
  j += source == MqttSettingsSource::YAML ? "true" : "false";
  j += ",\"configured\":";
  j += s.host.empty() ? "false" : "true";
  j += ",\"host\":";
  json_str(j, s.host);
  j += ",\"port\":";
  j += std::to_string(s.port);
  j += ",\"username\":";
  json_str(j, s.user);
  j += ",\"has_password\":";
  j += s.pass.empty() ? "false" : "true";
  j += ",\"prefix\":";
  json_str(j, s.prefix);
  j += ",\"tls\":";
  j += s.tls ? "true" : "false";
  j += ",\"connected\":";
  j += connected ? "true" : "false";
  j += ",\"last_error\":";
  json_str(j, last_error);
  j += '}';
  return j;
}

}  // namespace sungrow_mqtt
}  // namespace esphome
