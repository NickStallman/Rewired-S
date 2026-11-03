#include "derived.h"

#include <cmath>
#include <cstdio>
#include <string>

#include "signal_store.h"

namespace esphome {
namespace sungrow_datamodel {

namespace {

// Offsets into the 24-register SBR cell block (10757..10780).
constexpr uint8_t OFF_VMAX = 0, OFF_VMAX_POS = 1, OFF_VMIN = 2, OFF_VMIN_POS = 3;
constexpr uint8_t OFF_TMAX = 4, OFF_TMAX_POS = 5, OFF_TMIN = 6, OFF_TMIN_POS = 7;
constexpr uint8_t OFF_MODULES = 8;  // 8..15 module n max, 16..23 module n min

constexpr uint16_t kSbrDerived[] = {SIG_CELL_VOLTAGE_MAX_MODULE,       SIG_CELL_VOLTAGE_MAX_CELL,
                                    SIG_CELL_VOLTAGE_MIN_MODULE,       SIG_CELL_VOLTAGE_MIN_CELL,
                                    SIG_MODULE_TEMPERATURE_MAX_MODULE, SIG_MODULE_TEMPERATURE_MIN_MODULE,
                                    SIG_CELL_VOLTAGE_SPREAD};

bool raw16(const SignalStore &st, uint8_t dev, uint8_t off, uint16_t *out) {
  uint64_t b;
  if (!st.get_bits(dev, kSbrCellSigids[off], &b))
    return false;
  *out = static_cast<uint16_t>(b);
  return true;
}

void clear_all(SignalStore &st, uint8_t dev) {
  for (uint16_t sig : kSbrCellSigids)
    st.clear(dev, sig);
  for (uint16_t sig : kSbrDerived)
    st.clear(dev, sig);
}

void set_pos(SignalStore &st, uint8_t dev, uint16_t sig, bool ok, uint8_t v) {
  if (ok && v != 0 && v != 0xFF)
    st.set_float(dev, sig, v);
  else
    st.clear(dev, sig);
}

}  // namespace

SbrStatus decode_sbr_cells(SignalStore &st, uint8_t dev) {
  uint16_t vmax, vmin;
  bool have_max = raw16(st, dev, OFF_VMAX, &vmax) && vmax != 0;
  bool have_min = raw16(st, dev, OFF_VMIN, &vmin) && vmin != 0;
  if (!have_max || !have_min) {
    clear_all(st, dev);
    return SbrStatus::ABSENT;
  }
  double max_v = st.get_number(dev, kSbrCellSigids[OFF_VMAX]);
  double min_v = st.get_number(dev, kSbrCellSigids[OFF_VMIN]);
  if (!sbr_cells_plausible(min_v, max_v)) {
    clear_all(st, dev);
    return SbrStatus::IMPLAUSIBLE;
  }
  uint16_t p;
  bool ok = raw16(st, dev, OFF_VMAX_POS, &p);
  set_pos(st, dev, SIG_CELL_VOLTAGE_MAX_MODULE, ok, p >> 8);
  set_pos(st, dev, SIG_CELL_VOLTAGE_MAX_CELL, ok, p & 0xFF);
  ok = raw16(st, dev, OFF_VMIN_POS, &p);
  set_pos(st, dev, SIG_CELL_VOLTAGE_MIN_MODULE, ok, p >> 8);
  set_pos(st, dev, SIG_CELL_VOLTAGE_MIN_CELL, ok, p & 0xFF);
  ok = raw16(st, dev, OFF_TMAX_POS, &p);
  set_pos(st, dev, SIG_MODULE_TEMPERATURE_MAX_MODULE, ok, p >> 8);
  ok = raw16(st, dev, OFF_TMIN_POS, &p);
  set_pos(st, dev, SIG_MODULE_TEMPERATURE_MIN_MODULE, ok, p >> 8);
  // Temperatures: s16 sentinel 0x7FFF is handled by the decoder; RegisterBlocks.ts also
  // treats raw 0xFFFF as absent.
  for (uint8_t off : {OFF_TMAX, OFF_TMIN}) {
    if (raw16(st, dev, off, &p) && p == 0xFFFF)
      st.clear(dev, kSbrCellSigids[off]);
  }
  // Per-module cell voltages: 0 = module not present.
  for (uint8_t off = OFF_MODULES; off < SBR_CELL_BLOCK_COUNT; off++) {
    if (raw16(st, dev, off, &p) && p == 0)
      st.clear(dev, kSbrCellSigids[off]);
  }
  st.set_float(dev, SIG_CELL_VOLTAGE_SPREAD, static_cast<float>(max_v - min_v));
  return SbrStatus::OK;
}

bool is_mppt_sigid(uint16_t sigid) {
  for (const auto &p : kMpptPairs) {
    if (p.voltage_sigid == sigid || p.current_sigid == sigid)
      return true;
  }
  return false;
}

void update_mppt_total(SignalStore &st, uint8_t dev) {
  double total = 0;
  bool any = false;
  for (const auto &p : kMpptPairs) {
    const uint16_t sig = static_cast<uint16_t>(SIG_MPPT_POWER_BASE + p.mppt);
    double v = st.get_number(dev, p.voltage_sigid);
    double i = st.get_number(dev, p.current_sigid);
    if (std::isnan(v) || std::isnan(i)) {
      if (st.get(dev, sig) != nullptr)
        st.clear(dev, sig);
      continue;
    }
    st.set_float(dev, sig, static_cast<float>(v * i));
    total += v * i;
    any = true;
  }
  if (any)
    st.set_float(dev, SIG_MPPT_TOTAL_POWER, static_cast<float>(total));
  else
    st.clear(dev, SIG_MPPT_TOTAL_POWER);
}

namespace {
// Store a u32 like the polled row would (type/scale of the first inverter-family row with that
// sigid, so number() and the Modbus-TCP slave see the usual raw units).
void set_u32_like_row(SignalStore &st, uint8_t dev, uint16_t sigid, uint64_t v) {
  uint8_t type = TYPE_U32;
  float scale = 0.0f;
  for (const auto &t : kTables) {
    if (t.family != Family::INVERTER)
      continue;
    for (uint16_t i = 0; i < t.count; i++) {
      if (t.rows[i].sigid == sigid) {
        type = t.rows[i].type;
        scale = t.rows[i].scale;
        goto found;
      }
    }
  }
found:
  st.set_scalar(dev, sigid, type, scale, v & 0xFFFFFFFFULL, 4);
}

void clear_if_present(SignalStore &st, uint8_t dev, uint16_t sigid) {
  if (st.get(dev, sigid) != nullptr)
    st.clear(dev, sigid);
}
}  // namespace

void update_total_yield(SignalStore &st, uint8_t dev, bool from_alt) {
  uint64_t v;
  if (from_alt) {
    if (st.get_bits(dev, SIG_YIELD_TOTAL_ALT, &v)) {
      st.set_scalar(dev, SIG_TOTAL_YIELD, TYPE_U64, 0.1f, v, 8);
      set_u32_like_row(st, dev, SIG_YIELD_TOTAL_01KWH, v);
      set_u32_like_row(st, dev, SIG_YIELD_TOTAL_KWH, v / 10);
    } else {
      clear_if_present(st, dev, SIG_TOTAL_YIELD);
      clear_if_present(st, dev, SIG_YIELD_TOTAL_KWH);
      clear_if_present(st, dev, SIG_YIELD_TOTAL_01KWH);
    }
    return;
  }
  if (st.get_bits(dev, SIG_YIELD_TOTAL_01KWH, &v) && v != 0) {
    st.set_scalar(dev, SIG_TOTAL_YIELD, TYPE_U64, 0.1f, v, 8);
  } else if (st.get_bits(dev, SIG_YIELD_TOTAL_KWH, &v)) {
    st.set_scalar(dev, SIG_TOTAL_YIELD, TYPE_U64, 0.1f, v * 10, 8);
  } else {
    clear_if_present(st, dev, SIG_TOTAL_YIELD);
    clear_if_present(st, dev, SIG_YIELD_TOTAL_KWH);
    clear_if_present(st, dev, SIG_YIELD_TOTAL_01KWH);
  }
}

void update_grid_power(SignalStore &st, uint8_t dev) {
  double p = st.get_number(dev, SIG_EXPORT_POWER_SIGNED);
  if (std::isnan(p)) {
    clear_if_present(st, dev, SIG_GRID_IMPORT_POWER);
    clear_if_present(st, dev, SIG_GRID_EXPORT_POWER);
    return;
  }
  st.set_float(dev, SIG_GRID_IMPORT_POWER, p < 0 ? static_cast<float>(-p) : 0.0f);
  st.set_float(dev, SIG_GRID_EXPORT_POWER, p > 0 ? static_cast<float>(p) : 0.0f);
}

const BitfieldDesc *find_bitfield(uint8_t table_id, uint16_t parent_sigid) {
  for (size_t i = 0; i < kBitfieldCount; i++) {
    if (kBitfields[i].table == table_id && kBitfields[i].parent_sigid == parent_sigid)
      return &kBitfields[i];
  }
  return nullptr;
}


namespace {
constexpr uint16_t kHybridInputs[] = {0x2173, 0x20F1, 0x2308, 0x2309, 0x230A, 0x230B, 0x230C, 0x230D, 0x230E,
                                      0x230F, 0x2032, 0x2080, 0x2502, 0x223A, 0x2514, 0x2540, 0x2182, 0x252C,
                                      0x2501, 0x207A, 0x2077, 0x2078, 0x2079, 0x2003, 0x208C, 0x2510};

// Raw copy of `src` into `dst` (type/len as given), or clear `dst` when `src` is invalid.
void copy_raw(SignalStore &st, uint8_t dev, uint16_t src, uint16_t dst, uint8_t type, uint8_t len) {
  uint64_t v;
  if (st.get_bits(dev, src, &v)) {
    st.set_scalar(dev, dst, type, 1.0f, v, len);
  } else {
    clear_if_present(st, dev, dst);
  }
}

// Split a signed W value into (negative part, positive part) as two u32 sigids.
void split_sign(SignalStore &st, uint8_t dev, uint16_t src, uint16_t neg_dst, uint16_t pos_dst) {
  uint64_t raw;
  if (!st.get_bits(dev, src, &raw)) {
    clear_if_present(st, dev, neg_dst);
    clear_if_present(st, dev, pos_dst);
    return;
  }
  const int32_t v = static_cast<int32_t>(static_cast<uint32_t>(raw));
  const uint32_t mag = v < 0 ? static_cast<uint32_t>(-static_cast<int64_t>(v)) : static_cast<uint32_t>(v);
  st.set_scalar(dev, neg_dst, TYPE_U32, 1.0f, v < 0 ? mag : 0, 4);
  st.set_scalar(dev, pos_dst, TYPE_U32, 1.0f, v < 0 ? 0 : mag, 4);
}
}  // namespace

void clear_absent_modules(SignalStore &st, uint8_t dev) {
  for (uint16_t n = 0; n < 8; n++) {
    uint64_t type = 0;
    const uint16_t type_sig = static_cast<uint16_t>(0x2038 + 3 * n);
    if (!st.get_bits(dev, type_sig, &type) || type != 0)
      continue;
    // Empty slot: none of its per-module signals carries a value (no HA entities for it).
    clear_if_present(st, dev, static_cast<uint16_t>(type_sig - 2));  // cell voltage max
    clear_if_present(st, dev, static_cast<uint16_t>(type_sig - 1));  // cell voltage min
    clear_if_present(st, dev, type_sig);
    clear_if_present(st, dev, static_cast<uint16_t>(0x2073 + n));    // SOC
  }
}

namespace {
void set_string(SignalStore &st, uint8_t dev, uint16_t sigid, const std::string &text) {
  st.set(dev, sigid, TYPE_STRING, 0.0f, ValueKind::STRING, reinterpret_cast<const uint8_t *>(text.data()),
         static_cast<uint8_t>(text.size()));
}
}  // namespace

// The fault / alarm code names come from the generated fault_code_text() (register_tables.cpp,
// one table per family); codes without a name are shown numeric with their class word.
void decode_fault_list(SignalStore &st, uint8_t dev, const uint8_t *wire, uint8_t nreg) {
  if (wire == nullptr || nreg < FAULT_LIST_NREG)
    return;
  auto reg = [wire](uint8_t i) -> uint16_t {
    return static_cast<uint16_t>((wire[2 * i] << 8) | wire[2 * i + 1]);
  };
  const uint16_t count_raw = reg(4);
  if (count_raw == 0xFFFF) {
    clear_if_present(st, dev, SIG_ACTIVE_FAULT_COUNT);
    clear_if_present(st, dev, SIG_ACTIVE_FAULT_CODES);
    clear_if_present(st, dev, SIG_FIRST_FAULT);
    return;
  }
  const uint16_t count = count_raw > 10 ? 10 : count_raw;
  std::string text, first;
  for (uint16_t i = 0; i < count; i++) {
    const uint16_t code = reg(static_cast<uint8_t>(5 + 2 * i));
    const uint16_t cls = reg(static_cast<uint8_t>(6 + 2 * i));
    char buf[48];
    const char *name = fault_code_text(code);
    if (name != nullptr)
      std::snprintf(buf, sizeof(buf), "%u %s", code, name);
    else if (cls == 25)
      std::snprintf(buf, sizeof(buf), "%u fault", code);
    else if (cls == 24)
      std::snprintf(buf, sizeof(buf), "%u alarm", code);
    else
      std::snprintf(buf, sizeof(buf), "%u (class %u)", code, cls);
    if (text.empty())
      first = buf;
    else
      text += ", ";
    text += buf;
  }
  if (text.empty())
    text = first = "No active faults";
  st.set_scalar(dev, SIG_ACTIVE_FAULT_COUNT, TYPE_U16, 0.0f, count, 2);
  set_string(st, dev, SIG_ACTIVE_FAULT_CODES, text);
  set_string(st, dev, SIG_FIRST_FAULT, first);
}

void update_state_flags(SignalStore &st, uint8_t dev) {
  uint64_t code;
  if (!st.get_bits(dev, SIG_DEVICE_STATE, &code)) {
    clear_if_present(st, dev, SIG_STATE_TRANSIENT);
    return;
  }
  const bool string_family = inverter_model_family(st.dev_type(dev)) == ModelFamily::STRING;
  st.set_scalar(dev, SIG_STATE_TRANSIENT, TYPE_U16, 0.0f,
                state_transient(static_cast<uint16_t>(code), string_family) ? 1u : 0u, 2);
}

void update_bms_request_flags(SignalStore &st, uint8_t dev) {
  static constexpr uint16_t kOut[3] = {SIG_CHARGE_PROHIBITED, SIG_DISCHARGE_PROHIBITED,
                                       SIG_FORCED_CHARGE_REQUEST};
  uint64_t flags = 0;
  const bool ok = st.get_bits(dev, SIG_BMS_REQUEST_FLAGS, &flags);
  for (uint8_t b = 0; b < 3; b++) {
    if (ok)
      st.set_scalar(dev, kOut[b], TYPE_U16, 0.0f, (flags >> b) & 1u, 2);
    else
      clear_if_present(st, dev, kOut[b]);
  }
}

namespace {
bool is_single_phase(const SignalStore &st, uint8_t dev) {
  uint64_t t;
  return st.get_bits(dev, SIG_OUTPUT_TYPE, &t) && t == 0;
}
bool has_meter(const SignalStore &st, uint8_t dev) {
  return st.is_valid(dev, SIG_METER_ACTIVE_POWER) || st.is_valid(dev, SIG_METER_ACTIVE_POWER_SG);
}
}  // namespace

bool absent_by_rule(const SignalStore &st, Family family, uint8_t dev, uint16_t sigid, uint64_t bits,
                    uint8_t nreg) {
  const uint64_t mask = nreg >= 2 ? 0xFFFFFFFFULL : 0xFFFFULL;
  for (const auto &r : kAbsentRules) {
    if (r.family != family || r.sigid != sigid)
      continue;
    switch (r.kind) {
      case ABSENT_RAW:
        if ((bits & mask) == r.raw)
          return true;
        break;
      case ABSENT_SINGLE_PHASE:
        if (is_single_phase(st, dev))
          return true;
        break;
      case ABSENT_NO_METER:
        if (!has_meter(st, dev))
          return true;
        break;
    }
  }
  return false;
}

void apply_single_phase(SignalStore &st, uint8_t dev) {
  if (!is_single_phase(st, dev))
    return;
  for (const auto &r : kAbsentRules) {
    if (r.family == Family::INVERTER && r.kind == ABSENT_SINGLE_PHASE)
      clear_if_present(st, dev, r.sigid);
  }
}

std::string power_flow_text(uint16_t flags) {
  static const char *const kBits[8] = {"PV generating",     "Battery charging",    "Battery discharging",
                                       "Load consuming",    "Exporting to grid",   "Importing from grid",
                                       nullptr,             "Load feeding back"};
  std::string text;
  for (uint8_t b = 0; b < 16; b++) {
    if ((flags >> b & 1u) == 0)
      continue;
    char buf[8];
    const char *label = b < 8 ? kBits[b] : nullptr;
    if (label == nullptr) {
      std::snprintf(buf, sizeof(buf), "bit %u", b);
      label = buf;
    }
    if (!text.empty())
      text += ", ";
    text += label;
  }
  return text.empty() ? std::string("Idle") : text;
}

void update_power_flow(SignalStore &st, uint8_t dev) {
  uint64_t flags;
  if (!st.get_bits(dev, SIG_POWER_FLOW_STATUS, &flags)) {
    clear_if_present(st, dev, SIG_POWER_FLOW);
    return;
  }
  const std::string text = power_flow_text(static_cast<uint16_t>(flags));
  st.set(dev, SIG_POWER_FLOW, TYPE_STRING, 0.0f, ValueKind::STRING, reinterpret_cast<const uint8_t *>(text.data()),
         static_cast<uint8_t>(text.size()));
}

std::string module_date_text(uint64_t code) {
  if (code == 0 || code > 99999999u)
    return "";
  const unsigned yy = static_cast<unsigned>(code / 1000000u);
  const unsigned mm = static_cast<unsigned>(code / 10000u % 100u);
  const unsigned dd = static_cast<unsigned>(code / 100u % 100u);
  if (mm < 1 || mm > 12 || dd < 1 || dd > 31)
    return "";
  char buf[12];
  std::snprintf(buf, sizeof(buf), "20%02u-%02u-%02u", yy, mm, dd);
  return buf;
}

void update_module_dates(SignalStore &st, uint8_t dev) {
  for (uint16_t m = 0; m < 8; m++) {
    const uint16_t out = static_cast<uint16_t>(SIG_MODULE_MANUFACTURING_DATE_BASE + m);
    uint64_t code = 0;
    std::string text;
    if (st.get_bits(dev, static_cast<uint16_t>(SIG_MODULE_DATE_CODE_BASE + m), &code))
      text = module_date_text(code);
    if (text.empty())
      clear_if_present(st, dev, out);
    else
      st.set(dev, out, TYPE_STRING, 0.0f, ValueKind::STRING,
             reinterpret_cast<const uint8_t *>(text.data()), static_cast<uint8_t>(text.size()));
  }
}

bool is_hybrid_cloud_input(uint16_t sigid) {
  for (uint16_t s : kHybridInputs)
    if (s == sigid)
      return true;
  return false;
}

void update_hybrid_cloud(SignalStore &st, uint8_t dev) {
  uint64_t fe = 0, fw = 0;
  const bool bit22 = st.get_bits(dev, 0x2173, &fe) && (fe & (1u << 22)) != 0;
  const bool alt = bit22 && st.get_bits(dev, 0x20F1, &fw) && fw > 0x010004FFu;
  // Plant powers + SOC.
  copy_raw(st, dev, alt ? 0x2308 : 0x2032, 0x2320, TYPE_U32, 4);
  copy_raw(st, dev, alt ? 0x2309 : 0x2080, 0x2321, TYPE_S32, 4);
  copy_raw(st, dev, alt ? 0x230A : 0x2502, 0x2322, TYPE_S32, 4);
  copy_raw(st, dev, alt ? 0x230D : 0x223A, 0x2323, TYPE_S32, 4);
  copy_raw(st, dev, alt ? 0x230F : 0x2514, 0x2324, TYPE_U16, 2);
  copy_raw(st, dev, alt ? 0x230C : 0x2540, 0x2325, TYPE_S32, 4);
  copy_raw(st, dev, alt ? 0x230C : 0x2182, 0x2326, TYPE_S32, 4);  // stock reads 0x230C for both
  // Battery and grid splits (bit 22 alone selects the source).
  split_sign(st, dev, bit22 ? 0x230E : 0x252C, 0x5402, 0x5403);
  split_sign(st, dev, bit22 ? 0x230B : 0x2501, 0x5400, 0x5401);
  // Phase-A current, frequency, 0x2536.
  copy_raw(st, dev, 0x207A, 0x5008, TYPE_S16, 2);
  copy_raw(st, dev, 0x208C, 0x2083, TYPE_U32, 4);
  copy_raw(st, dev, 0x2510, 0x2536, TYPE_U16, 2);
  // Phase voltages by output type.
  uint64_t ot = 0;
  st.get_bits(dev, 0x2003, &ot);
  for (uint16_t i = 0; i < 3; i++) {
    const uint16_t src = static_cast<uint16_t>(0x2077 + i);
    if (ot == 2) {
      copy_raw(st, dev, src, static_cast<uint16_t>(0x2071 + i), TYPE_U16, 2);
      clear_if_present(st, dev, static_cast<uint16_t>(0x2074 + i));
    } else if (ot == 1) {
      copy_raw(st, dev, src, static_cast<uint16_t>(0x2074 + i), TYPE_U16, 2);
      clear_if_present(st, dev, static_cast<uint16_t>(0x2071 + i));
    } else {
      clear_if_present(st, dev, static_cast<uint16_t>(0x2071 + i));
      if (i == 0)
        copy_raw(st, dev, src, 0x2074, TYPE_U16, 2);
      else
        clear_if_present(st, dev, static_cast<uint16_t>(0x2074 + i));
    }
  }
}

}  // namespace sungrow_datamodel
}  // namespace esphome
