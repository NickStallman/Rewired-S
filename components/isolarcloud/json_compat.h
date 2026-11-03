#pragma once
// ArduinoJson with the same configuration macros as ESPHome's json component, so host tests and
// the firmware see an identical ArduinoJson (ODR-safe).
#ifdef ISOLARCLOUD_HOST
#define ARDUINOJSON_ENABLE_STD_STRING 1  // NOLINT
#define ARDUINOJSON_USE_LONG_LONG 1      // NOLINT
#include <ArduinoJson.h>
#else
#include "esphome/components/json/json_util.h"
#endif

#include <atomic>
#include <cstdint>
#include <string>

namespace esphome::isolarcloud {

/// Documents that hit an allocation failure while being built (ArduinoJson then drops values and
/// sets overflowed()). Never sent; isolarcloud logs when this count moves.
inline std::atomic<uint32_t> &json_overflow_count() {
  static std::atomic<uint32_t> n{0};
  return n;
}

/// Serialize `doc` into `out` unless it overflowed (a truncated payload must never go out).
inline bool serialize_checked(const JsonDocument &doc, std::string &out) {
  out.clear();
  if (doc.overflowed()) {
    json_overflow_count().fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  serializeJson(doc, out);
  return true;
}

/// Allocator for the large telemetry documents: PSRAM first, internal RAM as the fallback, when the
/// firmware has PSRAM; the default heap otherwise (and on the host).
inline ArduinoJson::Allocator *large_json_allocator() {
#if !defined(ISOLARCLOUD_HOST) && defined(USE_PSRAM)
  static json::SpiRamAllocator alloc;
  return &alloc;
#else
  return ArduinoJson::detail::DefaultAllocator::instance();
#endif
}

}  // namespace esphome::isolarcloud
