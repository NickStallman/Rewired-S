#include "poll_plan.h"

#include <algorithm>

namespace esphome {
namespace sungrow_modbus {

using sungrow_datamodel::Family;
using sungrow_datamodel::ReadRequest;
using sungrow_datamodel::RegisterRow;
using sungrow_datamodel::RegisterTable;
using sungrow_datamodel::TableId;
using sungrow_datamodel::TAG_END_REQUEST;

const char *poll_tier_name(PollTier t) {
  switch (t) {
    case PollTier::FAST:
      return "fast";
    case PollTier::LIVE:
      return "live";
    case PollTier::SLOW:
      return "slow";
    case PollTier::STATIC:
      return "static";
  }
  return "?";
}

// Evidence: docs/update_rates.md §7 (what the inverter refreshes how often, which values a
// consumer needs fast) and live snapshots of the SH10RS + SBR stacks and the SG5.0RS (which
// values move at all). Addresses are 1-based table addresses.
//
// Private map (inv_proto2 = protocol 2, inv_proto3 = protocol 3; same layout). Rows no range
// covers keep their group default: key 1 -> LIVE (states and faults 5448..5519, energies and
// battery 5609..5681 and 13041, backup port + meter detail 5719..5799, power flow 6099), key 600
// settings -> SLOW, hourly -> STATIC.
#define PRIVATE_MAP_RANGES(T)                                                                        \
  /* battery/export/load power, state, yields, AC/DC power, PF, Hz, bus V, temp, AC V/I */           \
  {T, 4, 5214, 5252, PollTier::FAST},                                                               \
  /* strings 1..40: 30 s where served (no values on residential units) */                        \
  {T, 4, 5253, 5332, PollTier::SLOW},                                                               \
  /* MPPT 1..4 voltage/current: the inverter refreshes these as 4.9 s means */                     \
  {T, 4, 5333, 5340, PollTier::LIVE},                                                               \
  /* MPPT 5..20: 30 s where served */                                                              \
  {T, 4, 5341, 5372, PollTier::SLOW},                                                               \
  /* meter power (total + phases); 5609.. energies and battery V/I/T/SOC/SOH are LIVE */          \
  {T, 4, 5601, 5608, PollTier::FAST},                                                               \
  /* versions, model/build strings */                                                              \
  {T, 4, 4950, 5044, PollTier::STATIC},                                                             \
  /* rated powers, output type, serial, model strings */                                           \
  {T, 4, 5374, 5447, PollTier::STATIC},                                                             \
  /* capability bitmap 0x2181 */                                                                   \
  {T, 4, 5520, 5579, PollTier::STATIC},                                                             \
  /* certification version + component version strings */                                         \
  {T, 4, 5800, 5967, PollTier::STATIC},                                                             \
  /* AFCI self-test, fans, status words, 6051.. block (6099 power flow stays LIVE) */              \
  {T, 4, 5968, 6098, PollTier::SLOW},                                                               \
  {T, 4, 6200, 6203, PollTier::SLOW},                                                               \
  {T, 4, 6303, 6501, PollTier::SLOW},                                                               \
  {T, 4, 6950, 6951, PollTier::SLOW},                                                               \
  /* per-cluster arrays (constant) */                                                              \
  {T, 4, 7100, 7815, PollTier::STATIC},                                                             \
  /* cloud-only source block 7816..7831, misc status, ground voltages */                           \
  {T, 4, 7816, 8215, PollTier::SLOW},                                                               \
  /* firmware version strings */                                                                   \
  {T, 4, 8801, 8920, PollTier::STATIC},                                                             \
  /* battery address list (child discovery), 10752.. status */                                    \
  {T, 4, 10451, 10458, PollTier::SLOW},                                                             \
  {T, 4, 10752, 10756, PollTier::SLOW},                                                             \
  /* holding rows of key 1 (settings) */                                                           \
  {T, 3, 31225, 31225, PollTier::SLOW},                                                             \
  {T, 3, 31452, 31461, PollTier::SLOW}

static constexpr TierRange kTierRanges[] = {
    PRIVATE_MAP_RANGES(TableId::INV_PROTO2),
    PRIVATE_MAP_RANGES(TableId::INV_PROTO3),
    // Public map, protocol 1/4 (SG string inverters).
    {TableId::INV_PROTO1_4, 4, 5003, 5038, PollTier::FAST},  // yields, MPPT 1..3, DC/AC power, phases, Hz, state
    {TableId::INV_PROTO1_4, 4, 5083, 5092, PollTier::FAST},  // meter power (total + phases), load power
    {TableId::INV_PROTO1_4, 4, 4950, 5002, PollTier::STATIC},  // versions, serial, type, rating, output type
    {TableId::INV_PROTO1_4, 4, 7816, 7854, PollTier::SLOW},
    // Public map, protocol 5 (SH hybrids on the public map).
    {TableId::INV_PROTO5, 4, 5003, 5038, PollTier::FAST},    // energies, MPPT 1/2, DC power, phase voltages, Hz
    {TableId::INV_PROTO5, 4, 13000, 13034, PollTier::FAST},  // state, power flow, load/export, battery, AC power
    {TableId::INV_PROTO5, 4, 5000, 5002, PollTier::STATIC},
    {TableId::INV_PROTO5, 4, 7816, 7854, PollTier::SLOW},
    {TableId::INV_PROTO5, 3, 13055, 13153, PollTier::SLOW},
    // SBR stack (served by the inverter from BCU CAN frames: 1 s for most, 10 s for some fields;
    // docs/battery_sbr.md). The cell block 10757..10780 is a separate whole-block job.
    {TableId::BATTERY, 4, 10740, 10745, PollTier::FAST},     // operation status, V, I, T, SOC, SOH
    {TableId::BATTERY, 4, 5448, 5472, PollTier::SLOW},       // fault list
    {TableId::BATTERY, 4, 10628, 10699, PollTier::SLOW},     // status, flags, module SOC, balancing, cycles
    {TableId::BATTERY, 4, 10700, 10708, PollTier::STATIC},
    {TableId::BATTERY, 4, 10709, 10710, PollTier::SLOW},     // stack online
    {TableId::BATTERY, 4, 10711, 10739, PollTier::STATIC},   // serial, firmware, protocol
    {TableId::BATTERY, 4, 10746, 10749, PollTier::SLOW},     // lifetime charge/discharge energy
    {TableId::BATTERY, 4, 10750, 10751, PollTier::STATIC},   // inverter address / model class
    {TableId::BATTERY, 4, 10781, 10788, PollTier::STATIC},   // module cell types
    {TableId::BATTERY, 4, 10789, 10790, PollTier::SLOW},     // DC contactor, faulted modules
    {TableId::BATTERY, 4, 10791, 10893, PollTier::STATIC},   // module dates, pack serials
    {TableId::BATTERY, 4, 19939, 19940, PollTier::STATIC},   // discharge cut-off SOC (BCU-tunnelled read)
};
#undef PRIVATE_MAP_RANGES

const TierRange *tier_ranges(size_t *count) {
  *count = sizeof(kTierRanges) / sizeof(kTierRanges[0]);
  return kTierRanges;
}

PollTier poll_tier(const RegisterTable &t, const RegisterRow &r) {
  for (const auto &x : kTierRanges)
    if (static_cast<uint8_t>(x.table) == t.id && x.fn == r.fn && r.regaddr >= x.first && r.regaddr <= x.last)
      return x.tier;
  if (t.family == Family::METER && r.group <= 1)
    return PollTier::FAST;
  if (r.group <= 50)
    return PollTier::LIVE;
  if (r.group <= 600)
    return PollTier::SLOW;
  return PollTier::STATIC;
}

std::vector<PlannedRead> build_poll_plan(const RegisterTable &t, const PollPlanOptions &opts) {
  // Pollable rows with their tier, sorted by (tier, fn 3 before 4, address, row index).
  struct Item {
    uint16_t row;
    PollTier tier;
  };
  std::vector<Item> items;
  items.reserve(t.count);
  for (uint16_t i = 0; i < t.count; i++) {
    const RegisterRow &r = t.rows[i];
    if (r.sigid == 0 || (r.fn != 3 && r.fn != 4) || r.nreg == 0 || r.nreg > opts.max_span)
      continue;
    if (opts.filter && !opts.filter(r))
      continue;
    items.push_back({i, poll_tier(t, r)});
  }
  std::stable_sort(items.begin(), items.end(), [&](const Item &a, const Item &b) {
    const RegisterRow &ra = t.rows[a.row], &rb = t.rows[b.row];
    if (a.tier != b.tier)
      return a.tier < b.tier;
    if (ra.fn != rb.fn)
      return ra.fn < rb.fn;
    return ra.regaddr < rb.regaddr;
  });
  std::vector<PlannedRead> out;
  bool have = false;
  bool closed = false;  // the previous row ends its read (TAG_END_REQUEST)
  PlannedRead cur{};
  uint32_t cur_end = 0;  // one past the last register covered
  for (const Item &it : items) {
    const RegisterRow &r = t.rows[it.row];
    const uint32_t r_end = static_cast<uint32_t>(r.regaddr) + r.nreg;
    const bool join = have && !closed && it.tier == cur.tier && r.fn == cur.req.fn &&
                      r_end - cur.req.start <= opts.max_span &&
                      (r.regaddr <= cur_end || r.regaddr - cur_end <= opts.max_gap);
    if (join) {
      if (r_end > cur_end)
        cur_end = r_end;
      cur.req.count = static_cast<uint16_t>(cur_end - cur.req.start);
      if (r.regaddr >= t.rows[cur.req.last_row].regaddr)
        cur.req.last_row = it.row;
    } else {
      if (have)
        out.push_back(cur);
      cur = PlannedRead{};
      cur.tier = it.tier;
      cur.req.start = r.regaddr;
      cur.req.count = r.nreg;
      cur.req.fn = r.fn;
      cur.req.group = r.group;
      cur.req.first_row = it.row;
      cur.req.last_row = it.row;
      cur_end = r_end;
      have = true;
    }
    closed = (r.tag & TAG_END_REQUEST) != 0;
  }
  if (have)
    out.push_back(cur);
  return out;
}

}  // namespace sungrow_modbus
}  // namespace esphome
