#include "decoder.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace sungrow_datamodel {

uint8_t type_width(uint8_t type) {
  switch (type) {
    case TYPE_S16:
    case TYPE_U16:
      return 1;
    case TYPE_S32:
    case TYPE_U32:
    case TYPE_F32:
      return 2;
    case TYPE_S64:
    case TYPE_U64:
      return 4;
    case TYPE_F32_WIRE:
      return 2;
    default:
      return 0;
  }
}

bool type_is_signed(uint8_t type) {
  // Type 9 is an IEEE float: signed by nature.
  return type == TYPE_S16 || type == TYPE_S32 || type == TYPE_S64 || type == TYPE_F32_WIRE;
}

ValueKind value_kind(uint8_t type, uint8_t nreg) {
  if (type == TYPE_STRING)
    return ValueKind::STRING;
  if (type == TYPE_F32)
    return ValueKind::FLOAT;
  if (type == TYPE_F32_WIRE)
    return nreg == 2 ? ValueKind::FLOAT : ValueKind::ARRAY;
  uint8_t w = type_width(type);
  if (w == 0)
    return ValueKind::ARRAY;
  // nreg < natural width (tcpslave_dev35 u32 x1): decoded by register count (as stock).
  return nreg <= w ? ValueKind::SCALAR : ValueKind::ARRAY;
}

uint8_t scalar_regs(uint8_t type, uint8_t nreg) {
  if (type == TYPE_F32_WIRE)
    return 2;
  uint8_t w = type_width(type);
  if (nreg >= 4 && w >= 4)
    return 4;
  if (nreg >= 2 && w >= 2)
    return 2;
  return 1;
}

