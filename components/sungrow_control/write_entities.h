#pragma once
// Presentation metadata for the WRITABLE (mkaiser) register subset, used by the sungrow_mqtt
// publisher to emit Home Assistant number / select / switch discovery and to convert incoming
// command payloads to a raw register value.
//
// The allow-list + raw ranges + read-back sigids are the authoritative, GENERATED data in
// write_registers.{h,cpp} (and the gate in control_core). This table only adds the HA-facing
// presentation that is not carried in C++ by the generator (display name, entity kind, unit,
// display scale/step/range and option texts). It is hand-kept in lock-step with
// data/write_registers.yaml / write_meta.py; the raw ranges here are advisory for command
// conversion only — every write is still re-checked against the generated allow-list by
// ControlCore::check(), so a drift here can never widen what may be written.
#include <cstdint>

#include "../sungrow_datamodel/dm_types.h"
#include "write_registers.h"  // Profile

namespace esphome {
namespace sungrow_control {

enum class WriteKind : uint8_t { NUMBER, SELECT, SWITCH };

struct WriteOptionText {
  uint16_t raw;
  const char *text;
};

/// One writable HA control of a profile. `options` is writable-only for SELECT (codes the user may
/// actually set); for SWITCH it is exactly {on, off} in that order. NUMBER has no options.
struct WriteEntity {
  const char *key;      ///< stable id == command/state topic slug and read-back key
  const char *name;     ///< HA entity name
  uint16_t address;     ///< 1-based (document) holding register
  uint16_t sigid;       ///< read-back signal in the SignalStore (0xF1xx)
  uint8_t type;         ///< sungrow_datamodel TypeCode (all current rows are TYPE_U16)
  WriteKind kind;
  float scale;          ///< display = raw x scale (0 is never used here)
  const char *unit;     ///< "" = none
  float step;           ///< number entity step (display units)
  float number_min;     ///< number entity range, display units
  float number_max;
  int32_t raw_min;      ///< raw range (advisory; the generated allow-list is authoritative)
  int32_t raw_max;
  const WriteOptionText *options;
  uint8_t noptions;
};

/// Writable entities of a profile (nullptr + *count 0 for Profile::NONE / unknown).
const WriteEntity *writable_entities(Profile p, uint8_t *count);

}  // namespace sungrow_control
}  // namespace esphome
