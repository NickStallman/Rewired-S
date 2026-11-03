#include "cloud_time.h"

#include <cstdio>

namespace esphome::isolarcloud {

namespace {
bool digits(const char *p, int n, int &out) {
  out = 0;
  for (int i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9')
      return false;
    out = out * 10 + (p[i] - '0');
  }
  return true;
}

// Howard Hinnant's days_from_civil / civil_from_days.
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
}  // namespace

bool parse_cloud_timezone(const std::string &tz, int32_t &offset_s) {
  if (tz.size() < 3 || (tz.compare(0, 3, "UTC") != 0 && tz.compare(0, 3, "GMT") != 0))
    return false;
  const char *p = tz.c_str() + 3;
  if (*p == '\0') {
    offset_s = 0;
    return true;
  }
  int sign = 0;
  if (*p == '+')
    sign = 1;
  else if (*p == '-')
    sign = -1;
  else
    return false;
  p++;
  int h = 0, m = 0, nd = 0;
  while (*p >= '0' && *p <= '9' && nd < 2) {
    h = h * 10 + (*p++ - '0');
    nd++;
  }
  if (nd == 0)
    return false;
  if (*p == ':') {
    p++;
    if (!digits(p, 2, m))
      return false;
    p += 2;
  }
  if (*p != '\0' || h > 14 || m > 59 || h * 60 + m > 14 * 60)
    return false;
  offset_s = sign * (h * 3600 + m * 60);
  return true;
}

bool settimezone_string_ok(const std::string &tz) {
  int32_t off;
  return tz.size() == 9 && tz.compare(0, 3, "UTC") == 0 && parse_cloud_timezone(tz, off);
}

CivilTime civil_from_epoch(int64_t epoch_s) {
  int64_t days = epoch_s / 86400;
  int64_t rem = epoch_s % 86400;
  if (rem < 0) {
    rem += 86400;
    days--;
  }
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  CivilTime t;
  t.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  t.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  t.year = static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (t.month <= 2));
  t.hour = static_cast<int>(rem / 3600);
  t.minute = static_cast<int>(rem % 3600 / 60);
  t.second = static_cast<int>(rem % 60);
  return t;
}

int64_t epoch_from_civil(const CivilTime &t) {
  return days_from_civil(t.year, static_cast<unsigned>(t.month), static_cast<unsigned>(t.day)) * 86400 +
         t.hour * 3600 + t.minute * 60 + t.second;
}

std::string format_cloud_time(const CivilTime &t) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d", t.year, t.month, t.day, t.hour, t.minute, t.second);
  return buf;
}

bool parse_cloud_time(const std::string &s, CivilTime &out) {
  if (s.size() != 14)
    return false;
  const char *p = s.c_str();
  CivilTime t;
  if (!digits(p, 4, t.year) || !digits(p + 4, 2, t.month) || !digits(p + 6, 2, t.day) || !digits(p + 8, 2, t.hour) ||
      !digits(p + 10, 2, t.minute) || !digits(p + 12, 2, t.second))
    return false;
  if (t.month < 1 || t.month > 12 || t.day < 1 || t.day > 31 || t.hour > 23 || t.minute > 59 || t.second > 59)
    return false;
  // Reject impossible dates (e.g. Feb 30) via a round trip.
  CivilTime back = civil_from_epoch(epoch_from_civil(t));
  if (back.day != t.day || back.month != t.month)
    return false;
  out = t;
  return true;
}

}  // namespace esphome::isolarcloud
