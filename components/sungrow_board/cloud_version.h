#pragma once
// The firmware version the dongle reports to iSolarCloud (logger signal 0x1000), and where it
// comes from (docs/cloud_identity.md, docs/prebuilt.md).
//
// Header-only, no ESPHome / IDF includes (tests/settings builds it on the host). Precedence:
//   OVERRIDE  the value stored from the web UI (POST /api/cloud_version)
//   YAML      sungrow_board.firmware_version / the cloud_firmware_version substitution, when set
//   DETECTED  the version string of the stock app left in the other OTA slot, noted on first
//             sight and kept in NVS (a second OTA overwrites that slot)
//   DEFAULT   the stock version each board file is built against
// Pref key fnv1_hash("sungrow_board_cloud_version").
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace esphome {
namespace sungrow_board {

struct CloudVersionPref {
  uint32_t magic;
  char override[40];  ///< "" = none
  char detected[40];  ///< "" = never seen a stock image in the other slot
};
static constexpr uint32_t CLOUD_VERSION_MAGIC = 0x3156434Bu;  // "KCV1"
static constexpr size_t CLOUD_VERSION_MAX = sizeof(CloudVersionPref::override) - 1;

enum class CloudVersionSource : uint8_t { OVERRIDE, YAML, DETECTED, DEFAULT };

inline const char *cloud_version_source_name(CloudVersionSource s) {
  switch (s) {
    case CloudVersionSource::OVERRIDE:
      return "override";
    case CloudVersionSource::YAML:
      return "yaml";
    case CloudVersionSource::DETECTED:
      return "detected";
    default:
      return "default";
  }
}

namespace cloud_version_detail {
inline bool digits(const char *p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i] < '0' || p[i] > '9')
      return false;
  return true;
}
}  // namespace cloud_version_detail

/// Length of the stock-looking prefix `WINET-SV\d{3}\.\d{3}\.\d{2}\.P\d{3}` at the start of `s`
/// (23 characters), or 0 when `s` does not start with one. Both stock apps report their
/// esp_app_desc_t.version in this form with a build suffix (`...P032B001-dir`).
inline size_t stock_version_prefix_len(const char *s) {
  using cloud_version_detail::digits;
  if (s == nullptr)
    return 0;
  static constexpr const char *HEAD = "WINET-SV";
  const size_t head = std::strlen(HEAD);
  if (std::strncmp(s, HEAD, head) != 0)
    return 0;
  const char *p = s + head;
  // \d{3} . \d{3} . \d{2} . P \d{3}
  if (std::strlen(p) < 15)
    return 0;
  if (!digits(p, 3) || p[3] != '.' || !digits(p + 4, 3) || p[7] != '.' || !digits(p + 8, 2) || p[10] != '.' ||
      p[11] != 'P' || !digits(p + 12, 3))
    return 0;
  return head + 15;
}

/// True when `s` starts with a stock version string (a build suffix may follow).
inline bool is_stock_version_string(const char *s) { return stock_version_prefix_len(s) != 0; }

/// The stock version with everything after `P\d{3}` stripped ("WINET-SV300.001.03.P032B001-dir"
/// -> "WINET-SV300.001.03.P032"); "" when `s` is not stock-looking.
inline std::string normalize_stock_version(const char *s) {
  const size_t n = stock_version_prefix_len(s);
  return n == 0 ? std::string() : std::string(s, n);
}

/// A value the web UI may store as the override: a normalized stock version, nothing else.
inline bool validate_cloud_version_override(const std::string &v, std::string *why) {
  if (v.size() > CLOUD_VERSION_MAX || stock_version_prefix_len(v.c_str()) != v.size()) {
    if (why != nullptr)
      *why = "expected a stock version such as WINET-SV200.001.00.P046";
    return false;
  }
  return true;
}

inline bool cloud_version_pref_valid(const CloudVersionPref &p) {
  return p.magic == CLOUD_VERSION_MAGIC && std::memchr(p.override, '\0', sizeof(p.override)) != nullptr &&
         std::memchr(p.detected, '\0', sizeof(p.detected)) != nullptr;
}

inline void cloud_version_pref_init(CloudVersionPref *p) {
  std::memset(p, 0, sizeof(*p));
  p->magic = CLOUD_VERSION_MAGIC;
}

namespace cloud_version_detail {
inline void copy_field(char *dst, size_t cap, const std::string &src) {
  const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
  std::memcpy(dst, src.data(), n);
  dst[n] = '\0';
}
}  // namespace cloud_version_detail

/// Note the other slot's version on first sight: true when the record changed (save it). A later
/// OTA overwrites that slot, so the first sighting is the one that is kept; a second stock image
/// seen later (the user went back to stock and then re-installed) replaces it only when it differs
/// and the record was empty.
inline bool note_detected_version(CloudVersionPref *p, const char *other_slot_version) {
  const std::string v = normalize_stock_version(other_slot_version);
  if (v.empty() || p->detected[0] != '\0')
    return false;
  cloud_version_detail::copy_field(p->detected, sizeof(p->detected), v);
  return true;
}

/// Set (or clear with "") the override: true when the record changed.
inline bool set_cloud_version_override(CloudVersionPref *p, const std::string &v) {
  if (std::strncmp(p->override, v.c_str(), sizeof(p->override)) == 0)
    return false;
  cloud_version_detail::copy_field(p->override, sizeof(p->override), v);
  return true;
}

struct CloudVersionChoice {
  std::string value;
  CloudVersionSource source{CloudVersionSource::DEFAULT};
};

/// Pure precedence: OVERRIDE > YAML > DETECTED > DEFAULT. `pref` may be null (no record);
/// `yaml` "" = not set; `fallback_default` is the board's stock version.
inline CloudVersionChoice choose_cloud_version(const CloudVersionPref *pref, const char *yaml,
                                               const char *fallback_default) {
  CloudVersionChoice c;
  if (pref != nullptr && pref->override[0] != '\0') {
    c.value = pref->override;
    c.source = CloudVersionSource::OVERRIDE;
    return c;
  }
  if (yaml != nullptr && yaml[0] != '\0') {
    c.value = yaml;
    c.source = CloudVersionSource::YAML;
    return c;
  }
  if (pref != nullptr && pref->detected[0] != '\0') {
    c.value = pref->detected;
    c.source = CloudVersionSource::DETECTED;
    return c;
  }
  c.value = fallback_default != nullptr ? fallback_default : "";
  c.source = CloudVersionSource::DEFAULT;
  return c;
}

}  // namespace sungrow_board
}  // namespace esphome
