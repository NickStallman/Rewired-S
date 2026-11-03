#pragma once
// sungrow_debug core: pure helpers for the debug build (no ESPHome headers, host-tested in
// tests/debug/). Hex formatting, Modbus RTU / MBAP frame summaries, per-row decode lines on top
// of the sungrow_datamodel decoder, trace filters (device / register range / only-changed / rate
// limit) and the slave-address scan planner.
//
// Nothing here talks to hardware. The ESPHome glue (sungrow_debug.h) feeds it bytes it observes
// through public APIs (a Transport wrapper around the sungrow_modbus hub, the hub's ad-hoc read
// queue, the signal store) and logs the strings it returns.
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../sungrow_datamodel/decoder.h"
#include "../sungrow_datamodel/dm_types.h"

namespace esphome {
namespace sungrow_debug {

using sungrow_datamodel::RegisterRow;
using sungrow_datamodel::RegisterTable;
using sungrow_datamodel::WordOrder;

// ---- hex ---------------------------------------------------------------------------------------
/// "01 04 13 87 00 01" (space separated, upper case). At most max_bytes bytes; if the input is
/// longer, " ..(+N)" is appended.
std::string hex(const uint8_t *data, size_t len, size_t max_bytes = 0xFFFF);
/// Compact register dump "0012 FFFF 7FFF" (big-endian words from wire bytes), at most max_regs.
std::string hex_words(const uint8_t *wire, size_t nregs, size_t max_regs = 0xFFFF);
/// Split `len` bytes into lines of `per_line` bytes each ("+000 01 04 ..."); returns the lines.
std::vector<std::string> hex_lines(const uint8_t *data, size_t len, size_t per_line = 32);

// ---- Modbus RTU frames -------------------------------------------------------------------------
const char *fn_name(uint8_t fn);
const char *exception_name(uint8_t code);
/// fn 3 / fn 4 only: the functions the debug build lets onto the bus when writes are blocked.
inline bool is_read_fn(uint8_t fn) { return fn == 0x03 || fn == 0x04; }

struct RtuInfo {
  bool parsed{false};    ///< long enough to read the fields below
  bool crc_ok{false};
  uint8_t addr{0};
  uint8_t fn{0};         ///< without the exception flag
  bool exception{false};
  uint8_t exc_code{0};
  uint16_t start{0};     ///< 1-based table address (wire + 1); requests and fn 6/16 echoes
  uint16_t count{0};     ///< registers (fn 6: 1); read replies: byte_count / 2
  uint16_t value{0};     ///< fn 6 request/echo value
  uint8_t byte_count{0}; ///< read reply byte count
};
/// Parse a request frame we sent (fn 3/4/6/16, anything else: addr + fn only).
RtuInfo parse_rtu_request(const uint8_t *f, size_t len);
/// Parse a reply frame (fn 3/4 read reply, fn 6/16 echo, exception).
RtuInfo parse_rtu_response(const uint8_t *f, size_t len);
/// "addr 1 fn 4 (read input) start 5000 count 1" / "... exception 2 (illegal data address)".
std::string describe_request(const RtuInfo &r);
std::string describe_response(const RtuInfo &r);

// ---- Modbus TCP (MBAP) -------------------------------------------------------------------------
struct MbapInfo {
  bool parsed{false};
  uint16_t tid{0}, pid{0}, len{0};
  uint8_t unit{0};
  uint8_t fc{0};          ///< without the exception flag
  bool exception{false};
  uint8_t exc_code{0};
  uint16_t start{0};      ///< request: 1-based (wire + 1); fn 3/4/6/16
  uint16_t count{0};      ///< request count / reply byte_count / 2
};
MbapInfo parse_mbap(const uint8_t *adu, size_t len, bool is_reply);
std::string describe_mbap(const MbapInfo &m, bool is_reply);

// ---- row decode --------------------------------------------------------------------------------
const char *type_name(uint8_t type);
/// Gate state of a row for the trace: no dependency / dependency bit set / bit clear (the store
/// clears the row instead of storing it).
enum class Gate : uint8_t { NONE, OK, GATED };

struct RowDecode {
  bool present{false};    ///< false = sentinel / absent
  bool kept_sentinel{false};
  bool numeric{false};
  double raw{0};          ///< raw integer (or float) before scaling
  double scaled{0};
  uint64_t bits{0};
  std::string text;       ///< STRING rows: printable text
};
/// Decode one row from its wire registers (wire = the row's first register).
RowDecode decode_row(const RegisterRow &r, const uint8_t *wire, WordOrder wo, bool float_scale = false);
/// One trace line for a row, e.g.
///   "5003+1 fn4 0x2004 daily_yield u16 [0012] raw=18 -> 1.8 kWh"
///   "5017+2 fn4 0x2010 total_dc_power u32 [FFFF FFFF] SENTINEL (absent)"
///   "5200+1 fn4 0x2040 - u16 [0001] raw=1 GATED cap w3 b4 (cleared, not stored)"
/// key/unit: names (nullptr = "-"/none).
std::string format_row(const RegisterRow &r, const uint8_t *wire, WordOrder wo, Gate gate, const char *key,
                       const char *unit, bool float_scale = false);

/// Rows of `t` with row.fn == fn lying fully inside [start, start + nregs) (the rule
/// SignalStore::ingest_block applies). Returns row indices in table order.
std::vector<uint16_t> rows_in_block(const RegisterTable &t, uint8_t fn, uint16_t start, uint16_t nregs);

// ---- filters -----------------------------------------------------------------------------------
struct Range {
  uint32_t lo;
  uint32_t hi;  ///< inclusive
};
/// Parse "1,3-5, 10" into ranges within [min, max]. Empty / "*" / "all" = no ranges (= all).
/// Returns false on a syntax error or a value out of bounds (out untouched).
bool parse_ranges(const std::string &s, uint32_t min, uint32_t max, std::vector<Range> *out);
std::string format_ranges(const std::vector<Range> &r);
bool in_ranges(const std::vector<Range> &r, uint32_t v);
/// [start, start+count) overlaps any range (empty list = true).
bool overlaps_ranges(const std::vector<Range> &r, uint32_t start, uint32_t count);

struct TraceFilter {
  std::vector<Range> addresses;  ///< Modbus slave addresses; empty = all
  std::vector<Range> registers;  ///< 1-based table addresses; empty = all
  bool address_ok(uint8_t addr) const { return this->addresses.empty() || in_ranges(this->addresses, addr); }
  bool block_ok(uint16_t start, uint16_t count) const { return overlaps_ranges(this->registers, start, count); }
  bool row_ok(uint16_t regaddr, uint8_t nreg) const { return overlaps_ranges(this->registers, regaddr, nreg); }
  std::string describe() const;
};

uint32_t fnv1a(const uint8_t *data, size_t len, uint32_t h = 2166136261u);

/// "Only changed values": remembers a hash per key; changed() is true the first time a key is
/// seen and whenever its bytes differ from the last call.
class ChangeTracker {
 public:
  bool changed(uint32_t key, const uint8_t *data, size_t len);
  void clear() { this->h_.clear(); }
  size_t size() const { return this->h_.size(); }

