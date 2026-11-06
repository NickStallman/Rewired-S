#pragma once
// SignalStore: the single source of truth for decoded device data.
//
//   (device_index, sigid) -> Signal { value bytes, len, type, scale, valid, updated_ms }
//
// Host-buildable (no ESPHome headers). The ESPHome hub (sungrow_datamodel.h) owns one.
//
// Flow
// ---------------------------------------------------------------------------------------
//   1. The Modbus master picks the table (inverter_table_for_protocol(proto), get_table(),
//      find_table()) and reads it in the blocks of its poll plan (sungrow_modbus/poll_plan.h).
//   2. Each response goes to ingest_block(dev, table, fn, start, payload, nregs, word_order):
//      payload = the response data bytes (2*nregs, big-endian registers). The decoder
//      (decoder.h) decodes every row of the table fully inside the block, clears sentinel
//      values (unless TAG_KEEP_SENTINEL), gates capability-dependent rows (optional), expands
//      bitfields and refreshes derived signals (MPPT powers, total yield, import/export, SBR
//      cells).
//   3. On a timeout or a Modbus exception the stored values stay as they are (matches the
//      stock dongle).
//   4. Device offline: clear_offline(dev, table) clears only the TAG_CLEAR_OFFLINE rows.
//      Device removed: clear_device(dev). invalidate_block() clears one block explicitly.
//
// Consumers (entities, cloud publisher, Modbus-TCP slave) read with get()/get_number()/
// get_string()/get_bits() and listen via add_listener()/subscribe().
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "decoder.h"
#include "dm_types.h"
#include "register_tables.h"
#include "request_builder.h"

namespace esphome {
namespace sungrow_datamodel {

/// Capability bitmap signal (self-describe, 60 registers) used for dep_word/dep_bit gating.
static constexpr uint16_t SIG_CAPABILITY_BITMAP = 0x2181;

struct Signal {
  uint16_t sigid{0};
  uint8_t device{0};
  uint8_t type{0};     ///< TypeCode of the row that last wrote it
  uint8_t len{0};      ///< bytes of value: SCALAR 2/4/8 host-LE, STRING/ARRAY raw wire, F32 4
  bool valid{false};   ///< false = absent / sentinel / cleared
  ValueKind kind{ValueKind::SCALAR};
  float scale{0.0f};   ///< row scale (0 = none), applied by number()
  uint32_t updated_ms{0};

  const uint8_t *data() const { return ext_.empty() ? inline_ : ext_.data(); }
  /// Integer bits of a SCALAR (zero-extended).
  uint64_t bits() const;
  /// Scaled numeric value; NaN if invalid or not numeric.
  double number() const;
  /// String value (STRING: up to first NUL, trailing spaces trimmed); "" if invalid.
  std::string str() const;

  // internal
  bool assign(const uint8_t *src, uint8_t n);  // returns true if bytes changed
  uint8_t inline_[8]{};
  std::vector<uint8_t> ext_;
};

/// Battery type of an inverter (enum signal; 2 = "No battery").
constexpr uint16_t SIG_BATTERY_TYPE = 0x2510;
constexpr uint32_t BATTERY_TYPE_NONE = 2;

class SignalStore {
 public:
  using Listener = std::function<void(const Signal &)>;
  using Clock = std::function<uint32_t()>;

  using RowFilter = std::function<bool(const RegisterTable &, const RegisterRow &)>;

  void set_clock(Clock clock) { this->clock_ = std::move(clock); }
  /// Optional RAM saver: ingest_block() only stores rows accepted by this filter (e.g. named
  /// rows + the sigids the cloud publisher needs). Default: store every row.
  void set_row_filter(RowFilter f) { this->row_filter_ = std::move(f); }
  /// Apply the point-table scale on top of type-9 float32 values (meters). Default false: the
  /// float is in engineering units and is stored with scale 0, as stock shows it unscaled and
  /// uses the scale only for the display decimals (the decimals come from the generated entity
  /// metadata). true = the meter sends floats in point-table units.
  void set_float_scale(bool on) { this->float_scale_ = on; }
  bool float_scale() const { return this->float_scale_; }
  /// Store bitfield children (kBitfields: child_base + bit = u16 0/1) like stock's DM does.
  /// Off by default: ~480 extra signals for inv_proto5; entities bind to parent + bit instead.
  void set_expand_bitfields(bool on) { this->expand_bitfields_ = on; }
  uint32_t now() const { return this->clock_ ? this->clock_() : 0; }

  // ---- writers --------------------------------------------------------------------------
  /// Store a decoded value. For SCALAR, `bytes` are host little-endian (len 2/4/8); for
  /// STRING/ARRAY raw wire bytes. Returns true if the value or validity changed (listeners
  /// fire only then).
  bool set(uint8_t device, uint16_t sigid, uint8_t type, float scale, ValueKind kind,
           const uint8_t *bytes, uint8_t len);
  bool set_scalar(uint8_t device, uint16_t sigid, uint8_t type, float scale, uint64_t bits,
                  uint8_t nbytes);
  bool set_float(uint8_t device, uint16_t sigid, float value);
  /// Mark absent (keeps the entry so consumers can tell "seen but invalid").
  bool clear(uint8_t device, uint16_t sigid);
  void clear_device(uint8_t device);

