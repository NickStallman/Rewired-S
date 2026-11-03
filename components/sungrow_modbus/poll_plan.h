#pragma once
// Poll plan: which registers are read how often (docs/update_rates.md).
//
// Every table row gets a refresh tier:
//   FAST   (1 s)   power flow: DC power, grid/meter, house load, battery power, AC output
//   LIVE   (5 s)   values the inverter refreshes as multi-second means (MPPT V/I, battery V/A/SOC)
//                  and the rest of the live measurements: states, faults, energies, backup port
//   SLOW   (30 s)  settings, slow-moving or cloud-only values, battery status/module data
//   STATIC (5 min) identity, versions, ratings, serials, capability bitmap
// The tier comes from kTierRanges (per table, by address) or, for a row no range covers, from
// its stock group key: METER family rows of key 1 are FAST (a standalone meter is the grid
// reading), keys <= 50 (5 s) LIVE, <= 600 (60 s) SLOW, anything longer STATIC.
//
// build_poll_plan() groups the rows of one tier into reads: rows in address order, one fn at a
// time, a read never spans more than max_span registers, never bridges a gap of more than
// max_gap unlisted registers, and ends after a TAG_END_REQUEST row. Rows with sigid 0 (stock
// gap fillers that store nothing) are skipped. Unlike the stock builder (request_builder.h),
// node boundaries follow the tiers, not the table order.
//
// Pure C++, host-testable.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "../sungrow_datamodel/register_tables.h"
#include "../sungrow_datamodel/request_builder.h"

namespace esphome {
namespace sungrow_modbus {

enum class PollTier : uint8_t { FAST = 0, LIVE = 1, SLOW = 2, STATIC = 3 };
static constexpr size_t POLL_TIER_COUNT = 4;
const char *poll_tier_name(PollTier t);

/// Default period of each tier.
static constexpr uint32_t DEFAULT_TIER_PERIOD_MS[POLL_TIER_COUNT] = {1000, 5000, 30000, 300000};

/// One address range of one table that is polled at `tier` (1-based, inclusive). A row belongs
/// to the range when its first register lies inside.
struct TierRange {
  sungrow_datamodel::TableId table;
  uint8_t fn;
  uint16_t first;
  uint16_t last;
  PollTier tier;
};
/// The curated ranges (poll_plan.cpp). Non-overlapping per (table, fn).
const TierRange *tier_ranges(size_t *count);

/// Tier of one row (range first, then the group-key default).
PollTier poll_tier(const sungrow_datamodel::RegisterTable &t, const sungrow_datamodel::RegisterRow &r);

struct PlannedRead {
  sungrow_datamodel::ReadRequest req;
  PollTier tier;
};

struct PollPlanOptions {
  uint16_t max_span{sungrow_datamodel::MAX_REQUEST_SPAN};
  /// Unlisted registers a read may bridge (one register costs ~2 ms of wire time at 9600; a
  /// separate read costs ~37 ms of overhead, about 16 registers).
  uint16_t max_gap{16};
  /// Optional row filter; rejected rows are not polled (e.g. the SBR cell block, read whole).
  std::function<bool(const sungrow_datamodel::RegisterRow &)> filter;
};

/// Reads covering every pollable row (fn 3/4, sigid != 0, 1..max_span registers) of `t`, tier
/// by tier (FAST first), each tier in address order (fn 3 before fn 4).
std::vector<PlannedRead> build_poll_plan(const sungrow_datamodel::RegisterTable &t,
                                         const PollPlanOptions &opts = PollPlanOptions());

}  // namespace sungrow_modbus
}  // namespace esphome
