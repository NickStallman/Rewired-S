#include "trace_core.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../sungrow_modbus/modbus_rtu.h"

namespace esphome {
namespace sungrow_debug {

using sungrow_datamodel::Decoded;
using sungrow_datamodel::ValueKind;

static inline uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

// ---- hex ---------------------------------------------------------------------------------------
std::string hex(const uint8_t *data, size_t len, size_t max_bytes) {
  static const char *const kHex = "0123456789ABCDEF";
  const size_t n = len < max_bytes ? len : max_bytes;
  std::string s;
  s.reserve(n * 3 + 12);
  for (size_t i = 0; i < n; i++) {
    if (i)
      s += ' ';
    s += kHex[data[i] >> 4];
    s += kHex[data[i] & 0xF];
  }
  if (n < len) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), " ..(+%u)", static_cast<unsigned>(len - n));
    s += buf;
  }
  return s;
}

std::string hex_words(const uint8_t *wire, size_t nregs, size_t max_regs) {
  const size_t n = nregs < max_regs ? nregs : max_regs;
  std::string s;
  s.reserve(n * 5 + 12);
  char buf[20];
  for (size_t i = 0; i < n; i++) {
    std::snprintf(buf, sizeof(buf), i ? " %04X" : "%04X", be16(wire + 2 * i));
    s += buf;
  }
  if (n < nregs) {
    std::snprintf(buf, sizeof(buf), " ..(+%u)", static_cast<unsigned>(nregs - n));
    s += buf;
  }
  return s;
}

std::vector<std::string> hex_lines(const uint8_t *data, size_t len, size_t per_line) {
  std::vector<std::string> out;
  if (per_line == 0)
    per_line = 32;
  for (size_t off = 0; off < len; off += per_line) {
    char pre[16];
    std::snprintf(pre, sizeof(pre), "+%03u ", static_cast<unsigned>(off));
    size_t n = len - off < per_line ? len - off : per_line;
    out.push_back(std::string(pre) + hex(data + off, n));
  }
  if (out.empty())
    out.emplace_back("(empty)");
  return out;
}

// ---- RTU ---------------------------------------------------------------------------------------
const char *fn_name(uint8_t fn) {
  switch (fn & 0x7F) {
    case 0x01:
      return "read coils";
    case 0x02:
      return "read discrete inputs";
    case 0x03:
      return "read holding";
    case 0x04:
      return "read input";
    case 0x05:
      return "write single coil";
    case 0x06:
      return "write single";
    case 0x0F:
      return "write multiple coils";
    case 0x10:
      return "write multiple";
    case 0x60:
      return "sungrow write";
    case 0x77:
      return "sungrow 0x77";
    default:
      return "?";
  }
}

const char *exception_name(uint8_t code) {
  switch (code) {
    case 0x01:
      return "illegal function";
    case 0x02:
      return "illegal data address";
    case 0x03:
      return "illegal data value";
    case 0x04:
      return "slave device failure";
    case 0x05:
      return "acknowledge";
    case 0x06:
      return "slave busy";
    case 0x0A:
      return "gateway path unavailable";
    case 0x0B:
      return "gateway target failed to respond";
    default:
      return "?";
  }
}

RtuInfo parse_rtu_request(const uint8_t *f, size_t len) {
  RtuInfo r;
  if (f == nullptr || len < 2)
    return r;
  r.addr = f[0];
  r.fn = f[1] & 0x7F;
  r.crc_ok = len >= 4 && sungrow_modbus::crc_ok(f, len);
  switch (r.fn) {
    case 0x03:
    case 0x04:
      if (len < 8)
        return r;
      r.start = static_cast<uint16_t>(be16(f + 2) + 1);
      r.count = be16(f + 4);
      break;
    case 0x06:
      if (len < 8)
        return r;
      r.start = static_cast<uint16_t>(be16(f + 2) + 1);
      r.count = 1;
      r.value = be16(f + 4);
      break;
    case 0x10:
      if (len < 9)
        return r;
      r.start = static_cast<uint16_t>(be16(f + 2) + 1);
      r.count = be16(f + 4);
      r.byte_count = f[6];
      break;
    default:
      break;
  }
  r.parsed = true;
  return r;
}

