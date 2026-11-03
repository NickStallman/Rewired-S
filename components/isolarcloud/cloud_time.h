#pragma once
// Local time for sendTime / dataTime. Host-buildable.
//
// Both are "%04d%02d%02d%02d%02d%02d" of the local time, where the zone comes from the 0xF7/0x1089
// string ("UTC+8:00" by default; the cloud's settimezone writes e.g. "UTC+10:00"). "UTC+10:00"
// means 10 h AHEAD of UTC: local = UTC + offset.

#include <cstdint>
#include <string>

namespace esphome::isolarcloud {

struct CivilTime {
  int year{1970};
  int month{1};  ///< 1..12
  int day{1};    ///< 1..31
  int hour{0};
  int minute{0};
  int second{0};
};

/// Parse a cloud timezone string "UTC", "UTC+H", "UTC+H:MM", "UTC-HH:MM" (also "GMT…").
/// Returns the offset to ADD to UTC, in seconds (+28800 for "UTC+8:00"). False if malformed or
/// beyond ±14 h.
bool parse_cloud_timezone(const std::string &tz, int32_t &offset_s);
/// settimezone (10324) accepts only strlen == 9 strings starting with "UTC" ("UTC+10:00").
bool settimezone_string_ok(const std::string &tz);

/// Seconds since the epoch -> civil fields (proleptic Gregorian, no leap seconds).
CivilTime civil_from_epoch(int64_t epoch_s);
/// Civil fields -> seconds since the epoch (inverse of civil_from_epoch).
int64_t epoch_from_civil(const CivilTime &t);
/// UTC epoch + cloud offset -> local civil time.
inline CivilTime local_from_utc(int64_t utc_epoch_s, int32_t offset_s) { return civil_from_epoch(utc_epoch_s + offset_s); }

/// "YYYYMMDDhhmmss" ("%04d%02d%02d%02d%02d%02d").
std::string format_cloud_time(const CivilTime &t);
/// Parse "YYYYMMDDhhmmss" (10307 settime dataTime, "%4ld%2ld%2ld%2ld%2ld%2ld"). Range-checked.
bool parse_cloud_time(const std::string &s, CivilTime &out);
/// The clock counts as set when the year is in 1971..2069.
inline bool clock_plausible(const CivilTime &t) { return t.year >= 1971 && t.year <= 2069; }

}  // namespace esphome::isolarcloud
