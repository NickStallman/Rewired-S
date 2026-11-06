#pragma once
// Usage reporting and the diagnostics report: the parts with no hardware in them (host-tested).
//
// Wire formats (the collector, docs/analytics.md):
//  - usage batch: one PostHog-style JSON body {api_key, sent_at, batch: [{distinct_id, event,
//    timestamp, properties}]} with the events `ping`, `device_registered` and `error`;
//  - diagnostics report: plain text, section by section, scrubbed of the dongle serial, device
//    serials, the node name, the Wi-Fi name and IPv4 addresses before it leaves the dongle.
// Timekeeping takes ms uptime values and epoch seconds; this header never reads a clock.
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../sungrow_datamodel/dm_types.h"

namespace esphome {
namespace sungrow_analytics {

/// `winetVersion` the collector shows for this firmware (the add-on reports 1..3 for stock).
static constexpr int WINET_VERSION = 4;
/// Upload triggers accepted by the collector (X-Trigger).
static constexpr const char *const TRIGGERS[] = {"manual", "crash",  "stall", "watchdog", "offline",
                                                 "cloud",  "heap",   "modbus", "device"};
inline bool trigger_known(const char *t) {
  for (const char *k : TRIGGERS)
    if (t != nullptr && std::strcmp(k, t) == 0)
      return true;
  return false;
}

// ---- small helpers ------------------------------------------------------------------------------

inline std::string hex_lower(const uint8_t *d, size_t n) {
  static const char *const H = "0123456789abcdef";
  std::string s;
  s.reserve(2 * n);
  for (size_t i = 0; i < n; i++) {
    s += H[d[i] >> 4];
    s += H[d[i] & 0xF];
  }
  return s;
}

inline void json_escape_into(std::string &out, const char *s) {
  for (; s != nullptr && *s != 0; s++) {
    const unsigned char c = static_cast<unsigned char>(*s);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", c);
          out += b;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
}

inline void json_str(std::string &out, const char *key, const char *value) {
  out += '"';
  out += key;
  out += "\":\"";
  json_escape_into(out, value);
  out += '"';
}

inline void json_str(std::string &out, const char *key, const std::string &value) { json_str(out, key, value.c_str()); }

inline void json_num(std::string &out, const char *key, long long value) {
  out += '"';
  out += key;
  out += "\":";
  out += std::to_string(value);
}

inline void json_bool(std::string &out, const char *key, bool value) {
  out += '"';
  out += key;
  out += value ? "\":true" : "\":false";
}

/// esp_reset_reason() value (diag_journal.h ResetReason numbering) as the collector's code.
inline const char *reset_reason_code(int r) {
  switch (r) {
    case 1:
      return "POWERON";
    case 2:
      return "EXT";
    case 3:
      return "SW";
    case 4:
      return "PANIC";
    case 5:
      return "INT_WDT";
    case 6:
      return "TASK_WDT";
    case 7:
      return "WDT";
    case 8:
      return "DEEPSLEEP";
    case 9:
      return "BROWNOUT";
    case 10:
      return "SDIO";
    case 11:
      return "USB";
    case 12:
      return "JTAG";
    case 13:
      return "EFUSE";
    case 14:
      return "PWR_GLITCH";
    case 15:
      return "CPU_LOCKUP";
    default:
      return "UNKNOWN";
  }
}

/// Device role of a register-table family, as the collector names it.
inline const char *role_name(sungrow_datamodel::Family f) {
  using sungrow_datamodel::Family;
  switch (f) {
    case Family::INVERTER:
      return "inverter";
    case Family::BATTERY:
      return "battery";
    case Family::CHARGER:
      return "charger";
    case Family::ENERGY_BRIDGE:
      return "bridge";
    case Family::METER:
      return "meter";
    case Family::LOGGER:
      return "logger";
  }
  return "other";
}

/// Firmware-version signals of a family: (sigid, property name). Returns the number filled.
struct VersionSig {
  uint16_t sigid;
  const char *name;
};
inline size_t version_sigids(sungrow_datamodel::Family f, const VersionSig **out) {
  using sungrow_datamodel::Family;
  static const VersionSig INV[] = {{0x2008, "arm_version"},
                                   {0x2009, "dsp_version"},
                                   {0x2369, "inverter_firmware"},
                                   {0x236B, "battery_firmware"},
                                   {0x236A, "comm_firmware"}};
  static const VersionSig BAT[] = {{0x2016, "firmware"}, {0x2002, "software"}};
  static const VersionSig CHG[] = {{0x2002, "software"}};
  static const VersionSig EB[] = {{0x2002, "firmware"}};
  switch (f) {
    case Family::INVERTER:
      *out = INV;
      return sizeof(INV) / sizeof(INV[0]);
    case Family::BATTERY:
      *out = BAT;
      return sizeof(BAT) / sizeof(BAT[0]);
    case Family::CHARGER:
      *out = CHG;
      return sizeof(CHG) / sizeof(CHG[0]);
    case Family::ENERGY_BRIDGE:
      *out = EB;
      return sizeof(EB) / sizeof(EB[0]);
    default:
      *out = nullptr;
      return 0;
  }
}

// ---- usage batch --------------------------------------------------------------------------------

struct PingProps {
  const char *version{""};          ///< firmware (project) version
  const char *esphome_version{""};
  const char *stock_version{""};    ///< stock firmware version the dongle presents to the cloud
  const char *board{""};            ///< "winet_s" / "winet_s2"
  const char *chip{""};             ///< IDF target
  const char *network{""};          ///< "wifi" / "ethernet"
  uint32_t uptime_s{0};
  const char *reset_reason{""};     ///< reset_reason_code()
  bool cloud_uplink{false};
  bool mqtt{false};
  bool modbus_tcp{false};
  bool control_writes{false};
  uint32_t psram_kb{0};
  uint32_t heap_min_kb{0};
};

struct DeviceEvent {
  std::string model;
  const char *role{"other"};
  uint8_t dev_type{0};
  uint8_t protocol{0};
  uint16_t dev_code{0};
  bool inv_type_known{false};
  uint16_t inv_type{0};
  std::vector<std::pair<const char *, std::string>> versions;
};

struct ErrorEvent {
  std::string type;
  std::string error;
  std::string device;  ///< model, "" when not about one device
  uint32_t count{1};   ///< occurrences this event stands for (ErrorThrottle)
};

/// One usage batch. `sent_at` is an RFC 3339 UTC stamp or "" (no valid clock: no timestamps;
/// the collector uses its receive time). `ping` may be nullptr for an error-only batch.
inline std::string build_batch(const char *api_key, const std::string &distinct_id, const std::string &sent_at,
                               const PingProps *ping, const std::vector<DeviceEvent> &devices,
                               const std::vector<ErrorEvent> &errors) {
  std::string b = "{";
  json_str(b, "api_key", api_key);
  if (!sent_at.empty()) {
    b += ',';
    json_str(b, "sent_at", sent_at);
  }
  b += ",\"batch\":[";
  bool first = true;
  auto head = [&](const char *event) {
    if (!first)
      b += ',';
    first = false;
    b += '{';
    json_str(b, "distinct_id", distinct_id);
    b += ',';
    json_str(b, "event", event);
    if (!sent_at.empty()) {
      b += ',';
      json_str(b, "timestamp", sent_at);
    }
    b += ",\"properties\":{";
  };
  auto tail = [&]() { b += "}}"; };
  const char *version = ping != nullptr ? ping->version : "";
  if (ping != nullptr) {
    head("ping");
    json_str(b, "version", ping->version);
    b += ',';
    json_str(b, "esphome_version", ping->esphome_version);
    b += ',';
    json_str(b, "stock_version", ping->stock_version);
    b += ',';
    json_str(b, "board", ping->board);
    b += ',';
    json_str(b, "chip", ping->chip);
    b += ',';
    json_str(b, "network", ping->network);
    b += ',';
    json_num(b, "uptime_s", ping->uptime_s);
    b += ',';
    json_str(b, "reset_reason", ping->reset_reason);
    b += ',';
    json_bool(b, "cloud_uplink", ping->cloud_uplink);
    b += ',';
    json_bool(b, "mqtt", ping->mqtt);
    b += ',';
    json_bool(b, "modbus_tcp", ping->modbus_tcp);
    b += ',';
    json_bool(b, "control_writes", ping->control_writes);
    b += ',';
    json_num(b, "psram_kb", ping->psram_kb);
    b += ',';
    json_num(b, "heap_min_kb", ping->heap_min_kb);
    b += ',';
    json_num(b, "winetVersion", WINET_VERSION);
    b += ',';
    json_num(b, "winetCount", 1);
    tail();
    for (const DeviceEvent &d : devices) {
      head("device_registered");
      json_str(b, "device", d.model);
      b += ',';
      json_str(b, "role", d.role);
      b += ',';
      json_num(b, "dev_type", d.dev_type);
      b += ',';
      json_num(b, "protocol", d.protocol);
      b += ',';
      json_num(b, "dev_code", d.dev_code);
      if (d.inv_type_known) {
        b += ',';
        json_num(b, "inv_type", d.inv_type);
      }
      for (const auto &v : d.versions) {
        b += ',';
        json_str(b, v.first, v.second);
      }
      b += ',';
      json_str(b, "version", version);
      b += ',';
      json_num(b, "winetVersion", WINET_VERSION);
      tail();
    }
  }
  for (const ErrorEvent &e : errors) {
    head("error");
    json_str(b, "type", e.type);
    b += ',';
    json_str(b, "error", e.error);
    if (!e.device.empty()) {
      b += ',';
      json_str(b, "device", e.device);
    }
    b += ',';
    json_num(b, "count", e.count);
    b += ',';
    json_str(b, "version", version);
    b += ',';
    json_num(b, "winetVersion", WINET_VERSION);
    tail();
  }
  b += "]}";
  return b;
}

/// Error events as the add-on sends them: the first occurrence of a key goes out at once, a
/// repeat at most once per interval carrying the number of occurrences it stands for.
class ErrorThrottle {
 public:
  static constexpr size_t MAX_KEYS = 32;
  explicit ErrorThrottle(uint32_t interval_ms = 3600000u) : interval_ms_(interval_ms) {}
  /// Returns the `count` to report (>= 1), or 0 when this occurrence is suppressed.
  uint32_t report(const std::string &key, uint32_t now_ms) {
    for (Entry &e : this->entries_) {
      if (e.key != key)
        continue;
      if (now_ms - e.last_sent_ms < this->interval_ms_) {
        e.suppressed++;
        return 0;
      }
      const uint32_t n = e.suppressed + 1;
      e.suppressed = 0;
      e.last_sent_ms = now_ms;
      return n;
    }
    if (this->entries_.size() >= MAX_KEYS)
      this->entries_.erase(this->entries_.begin());
    this->entries_.push_back({key, now_ms, 0});
    return 1;
  }
  size_t size() const { return this->entries_.size(); }

 private:
  struct Entry {
    std::string key;
    uint32_t last_sent_ms;
    uint32_t suppressed;
  };
  std::vector<Entry> entries_;
  uint32_t interval_ms_;
};

/// Ping cadence: the first ping 60..120 s after boot (spread from `seed`), then every 24 h. A
/// due ping waits up to TIME_WAIT_MS for a valid clock, then goes without timestamps.
struct Schedule {
  static constexpr uint32_t FIRST_MIN_MS = 60000u;
  static constexpr uint32_t FIRST_SPREAD_MS = 60000u;
  static constexpr uint32_t PERIOD_MS = 24u * 3600u * 1000u;
  static constexpr uint32_t TIME_WAIT_MS = 5u * 60u * 1000u;
  uint32_t next_due_ms{0};
  bool armed{false};
  void arm(uint32_t seed, uint32_t now_ms) {
    this->next_due_ms = now_ms + FIRST_MIN_MS + seed % FIRST_SPREAD_MS;
    this->armed = true;
  }
  bool due(uint32_t now_ms, bool time_valid) const {
    if (!this->armed)
      return false;
    const uint32_t over = now_ms - this->next_due_ms;
    if (static_cast<int32_t>(over) < 0)
      return false;
    return time_valid || over >= TIME_WAIT_MS;
  }
  void sent(uint32_t now_ms) { this->next_due_ms = now_ms + PERIOD_MS; }
};

/// Automatic diagnostics uploads: at most MAX per 24 h window, counted in persisted state. The
/// window is measured in epoch seconds and only advances with a valid clock; without one the
/// count never resets (a dongle in a crash loop with no time cannot flood the collector).
struct UploadCap {
  static constexpr uint8_t MAX = 3;
  static constexpr uint32_t WINDOW_S = 24u * 3600u;
  uint32_t window_start_epoch{0};
  uint8_t uploads{0};
  /// `now_epoch` = 0 means no valid clock.
  bool allow(uint32_t now_epoch) {
    this->roll_(now_epoch);
    return this->uploads < MAX;
  }
  void record(uint32_t now_epoch) {
    this->roll_(now_epoch);
    if (this->window_start_epoch == 0 && now_epoch != 0)
      this->window_start_epoch = now_epoch;
    if (this->uploads < 255)
      this->uploads++;
  }

 private:
  void roll_(uint32_t now_epoch) {
    if (now_epoch == 0)
      return;
    if (this->window_start_epoch != 0 && now_epoch - this->window_start_epoch >= WINDOW_S) {
      this->window_start_epoch = now_epoch;
      this->uploads = 0;
    } else if (this->window_start_epoch == 0 && this->uploads > 0) {
      // Uploads counted before the clock was valid start the window now.
      this->window_start_epoch = now_epoch;
    }
  }
};

// ---- scrubbing ----------------------------------------------------------------------------------

/// Replaces secrets (serial numbers, the node name, the Wi-Fi name) and IPv4 addresses in text
/// and the secrets' bytes in raw register data. Secrets shorter than 4 characters are ignored.
class Scrubber {
 public:
  void add(const std::string &secret, const char *placeholder) {
    if (secret.size() < 4)
      return;
    for (const auto &s : this->secrets_)
      if (s.first == secret)
        return;
    this->secrets_.emplace_back(secret, placeholder);
  }
  size_t size() const { return this->secrets_.size(); }

  std::string apply(const std::string &text) const {
    std::string t = text;
    for (const auto &s : this->secrets_) {
      size_t pos = 0;
      while ((pos = t.find(s.first, pos)) != std::string::npos) {
        t.replace(pos, s.first.size(), s.second);
        pos += s.second.size();
      }
    }
    return mask_ipv4(t);
  }

  /// Overwrites every occurrence of a secret's bytes in `data` with '*'.
  void apply_bytes(uint8_t *data, size_t len) const {
    for (const auto &s : this->secrets_) {
      const size_t n = s.first.size();
      if (n > len)
        continue;
      for (size_t i = 0; i + n <= len; i++) {
        if (std::memcmp(data + i, s.first.data(), n) == 0) {
          std::memset(data + i, '*', n);
          i += n - 1;
        }
      }
    }
  }

  /// Dotted quads (exactly four groups of 1..3 digits, not inside a longer dotted number) become
  /// <ip>; version strings such as 0.9.0 or 200.001.00.P046 are left alone.
  static std::string mask_ipv4(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    const size_t n = text.size();
    size_t i = 0;
    auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
    while (i < n) {
      const bool boundary = i == 0 || !(is_digit(text[i - 1]) || text[i - 1] == '.');
      if (boundary && is_digit(text[i])) {
        size_t j = i;
        int groups = 0;
        bool ok = true;
        while (ok) {
          size_t k = j;
          while (k < n && k - j < 3 && is_digit(text[k]))
            k++;
          if (k == j) {
            ok = false;
            break;
          }
          groups++;
          j = k;
          if (groups == 4)
            break;
          if (j < n && text[j] == '.')
            j++;
          else
            ok = false;
        }
        // A fifth dotted group disqualifies; a full stop after the address does not.
        const bool trailing_ok =
            j >= n || !(is_digit(text[j]) || (text[j] == '.' && j + 1 < n && is_digit(text[j + 1])));
        if (ok && groups == 4 && trailing_ok) {
          out += "<ip>";
          i = j;
          continue;
        }
      }
      out += text[i++];
    }
    return out;
  }

 private:
  std::vector<std::pair<std::string, std::string>> secrets_;
};

// ---- diagnostics report sections ----------------------------------------------------------------

struct BundleBuild {
  const char *project_version{""};
  const char *esphome_version{""};
  const char *idf_version{""};
  const char *build_id{""};  ///< "0x<config hash> <build time>"
  const char *board{""};
  const char *chip{""};
  const char *network{""};
  const char *stock_version{""};
  uint32_t flash_kb{0};
  uint32_t psram_kb{0};
  uint32_t boot_count{0};
  uint32_t uptime_s{0};
  const char *reset_reason{""};
  std::string last_reset;  ///< the "Last reset" verdict of this boot
};

struct BundleFeatures {
  bool analytics_enabled{false};
  bool cloud_present{false}, cloud_enabled{false}, cloud_connected{false}, cloud_error{false};
  uint32_t cloud_failures{0};
  bool mqtt_present{false}, mqtt_connected{false};
  bool mbtcp_present{false}, mbtcp_enabled{false};
  uint32_t mbtcp_requests{0}, mbtcp_exceptions{0}, mbtcp_dropped{0}, mbtcp_rejected{0};
  bool control_present{false}, control_enabled{false};
  uint32_t writes_ok{0}, writes_failed{0}, writes_rejected{0}, writes_mismatch{0};
  bool capture_present{false}, capture_running{false};
  const char *capture_kind{""};
  const char *net_fallback{nullptr};  ///< network fallback state line (nullptr: not built)
};

struct BundleMemory {
  uint32_t heap_free{0};
  uint32_t heap_min{0};
  uint32_t psram_total{0};
  uint32_t psram_free{0};
};

struct BundleJob {
  uint8_t fn{0};
  uint16_t start{0};
  uint16_t count{0};
  const char *tier{""};
  const char *effective_tier{""};
  bool pending{false};
  bool unserved{false};
  uint8_t last_exc{0};
  uint32_t exc_count{0};
  int32_t last_ok_age_s{-1};  ///< -1 = never answered with data
};

struct BundleDevice {
  uint8_t index{0};
  uint8_t address{0};
  const char *child_kind{"none"};
  int parent{-1};
  const char *state{""};
  uint16_t dev_code{0};
  std::string model;
  std::string proto_code;
  std::string tag;
  uint8_t protocol{0};
  uint8_t dev_type{0};
  const char *dev_type_source{""};
  bool inv_type_known{false};
  uint16_t inv_type{0};
  std::string table;
  const char *family{""};
  bool fallback_used{false};
  const char *xml_family{""};
  bool has_descriptor{false};
  uint16_t country_reg{0}, grid_reg{0}, power_switch_reg{0};
  bool identified{false}, discovered{false}, online{false}, abandoned{false}, first_round_done{false};
  std::vector<uint8_t> ident_raw;  ///< big-endian registers; empty = no identification reply kept
  uint16_t ident_start{0};
  uint8_t ident_fn{0};
  uint32_t sent{0}, received{0}, timeouts{0}, crc_errors{0}, exceptions{0};
  float loss_pct{-1.0f};
  std::vector<BundleJob> jobs;
  std::vector<std::pair<std::string, std::string>> static_values;
  std::vector<std::pair<const char *, std::string>> versions;
};

inline void write_line(std::string &out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
inline void write_line(std::string &out, const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  out += buf;
  out += '\n';
}

/// First line of the report: what the collector lists an upload by.
inline void write_headline(std::string &out, const BundleBuild &b) {
  write_line(out, "Rewired-S %s on %s (%s, %s), uptime %u s, reset %s", b.project_version, b.board, b.chip, b.network,
             static_cast<unsigned>(b.uptime_s), b.reset_reason);
}

inline void write_build(std::string &out, const BundleBuild &b) {
  out += "== Build ==\n";
  write_line(out, "firmware: Rewired-S %s", b.project_version);
  write_line(out, "esphome: %s, idf: %s", b.esphome_version, b.idf_version);
  write_line(out, "build: %s", b.build_id);
  write_line(out, "board: %s, chip: %s, network: %s", b.board, b.chip, b.network);
  write_line(out, "stock version presented: %s", b.stock_version);
  write_line(out, "flash: %u KB, psram: %u KB", static_cast<unsigned>(b.flash_kb), static_cast<unsigned>(b.psram_kb));
  write_line(out, "boot: #%u, uptime %u s, reset reason %s", static_cast<unsigned>(b.boot_count),
             static_cast<unsigned>(b.uptime_s), b.reset_reason);
  write_line(out, "last reset: %s", b.last_reset.c_str());
}

inline void write_features(std::string &out, const BundleFeatures &f) {
  out += "== Features ==\n";
  write_line(out, "usage reporting: %s", f.analytics_enabled ? "on" : "off");
  if (f.cloud_present)
    write_line(out, "cloud uplink: %s, %s%s, connect failures %u", f.cloud_enabled ? "enabled" : "disabled",
               f.cloud_connected ? "connected" : "not connected", f.cloud_error ? ", error" : "",
               static_cast<unsigned>(f.cloud_failures));
  else
    out += "cloud uplink: not built\n";
  if (f.mqtt_present)
    write_line(out, "mqtt: %s", f.mqtt_connected ? "connected" : "not connected");
  else
    out += "mqtt: not built\n";
  if (f.mbtcp_present)
    write_line(out, "modbus tcp: %s, requests %u, exceptions %u, dropped %u, rejected connections %u",
               f.mbtcp_enabled ? "enabled" : "disabled", static_cast<unsigned>(f.mbtcp_requests),
               static_cast<unsigned>(f.mbtcp_exceptions), static_cast<unsigned>(f.mbtcp_dropped),
               static_cast<unsigned>(f.mbtcp_rejected));
  else
    out += "modbus tcp: not built\n";
  if (f.control_present)
    write_line(out, "inverter writes: %s, ok %u, failed %u, rejected %u, verify mismatch %u",
               f.control_enabled ? "enabled" : "disabled", static_cast<unsigned>(f.writes_ok),
               static_cast<unsigned>(f.writes_failed), static_cast<unsigned>(f.writes_rejected),
               static_cast<unsigned>(f.writes_mismatch));
  else
    out += "inverter writes: not built\n";
  if (f.capture_present)
    write_line(out, "captures: %s%s", f.capture_running ? "running " : "idle", f.capture_running ? f.capture_kind : "");
  else
    out += "captures: not built\n";
  if (f.net_fallback != nullptr)
    write_line(out, "network fallback: %s", f.net_fallback);
}

inline void write_memory(std::string &out, const BundleMemory &m) {
  out += "== Memory ==\n";
  write_line(out, "internal heap: %u free, %u minimum", static_cast<unsigned>(m.heap_free),
             static_cast<unsigned>(m.heap_min));
  write_line(out, "psram: %u KB total, %u KB free", static_cast<unsigned>(m.psram_total / 1024),
             static_cast<unsigned>(m.psram_free / 1024));
}

/// Hex dump of big-endian registers, 8 per line with the register address and an ASCII column.
inline void hex_dump_into(std::string &out, const uint8_t *data, size_t len, uint16_t start_reg) {
  for (size_t off = 0; off < len; off += 16) {
    char line[96];
    int n = std::snprintf(line, sizeof(line), "  %5u:", static_cast<unsigned>(start_reg + off / 2));
    for (size_t i = off; i < off + 16 && i < len; i += 2) {
      if (i + 1 < len)
        n += std::snprintf(line + n, sizeof(line) - n, " %02x%02x", data[i], data[i + 1]);
      else
        n += std::snprintf(line + n, sizeof(line) - n, " %02x", data[i]);
    }
    out += line;
    out += "  |";
    for (size_t i = off; i < off + 16 && i < len; i++) {
      const char c = static_cast<char>(data[i]);
      out += (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    out += "|\n";
  }
}

inline void write_device_support(std::string &out, const BundleDevice &d) {
  write_line(out, "-- device %u: %s (%s, addr %u) --", d.index, d.model.empty() ? "(no model)" : d.model.c_str(),
             d.family[0] != 0 ? d.family : "no table", d.address);
  write_line(out, "state: %s, %s, %s%s%s%s", d.state, d.online ? "online" : "offline",
             d.discovered ? "discovered" : (d.identified ? "identified" : "unidentified"),
             d.first_round_done ? ", first poll round done" : "", d.abandoned ? ", abandoned" : "",
             d.fallback_used ? ", fallback table" : "");
  write_line(out, "identity: dev_code 0x%04X, model \"%s\", protocol code \"%s\", tag \"%s\", protocol type %u, dev_type %u (%s)",
             d.dev_code, d.model.c_str(), d.proto_code.c_str(), d.tag.c_str(), d.protocol, d.dev_type, d.dev_type_source);
  if (d.child_kind != nullptr && std::strcmp(d.child_kind, "none") != 0)
    write_line(out, "reported by: device %d as %s", d.parent, d.child_kind);
  if (d.inv_type_known)
    write_line(out, "inverter type (30644): %u (0x%04X)", d.inv_type, d.inv_type);
  else
    out += "inverter type (30644): unknown\n";
  write_line(out, "table: %s, xml family: %s, descriptor: %s", d.table.empty() ? "(none)" : d.table.c_str(),
             d.xml_family, d.has_descriptor ? "yes" : "none");
  if (d.has_descriptor)
    write_line(out, "descriptor registers: country %u, grid code %u, power switch %u", d.country_reg, d.grid_reg,
               d.power_switch_reg);
  for (const auto &v : d.versions)
    write_line(out, "version %s: %s", v.first, v.second.c_str());
  write_line(out, "link: sent %u, received %u, timeouts %u, crc errors %u, exceptions %u, loss %.1f %%",
             static_cast<unsigned>(d.sent), static_cast<unsigned>(d.received), static_cast<unsigned>(d.timeouts),
             static_cast<unsigned>(d.crc_errors), static_cast<unsigned>(d.exceptions),
             static_cast<double>(d.loss_pct));
  if (!d.ident_raw.empty()) {
    write_line(out, "identification reply: fn %u, %u registers from %u", d.ident_fn,
               static_cast<unsigned>(d.ident_raw.size() / 2), d.ident_start);
    hex_dump_into(out, d.ident_raw.data(), d.ident_raw.size(), d.ident_start);
  } else {
    out += "identification reply: none kept\n";
  }
  write_line(out, "register blocks (%u):", static_cast<unsigned>(d.jobs.size()));
  for (const BundleJob &j : d.jobs) {
    char age[24];
    if (j.last_ok_age_s < 0)
      std::snprintf(age, sizeof(age), "never");
    else
      std::snprintf(age, sizeof(age), "%ld s ago", static_cast<long>(j.last_ok_age_s));
    write_line(out, "  fn %u %5u+%-3u %-6s %s%s%s exc %u (last %u), data %s", j.fn, j.start, j.count, j.tier,
               j.unserved ? "unserved" : "served", std::strcmp(j.tier, j.effective_tier) != 0 ? " (polled static)" : "",
               j.pending ? ", pending" : "", static_cast<unsigned>(j.exc_count), j.last_exc, age);
  }
  write_line(out, "static values (%u):", static_cast<unsigned>(d.static_values.size()));
  for (const auto &v : d.static_values)
    write_line(out, "  %s = %s", v.first.c_str(), v.second.c_str());
}

}  // namespace sungrow_analytics
}  // namespace esphome