RtuInfo parse_rtu_response(const uint8_t *f, size_t len) {
  RtuInfo r;
  if (f == nullptr || len < 2)
    return r;
  r.addr = f[0];
  r.fn = f[1] & 0x7F;
  r.exception = (f[1] & 0x80) != 0;
  r.crc_ok = len >= 4 && sungrow_modbus::crc_ok(f, len);
  if (r.exception) {
    if (len < 5)
      return r;
    r.exc_code = f[2];
    r.parsed = true;
    return r;
  }
  switch (r.fn) {
    case 0x03:
    case 0x04:
      if (len < 5)
        return r;
      r.byte_count = f[2];
      r.count = static_cast<uint16_t>(f[2] / 2);
      break;
    case 0x06:
    case 0x10:
      if (len < 8)
        return r;
      r.start = static_cast<uint16_t>(be16(f + 2) + 1);
      if (r.fn == 0x06) {
        r.count = 1;
        r.value = be16(f + 4);
      } else {
        r.count = be16(f + 4);
      }
      break;
    default:
      break;
  }
  r.parsed = true;
  return r;
}

std::string describe_request(const RtuInfo &r) {
  char buf[120];
  if (!r.parsed) {
    std::snprintf(buf, sizeof(buf), "unparsable request");
  } else if (r.fn == 0x06) {
    std::snprintf(buf, sizeof(buf), "addr %u fn %u (%s) reg %u value 0x%04X%s", r.addr, r.fn, fn_name(r.fn), r.start,
                  r.value, r.crc_ok ? "" : " CRC-BAD");
  } else if (r.count != 0) {
    std::snprintf(buf, sizeof(buf), "addr %u fn %u (%s) start %u count %u (..%u)%s", r.addr, r.fn, fn_name(r.fn),
                  r.start, r.count, static_cast<unsigned>(r.start + r.count - 1), r.crc_ok ? "" : " CRC-BAD");
  } else {
    std::snprintf(buf, sizeof(buf), "addr %u fn %u (%s)%s", r.addr, r.fn, fn_name(r.fn), r.crc_ok ? "" : " CRC-BAD");
  }
  return buf;
}

std::string describe_response(const RtuInfo &r) {
  char buf[120];
  if (!r.parsed) {
    std::snprintf(buf, sizeof(buf), "unparsable reply");
  } else if (r.exception) {
    std::snprintf(buf, sizeof(buf), "addr %u fn %u EXCEPTION %u (%s) CRC %s", r.addr, r.fn, r.exc_code,
                  exception_name(r.exc_code), r.crc_ok ? "ok" : "BAD");
  } else if (r.fn == 0x03 || r.fn == 0x04) {
    std::snprintf(buf, sizeof(buf), "addr %u fn %u %u B = %u regs CRC %s", r.addr, r.fn, r.byte_count, r.count,
                  r.crc_ok ? "ok" : "BAD");
  } else if (r.fn == 0x06 || r.fn == 0x10) {
    std::snprintf(buf, sizeof(buf), "addr %u fn %u echo start %u count %u CRC %s", r.addr, r.fn, r.start, r.count,
                  r.crc_ok ? "ok" : "BAD");
  } else {
    std::snprintf(buf, sizeof(buf), "addr %u fn %u CRC %s", r.addr, r.fn, r.crc_ok ? "ok" : "BAD");
  }
  return buf;
}

// ---- MBAP --------------------------------------------------------------------------------------
MbapInfo parse_mbap(const uint8_t *adu, size_t len, bool is_reply) {
  MbapInfo m;
  if (adu == nullptr || len < 8)
    return m;
  m.tid = be16(adu);
  m.pid = be16(adu + 2);
  m.len = be16(adu + 4);
  m.unit = adu[6];
  m.fc = adu[7] & 0x7F;
  m.exception = (adu[7] & 0x80) != 0;
  if (m.exception) {
    if (len >= 9)
      m.exc_code = adu[8];
    m.parsed = true;
    return m;
  }
  const bool rw = m.fc == 0x03 || m.fc == 0x04;
  if (!is_reply && (rw || m.fc == 0x06 || m.fc == 0x10) && len >= 12) {
    m.start = static_cast<uint16_t>(be16(adu + 8) + 1);
    m.count = m.fc == 0x06 ? 1 : be16(adu + 10);
  } else if (is_reply && rw && len >= 9) {
    m.count = static_cast<uint16_t>(adu[8] / 2);
  } else if (is_reply && (m.fc == 0x06 || m.fc == 0x10) && len >= 12) {
    m.start = static_cast<uint16_t>(be16(adu + 8) + 1);
    m.count = m.fc == 0x06 ? 1 : be16(adu + 10);
  }
  m.parsed = true;
  return m;
}

