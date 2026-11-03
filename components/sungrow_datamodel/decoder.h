#pragma once
// Register decoder: wire bytes (big-endian registers) -> host values, per RTU type code and
// word-order mode, with the stock dongle's store and sentinel rules. Host-buildable (no ESPHome
// headers).
#include <cstddef>
#include <cstdint>

#include "dm_types.h"

namespace esphome {
namespace sungrow_datamodel {

/// How a row's value is held.
enum class ValueKind : uint8_t {
  SCALAR,  ///< integer of 2/4/8 bytes, kept as host little-endian bits
  STRING,  ///< type 12: raw wire bytes
  ARRAY,   ///< numeric type whose nreg exceeds its natural width (curves, tables): raw wire bytes
  FLOAT,   ///< TYPE_F32 (derived values) or TYPE_F32_WIRE (meter float32): 4-byte host float
};

/// Natural register width of a type code (1, 2 or 4); 0 for strings / unknown.
uint8_t type_width(uint8_t type);
bool type_is_signed(uint8_t type);
/// Kind of value a (type, nreg) pair decodes to.
ValueKind value_kind(uint8_t type, uint8_t nreg);
/// Width in registers actually used for a SCALAR (min(nreg, natural width)).
uint8_t scalar_regs(uint8_t type, uint8_t nreg);

/// Reassemble `nreg` (1, 2 or 4) registers into an unsigned integer per word order.
/// LOW_WORD_FIRST with 4 registers is stock's (r1<<16|r0)<<32 | (r3<<16|r2).
uint64_t assemble(const uint8_t *wire, uint8_t nreg, WordOrder wo);
/// Inverse of assemble(): write `nreg` registers of `bits` to wire (for the Modbus-TCP slave).
void disassemble(uint64_t bits, uint8_t nreg, WordOrder wo, uint8_t *wire);

/// Stock "no data" sentinels: u16 0xFFFF, s16 0x7FFF, u32 0xFFFFFFFF, s32 0x7FFFFFFF,
/// u64 all-ones, s64 0x7FFF...F. `nreg` gives the actual width (1/2/4 registers); a width
/// below the type's natural width (u32 x1) is never a sentinel. Floats have none.
bool is_sentinel(uint64_t bits, uint8_t type, uint8_t nreg);
/// Sign- or zero-extend `bits` of width nreg*16 per type, as double.
double bits_to_double(uint64_t bits, uint8_t type, uint8_t nreg);
int64_t bits_to_int(uint64_t bits, uint8_t type, uint8_t nreg);
/// Reinterpret 32 bits as an IEEE float.
float bits_to_float(uint32_t bits);
/// raw * scale, scale 0 meaning "no scaling".
inline double apply_scale(double raw, float scale) { return scale != 0.0f ? raw * scale : raw; }

/// Result of decoding one row's registers.
struct Decoded {
  ValueKind kind;
  bool present;    ///< false = sentinel / absent (the signal must be cleared)
  uint64_t bits;   ///< SCALAR: value bits (zero-extended); FLOAT: float bits (NaN mapped to 0); else 0
  uint8_t nbytes;  ///< SCALAR: 2/4/8; STRING/ARRAY: nreg*2 (raw bytes are the input)
};

/// Decode `nreg` registers at `wire` for a row of `type`.
Decoded decode(const uint8_t *wire, uint8_t nreg, uint8_t type, WordOrder wo);
/// Convenience: decode + scale to a double (SCALAR or FLOAT); false if absent or not numeric.
bool decode_number(const uint8_t *wire, uint8_t nreg, uint8_t type, float scale, WordOrder wo,
                   double *out);

/// Expand the low `nbits` bits of `value` (status bitfield rows, RTU +0x16): calls
/// cb(bit_index, is_set, ctx) for each bit. Returns the number of bits expanded (<= 64).
size_t expand_bits(uint64_t value, uint8_t nbits, void (*cb)(uint8_t bit, bool on, void *ctx),
                   void *ctx);

/// Capability-bitmap check (the stock self-describe check): bitmap = raw
/// bytes of sig 0x2181 (60 registers). dep_word is 1-based; dep_word 0 = no dependency
/// (always true). Out of range -> false.
bool capability_bit_set(const uint8_t *bitmap, size_t len, uint8_t dep_word, uint8_t dep_bit);

}  // namespace sungrow_datamodel
}  // namespace esphome
