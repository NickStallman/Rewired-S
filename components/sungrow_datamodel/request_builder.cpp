#include "request_builder.h"

namespace esphome {
namespace sungrow_datamodel {

/// Register 14701 (0x396d): stock skips it when its group differs from the node's.
static constexpr uint16_t SKIP_ON_GROUP_CHANGE_ADDR = 0x396d;

RequestBuildResult build_requests(const RegisterTable &table, const RequestBuildOptions &opts,
                                  ReadRequest *out, size_t out_cap) {
  RequestBuildResult res;
  size_t cap = out_cap < opts.max_requests ? out_cap : opts.max_requests;
  const uint8_t fns[2] = {3, 4};  // stock order: fn 1, 2, 3, 4 (no fn 1/2 rows in any table)
  for (uint8_t fn : fns) {
    if ((fn == 4 && !opts.include_input) || (fn == 3 && !opts.include_holding))
      continue;
    ReadRequest cur{};
    bool have = false;
    auto flush = [&]() {
      if (!have)
        return;
      if (res.count < cap)
        out[res.count++] = cur;
      else
        res.overflow = true;
      have = false;
    };
    for (uint16_t i = 0; i < table.count; i++) {
      const RegisterRow &r = table.rows[i];
      if (r.fn != fn)
        continue;
      if (opts.group >= 0 && r.group != static_cast<uint16_t>(opts.group))
        continue;
      if (r.nreg == 0 || r.nreg > opts.max_span)
        continue;
      if (opts.filter && !opts.filter(r))
        continue;
      const bool end_node = (r.tag & TAG_END_REQUEST) != 0;
      if (have && !end_node && r.group != cur.group && r.regaddr == SKIP_ON_GROUP_CHANGE_ADDR)
        continue;
      uint16_t span = have ? static_cast<uint16_t>(r.regaddr + r.nreg - cur.start) : 0;
      if (have && span <= opts.max_span) {
        // Stock sets count = this row's span; a shorter row after a longer one would shrink the
        // node, so keep the maximum (identical node boundaries, every row stays covered).
        if (span > cur.count)
          cur.count = span;
        cur.last_row = i;
      } else {
        flush();
        cur.start = r.regaddr;
        cur.count = r.nreg;
        cur.fn = fn;
        cur.group = r.group;
        cur.first_row = i;
        cur.last_row = i;
        have = true;
      }
      if (end_node && i + 1 < table.count && table.rows[i + 1].fn == fn)
        flush();
    }
    flush();
  }
  return res;
}

std::vector<ReadRequest> build_requests(const RegisterTable &table, const RequestBuildOptions &opts,
                                        bool *overflow) {
  std::vector<ReadRequest> v(opts.max_requests);
  RequestBuildResult r = build_requests(table, opts, v.data(), v.size());
  v.resize(r.count);
  if (overflow != nullptr)
    *overflow = r.overflow;
  return v;
}

}  // namespace sungrow_datamodel
}  // namespace esphome