std::string describe_mbap(const MbapInfo &m, bool is_reply) {
  char buf[140];
  if (!m.parsed) {
    std::snprintf(buf, sizeof(buf), "short ADU");
  } else if (m.exception) {
    std::snprintf(buf, sizeof(buf), "tid %u unit %u fn %u EXCEPTION %u (%s)", m.tid, m.unit, m.fc, m.exc_code,
                  exception_name(m.exc_code));
  } else if (is_reply) {
    std::snprintf(buf, sizeof(buf), "tid %u unit %u fn %u (%s) %u regs", m.tid, m.unit, m.fc, fn_name(m.fc), m.count);
  } else {
    std::snprintf(buf, sizeof(buf), "tid %u pid %u len %u unit %u fn %u (%s) start %u count %u", m.tid, m.pid, m.len,
                  m.unit, m.fc, fn_name(m.fc), m.start, m.count);
  }
  return buf;
}

// ---- row decode --------------------------------------------------------------------------------
const char *type_name(uint8_t type) {
  switch (type) {
    case sungrow_datamodel::TYPE_S16:
      return "s16";
    case sungrow_datamodel::TYPE_U16:
      return "u16";
    case sungrow_datamodel::TYPE_S32:
      return "s32";
    case sungrow_datamodel::TYPE_U32:
      return "u32";
    case sungrow_datamodel::TYPE_S64:
      return "s64";
    case sungrow_datamodel::TYPE_U64:
      return "u64";
    case sungrow_datamodel::TYPE_F32_WIRE:
      return "f32";
    case sungrow_datamodel::TYPE_STRING:
      return "str";
    default:
      return "t?";
  }
}

RowDecode decode_row(const RegisterRow &r, const uint8_t *wire, WordOrder wo, bool float_scale) {
  RowDecode out;
  Decoded d = sungrow_datamodel::decode(wire, r.nreg, r.type, wo);
  out.bits = d.bits;
  out.present = d.present;
  if (!d.present && (r.tag & sungrow_datamodel::TAG_KEEP_SENTINEL) != 0) {
    out.present = true;
    out.kept_sentinel = true;
  }
  switch (d.kind) {
    case ValueKind::SCALAR: {
      uint8_t n = sungrow_datamodel::scalar_regs(r.type, r.nreg);
      out.numeric = true;
      out.raw = sungrow_datamodel::bits_to_double(d.bits, r.type, n);
      out.scaled = sungrow_datamodel::apply_scale(out.raw, r.scale);
      break;
    }
    case ValueKind::FLOAT: {
      out.numeric = true;
      out.raw = sungrow_datamodel::bits_to_float(static_cast<uint32_t>(d.bits));
      out.scaled = float_scale ? sungrow_datamodel::apply_scale(out.raw, r.scale) : out.raw;
      break;
    }
    case ValueKind::STRING: {
      for (uint8_t i = 0; i < r.nreg * 2; i++) {
        char c = static_cast<char>(wire[i]);
        if (c == 0)
          break;
        out.text += std::isprint(static_cast<unsigned char>(c)) ? c : '.';
      }
      while (!out.text.empty() && out.text.back() == ' ')
        out.text.pop_back();
      break;
    }
    case ValueKind::ARRAY:
    default:
      break;
  }
  return out;
}

