#pragma once
// Discovery metadata for the per-device MQTT publisher.
//
// One SensorMeta row per decoded signal, grouped by device family. The table is BAKED at
// codegen from components/sungrow_datamodel/register_meta.py (ha_meta_gen.py writes
// ha_meta_gen.h, included below). The publisher reads SignalStore *values* and this table's
// *presentation* (name/unit/classes/slug) side by side, so connected-device data stays in our
// own optimised structures instead of hundreds of hidden ESPHome entity objects.
//
// `slug` is slugify(register_meta name); everything downstream (topics, unique_id) keys off it.

#include <cstdint>

#include "esphome/components/sungrow_datamodel/dm_types.h"  // sungrow_datamodel::Family

namespace esphome {
namespace sungrow_mqtt {

/// One raw->label pair of a read-side enum (operating status, output type, ...).
struct EnumEntry {
  uint16_t raw;
  const char *text;
};
/// A named read-side enum map (baked from register_meta ENUMS, or the built-in operating-status map).
struct EnumMap {
  const EnumEntry *entries;
  uint16_t count;
};

/// One publishable signal: SignalStore value + HA presentation. All strings are static (flash).
struct SensorMeta {
  uint16_t sigid;           ///< SignalStore sigid (matches register_meta)
  const char *name;         ///< human label (register_meta name)
  const char *slug;         ///< slugify(label): sensor_slug, pinned (see `slug` above)
  const char *unit;         ///< HA-normalised unit ("" = none); multiplier already applied below
  float multiplier;         ///< value multiplier for MQTT (register_meta already HA-normalised)
  const char *device_class; ///< "" = none or derive from unit (device_class_for_unit)
  const char *state_class;  ///< "" = none or derive from unit (state_class_for_unit)
  int8_t precision;         ///< suggested_display_precision; -1 = none
  float deadband;           ///< min |change| (published unit) before a new state is sent; 0 = every change (deadband.py)
  bool is_string;           ///< text signal (value_template {{ value_json.value }})
  const EnumMap *enum_map;  ///< non-null: publish the DECODED TEXT for this raw value (enum read)
  bool diagnostic;          ///< group==static -> HA entity_category "diagnostic"
};

/// Raw -> label of an enum map; nullptr when the code is not in the map.
const char *enum_lookup(const EnumMap *map, uint16_t raw);

/// A family's signal table (inverter / battery / charger / energy_bridge / meter / logger).
struct FamilyMeta {
  const char *family;
  const SensorMeta *rows;
  uint16_t count;
};

// ---- helpers (ha_meta.cpp) ----------------------------------------------------------------

/// register_meta family string for a datamodel Family enum ("inverter", ...).
const char *family_name(sungrow_datamodel::Family f);
/// Metadata for (family, sigid), or nullptr if the sigid is unnamed in that family.
const SensorMeta *lookup_meta(sungrow_datamodel::Family family, uint16_t sigid);

/// winet-extractor HaTypes.ts normalisation (defensive: register_meta is already normalised, but
/// a raw unit slipping through still maps correctly). Returns the HA unit and sets *multiplier.
const char *normalise_unit(const char *unit, float *multiplier);
/// winet DeviceClasses[unit] fallback when the row has no explicit device_class ("" = none).
const char *device_class_for_unit(const char *unit);
/// winet StateClasses[unit] fallback when the row has no explicit state_class ("" = none).
const char *state_class_for_unit(const char *unit);

}  // namespace sungrow_mqtt
}  // namespace esphome

// Baked table: static const FamilyMeta HA_FAMILIES[HA_FAMILY_COUNT]. Generated into the component
// dir by ha_meta_gen.py at config load; included here after the struct definitions above.
#include "ha_meta_gen.h"