  /// Decode every row of `table` with row.fn == fn lying fully inside
  /// [start_reg, start_reg + nregs) (1-based table addresses) from `data` (2*nregs bytes,
  /// big-endian registers as received). Rows with sigid 0 are skipped. With
  /// gate_capabilities, rows whose dep_word/dep_bit bit is clear in this device's sig 0x2181
  /// are cleared instead (no bitmap stored -> every dependent row is absent, as stock).
  /// A sentinel clears the signal unless the row has TAG_KEEP_SENTINEL. Type-9 rows are
  /// stored as FLOAT. Runs derived hooks afterwards.
  /// Returns the number of rows decoded.
  size_t ingest_block(uint8_t device, const RegisterTable &table, uint8_t fn, uint16_t start_reg,
                      const uint8_t *data, uint16_t nregs,
                      WordOrder word_order = WordOrder::LOW_WORD_FIRST,
                      bool gate_capabilities = false);
  /// Clear every row inside the block.
  size_t invalidate_block(uint8_t device, const RegisterTable &table, uint8_t fn,
                          uint16_t start_reg, uint16_t nregs);

  /// Device went offline (stock OfflineFreshData): clear the rows of `table` that carry
  /// TAG_CLEAR_OFFLINE (and their bitfield children), keep every other value, then refresh
  /// derived signals. Returns the number of rows cleared.
  size_t clear_offline(uint8_t device, const RegisterTable &table);

  // ---- readers --------------------------------------------------------------------------
  const Signal *get(uint8_t device, uint16_t sigid) const;
  bool is_valid(uint8_t device, uint16_t sigid) const {
    const Signal *s = this->get(device, sigid);
    return s != nullptr && s->valid;
  }
  /// Scaled value or NaN.
  double get_number(uint8_t device, uint16_t sigid) const;
  /// Raw integer bits of a valid SCALAR signal.
  bool get_bits(uint8_t device, uint16_t sigid, uint64_t *out) const;
  std::string get_string(uint8_t device, uint16_t sigid) const;
  /// Capability check against this device's stored sig 0x2181. No dependency -> true; no
  /// bitmap stored -> false (as stock: a missing bitmap has every bit clear).
  bool capability_ok(uint8_t device, uint8_t dep_word, uint8_t dep_bit) const;
  size_t size() const { return this->signals_.size(); }
  /// Iterate all signals (e.g. cloud rundata publisher).
  void for_each(const std::function<void(const Signal &)> &fn) const;

  // ---- change notification ------------------------------------------------------------
  /// Called for every changed signal (any device/sigid).
  /// Stock dev_type of a device (35 = hybrid ...): selects the per-type derived hooks.
  void set_dev_type(uint8_t device, uint8_t dev_type) { this->dev_types_[device] = dev_type; }
  uint8_t dev_type(uint8_t device) const {
    auto it = this->dev_types_.find(device);
    return it == this->dev_types_.end() ? 0 : it->second;
  }
  void add_listener(Listener cb) { this->listeners_.push_back(std::move(cb)); }
  /// Called when (device, sigid) changes.
  void subscribe(uint8_t device, uint16_t sigid, Listener cb) {
    this->subs_[key_(device, sigid)].push_back(std::move(cb));
  }

 protected:
  static uint32_t key_(uint8_t device, uint16_t sigid) {
    return (static_cast<uint32_t>(device) << 16) | sigid;
  }
  Signal &entry_(uint8_t device, uint16_t sigid);
  void notify_(const Signal &s);
  void run_derived_(uint8_t device, const RegisterTable &table, uint8_t fn, uint16_t start,
                    uint16_t end);
  void expand_bitfield_(uint8_t device, const RegisterTable &table, const RegisterRow &r, uint64_t bits);
  void clear_bitfield_(uint8_t device, const RegisterTable &table, const RegisterRow &r);

  std::unordered_map<uint32_t, Signal> signals_;
  std::unordered_map<uint32_t, std::vector<Listener>> subs_;
  std::vector<Listener> listeners_;
  std::unordered_map<uint8_t, uint8_t> dev_types_;
  Clock clock_;
  RowFilter row_filter_;
  bool float_scale_{false};
  bool expand_bitfields_{false};
};

/// False only when the device's battery type reads "No battery"; an unknown or invalid battery
/// type counts as present (nothing is hidden on missing data).
inline bool has_battery(const SignalStore &store, uint8_t device) {
  const Signal *b = store.get(device, SIG_BATTERY_TYPE);
  return b == nullptr || !b->valid || b->bits() != BATTERY_TYPE_NONE;
}

}  // namespace sungrow_datamodel
}  // namespace esphome