std::string format_row(const RegisterRow &r, const uint8_t *wire, WordOrder wo, Gate gate, const char *key,
                       const char *unit, bool float_scale) {
  char head[96];
  std::snprintf(head, sizeof(head), "%u+%u fn%u 0x%04X %s %s [", r.regaddr, r.nreg, r.fn, r.sigid,
                key != nullptr && key[0] != 0 ? key : "-", type_name(r.type));
  std::string s = head;
  s += hex_words(wire, r.nreg, 8);
  s += "] ";
  RowDecode d = decode_row(r, wire, wo, float_scale);
  char buf[96];
  const auto kind = sungrow_datamodel::value_kind(r.type, r.nreg);
  if (!d.present) {
    s += kind == ValueKind::STRING ? "EMPTY (absent)" : "SENTINEL (absent)";
  } else if (d.numeric) {
    if (kind == ValueKind::FLOAT) {
      std::snprintf(buf, sizeof(buf), "f=%.7g", d.raw);
    } else {
      std::snprintf(buf, sizeof(buf), "raw=%.0f", d.raw);
    }
    s += buf;
    if (r.scale != 0.0f && (kind != ValueKind::FLOAT || float_scale)) {
      std::snprintf(buf, sizeof(buf), " x%g -> %.7g", static_cast<double>(r.scale), d.scaled);
      s += buf;
    }
    if (unit != nullptr && unit[0] != 0) {
      s += ' ';
      s += unit;
    }
    if (r.nbits != 0) {
      std::snprintf(buf, sizeof(buf), " bits=0x%" PRIX64 " set:", d.bits);
      s += buf;
      int shown = 0;
      for (uint8_t b = 0; b < r.nbits && b < 64; b++) {
        if ((d.bits >> b) & 1ULL) {
          if (shown++ >= 12) {
            s += " ..";
            break;
          }
          std::snprintf(buf, sizeof(buf), " %u", b);
          s += buf;
        }
      }
      if (shown == 0)
        s += " none";
    }
    if (d.kept_sentinel)
      s += " (sentinel kept: tag bit4)";
  } else if (kind == ValueKind::STRING) {
    s += "\"" + d.text + "\"";
  } else {
    std::snprintf(buf, sizeof(buf), "array %u regs", r.nreg);
    s += buf;
  }
  if (gate == Gate::GATED) {
    std::snprintf(buf, sizeof(buf), " GATED cap w%u b%u (cleared, not stored)", r.dep_word, r.dep_bit);
    s += buf;
  } else if (gate == Gate::OK) {
    std::snprintf(buf, sizeof(buf), " cap w%u b%u ok", r.dep_word, r.dep_bit);
    s += buf;
  }
  return s;
}

std::vector<uint16_t> rows_in_block(const RegisterTable &t, uint8_t fn, uint16_t start, uint16_t nregs) {
  std::vector<uint16_t> out;
  const uint32_t end = static_cast<uint32_t>(start) + nregs;
  for (uint16_t i = 0; i < t.count; i++) {
    const RegisterRow &r = t.rows[i];
    if (r.sigid == 0 || r.fn != fn || r.regaddr < start || static_cast<uint32_t>(r.regaddr) + r.nreg > end)
      continue;
    out.push_back(i);
  }
  return out;
}

// ---- filters -----------------------------------------------------------------------------------
static bool parse_uint(const char *&p, uint32_t *out) {
  while (*p == ' ' || *p == '\t')
    p++;
  if (!std::isdigit(static_cast<unsigned char>(*p)))
    return false;
  char *end = nullptr;
  unsigned long v = std::strtoul(p, &end, 0);
  if (end == p || v > 0xFFFFFFFFul)
    return false;
  p = end;
  *out = static_cast<uint32_t>(v);
  while (*p == ' ' || *p == '\t')
    p++;
  return true;
}

bool parse_ranges(const std::string &s, uint32_t min, uint32_t max, std::vector<Range> *out) {
  std::vector<Range> r;
  std::string t = s;
  // trim
  while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
    t.pop_back();
  size_t b = 0;
  while (b < t.size() && std::isspace(static_cast<unsigned char>(t[b])))
    b++;
  t = t.substr(b);
  if (t.empty() || t == "*" || t == "all") {
    out->clear();
    return true;
  }
  const char *p = t.c_str();
  while (*p != 0) {
    uint32_t lo = 0, hi = 0;
    if (!parse_uint(p, &lo))
      return false;
    hi = lo;
    if (*p == '-') {
      p++;
      if (!parse_uint(p, &hi))
        return false;
    } else if (*p == '+') {  // start+count
      p++;
      uint32_t n = 0;
      if (!parse_uint(p, &n) || n == 0)
        return false;
      hi = lo + n - 1;
    }
    if (lo > hi || lo < min || hi > max)
      return false;
    r.push_back({lo, hi});
    if (*p == ',') {
      p++;
      continue;
    }
    if (*p != 0)
      return false;
  }
  *out = std::move(r);
  return true;
}

std::string format_ranges(const std::vector<Range> &r) {
  if (r.empty())
    return "all";
  std::string s;
  char buf[32];
  for (size_t i = 0; i < r.size(); i++) {
    if (r[i].lo == r[i].hi)
      std::snprintf(buf, sizeof(buf), "%s%u", i ? "," : "", static_cast<unsigned>(r[i].lo));
    else
      std::snprintf(buf, sizeof(buf), "%s%u-%u", i ? "," : "", static_cast<unsigned>(r[i].lo),
                    static_cast<unsigned>(r[i].hi));
    s += buf;
  }
  return s;
}

