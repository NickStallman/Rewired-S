#include "discovery_meta.h"

#include <cstring>

namespace esphome {
namespace sungrow_mqtt {

using sungrow_datamodel::Family;

const char *family_name(Family f) {
  switch (f) {
    case Family::INVERTER: return "inverter";
    case Family::BATTERY: return "battery";
    case Family::CHARGER: return "charger";
    case Family::ENERGY_BRIDGE: return "energy_bridge";
    case Family::METER: return "meter";
    case Family::LOGGER: return "logger";
  }
  return "inverter";
}

const char *enum_lookup(const EnumMap *map, uint16_t raw) {
  if (map == nullptr)
    return nullptr;
  for (uint16_t i = 0; i < map->count; i++)
    if (map->entries[i].raw == raw)
      return map->entries[i].text;
  return nullptr;
}

const SensorMeta *lookup_meta(Family family, uint16_t sigid) {
  const char *want = family_name(family);
  for (const FamilyMeta &fam : HA_FAMILIES) {
    if (std::strcmp(fam.family, want) != 0)
      continue;
    // Tables are small per family; a linear scan is fine and avoids a runtime index.
    for (uint16_t i = 0; i < fam.count; i++) {
      if (fam.rows[i].sigid == sigid)
        return &fam.rows[i];
    }
    return nullptr;
  }
  return nullptr;
}

// Mirrors winet-extractor src/types/HaTypes.ts normaliseUnit(). register_meta already emits the
// normalised forms (var/VA with the x1000 multiplier baked in, °C not ℃), so this only ever fires
// if a raw unit is fed in directly; it keeps the two code paths in sync.
const char *normalise_unit(const char *unit, float *multiplier) {
  *multiplier = 1.0f;
  if (unit == nullptr)
    return "";
  if (std::strcmp(unit, "kWp") == 0)
    return "kW";
  if (std::strcmp(unit, "\xe2\x84\x83") == 0)  // ℃ (U+2103)
    return "\xc2\xb0""C";                       // °C
  if (std::strcmp(unit, "kvar") == 0) {
    *multiplier = 1000.0f;
    return "var";
  }
  if (std::strcmp(unit, "kVA") == 0) {
    *multiplier = 1000.0f;
    return "VA";
  }
  return unit;
}

// winet DeviceClasses table (keyed by the normalised unit).
const char *device_class_for_unit(const char *unit) {
  if (unit == nullptr || unit[0] == '\0')
    return "";
  struct Pair { const char *unit, *cls; };
  static const Pair kPairs[] = {
      {"W", "power"},       {"kW", "power"},     {"V", "voltage"},
      {"A", "current"},     {"kWh", "energy"},   {"Wh", "energy"},
      {"\xc2\xb0""C", "temperature"},            {"var", "reactive_power"},
      {"VA", "apparent_power"}, {"Hz", "frequency"}, {"h", "duration"},
  };
  for (const Pair &p : kPairs)
    if (std::strcmp(p.unit, unit) == 0)
      return p.cls;
  return "";
}

// winet StateClasses table (keyed by the normalised unit).
const char *state_class_for_unit(const char *unit) {
  if (unit == nullptr || unit[0] == '\0')
    return "";
  struct Pair { const char *unit, *cls; };
  static const Pair kPairs[] = {
      {"W", "measurement"},  {"kW", "measurement"}, {"V", "measurement"},
      {"A", "measurement"},  {"\xc2\xb0""C", "measurement"},
      {"var", "measurement"}, {"VA", "measurement"}, {"Hz", "measurement"},
      {"%", "measurement"},  {"k\xce\xa9", "measurement"},  // kΩ
      {"kWh", "total_increasing"}, {"Wh", "total_increasing"}, {"h", "total_increasing"},
  };
  for (const Pair &p : kPairs)
    if (std::strcmp(p.unit, unit) == 0)
      return p.cls;
  return "";
}

}  // namespace sungrow_mqtt
}  // namespace esphome
