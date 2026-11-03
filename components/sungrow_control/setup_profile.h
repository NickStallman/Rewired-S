#pragma once
// Setup profile resolver: turns (device family context, company / standard option) into the
// ordered register list the stock dongle writes for an AU setup, from the generated run
// dictionary in grid_profiles.gen.h.
// Pure C++, host-tested in tests/control/test_setup_profile.cpp.
//
// Order: 1. the descriptor's country register := country_code;
//        2. unit family with W32B5 (virtualPointFlag 0): 30036 := option code;
//        3. the option's <set> rows in XML order, each run as consecutive registers with the
//           words exactly as writeData (U32 low word first).
// needreadtype=1 options pick the variant whose 3x_5479 ref list contains the live input 5479.
// needparam=0 options would continue through the POWER_GRID_TYPE items (30037 on unit + W32B5);
// no shipped XML has one (tools/gen_grid_profiles.py fails if one appears), so the resolver
// reports GRID_TYPE_UNSUPPORTED for them.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../sungrow_modbus/discovery.h"
#include "grid_profiles.gen.h"

namespace esphome {
namespace sungrow_control {

struct SetupContext {
  sungrow_modbus::XmlFamily family{sungrow_modbus::XmlFamily::NONE};
  const sungrow_modbus::FamilyDescriptor *desc{nullptr};
  bool w32b5{false};   ///< self-description feature W32B5: company / grid type in real registers
  bool w29b12{false};  ///< self-description feature W29B12 (POWER_GRID_TYPE matching only)
  bool have_5479{false};
  uint16_t reg5479{0};  ///< input register 5479 (1/2 PV, 3..6 ES) when have_5479
  uint16_t country_code{6};
};

enum class ResolveStatus : uint8_t {
  OK,
  NO_FAMILY,              ///< family NONE: stock has no XML for it
  NO_DESCRIPTOR,          ///< UNIT_NODESC or no descriptor: stock aborts the set
  UNKNOWN_OPTION,         ///< no option with that code in the family
  NEED_CAPABILITY,        ///< protocol 2/3 device whose capability bitmap (sig 0x2181) is not read yet
  NEED_5479,              ///< the option's variant depends on input 5479; read it first
  NO_VARIANT_FOR_5479,    ///< no variant lists the given 5479 value
  GRID_TYPE_UNSUPPORTED,  ///< needparam=0 (POWER_GRID_TYPE path)
};

enum class ItemKind : uint8_t {
  COUNTRY,    ///< descriptor country register
  COMPANY,    ///< 30036 (unit + W32B5)
  GRID_TYPE,  ///< 30037 (unit + W32B5, needparam=0 only)
  SET,        ///< a <set> register
};

static constexpr uint16_t NO_RUN = 0xFFFF;

struct ResolvedItem {
  uint16_t address;
  uint16_t value;
  ItemKind kind;
  uint16_t run_id;  ///< GP_RUNS index for SET items, NO_RUN otherwise
};

const char *resolve_status_name(ResolveStatus s);

/// The family's options (contiguous in GP_OPTIONS, XML order); nullptr / 0 when it has none.
const GpOption *setup_options_for(sungrow_modbus::XmlFamily family, size_t *n);

/// The family's option with `code`, or nullptr.
const GpOption *find_setup_option(sungrow_modbus::XmlFamily family, uint16_t code);

/// Resolve `option_code` for `ctx` into `items` (cleared first). Only OK yields a complete list;
/// other statuses leave the items built before the failure (country, company, grid type).
ResolveStatus resolve_profile(const SetupContext &ctx, uint16_t option_code, std::vector<ResolvedItem> *items);

}  // namespace sungrow_control
}  // namespace esphome