bool in_ranges(const std::vector<Range> &r, uint32_t v) {
  for (const auto &x : r)
    if (v >= x.lo && v <= x.hi)
      return true;
  return false;
}

bool overlaps_ranges(const std::vector<Range> &r, uint32_t start, uint32_t count) {
  if (r.empty())
    return true;
  if (count == 0)
    return false;
  const uint32_t last = start + count - 1;
  for (const auto &x : r)
    if (start <= x.hi && last >= x.lo)
      return true;
  return false;
}

std::string TraceFilter::describe() const {
  return "addresses " + format_ranges(this->addresses) + ", registers " + format_ranges(this->registers);
}

uint32_t fnv1a(const uint8_t *data, size_t len, uint32_t h) {
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

bool ChangeTracker::changed(uint32_t key, const uint8_t *data, size_t len) {
  const uint32_t h = fnv1a(data, len, 2166136261u ^ static_cast<uint32_t>(len));
  auto it = this->h_.find(key);
  if (it == this->h_.end()) {
    this->h_.emplace(key, h);
    return true;
  }
  if (it->second == h)
    return false;
  it->second = h;
  return true;
}

bool RateLimiter::allow(uint32_t key, uint32_t now, uint32_t min_interval_ms) {
  if (min_interval_ms == 0)
    return true;
  auto it = this->last_.find(key);
  if (it != this->last_.end() && now - it->second < min_interval_ms)
    return false;
  this->last_[key] = now;
  return true;
}

// ---- scan --------------------------------------------------------------------------------------
const char *probe_result_name(ProbeResult r) {
  switch (r) {
    case ProbeResult::OK:
      return "ok";
    case ProbeResult::EXCEPTION:
      return "exception";
    case ProbeResult::TIMEOUT:
      return "timeout";
    case ProbeResult::CRC_ERROR:
      return "crc error";
    case ProbeResult::NOT_QUEUED:
    default:
      return "not queued";
  }
}

bool ScanPlan::begin(uint8_t first, uint8_t last, uint32_t interval_ms, uint32_t now) {
  if (first < 1)
    first = 1;
  if (last > 247)
    last = 247;
  this->found_.clear();
  this->probed_ = 0;
  this->waiting_ = false;
  if (first > last) {
    this->active_ = false;
    return false;
  }
  this->first_ = first;
  this->last_ = last;
  this->cur_ = first;
  this->interval_ms_ = interval_ms;
  this->next_due_ = now;
  this->active_ = true;
  return true;
}

bool ScanPlan::next(uint32_t now, uint8_t *addr) {
  if (!this->active_ || this->waiting_)
    return false;
  if (static_cast<int32_t>(now - this->next_due_) < 0)
    return false;
  *addr = this->cur_;
  this->waiting_ = true;
  return true;
}

void ScanPlan::done(uint8_t addr, ProbeResult r, uint8_t exc_code, uint16_t value, uint32_t now) {
  if (!this->active_ || !this->waiting_ || addr != this->cur_)
    return;
  this->waiting_ = false;
  if (r == ProbeResult::NOT_QUEUED) {
    // Ad-hoc queue full: retry the same address after the interval.
    this->next_due_ = now + (this->interval_ms_ ? this->interval_ms_ : 100);
    return;
  }
  this->probed_++;
  if (r == ProbeResult::OK || r == ProbeResult::EXCEPTION || r == ProbeResult::CRC_ERROR)
    this->found_.push_back({addr, r, exc_code, value});
  if (this->cur_ >= this->last_) {
    this->active_ = false;
    return;
  }
  this->cur_++;
  this->next_due_ = now + this->interval_ms_;
}

std::string ScanPlan::summary() const {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "scan %u..%u: %u/%u probed%s, found:", this->first_, this->last_,
                static_cast<unsigned>(this->probed_), static_cast<unsigned>(this->total()),
                this->active_ ? " (running)" : "");
  std::string s = buf;
  if (this->found_.empty())
    s += " none";
  for (const auto &f : this->found_) {
    if (f.result == ProbeResult::OK)
      std::snprintf(buf, sizeof(buf), " %u (ok 0x%04X)", f.addr, f.value);
    else if (f.result == ProbeResult::EXCEPTION)
      std::snprintf(buf, sizeof(buf), " %u (exception %u)", f.addr, f.exc_code);
    else
      std::snprintf(buf, sizeof(buf), " %u (%s)", f.addr, probe_result_name(f.result));
    s += buf;
  }
  return s;
}

}  // namespace sungrow_debug
}  // namespace esphome
