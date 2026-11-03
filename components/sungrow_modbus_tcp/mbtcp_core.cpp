#include "mbtcp_core.h"

#include <cmath>
#include <cstring>

namespace esphome {
namespace sungrow_modbus_tcp {

using sungrow_datamodel::ValueKind;

static inline uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
static inline uint8_t *put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
  return p + 2;
}

// ---- FrameAssembler ----------------------------------------------------------------------------

size_t FrameAssembler::feed(const uint8_t *data, size_t n) {
  size_t take = n < this->space() ? n : this->space();
  std::memcpy(this->buf_ + this->len_, data, take);
  this->len_ += take;
  return take;
}

FrameAssembler::Status FrameAssembler::peek(Frame *out) const {
  if (this->len_ < MBAP_HEADER_LEN)
    return Status::NEED_MORE;
  const uint16_t mlen = be16(this->buf_ + 4);
  // Length covers unit id + PDU: at least unit + function code, at most unit + 253.
  if (mlen < 2 || mlen > MAX_PDU_LEN + 1)
    return Status::BAD;
  const size_t total = 6u + mlen;
  if (this->len_ < total)
    return Status::NEED_MORE;
  out->hdr.tid = be16(this->buf_);
  // The protocol id is echoed without checking it (as stock): a non-zero id is not rejected.
  out->hdr.pid = be16(this->buf_ + 2);
  out->hdr.unit = this->buf_[6];
  out->hdr.fc = this->buf_[7];
  out->pdu = this->buf_ + 7;
  out->pdu_len = static_cast<uint16_t>(mlen - 1);
  out->total_len = static_cast<uint16_t>(total);
  return Status::FRAME;
}

void FrameAssembler::pop(const Frame &f) {
  size_t n = f.total_len <= this->len_ ? f.total_len : this->len_;
  std::memmove(this->buf_, this->buf_ + n, this->len_ - n);
  this->len_ -= n;
}

// ---- Backend -------------------------------------------------------------------------------------

Backend::Resolve StaticBackend::resolve_unit(uint8_t unit, UnitTarget *out) {
  for (const auto &e : this->units_) {
    if (e.unit == unit) {
      if (e.target.table == nullptr)
        return Resolve::NOT_READY;
      *out = e.target;
      return Resolve::OK;
    }
  }
  return Resolve::UNKNOWN;
}

// ---- value encoding ------------------------------------------------------------------------------

static inline uint64_t width_mask(uint8_t nreg) {
  return nreg >= 4 ? ~0ULL : ((1ULL << (16 * nreg)) - 1);
}

uint64_t sentinel_bits(uint8_t type, uint8_t nreg) {
  const uint64_t mask = width_mask(nreg);
  switch (type) {
    case sungrow_datamodel::TYPE_S16:
    case sungrow_datamodel::TYPE_S32:
    case sungrow_datamodel::TYPE_S64:
      return mask >> 1;
    default:
      // u16/u32/u64. Type 9 float32 rows use F32_ABSENT_BITS (write_sentinel), not this.
      return mask;
  }
}

/// Wire bits of an absent / invalid float32 row.
/// Differs from stock, which has no float sentinel: a quiet NaN lets clients see "no data"
/// instead of a plausible 0.
static constexpr uint32_t F32_ABSENT_BITS = 0x7FC00000u;

static void write_sentinel(const RegisterRow &row, WordOrder wo, uint8_t *out) {
  std::memset(out, 0, 2u * row.nreg);
  const ValueKind rk = sungrow_datamodel::value_kind(row.type, row.nreg);
  if (rk == ValueKind::FLOAT) {
    sungrow_datamodel::disassemble(F32_ABSENT_BITS, 2, wo, out);
    return;
  }
  if (rk != ValueKind::SCALAR) {
    // Strings/arrays of an absent signal are served as zeros.
    return;
  }
  const uint8_t w = sungrow_datamodel::scalar_regs(row.type, row.nreg);
  sungrow_datamodel::disassemble(sentinel_bits(row.type, w), w, wo, out);
}

// Float32 row (TYPE_F32_WIRE x2, tcpslave_dev18 meters). As in stock, a float row carries the
// value in engineering units (V, A, kW, kWh); the row scale only sets the display decimals. The
// row therefore gets the signal's number(): a meter float stored unscaled (float_scale off, the
// default) is re-packed bit for bit, and integer or derived sources are converted to units.
static void encode_float_row_(const RegisterRow &row, const Signal *sig, WordOrder wo, uint8_t *out) {
  if (!((sig->kind == ValueKind::FLOAT && sig->len == 4) || sig->kind == ValueKind::SCALAR)) {
    write_sentinel(row, wo, out);
    return;
  }
  const float v = static_cast<float>(sig->number());
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  if (std::isnan(v))
    bits = F32_ABSENT_BITS;
  std::memset(out, 0, 2u * row.nreg);
  sungrow_datamodel::disassemble(bits, 2, wo, out);
}

void encode_row(const RegisterRow &row, const Signal *sig, WordOrder wo, uint8_t *out) {
  if (sig == nullptr || !sig->valid) {
    write_sentinel(row, wo, out);
    return;
  }
  std::memset(out, 0, 2u * row.nreg);
  const ValueKind rk = sungrow_datamodel::value_kind(row.type, row.nreg);
  if (rk == ValueKind::FLOAT) {
    encode_float_row_(row, sig, wo, out);
    return;
  }
  if (rk != ValueKind::SCALAR) {
    // String / raw array: the store holds the raw wire bytes as received.
    if (sig->kind == ValueKind::STRING || sig->kind == ValueKind::ARRAY) {
      size_t n = sig->len < 2u * row.nreg ? sig->len : 2u * row.nreg;
      std::memcpy(out, sig->data(), n);
    } else {
      write_sentinel(row, wo, out);
    }
    return;
  }
  const uint8_t w = sungrow_datamodel::scalar_regs(row.type, row.nreg);
  const bool row_signed = sungrow_datamodel::type_is_signed(row.type);
  int64_t value;
  bool value_unsigned64 = false;  // value is a u64 beyond INT64_MAX
  if (sig->kind == ValueKind::FLOAT) {
    // Derived value (host float, already scaled): back to raw units of this row.
    double v = sig->number();
    if (row.scale != 0.0f)
      v /= row.scale;
    if (!std::isfinite(v)) {
      write_sentinel(row, wo, out);
      return;
    }
    value = static_cast<int64_t>(std::llround(v));
  } else if (sig->kind == ValueKind::SCALAR) {
    // The store keeps raw integer bits (unscaled), as the stock DM does. Normally the poll row
    // and the tcpslave row share the scale (gen_tables writes one scale per sigid) and the raw
    // value is re-packed as is; if both carry a different non-zero scale, rescale to the row's.
    const uint8_t sw = static_cast<uint8_t>(sig->len / 2);
    value = sungrow_datamodel::bits_to_int(sig->bits(), sig->type, sw == 0 ? 1 : sw);
    value_unsigned64 = !sungrow_datamodel::type_is_signed(sig->type) && sw >= 4 && value < 0;
    if (!value_unsigned64 && sig->scale != 0.0f && row.scale != 0.0f && sig->scale != row.scale) {
      double v = static_cast<double>(value) * sig->scale / row.scale;
      value = static_cast<int64_t>(std::llround(v));
    }
  } else {
    write_sentinel(row, wo, out);
    return;
  }
  // Width conversion (poll row narrower or wider than the tcpslave row).
  // Differs from stock (which leaves the registers zero when the widths differ): the value is
  // extended or range-checked.
  uint64_t bits;
  if (w >= 4) {
    bits = static_cast<uint64_t>(value);
  } else {
    const int bitsw = 16 * w;
    const int64_t umax = (1LL << bitsw) - 1;
    const int64_t smin = -(1LL << (bitsw - 1));
    const int64_t smax = (1LL << (bitsw - 1)) - 1;
    bool fits;
    if (value_unsigned64)
      fits = false;
    else if (row_signed)
      fits = value >= smin && value <= smax;
    else
      fits = value >= 0 ? value <= umax : value >= smin;  // negative into unsigned: keep bits
    if (!fits) {
      write_sentinel(row, wo, out);
      return;
    }
    bits = static_cast<uint64_t>(value) & width_mask(w);
  }
  sungrow_datamodel::disassemble(bits, w, wo, out);
}

// ---- register assembly ---------------------------------------------------------------------------

AssembleStats assemble_registers(const RegisterTable &table, uint8_t fn, uint16_t start, uint16_t count,
                                 Backend &backend, uint8_t device, WordOrder wo, uint8_t *out,
                                 const RegisterTable *fallback) {
  AssembleStats st;
  std::memset(out, 0, 2u * count);
  if (count == 0)
    return st;
  // One pass over the table: first row (table order) starting at each requested address.
  const RegisterRow *slot[MAX_READ_REGS] = {};
  const uint32_t end = static_cast<uint32_t>(start) + count;
  const uint16_t n = count > MAX_READ_REGS ? MAX_READ_REGS : count;
  for (uint16_t i = 0; i < table.count; i++) {
    const RegisterRow &r = table.rows[i];
    if (r.fn != fn || r.regaddr < start || r.regaddr >= end)
      continue;
    const RegisterRow *&s = slot[r.regaddr - start];
    if (s == nullptr)
      s = &r;
  }
  // Registers no row covers (holes) are zero.
  uint8_t tmp[2 * 255];
  uint16_t off = 0;
  while (off < n) {
    const RegisterRow *r = slot[off];
    if (r == nullptr || r->sigid == 0 || r->nreg == 0) {
      st.unresolved++;
      off++;
      continue;
    }
    st.resolved++;
    const Signal *sig = backend.get_signal(device, r->sigid);
    if (sig == nullptr && fallback != nullptr) {
      // Poll-table row at the same address and fn with a different sigid (opt-in).
      for (uint16_t i = 0; i < fallback->count; i++) {
        const RegisterRow &pr = fallback->rows[i];
        if (pr.regaddr != r->regaddr || pr.fn != fn || pr.sigid == 0 || pr.sigid == r->sigid)
          continue;
        sig = backend.get_signal(device, pr.sigid);
        if (sig != nullptr) {
          st.fallback++;
          break;
        }
      }
    }
    if (sig == nullptr)
      st.absent++;
    encode_row(*r, sig, wo, tmp);
    uint16_t take = r->nreg;
    if (off + take > n)
      take = static_cast<uint16_t>(n - off);  // row runs past the request: clip
    std::memcpy(out + 2 * off, tmp, 2u * take);
    off = static_cast<uint16_t>(off + r->nreg);
  }
  return st;
}

// ---- replies -------------------------------------------------------------------------------------

static uint8_t *put_mbap(const ReqHeader &h, uint16_t len_after, uint8_t *out) {
  uint8_t *p = put16(out, h.tid);
  p = put16(p, h.pid);
  p = put16(p, len_after);
  *p++ = h.unit;
  return p;
}

size_t build_exception(const ReqHeader &h, uint8_t code, uint8_t *out) {
  uint8_t *p = put_mbap(h, 3, out);
  *p++ = static_cast<uint8_t>(h.fc | 0x80);
  *p++ = code;
  return static_cast<size_t>(p - out);
}

size_t build_read_reply(const ReqHeader &h, const uint8_t *data, uint16_t nregs, uint8_t *out) {
  const uint16_t nbytes = static_cast<uint16_t>(2 * nregs);
  uint8_t *p = put_mbap(h, static_cast<uint16_t>(3 + nbytes), out);
  *p++ = h.fc;
  *p++ = static_cast<uint8_t>(nbytes);
  std::memcpy(p, data, nbytes);
  p += nbytes;
  return static_cast<size_t>(p - out);
}

size_t build_write_reply(const ReqHeader &h, uint16_t start, uint16_t value_or_count, uint8_t exception,
                         uint8_t *out) {
  // Always the standard echo. Differs from stock, which omits the address for wire address 0.
  if (exception != EX_NONE)
    return build_exception(h, exception, out);
  uint8_t *p = put_mbap(h, 6, out);
  *p++ = h.fc;
  p = put16(p, static_cast<uint16_t>(start - 1));
  p = put16(p, value_or_count);
  return static_cast<size_t>(p - out);
}

size_t build_passthrough_reply(const ReqHeader &h, bool ok, const uint8_t *data, uint16_t nregs,
                               uint16_t expected, uint8_t *out) {
  // Device error or register-count mismatch -> exception 4. A device exception on a live read
  // is reported as exception 4 (as stock), not forwarded with its own code.
  if (!ok || data == nullptr || nregs != expected)
    return build_exception(h, EX_DEVICE_FAILURE, out);
  return build_read_reply(h, data, nregs, out);
}

static Decision reply_exception(const ReqHeader &h, uint8_t code, uint8_t *out, size_t *out_len) {
  Decision d;
  d.kind = Decision::REPLY;
  d.exception = code;
  *out_len = build_exception(h, code, out);
  return d;
}

// fn 6 / fn 16: parse, gate (Backend::check_write), then hand to the transport.
static Decision handle_write_(const Frame &f, const UnitTarget &tgt, Backend &backend, uint8_t *out,
                              size_t *out_len) {
  const ReqHeader &h = f.hdr;
  Decision d;
  uint16_t wire, count;
  if (h.fc == FC_WRITE_SINGLE) {
    if (f.pdu_len != 5)
      return reply_exception(h, EX_ILLEGAL_VALUE, out, out_len);
    wire = be16(f.pdu + 1);
    count = 1;
    d.values[0] = be16(f.pdu + 3);
  } else {
    if (f.pdu_len < 6)
      return reply_exception(h, EX_ILLEGAL_VALUE, out, out_len);
    wire = be16(f.pdu + 1);
    count = be16(f.pdu + 3);
    const uint8_t nbytes = f.pdu[5];
    // Stock: "datalen != regnum*2" -> no reply; we answer exception 3 (spec).
    if (count == 0 || count > MAX_WRITE_REGS || nbytes != 2 * count || f.pdu_len != 6u + nbytes)
      return reply_exception(h, EX_ILLEGAL_VALUE, out, out_len);
    for (uint16_t i = 0; i < count; i++)
      d.values[i] = be16(f.pdu + 6 + 2 * i);
  }
  if (wire == 0xFFFF || static_cast<uint32_t>(wire) + count > 0xFFFFu)
    return reply_exception(h, EX_ILLEGAL_ADDRESS, out, out_len);
  const uint16_t start = static_cast<uint16_t>(wire + 1);
  const uint8_t ex = backend.check_write(tgt, h.fc, start, d.values, count);
  if (ex != EX_NONE)
    return reply_exception(h, ex, out, out_len);
  if (tgt.rtu_address == 0)
    return reply_exception(h, EX_GATEWAY_PATH, out, out_len);  // no RS485 hub to forward to
  d.kind = Decision::WRITE;
  d.rtu_address = tgt.rtu_address;
  d.device = tgt.device;
  d.fn = h.fc;
  d.start = start;
  d.count = count;
  return d;
}

Decision handle_frame(const Frame &f, Backend &backend, const Options &opt, uint8_t *out, size_t *out_len) {
  *out_len = 0;
  const ReqHeader &h = f.hdr;

  // Unit first, as stock (the unit is looked up before the function is dispatched).
  UnitTarget tgt;
  Backend::Resolve rs = backend.resolve_unit(h.unit, &tgt);
  if (rs != Backend::Resolve::OK) {
    // Differs from stock (which silently drops frames for unit ids that match no bus device):
    // exception 0x0B by default (configurable). NOT_READY (configured device, table not known
    // yet, e.g. discovery pending) is treated the same way. Unit 247 (the logger's own virtual
    // registers) is not served.
    if (opt.unknown_unit_exception)
      return reply_exception(h, EX_GATEWAY_TARGET, out, out_len);
    Decision d;
    d.kind = Decision::DROP;
    return d;
  }

  switch (h.fc) {
    case FC_READ_HOLDING:
    case FC_READ_INPUT:
      break;
    case FC_WRITE_SINGLE:
    case FC_WRITE_MULTIPLE:
      return handle_write_(f, tgt, backend, out, out_len);
    case FC_WRITE_SINGLE_COIL:
    case FC_SUNGROW_WRITE:
      // Refused on purpose. fn 5: no Sungrow register is a coil. fn 0x60: stock forwards it only
      // for unit 0, as a no-reply RS-485 broadcast (WiNet-S: a dispatch list; WiNet-S2: any mode,
      // including a batch write of 30000-36000) that bypasses every write check. Each dongle
      // drives one inverter, and gated unicast writes (13051/13052/31205) do the same with range
      // checks and a reply.
      return reply_exception(h, EX_ILLEGAL_FUNCTION, out, out_len);
    default:
      // Differs from stock (which ignores unknown function codes without a reply): exception 1,
      // so clients do not wait for a timeout.
      return reply_exception(h, EX_ILLEGAL_FUNCTION, out, out_len);
  }

  if (f.pdu_len < 5)
    return reply_exception(h, EX_ILLEGAL_VALUE, out, out_len);
  const uint16_t wire = be16(f.pdu + 1);
  const uint16_t count = be16(f.pdu + 3);
  // Stock: count > 125 -> exception 2 (not 3). count 0 is rejected with 2 as well.
  if (count == 0 || count > MAX_READ_REGS || static_cast<uint32_t>(wire) + count > 0x10000u)
    return reply_exception(h, EX_ILLEGAL_ADDRESS, out, out_len);
  // Wire address 0 maps to table address 1 (stock `addr = wire + 1`); wire 0xFFFF is out of map.
  if (wire == 0xFFFF)
    return reply_exception(h, EX_ILLEGAL_ADDRESS, out, out_len);
  const uint16_t start = static_cast<uint16_t>(wire + 1);

  uint8_t data[2 * MAX_READ_REGS];
  AssembleStats st = assemble_registers(*tgt.table, h.fc, start, count, backend, tgt.device, opt.word_order, data,
                                        opt.poll_fallback ? tgt.poll_table : nullptr);

  if (h.fc == FC_READ_HOLDING && opt.passthrough && tgt.rtu_address != 0) {
    // Stock: any unresolved register -> live read of the whole range. A resolved row whose
    // signal we never stored (our poll plan may not cover it) is treated as unresolved too, so
    // the client gets real device data instead of a sentinel.
    if (st.resolved == 0 || st.unresolved > 0 || st.absent > 0) {
      Decision d;
      d.kind = Decision::PASSTHROUGH;
      d.rtu_address = tgt.rtu_address;
      d.fn = h.fc;
      d.start = start;
      d.count = count;
      return d;
    }
  }
  if (st.resolved == 0)
    return reply_exception(h, EX_ILLEGAL_ADDRESS, out, out_len);

  Decision d;
  d.kind = Decision::REPLY;
  *out_len = build_read_reply(h, data, count, out);
  return d;
}

// ---- allow-list ------------------------------------------------------------------------------------

bool parse_ip_rule(const char *s, IpRule *out) {
  uint32_t parts[4];
  int np = 0;
  uint32_t cur = 0;
  int digits = 0;
  const char *p = s;
  for (;; p++) {
    char c = *p;
    if (c >= '0' && c <= '9') {
      cur = cur * 10 + static_cast<uint32_t>(c - '0');
      if (++digits > 3 || cur > 255)
        return false;
      continue;
    }
    if (digits == 0 || np >= 4)
      return false;
    parts[np++] = cur;
    cur = 0;
    digits = 0;
    if (c == '.')
      continue;
    break;
  }
  if (np != 4)
    return false;
  int prefix = 32;
  if (*p == '/') {
    p++;
    prefix = 0;
    int pd = 0;
    while (*p >= '0' && *p <= '9') {
      prefix = prefix * 10 + (*p - '0');
      if (++pd > 2)
        return false;
      p++;
    }
    if (pd == 0 || prefix > 32)
      return false;
  }
  if (*p != '\0')
    return false;
  out->mask = prefix == 0 ? 0u : (0xFFFFFFFFu << (32 - prefix));
  out->addr = ((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]) & out->mask;
  return true;
}

bool ip_allowed(const std::vector<IpRule> &rules, uint32_t addr) {
  if (rules.empty())
    return true;
  for (const auto &r : rules)
    if ((addr & r.mask) == r.addr)
      return true;
  return false;
}

}  // namespace sungrow_modbus_tcp
}  // namespace esphome
