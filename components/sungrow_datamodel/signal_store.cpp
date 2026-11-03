#include "signal_store.h"

#include <cstring>
#include <limits>

#include "derived.h"

namespace esphome {
namespace sungrow_datamodel {

uint64_t Signal::bits() const {
  uint64_t v = 0;
  const uint8_t *p = this->data();
  uint8_t n = this->len > 8 ? 8 : this->len;
  for (uint8_t i = 0; i < n; i++)
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

double Signal::number() const {
  if (!this->valid)
    return std::numeric_limits<double>::quiet_NaN();
  if (this->kind == ValueKind::FLOAT) {
    float f;
    std::memcpy(&f, this->data(), sizeof(f));
    return apply_scale(f, this->scale);  // derived floats carry scale 0
  }
  if (this->kind != ValueKind::SCALAR)
    return std::numeric_limits<double>::quiet_NaN();
  return apply_scale(bits_to_double(this->bits(), this->type, this->len / 2), this->scale);
}

std::string Signal::str() const {
  if (!this->valid)
    return {};
  if (this->kind != ValueKind::STRING)
    return {};
  const char *p = reinterpret_cast<const char *>(this->data());
  size_t n = 0;
  while (n < this->len && p[n] != '\0')
    n++;
  while (n > 0 && (p[n - 1] == ' ' || static_cast<uint8_t>(p[n - 1]) == 0xFF))
    n--;
  return std::string(p, n);
}

bool Signal::assign(const uint8_t *src, uint8_t n) {
  bool changed = n != this->len || std::memcmp(this->data(), src, n) != 0;
  if (!changed)
    return false;
  if (n <= sizeof(this->inline_)) {
    this->ext_.clear();
    this->ext_.shrink_to_fit();
    std::memcpy(this->inline_, src, n);
  } else {
    this->ext_.assign(src, src + n);
  }
  this->len = n;
  return true;
}

Signal &SignalStore::entry_(uint8_t device, uint16_t sigid) {
  Signal &s = this->signals_[key_(device, sigid)];
  s.device = device;
  s.sigid = sigid;
  return s;
}

void SignalStore::notify_(const Signal &s) {
  for (auto &cb : this->listeners_)
    cb(s);
  auto it = this->subs_.find(key_(s.device, s.sigid));
  if (it != this->subs_.end()) {
    for (auto &cb : it->second)
      cb(s);
  }
}

bool SignalStore::set(uint8_t device, uint16_t sigid, uint8_t type, float scale, ValueKind kind,
                      const uint8_t *bytes, uint8_t len) {
  Signal &s = this->entry_(device, sigid);
  bool changed = s.assign(bytes, len);
  changed |= !s.valid || s.type != type || s.scale != scale || s.kind != kind;
  s.type = type;
  s.scale = scale;
  s.kind = kind;
  s.valid = true;
  s.updated_ms = this->now();
  if (changed)
    this->notify_(s);
  return changed;
}

bool SignalStore::set_scalar(uint8_t device, uint16_t sigid, uint8_t type, float scale,
                             uint64_t bits, uint8_t nbytes) {
  uint8_t b[8];
  for (uint8_t i = 0; i < nbytes && i < 8; i++)
    b[i] = static_cast<uint8_t>(bits >> (8 * i));
  return this->set(device, sigid, type, scale, ValueKind::SCALAR, b, nbytes);
}

bool SignalStore::set_float(uint8_t device, uint16_t sigid, float value) {
  uint8_t b[4];
  std::memcpy(b, &value, 4);
  return this->set(device, sigid, TYPE_F32, 0.0f, ValueKind::FLOAT, b, 4);
}

bool SignalStore::clear(uint8_t device, uint16_t sigid) {
  // A signal that was never stored stays absent (no entry is created just to mark it invalid).
  auto it = this->signals_.find(key_(device, sigid));
  if (it == this->signals_.end())
    return false;
  Signal &s = it->second;
  s.updated_ms = this->now();
  bool was_valid = s.valid;
  s.valid = false;
  if (was_valid)
    this->notify_(s);
  return was_valid;
}

void SignalStore::clear_device(uint8_t device) {
  for (auto &kv : this->signals_) {
    Signal &s = kv.second;
    if (s.device == device && s.valid) {
      s.valid = false;
      s.updated_ms = this->now();
      this->notify_(s);
    }
  }
}

const Signal *SignalStore::get(uint8_t device, uint16_t sigid) const {
  auto it = this->signals_.find(key_(device, sigid));
  return it == this->signals_.end() ? nullptr : &it->second;
}

double SignalStore::get_number(uint8_t device, uint16_t sigid) const {
  const Signal *s = this->get(device, sigid);
  return s == nullptr ? std::numeric_limits<double>::quiet_NaN() : s->number();
}

bool SignalStore::get_bits(uint8_t device, uint16_t sigid, uint64_t *out) const {
  const Signal *s = this->get(device, sigid);
  if (s == nullptr || !s->valid || s->kind != ValueKind::SCALAR)
    return false;
  *out = s->bits();
  return true;
}

std::string SignalStore::get_string(uint8_t device, uint16_t sigid) const {
  const Signal *s = this->get(device, sigid);
  return s == nullptr ? std::string() : s->str();
}

bool SignalStore::capability_ok(uint8_t device, uint8_t dep_word, uint8_t dep_bit) const {
  if (dep_word == 0)
    return true;
  const Signal *cap = this->get(device, SIG_CAPABILITY_BITMAP);
  if (cap == nullptr || !cap->valid)
    return false;  // as stock: missing bitmap = every bit clear = dependent rows absent
  return capability_bit_set(cap->data(), cap->len, dep_word, dep_bit);
}

void SignalStore::for_each(const std::function<void(const Signal &)> &fn) const {
  for (const auto &kv : this->signals_)
    fn(kv.second);
}

size_t SignalStore::ingest_block(uint8_t device, const RegisterTable &table, uint8_t fn,
                                 uint16_t start_reg, const uint8_t *data, uint16_t nregs,
                                 WordOrder word_order, bool gate_capabilities) {
  if (data == nullptr || nregs == 0)
    return 0;
  const uint32_t end = static_cast<uint32_t>(start_reg) + nregs;
  size_t n = 0;
  // Capability bitmap first, so rows in the same block can be gated against fresh data.
  for (int pass = 0; pass < 2; pass++) {
    for (uint16_t i = 0; i < table.count; i++) {
      const RegisterRow &r = table.rows[i];
      if (r.sigid == 0 || r.fn != fn || r.regaddr < start_reg ||
          static_cast<uint32_t>(r.regaddr) + r.nreg > end)
        continue;
      bool is_cap = r.sigid == SIG_CAPABILITY_BITMAP;
      if ((pass == 0) != is_cap)
        continue;
      if (!is_cap && this->row_filter_ && !this->row_filter_(table, r))
        continue;
      const uint8_t *p = data + 2 * (r.regaddr - start_reg);
      if (gate_capabilities && !is_cap && !this->capability_ok(device, r.dep_word, r.dep_bit)) {
        this->clear(device, r.sigid);
        continue;
      }
      if (r.sigid == SIG_FAULT_LIST && (table.family == Family::BATTERY || table.family == Family::INVERTER))
        decode_fault_list(*this, device, p, r.nreg);
      Decoded d = decode(p, r.nreg, r.type, word_order);
      // The capability bitmap is stored as a "string" but a leading 0 byte is normal there.
      // The raw copy is always kept (as stock).
      if (is_cap || (r.tag & TAG_KEEP_SENTINEL) != 0)
        d.present = true;
      if (d.present && d.kind == ValueKind::SCALAR &&
          absent_by_rule(*this, table.family, device, r.sigid, d.bits, r.nreg))
        d.present = false;
      if (!d.present) {
        // The parent is cleared; bitfield children are left as they were (as stock).
        this->clear(device, r.sigid);
      } else if (d.kind == ValueKind::SCALAR) {
        this->set_scalar(device, r.sigid, r.type, r.scale, d.bits, d.nbytes);
        if (r.nbits != 0 && this->expand_bitfields_)
          this->expand_bitfield_(device, table, r, d.bits);
      } else if (d.kind == ValueKind::FLOAT) {
        uint8_t b[4];
        for (uint8_t k = 0; k < 4; k++)
          b[k] = static_cast<uint8_t>(d.bits >> (8 * k));
        this->set(device, r.sigid, r.type, this->float_scale_ ? r.scale : 0.0f, ValueKind::FLOAT, b, 4);
      } else {
        this->set(device, r.sigid, r.type, r.scale, d.kind, p, d.nbytes);
      }
      n++;
    }
  }
  this->run_derived_(device, table, fn, start_reg, static_cast<uint16_t>(end > 0xFFFF ? 0xFFFF : end));
  return n;
}

size_t SignalStore::invalidate_block(uint8_t device, const RegisterTable &table, uint8_t fn,
                                     uint16_t start_reg, uint16_t nregs) {
  const uint32_t end = static_cast<uint32_t>(start_reg) + nregs;
  size_t n = 0;
  for (uint16_t i = 0; i < table.count; i++) {
    const RegisterRow &r = table.rows[i];
    if (r.sigid == 0 || r.fn != fn || r.regaddr < start_reg ||
        static_cast<uint32_t>(r.regaddr) + r.nreg > end)
      continue;
    this->clear(device, r.sigid);
    n++;
  }
  this->run_derived_(device, table, fn, start_reg, static_cast<uint16_t>(end > 0xFFFF ? 0xFFFF : end));
  return n;
}

void SignalStore::expand_bitfield_(uint8_t device, const RegisterTable &table, const RegisterRow &r,
                                   uint64_t bits) {
  const BitfieldDesc *bf = find_bitfield(table.id, r.sigid);
  if (bf == nullptr)
    return;
  for (uint8_t b = 0; b < bf->nbits; b++)
    this->set_scalar(device, static_cast<uint16_t>(bf->child_base + b), TYPE_U16, 0.0f, (bits >> b) & 1ULL, 2);
}

void SignalStore::clear_bitfield_(uint8_t device, const RegisterTable &table, const RegisterRow &r) {
  const BitfieldDesc *bf = find_bitfield(table.id, r.sigid);
  if (bf == nullptr)
    return;
  for (uint8_t b = 0; b < bf->nbits; b++) {
    uint16_t child = static_cast<uint16_t>(bf->child_base + b);
    if (this->get(device, child) != nullptr)
      this->clear(device, child);
  }
}

size_t SignalStore::clear_offline(uint8_t device, const RegisterTable &table) {
  size_t n = 0;
  for (uint16_t i = 0; i < table.count; i++) {
    const RegisterRow &r = table.rows[i];
    if (r.sigid == 0 || (r.tag & TAG_CLEAR_OFFLINE) == 0)
      continue;
    const Signal *s = this->get(device, r.sigid);
    if (s != nullptr) {
      this->clear(device, r.sigid);
      n++;
    }
    if (r.nbits != 0)
      this->clear_bitfield_(device, table, r);
  }
  for (uint8_t fn : {3, 4})
    this->run_derived_(device, table, fn, 0, 0xFFFF);
  return n;
}

void SignalStore::run_derived_(uint8_t device, const RegisterTable &table, uint8_t fn,
                               uint16_t start, uint16_t end) {
  if (table.family == Family::BATTERY && fn == 4 && start < SBR_CELL_BLOCK_START + SBR_CELL_BLOCK_COUNT &&
      end > SBR_CELL_BLOCK_START) {
    decode_sbr_cells(*this, device);
  }
  if (table.family == Family::BATTERY && fn == 4) {
    clear_absent_modules(*this, device);
    update_bms_request_flags(*this, device);
    update_module_dates(*this, device);
  }
  if (table.family == Family::INVERTER) {
    // Recompute only what the block touched (cheap scan over the block's rows).
    bool mppt = false, yield = false, yield_2024 = false, grid = false, hybrid = false;
    bool output_type = false, flow = false, state = false;
    for (uint16_t i = 0; i < table.count; i++) {
      const RegisterRow &r = table.rows[i];
      if (r.fn != fn || r.regaddr < start || r.regaddr >= end)
        continue;
      mppt |= is_mppt_sigid(r.sigid);
      yield |= r.sigid == SIG_YIELD_TOTAL_KWH || r.sigid == SIG_YIELD_TOTAL_01KWH;
      yield_2024 |= r.sigid == SIG_YIELD_TOTAL_ALT;
      grid |= r.sigid == SIG_EXPORT_POWER_SIGNED;
      hybrid |= is_hybrid_cloud_input(r.sigid);
      output_type |= r.sigid == SIG_OUTPUT_TYPE;
      flow |= r.sigid == SIG_POWER_FLOW_STATUS;
      state |= r.sigid == SIG_DEVICE_STATE;
    }
    if (output_type)
      apply_single_phase(*this, device);
    if (flow)
      update_power_flow(*this, device);
    if (state)
      update_state_flags(*this, device);
    if (mppt)
      update_mppt_total(*this, device);
    if (yield || yield_2024)
      update_total_yield(*this, device, yield_2024);
    if (grid)
      update_grid_power(*this, device);
    if (hybrid && inverter_model_family(this->dev_type(device)) == ModelFamily::HYBRID)
      update_hybrid_cloud(*this, device);
  }
}

}  // namespace sungrow_datamodel
}  // namespace esphome