 protected:
  std::unordered_map<uint32_t, uint32_t> h_;
};

/// At most one event per key per interval (0 = unlimited).
class RateLimiter {
 public:
  bool allow(uint32_t key, uint32_t now, uint32_t min_interval_ms);
  void clear() { this->last_.clear(); }

 protected:
  std::unordered_map<uint32_t, uint32_t> last_;
};

// ---- slave address scan ------------------------------------------------------------------------
enum class ProbeResult : uint8_t { OK, EXCEPTION, TIMEOUT, CRC_ERROR, NOT_QUEUED };
const char *probe_result_name(ProbeResult r);

/// One probe at a time over [first, last], `interval_ms` of bus time left to the normal polls
/// between probes. The glue asks next() every loop and reports done() from the read callback.
class ScanPlan {
 public:
  struct Found {
    uint8_t addr;
    ProbeResult result;
    uint8_t exc_code;
    uint16_t value;  ///< first register of an OK reply
  };
  /// Clamped to 1..247 (0 = broadcast is never probed). Returns false if the range is empty.
  bool begin(uint8_t first, uint8_t last, uint32_t interval_ms, uint32_t now);
  void stop() { this->active_ = false; }
  bool active() const { return this->active_; }
  bool waiting() const { return this->waiting_; }
  /// True when the next probe should go out now; *addr = its slave address.
  bool next(uint32_t now, uint8_t *addr);
  void done(uint8_t addr, ProbeResult r, uint8_t exc_code, uint16_t value, uint32_t now);
  const std::vector<Found> &found() const { return this->found_; }
  uint32_t probed() const { return this->probed_; }
  uint32_t total() const { return this->last_ >= this->first_ ? this->last_ - this->first_ + 1u : 0u; }
  uint8_t current() const { return this->cur_; }
  /// "scan 1..247: 247/247 probed, found: 1 (ok 0x0E0F), 3 (exception 2)".
  std::string summary() const;

 protected:
  bool active_{false};
  bool waiting_{false};
  uint8_t first_{1}, last_{247}, cur_{0};
  uint32_t interval_ms_{0};
  uint32_t next_due_{0};
  uint32_t probed_{0};
  std::vector<Found> found_;
};

}  // namespace sungrow_debug
}  // namespace esphome
