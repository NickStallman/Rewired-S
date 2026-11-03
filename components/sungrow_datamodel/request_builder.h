#pragma once
// Request-list builder: the stock dongle's request grouping.
// Pure table logic, host-buildable.
//
// Rules (the stock request grouping, reference implementation in
// tests/fixtures/stock_request_nodes.py): one pass per fn (3, then 4; fn 16/119 rows are never cyclic).
// Within a pass the table is walked in order; rows of other fns are skipped without breaking a
// node. A row joins the current node while the u16 span (regaddr + nreg - node start) stays
// <= 120, so a lower address wraps and starts a new node. A different group key does NOT split:
// the node keeps the group of its first row. Exception: regaddr 14701 (0x396d) with a group
// differing from the node's is skipped entirely. Tag bit0 (TAG_END_REQUEST) closes the node
// after the row, but only when the next table row has the same fn. At most 59 nodes per
// device (stock aborts beyond that; we truncate and report overflow).
// Node counts match stock: inv_proto2 54, inv_proto3 53, inv_proto1_4 18.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "dm_types.h"

namespace esphome {
namespace sungrow_datamodel {

static constexpr uint16_t MAX_REQUEST_SPAN = 120;  // "< 0x79"
static constexpr uint16_t MAX_REQUESTS_PER_DEVICE = 59;  // 0x3b

struct ReadRequest {
  uint16_t start;      ///< 1-based table address of the first register
  uint16_t count;      ///< registers to read (<= max_span)
  uint8_t fn;          ///< 3 (holding) or 4 (input)
  uint16_t group;      ///< group key of the first row (poll period = group x 100 ms)
  uint16_t first_row;  ///< index of the first table row covered
  uint16_t last_row;   ///< index of the last table row merged
  /// Start address to put in the Modbus frame.
  uint16_t wire_start() const { return static_cast<uint16_t>(this->start - 1); }
};

struct RequestBuildOptions {
  int32_t group{-1};            ///< only rows with this group key; -1 = all groups
  bool include_input{true};     ///< fn 4
  bool include_holding{true};   ///< fn 3
  uint16_t max_span{MAX_REQUEST_SPAN};
  uint16_t max_requests{MAX_REQUESTS_PER_DEVICE};
  /// Optional per-row filter (e.g. "rows outside the SBR cell block"). Rejected rows are
  /// skipped like rows of another fn: they neither start, extend nor break a request.
  std::function<bool(const RegisterRow &)> filter;
};

struct RequestBuildResult {
  size_t count{0};      ///< requests written
  bool overflow{false}; ///< more requests were needed than max_requests / out_cap
};

/// Build into a caller-provided array (no allocation).
RequestBuildResult build_requests(const RegisterTable &table, const RequestBuildOptions &opts,
                                  ReadRequest *out, size_t out_cap);
/// Convenience wrapper.
std::vector<ReadRequest> build_requests(const RegisterTable &table,
                                        const RequestBuildOptions &opts = RequestBuildOptions(),
                                        bool *overflow = nullptr);

}  // namespace sungrow_datamodel
}  // namespace esphome