static inline uint16_t reg_be(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint64_t assemble(const uint8_t *wire, uint8_t nreg, WordOrder wo) {
  uint64_t v = 0;
  switch (wo) {
    case WordOrder::HIGH_WORD_FIRST:
      for (uint8_t i = 0; i < nreg; i++)
        v = (v << 16) | reg_be(wire + 2 * i);
      return v;
    case WordOrder::LOW_WORD_FIRST:
      if (nreg == 4) {
        // As stock: (reg1<<16 | reg0) << 32 | (reg3<<16 | reg2). Not a pure
        // low-word-first u64: the two 32-bit halves are high half first.
        return (static_cast<uint64_t>(reg_be(wire + 2)) << 48) | (static_cast<uint64_t>(reg_be(wire)) << 32) |
               (static_cast<uint64_t>(reg_be(wire + 6)) << 16) | reg_be(wire + 4);
      }
      // Registers big-endian, reg0 = least significant word. Single registers are plain BE.
      for (uint8_t i = 0; i < nreg; i++)
        v |= static_cast<uint64_t>(reg_be(wire + 2 * i)) << (16 * i);
      return v;
    case WordOrder::UNSWAPPED:
      // Stock mode 3 is a raw copy for 1, 2 and 4 registers: same as RAW.
    case WordOrder::RAW:
    default:
      // Bytes memcpy'd into little-endian host memory: b0 is the least significant byte.
      for (uint8_t i = 0; i < 2 * nreg; i++)
        v |= static_cast<uint64_t>(wire[i]) << (8 * i);
      return v;
  }
}

void disassemble(uint64_t bits, uint8_t nreg, WordOrder wo, uint8_t *wire) {
  switch (wo) {
    case WordOrder::HIGH_WORD_FIRST:
      for (int i = nreg - 1; i >= 0; i--) {
        uint16_t r = static_cast<uint16_t>(bits);
        wire[2 * i] = static_cast<uint8_t>(r >> 8);
        wire[2 * i + 1] = static_cast<uint8_t>(r);
        bits >>= 16;
      }
      return;
    case WordOrder::LOW_WORD_FIRST:
      if (nreg == 4) {
        const uint16_t r[4] = {static_cast<uint16_t>(bits >> 32), static_cast<uint16_t>(bits >> 48),
                               static_cast<uint16_t>(bits), static_cast<uint16_t>(bits >> 16)};
        for (uint8_t i = 0; i < 4; i++) {
          wire[2 * i] = static_cast<uint8_t>(r[i] >> 8);
          wire[2 * i + 1] = static_cast<uint8_t>(r[i]);
        }
        return;
      }
      for (uint8_t i = 0; i < nreg; i++) {
        uint16_t r = static_cast<uint16_t>(bits >> (16 * i));
        wire[2 * i] = static_cast<uint8_t>(r >> 8);
        wire[2 * i + 1] = static_cast<uint8_t>(r);
      }
      return;
    case WordOrder::UNSWAPPED:
    case WordOrder::RAW:
    default:
      for (uint8_t i = 0; i < 2 * nreg; i++)
        wire[i] = static_cast<uint8_t>(bits >> (8 * i));
      return;
  }
}

static inline uint64_t width_mask(uint8_t nreg) {
  return nreg >= 4 ? ~0ULL : ((1ULL << (16 * nreg)) - 1);
}

bool is_sentinel(uint64_t bits, uint8_t type, uint8_t nreg) {
  // A row narrower than its type (u32 x1): stock tests the full type width, whose upper
  // bytes are zero, so the sentinel can never match.
  if (nreg < type_width(type))
    return false;
  uint64_t mask = width_mask(nreg);
  bits &= mask;
  switch (type) {
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
      return bits == mask;
    case TYPE_S16:
    case TYPE_S32:
    case TYPE_S64:
      return bits == (mask >> 1);
    default:
      // Type 9 (float) and others: always "present" (no sentinel).
      return false;
  }
}

int64_t bits_to_int(uint64_t bits, uint8_t type, uint8_t nreg) {
  uint64_t mask = width_mask(nreg);
  bits &= mask;
  if (type_is_signed(type) && nreg < 4) {
    uint64_t sign = 1ULL << (16 * nreg - 1);
    if (bits & sign)
      return static_cast<int64_t>(bits | ~mask);
  }
  return static_cast<int64_t>(bits);
}

double bits_to_double(uint64_t bits, uint8_t type, uint8_t nreg) {
  if (!type_is_signed(type) && nreg >= 4)
    return static_cast<double>(bits);  // u64 above INT64_MAX
  return static_cast<double>(bits_to_int(bits, type, nreg));
}

Decoded decode(const uint8_t *wire, uint8_t nreg, uint8_t type, WordOrder wo) {
  Decoded d{};
  d.kind = value_kind(type, nreg);
  switch (d.kind) {
    case ValueKind::STRING:
      d.nbytes = static_cast<uint8_t>(nreg * 2);
      d.present = nreg > 0 && wire[0] != 0;
      return d;
    case ValueKind::ARRAY: {
      d.nbytes = static_cast<uint8_t>(nreg * 2);
      // An array is absent when its first register is the type's sentinel.
      uint16_t first = reg_be(wire);
      uint8_t t16 = type_is_signed(type) ? static_cast<uint8_t>(TYPE_S16) : static_cast<uint8_t>(TYPE_U16);
      d.present = type == TYPE_F32_WIRE || !is_sentinel(first, t16, 1);
      return d;
    }
    case ValueKind::FLOAT: {
      d.nbytes = 4;
      uint32_t raw = static_cast<uint32_t>(assemble(wire, 2, wo));
      if (type == TYPE_F32_WIRE) {
        // Stock: NaN -> 0, then offset (+0x10) + f * mult (+0x0C). mult = 1 and offset = 0
        // in every table (gen_tables.py checks mult_f32), so f is stored as received.
        float f;
        std::memcpy(&f, &raw, 4);
        if (std::isnan(f))
          f = 0.0f;
        std::memcpy(&raw, &f, 4);
      }
      d.bits = raw;
      d.present = true;
      return d;
    }
    case ValueKind::SCALAR:
    default: {
      uint8_t n = scalar_regs(type, nreg);
      d.nbytes = static_cast<uint8_t>(n * 2);
      d.bits = assemble(wire, n, wo);
      d.present = !is_sentinel(d.bits, type, n);
      return d;
    }
  }
}

bool decode_number(const uint8_t *wire, uint8_t nreg, uint8_t type, float scale, WordOrder wo,
                   double *out) {
  Decoded d = decode(wire, nreg, type, wo);
  if (!d.present)
    return false;
  if (d.kind == ValueKind::FLOAT) {
    *out = apply_scale(bits_to_float(static_cast<uint32_t>(d.bits)), scale);
    return true;
  }
  if (d.kind != ValueKind::SCALAR)
    return false;
  *out = apply_scale(bits_to_double(d.bits, type, d.nbytes / 2), scale);
  return true;
}

float bits_to_float(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

size_t expand_bits(uint64_t value, uint8_t nbits, void (*cb)(uint8_t bit, bool on, void *ctx),
                   void *ctx) {
  if (nbits > 64)
    nbits = 64;
  for (uint8_t i = 0; i < nbits; i++)
    cb(i, ((value >> i) & 1ULL) != 0, ctx);
  return nbits;
}

bool capability_bit_set(const uint8_t *bitmap, size_t len, uint8_t dep_word, uint8_t dep_bit) {
  if (dep_word == 0)
    return true;
  size_t off = static_cast<size_t>(dep_word - 1) * 2;
  if (bitmap == nullptr || off + 1 >= len || dep_bit > 15)
    return false;
  // Word w = big-endian register w-1 of the raw 0x2181 copy, LSB = bit 0 (as stock).
  uint16_t w = reg_be(bitmap + off);
  return ((w >> dep_bit) & 1) != 0;
}

}  // namespace sungrow_datamodel
}  // namespace esphome
