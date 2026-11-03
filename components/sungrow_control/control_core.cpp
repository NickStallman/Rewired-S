#include "control_core.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome {
namespace sungrow_control {

using sungrow_modbus::WriteOptions;

static constexpr uint8_t EX_ILLEGAL_FUNCTION = 0x01;
static constexpr uint8_t EX_ILLEGAL_ADDRESS = 0x02;
static constexpr uint8_t EX_ILLEGAL_VALUE = 0x03;
static constexpr uint8_t EX_DEVICE_FAILURE = 0x04;
static constexpr uint8_t EX_BUSY = 0x06;
static constexpr uint8_t EX_GATEWAY_TARGET = 0x0B;
/// A read-back right after a write waits this long (on top of the master's settle gap).
static constexpr uint32_t REFRESH_DELAY_MS = 0;

std::vector<Rule> profile_rules(const ProfileDef &p, uint8_t protocol) {
  std::vector<Rule> out;
  for (uint8_t i = 0; i < p.nrules; i++) {
    const RuleDef &d = p.rules[i];
    Rule r;
    r.address = rule_address(d, protocol);
    r.sigid = d.sigid;
    r.count = 1;
    r.type = d.type;
    r.min = d.min;
    r.max = d.max;
    r.values.assign(d.values, d.values + d.nvalues);
    r.key = d.key;
    out.push_back(std::move(r));
  }
  return out;
}

const char *verdict_name(Verdict v) {
  switch (v) {
    case Verdict::OK:
      return "ok";
    case Verdict::DISABLED:
      return "writes disabled";
    case Verdict::NOT_ALLOWED:
      return "register not in allow-list";
    case Verdict::BAD_VALUE:
      return "value out of range";
    case Verdict::OFFLINE:
      return "device offline";
    case Verdict::BUSY:
      return "busy";
    case Verdict::INVALID:
      return "invalid request";
    case Verdict::PREVIEW_REQUIRED:
      return "no fresh preview of this setup";
  }
  return "?";
}

const char *verdict_code(Verdict v) {
  switch (v) {
    case Verdict::OK:
      return "OK";
    case Verdict::DISABLED:
      return "DISABLED";
    case Verdict::NOT_ALLOWED:
      return "NOT_ALLOWED";
    case Verdict::BAD_VALUE:
      return "BAD_VALUE";
    case Verdict::OFFLINE:
      return "OFFLINE";
    case Verdict::BUSY:
      return "BUSY";
    case Verdict::INVALID:
      return "INVALID";
    case Verdict::PREVIEW_REQUIRED:
      return "PREVIEW_REQUIRED";
  }
  return "?";
}

const char *read_reason_name(ReadReason r) {
  switch (r) {
    case ReadReason::OK:
      return "ok";
    case ReadReason::OFF_LIST:
      return "off-list";
    case ReadReason::EXCEPTION:
      return "exception";
    case ReadReason::TIMEOUT:
      return "timeout";
    case ReadReason::PENDING:
      return "pending";
  }
  return "?";
}

const char *frame_status_name(WriteStatus s, bool expired) {
  if (expired)
    return "expired";
  switch (s) {
    case WriteStatus::QUEUED:
      return "pending";
    case WriteStatus::OK:
      return "ok";
    case WriteStatus::EXCEPTION:
      return "exception";
    case WriteStatus::TIMEOUT:
      return "timeout";
    case WriteStatus::VERIFY_MISMATCH:
      return "verify_mismatch";
    case WriteStatus::VERIFY_FAILED:
      return "verify_failed";
    case WriteStatus::OFFLINE:
      return "offline";
    case WriteStatus::RATE_LIMITED:
      return "rate_limited";
    case WriteStatus::QUEUE_FULL:
      return "queue_full";
    case WriteStatus::INVALID:
      return "invalid";
  }
  return "?";
}

static int32_t as_typed(uint16_t raw, uint8_t type) {
  return type == sungrow_datamodel::TYPE_S16 ? static_cast<int32_t>(static_cast<int16_t>(raw))
                                             : static_cast<int32_t>(raw);
}

Check check_rules(const std::vector<Rule> &rules, uint16_t start, const uint16_t *values, uint16_t count) {
  Check c;
  if (count == 0 || count > sungrow_modbus::MAX_WRITE_REGS || values == nullptr || start == 0 ||
      static_cast<uint32_t>(start) + count > 0x10000u) {
    c.verdict = Verdict::INVALID;
    return c;
  }
  for (uint16_t i = 0; i < count; i++) {
    const uint16_t reg = static_cast<uint16_t>(start + i);
    const Rule *hit = nullptr;
    for (const auto &r : rules)
      if (reg >= r.address && static_cast<uint32_t>(reg) < static_cast<uint32_t>(r.address) + r.count) {
        hit = &r;
        break;
      }
    if (hit == nullptr) {
      c.verdict = Verdict::NOT_ALLOWED;
      c.reg = reg;
      return c;
    }
    const int32_t v = as_typed(values[i], hit->type);
    bool ok = v >= hit->min && v <= hit->max;
    if (ok && !hit->values.empty()) {
      ok = false;
      for (uint16_t a : hit->values)
        ok |= a == values[i];
    }
    if (!ok) {
      c.verdict = Verdict::BAD_VALUE;
      c.reg = reg;
      c.key = hit->key;
      return c;
    }
  }
  return c;
}

uint8_t exception_for(Verdict v) {
  switch (v) {
    case Verdict::OK:
      return 0;
    case Verdict::DISABLED:
      return EX_ILLEGAL_FUNCTION;  // same as the read-only server: "writes not supported"
    case Verdict::NOT_ALLOWED:
      return EX_ILLEGAL_ADDRESS;
    case Verdict::BAD_VALUE:
    case Verdict::INVALID:
    case Verdict::PREVIEW_REQUIRED:
      return EX_ILLEGAL_VALUE;
    case Verdict::OFFLINE:
      return EX_GATEWAY_TARGET;  // as reads of a unit whose device is not ready
    case Verdict::BUSY:
      return EX_BUSY;
  }
  return EX_DEVICE_FAILURE;
}

uint8_t exception_for(const WriteResult &r) {
  switch (r.status) {
    case WriteStatus::OK:
      return 0;
    case WriteStatus::EXCEPTION:
      // Any failed item -> exception 4; the inverter's own exception code is not forwarded (as
      // stock; the code is in the log / last_write).
      return EX_DEVICE_FAILURE;
    case WriteStatus::OFFLINE:
      return EX_GATEWAY_TARGET;
    case WriteStatus::RATE_LIMITED:
    case WriteStatus::QUEUE_FULL:
      return EX_BUSY;
    case WriteStatus::INVALID:
      return EX_ILLEGAL_VALUE;
    default:
      return EX_DEVICE_FAILURE;  // timeout, verify failures
  }
}

// ---- value mapping ----------------------------------------------------------------------------

bool option_to_raw(const std::vector<Option> &opts, const std::string &text, uint16_t *raw) {
  for (const auto &o : opts)
    if (text == o.text) {
      *raw = o.raw;
      return true;
    }
  return false;
}

const char *raw_to_option(const std::vector<Option> &opts, uint16_t raw) {
  for (const auto &o : opts)
    if (o.raw == raw)
      return o.text;
  return nullptr;
}

bool number_to_raw(float value, float scale, uint8_t type, int32_t min, int32_t max, uint16_t *raw) {
  if (!std::isfinite(value) || scale == 0.0f)
    return false;
  const double r = std::round(static_cast<double>(value) / static_cast<double>(scale));
  if (r < min || r > max)
    return false;
  const int32_t v = static_cast<int32_t>(r);
  if (type == sungrow_datamodel::TYPE_S16) {
    if (v < -32768 || v > 32767)
      return false;
  } else if (v < 0 || v > 0xFFFF) {
    return false;
  }
  *raw = static_cast<uint16_t>(v & 0xFFFF);
  return true;
}

float raw_to_number(uint16_t raw, uint8_t type, float scale) {
  return static_cast<float>(as_typed(raw, type) * static_cast<double>(scale));
}

std::string format_write(uint16_t start, const uint16_t *values, uint16_t count) {
  char buf[96];
  int n = std::snprintf(buf, sizeof(buf), "%u=[", start);
  for (uint16_t i = 0; i < count && i < 8 && n > 0 && n < (int) sizeof(buf); i++)
    n += std::snprintf(buf + n, sizeof(buf) - n, values[i] > 9 ? "%s0x%X" : "%s%u", i ? ", " : "", values[i]);
  if (n > 0 && n < (int) sizeof(buf))
    std::snprintf(buf + n, sizeof(buf) - n, "%s]", count > 8 ? ", ..." : "");
  return buf;
}

// ---- ControlCore ------------------------------------------------------------------------------

void ControlCore::log_(int level, const char *fmt, ...) const {
  if (!this->log_fn_)
    return;
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  this->log_fn_(level, buf);
}

const ReadbackDef *ControlCore::readback_(const Dev &d) const {
  return d.cfg.profile != nullptr ? &profile_readback(*d.cfg.profile, d.cfg.protocol) : nullptr;
}

void ControlCore::add_device(DeviceCfg cfg) {
  Dev d;
  d.cfg = std::move(cfg);
  const ReadbackDef *rb = this->readback_(d);
  if (rb != nullptr)
    d.blocks.resize(rb->nblocks);
  this->devs_.push_back(std::move(d));
}

ControlCore::DeviceCfg *ControlCore::device(uint8_t index) {
  Dev *d = this->find_(index);
  return d != nullptr ? &d->cfg : nullptr;
}

void ControlCore::bind_protocol(uint8_t index, uint8_t protocol) {
  Dev *d = this->find_(index);
  if (d == nullptr || d->cfg.profile == nullptr || d->cfg.protocol == protocol)
    return;
  const ProfileDef &p = *d->cfg.profile;
  for (Rule &r : d->cfg.rules) {
    if (r.sigid == 0)
      continue;  // YAML extra: fixed address
    for (uint8_t i = 0; i < p.nrules; i++)
      if (p.rules[i].sigid == r.sigid)
        r.address = rule_address(p.rules[i], protocol);
  }
  d->cfg.protocol = protocol;
  d->blocks.assign(this->readback_(*d)->nblocks, BlockState{});
  this->log_(5, "dev %u: '%s' control profile bound to protocol %u", index, p.name, protocol);
}

uint16_t ControlCore::address_for_sigid(uint8_t device, uint16_t sigid, uint16_t fallback) const {
  const Dev *d = this->find_(device);
  if (d != nullptr && sigid != 0)
    for (const Rule &r : d->cfg.rules)
      if (r.sigid == sigid)
        return r.address;
  return fallback;
}

bool ControlCore::address_for_key(uint8_t device, const char *key, uint16_t *address) const {
  const Dev *d = this->find_(device);
  if (d == nullptr || key == nullptr || key[0] == '\0')
    return false;
  for (const Rule &r : d->cfg.rules)
    if (std::strcmp(r.key, key) == 0) {
      *address = r.address;
      return true;
    }
  return false;
}

const char *ControlCore::profile_name(uint8_t device) const {
  const Dev *d = this->find_(device);
  return d != nullptr && d->cfg.profile != nullptr ? d->cfg.profile->name : nullptr;
}

DeviceIdentity ControlCore::identity(uint8_t device) const {
  if (this->family_fn_)
    return this->family_fn_(device);
  DeviceIdentity id;
  if (this->master_ == nullptr)
    return id;
  for (const auto &d : this->master_->devices())
    if (d.config().index == device) {
      id.known = true;
      id.family = d.xml_family();
      id.desc = d.descriptor();
      id.dev_code = d.devinfo().dev_code;
      id.dev_type = d.devinfo().dev_type;
      break;
    }
  return id;
}

const ControlCore::Dev *ControlCore::find_(uint8_t index) const {
  for (const auto &d : this->devs_)
    if (d.cfg.index == index)
      return &d;
  return nullptr;
}

ControlCore::Dev *ControlCore::find_(uint8_t index) {
  for (auto &d : this->devs_)
    if (d.cfg.index == index)
      return &d;
  return nullptr;
}

uint8_t ControlCore::address_of(uint8_t device) const {
  if (this->master_ == nullptr)
    return 0;
  for (const auto &d : this->master_->devices())
    if (d.config().index == device)
      return d.config().address;
  return 0;
}

bool ControlCore::online_(uint8_t index) const {
  const uint8_t a = this->address_of(index);
  return a != 0 && this->master_->can_write(a);
}

bool ControlCore::reply_was_exception_(uint8_t device) const {
  // Called from inside an ad-hoc read callback: the master counts an exception reply as a link
  // success (consecutive_failures = 0) and a timeout / CRC failure as a failure before it.
  if (this->master_ == nullptr)
    return false;
  for (const auto &d : this->master_->devices())
    if (d.config().index == device)
      return d.stats().consecutive_failures == 0;
  return false;
}

void ControlCore::set_enabled(bool on) {
  if (on == this->enabled_)
    return;
  this->enabled_ = on;
  this->log_(3, "register writes %s", on ? "ENABLED" : "disabled");
}

void ControlCore::report_(uint8_t device, const char *text, bool ok) {
  if (this->listener_)
    this->listener_(device, text, ok);
}

bool ControlCore::feature_w32b5(uint8_t device) const {
  return this->store_ != nullptr && this->store_->capability_ok(device, 32, 5);
}

bool ControlCore::feature_w29b12(uint8_t device) const {
  return this->store_ != nullptr && this->store_->capability_ok(device, 29, 12);
}

// ---- gate ---------------------------------------------------------------------------------------

Check ControlCore::check(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count) const {
  Check c;
  const Dev *d = this->find_(device);
  if (!this->enabled_) {
    c.verdict = Verdict::DISABLED;
    return c;
  }
  if (d == nullptr) {
    // A device without a sungrow_control entry has an empty allow-list.
    c = check_rules({}, start, values, count);
    return c;
  }
  c = check_rules(d->cfg.rules, start, values, count);
  if (c.verdict != Verdict::OK)
    return c;
  if (!this->online_(device))
    c.verdict = Verdict::OFFLINE;
  return c;
}

Verdict ControlCore::write(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count, bool verify,
                           const char *source, Callback cb, WriteOptions opt, bool authorized) {
  Check c;
  if (authorized) {
    const Dev *d = this->find_(device);
    c = check_rules(d != nullptr ? d->cfg.rules : std::vector<Rule>{}, start, values, count);
    if (c.verdict == Verdict::OK && !this->online_(device))
      c.verdict = Verdict::OFFLINE;
  } else {
    c = this->check(device, start, values, count);
  }
  return this->submit_(device, start, values, count, verify, source, std::move(cb), opt, c, authorized);
}

Verdict ControlCore::write_clock(uint8_t device, const uint16_t fields[6], const char *source, Callback cb) {
  static const std::vector<Rule> CLOCK_RULES = [] {
    static constexpr int32_t LIMITS[6][2] = {{2000, 2099}, {1, 12}, {1, 31}, {0, 23}, {0, 59}, {0, 59}};
    std::vector<Rule> r;
    for (uint16_t i = 0; i < 6; i++) {
      Rule x;
      x.address = static_cast<uint16_t>(CLOCK_ADDRESS + i);
      x.min = LIMITS[i][0];
      x.max = LIMITS[i][1];
      x.key = "inverter_clock";
      r.push_back(x);
    }
    return r;
  }();
  Check c;
  const Dev *d = this->find_(device);
  if (d == nullptr || d->cfg.profile == nullptr) {
    c.verdict = Verdict::NOT_ALLOWED;
    c.reg = CLOCK_ADDRESS;
  } else {
    c = check_rules(CLOCK_RULES, CLOCK_ADDRESS, fields, 6);
    if (c.verdict == Verdict::OK && !this->online_(device))
      c.verdict = Verdict::OFFLINE;
  }
  WriteOptions opt;
  opt.fn = sungrow_modbus::FN_WRITE_MULTIPLE;
  return this->submit_(device, CLOCK_ADDRESS, fields, 6, false, source, std::move(cb), opt, c);
}

Verdict ControlCore::write_diag(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count,
                                const char *source, Callback cb) {
  static const std::vector<Rule> DIAG_RULES = [] {
    std::vector<Rule> r;
    Rule fmt;
    fmt.address = DIAG_FORMAT_REG;
    fmt.max = 0;
    fmt.key = "export_format";
    r.push_back(fmt);
    Rule cmd;
    cmd.address = DIAG_COMMAND_REG;
    cmd.values = {0x01A0, 0x00A0, 0x01B0, 0x01A1, 0x00B0};
    cmd.key = "diag_command";
    r.push_back(cmd);
    Rule scan;
    scan.address = DIAG_FAULT_RECORD_REG;
    scan.values = {0x00AA};
    scan.key = "fault_record_scan";
    r.push_back(scan);
    Rule arm;
    arm.address = DIAG_GRID_WAVE_REG;
    arm.values = {0x00AA};
    arm.key = "grid_wave_arm";
    r.push_back(arm);
    Rule cycles;
    cycles.address = DIAG_GRID_WAVE_REG + 1;
    cycles.min = 1;
    cycles.max = 8;
    cycles.key = "grid_wave_cycles";
    r.push_back(cycles);
    return r;
  }();
  Check c;
  const Dev *d = this->find_(device);
  if (d == nullptr || d->cfg.profile == nullptr) {
    c.verdict = Verdict::NOT_ALLOWED;
    c.reg = start;
  } else {
    c = check_rules(DIAG_RULES, start, values, count);
    if (c.verdict == Verdict::OK && !this->online_(device))
      c.verdict = Verdict::OFFLINE;
  }
  WriteOptions opt;
  opt.fn = sungrow_modbus::FN_WRITE_MULTIPLE;
  return this->submit_(device, start, values, count, false, source, std::move(cb), opt, c);
}

Verdict ControlCore::submit_(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count, bool verify,
                             const char *source, Callback cb, WriteOptions opt, const Check &c, bool authorized) {
  const std::string what = values != nullptr && count > 0 ? format_write(start, values, count) : std::string("?");
  const std::string src = source != nullptr ? source : "?";
  Verdict v = c.verdict;
  if (v == Verdict::OK) {
    opt.verify = verify;
    auto self = this;
    const uint8_t dev = device;
    // Follow-ups once the write took effect: a start/stop expectation, the power-limit memo.
    const Dev *d = this->find_(device);
    const char *ekey = "";
    const ExpectKind ekind = d != nullptr ? this->expect_kind_for_(*d, start, values, count, &ekey) : ExpectKind::NONE;
    uint16_t pl_sw = 0, pl_pct = 0;
    const bool plimit = d != nullptr && this->power_limit_regs_(*d, &pl_sw, &pl_pct) && start <= pl_pct &&
                        pl_sw < static_cast<uint32_t>(start) + count;
    std::vector<uint16_t> vals;
    if (plimit)
      vals.assign(values, values + count);
    WriteStatus st = this->master_->submit_write(
        this->address_of(device), start, values, count, opt,
        [self, dev, what, src, cb, ekind, ekey, plimit, vals, authorized](const WriteResult &r) {
          const bool ok = r.status == WriteStatus::OK;
          if (ok)
            self->ok_++;
          else
            self->failed_++;
          char text[160];
          if (r.status == WriteStatus::EXCEPTION)
            std::snprintf(text, sizeof(text), "dev %u %s (%s): exception %u", dev, what.c_str(), src.c_str(),
                          r.exception);
          else
            std::snprintf(text, sizeof(text), "dev %u %s (%s): %s", dev, what.c_str(), src.c_str(),
                          sungrow_modbus::write_status_name(r.status));
          self->log_(ok ? 3 : 2, "write %s", text);
          if (!r.readback.empty())
            self->ingest_(dev, r.start, r.readback.data(), static_cast<uint16_t>(r.readback.size()));
          // Re-read the written range now (also after a failure: show what the inverter holds).
          self->refresh(dev, r.start, r.count);
          self->report_(dev, text, ok);
          if (ok) {
            Dev *dd = self->find_(dev);
            if (plimit && dd != nullptr)
              self->capture_power_limit_(*dd, r.start, vals, src, authorized);
            if (ekind != ExpectKind::NONE)
              self->begin_expect_(dev, ekind, ekey);
          }
          if (cb)
            cb(r);
        });
    switch (st) {
      case WriteStatus::QUEUED:
        this->log_(3, "write dev %u %s (%s%s): queued", device, what.c_str(), src.c_str(), verify ? ", verify" : "");
        return Verdict::OK;
      case WriteStatus::OFFLINE:
        v = Verdict::OFFLINE;
        break;
      case WriteStatus::INVALID:
        v = Verdict::INVALID;
        break;
      default:
        v = Verdict::BUSY;
        break;
    }
  }
  this->rejected_++;
  char text[160];
  if (v == Verdict::NOT_ALLOWED || v == Verdict::BAD_VALUE)
    std::snprintf(text, sizeof(text), "dev %u %s (%s): rejected, %s (register %u%s%s)", device, what.c_str(),
                  src.c_str(), verdict_name(v), c.reg, c.key[0] ? " " : "", c.key);
  else
    std::snprintf(text, sizeof(text), "dev %u %s (%s): rejected, %s", device, what.c_str(), src.c_str(),
                  verdict_name(v));
  this->log_(v == Verdict::DISABLED ? 3 : 2, "write %s", text);
  this->report_(device, text, false);
  return v;
}

Check ControlCore::check_grid_code(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count) const {
  Check c;
  if (count == 0 || count > sungrow_modbus::MAX_WRITE_REGS || values == nullptr || start == 0 ||
      static_cast<uint32_t>(start) + count > 0x10000u) {
    c.verdict = Verdict::INVALID;
    return c;
  }
  const XmlFamily fam = this->identity(device).family;
  for (uint16_t i = 0; i < count; i++) {
    const uint16_t reg = static_cast<uint16_t>(start + i);
    if (!installer_addr_allowed(fam, reg)) {
      c.verdict = Verdict::NOT_ALLOWED;
      c.reg = reg;
      c.key = "grid_code";
      return c;
    }
  }
  return c;
}

Verdict ControlCore::write_grid_code_(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count,
                                      const char *source, Callback cb) {
  const std::string what = values != nullptr && count > 0 ? format_write(start, values, count) : std::string("?");
  const std::string src = source != nullptr ? source : "?";
  Check c = this->check_grid_code(device, start, values, count);
  Verdict v = c.verdict;
  if (v == Verdict::OK && !this->online_(device))
    v = Verdict::OFFLINE;
  if (v == Verdict::OK) {
    WriteOptions opt;
    opt.fn = sungrow_modbus::FN_WRITE_MULTIPLE;  // stock applies the protection params as fn 16
    opt.verify = true;
    auto self = this;
    const uint8_t dev = device;
    std::vector<uint16_t> vals(values, values + count);
    WriteStatus st = this->master_->submit_write(
        this->address_of(device), start, values, count, opt,
        [self, dev, what, src, cb, vals](const WriteResult &r) {
          const bool ok = r.status == WriteStatus::OK;
          if (ok)
            self->ok_++;
          else
            self->failed_++;
          if (ok && self->gr_.device == dev) {
            // Keep the grid-read cache in step with what the inverter now holds.
            for (auto &e : self->gr_.vals)
              if (e.fn == sungrow_modbus::FN_READ_HOLDING && e.address >= r.start && e.address < r.start + vals.size()) {
                e.value = vals[e.address - r.start];
                e.valid = true;
                e.reason = ReadReason::OK;
              }
          }
          char t[180];
          if (r.status == WriteStatus::EXCEPTION)
            std::snprintf(t, sizeof(t), "grid-code dev %u %s (%s): exception %u", dev, what.c_str(), src.c_str(),
                          r.exception);
          else
            std::snprintf(t, sizeof(t), "grid-code dev %u %s (%s): %s", dev, what.c_str(), src.c_str(),
                          sungrow_modbus::write_status_name(r.status));
          self->log_(ok ? 3 : 2, "write %s", t);
          self->report_(dev, t, ok);
          if (cb)
            cb(r);
        });
    switch (st) {
      case WriteStatus::QUEUED:
        this->log_(3, "write grid-code dev %u %s (%s): queued", device, what.c_str(), src.c_str());
        return Verdict::OK;
      case WriteStatus::OFFLINE:
        v = Verdict::OFFLINE;
        break;
      case WriteStatus::INVALID:
        v = Verdict::INVALID;
        break;
      default:
        v = Verdict::BUSY;
        break;
    }
  }
  this->rejected_++;
  char text[180];
  std::snprintf(text, sizeof(text), "grid-code dev %u %s (%s): rejected, %s (register %u)", device, what.c_str(),
                src.c_str(), verdict_name(v), c.reg);
  this->log_(2, "write %s", text);
  this->report_(device, text, false);
  return v;
}

// ---- read-back poller ---------------------------------------------------------------------------

void ControlCore::ingest_(uint8_t device, uint16_t start, const uint16_t *regs, uint16_t n) {
  const Dev *d = this->find_(device);
  if (this->store_ == nullptr || d == nullptr || d->cfg.profile == nullptr || n == 0)
    return;
  std::vector<uint8_t> be(2u * n);
  for (uint16_t i = 0; i < n; i++) {
    be[2 * i] = static_cast<uint8_t>(regs[i] >> 8);
    be[2 * i + 1] = static_cast<uint8_t>(regs[i]);
  }
  this->store_->ingest_block(device, *this->readback_(*d)->table, sungrow_modbus::FN_READ_HOLDING, start, be.data(),
                             n);
}

void ControlCore::refresh(uint8_t device, uint16_t start, uint16_t count) {
  Dev *d = this->find_(device);
  if (d == nullptr || d->cfg.profile == nullptr)
    return;
  const ReadbackDef *rb = this->readback_(*d);
  const uint32_t now = this->now_();
  for (uint8_t i = 0; i < rb->nblocks && i < d->blocks.size(); i++) {
    const BlockDef &b = rb->blocks[i];
    if (start < b.start + b.count && b.start < start + count)
      d->blocks[i].next = now + REFRESH_DELAY_MS;
  }
}

void ControlCore::tick() {
  if (this->master_ == nullptr)
    return;
  const uint32_t now = this->now_();
  for (auto &d : this->devs_) {
    const ReadbackDef *rb = this->readback_(d);
    const bool readback = rb != nullptr && d.cfg.readback;
    const bool online = this->online_(d.cfg.index);
    this->evaluate_expect_(d, now, online);
    if (!online) {
      if (d.was_online && readback && this->store_ != nullptr) {
        // Offline: the read-back values are unknown (entities go unknown).
        for (uint16_t i = 0; i < rb->table->count; i++)
          if (this->store_->get(d.cfg.index, rb->table->rows[i].sigid) != nullptr)
            this->store_->clear(d.cfg.index, rb->table->rows[i].sigid);
      }
      d.was_online = false;
      d.seen_running = false;
      for (auto &b : d.blocks)
        b.next = now;  // read everything as soon as it is back
      continue;
    }
    if (!d.was_online)
      this->trigger_reassert_(d, now, "reconnect");
    d.was_online = true;
    this->track_state_(d, now);
    this->evaluate_reassert_(d, now);
    if (!readback)
      continue;
    const uint8_t addr = this->address_of(d.cfg.index);
    for (uint8_t i = 0; i < rb->nblocks && i < d.blocks.size(); i++) {
      BlockState &bs = d.blocks[i];
      if (bs.pending || static_cast<int32_t>(now - bs.next) < 0)
        continue;
      const BlockDef b = rb->blocks[i];
      const uint8_t dev = d.cfg.index;
      auto self = this;
      const uint8_t bi = i;
      const sungrow_datamodel::RegisterTable *table = rb->table;
      bool q = this->master_->read_registers(
          addr, sungrow_modbus::FN_READ_HOLDING, b.start, b.count,
          [self, dev, bi, b, table](bool ok, const uint8_t *data, uint16_t n) {
            Dev *dd = self->find_(dev);
            if (dd == nullptr || bi >= dd->blocks.size())
              return;
            dd->blocks[bi].pending = false;
            if (!ok || data == nullptr || n != b.count || self->readback_(*dd)->table != table)
              return;  // exception / timeout: keep the old values (as the cyclic poller)
            self->readbacks_++;
            if (self->store_ != nullptr)
              self->store_->ingest_block(dev, *table, sungrow_modbus::FN_READ_HOLDING, b.start, data, n);
          });
      if (q) {
        bs.pending = true;
        bs.next = now + b.interval_ms;
      }
    }
  }
  this->drain_setup_();
  this->advance_prepare_();
  this->drain_reads_(this->gr_);
  this->drain_reads_(this->info_);
  if (this->prep_.active)
    this->drain_reads_(this->prep_.reads);
}

// ---- start/stop follow-up -------------------------------------------------------------------------

ExpectKind ControlCore::expect_kind_for_(const Dev &d, uint16_t start, const uint16_t *values, uint16_t count,
                                         const char **key) const {
  if (count != 1 || values == nullptr || (values[0] != 0xCE && values[0] != 0xCF))
    return ExpectKind::NONE;
  const ExpectKind kind = values[0] == 0xCF ? ExpectKind::START : ExpectKind::STOP;
  for (const Rule &r : d.cfg.rules)
    if ((r.sigid == SIG_START_STOP || r.sigid == SIG_HYBRID_START_STOP) && r.address == start) {
      *key = r.key;
      return kind;
    }
  const FamilyDescriptor *desc = this->identity(d.cfg.index).desc;
  if (desc != nullptr && desc->power_switch_reg != 0 && desc->power_switch_reg == start) {
    *key = "power_switch";
    return kind;
  }
  return ExpectKind::NONE;
}

void ControlCore::set_expect_text_(Dev &d, const char *text, bool final, bool ok) {
  Expectation &e = d.expect;
  if (e.text == text && e.final == final)
    return;
  e.text = text;
  if (final && !e.final)
    e.final_ms = this->now_();
  e.final = final;
  e.ok = ok;
  char line[160];
  std::snprintf(line, sizeof(line), "dev %u %s", d.cfg.index, text);
  this->log_(final && !ok ? 2 : 3, "%s", line);
  this->report_(d.cfg.index, line, !final || ok);
  if (!this->expect_listeners_.empty()) {
    ExpectStatus st;
    this->expect_status(d.cfg.index, &st);
    for (const auto &l : this->expect_listeners_)
      l(d.cfg.index, st);
  }
}

bool ControlCore::expect_status(uint8_t device, ExpectStatus *out) const {
  const Dev *d = this->find_(device);
  if (d == nullptr || d->expect.kind == ExpectKind::NONE)
    return false;
  const Expectation &e = d->expect;
  out->kind = e.kind;
  out->key = e.key;
  out->text = e.text;
  out->final = e.final;
  out->ok = e.ok;
  out->age_ms = this->now_() - (e.final ? e.final_ms : e.started_ms);
  out->state_valid = e.state_valid;
  out->state = e.state;
  return true;
}

void ControlCore::begin_expect_(uint8_t device, ExpectKind kind, const char *key) {
  Dev *d = this->find_(device);
  if (d == nullptr || kind == ExpectKind::NONE)
    return;
  const uint32_t now = this->now_();
  Expectation &e = d->expect;
  e = Expectation{};
  e.kind = kind;
  e.key = key;
  e.gen = ++this->read_gen_;
  e.started_ms = now;
  if (kind == ExpectKind::STOP) {
    e.deadline_ms = now + EXPECT_STOP_TIMEOUT_MS;
    this->evaluate_expect_(*d, now, true);
    return;
  }
  e.deadline_ms = now + e.delay_s * 1000u + EXPECT_START_GRACE_MS;
  this->evaluate_expect_(*d, now, true);
  // The inverter's reconnection delay decides how long a start may take.
  auto self = this;
  const uint32_t gen = e.gen;
  this->master_->read_registers(this->address_of(device), sungrow_modbus::FN_READ_HOLDING, RECONNECT_DELAY_REG, 1,
                                [self, device, gen](bool ok, const uint8_t *data, uint16_t n) {
                                  Dev *dd = self->find_(device);
                                  if (dd == nullptr || dd->expect.gen != gen || dd->expect.final || !ok ||
                                      data == nullptr || n != 1)
                                    return;
                                  uint16_t s = static_cast<uint16_t>((data[0] << 8) | data[1]);
                                  s = std::max(RECONNECT_DELAY_MIN_S, std::min(RECONNECT_DELAY_MAX_S, s));
                                  dd->expect.delay_s = s;
                                  dd->expect.deadline_ms = dd->expect.started_ms + s * 1000u + EXPECT_START_GRACE_MS;
                                });
}

void ControlCore::evaluate_expect_(Dev &d, uint32_t now, bool online) {
  using sungrow_datamodel::StateClass;
  Expectation &e = d.expect;
  if (e.kind == ExpectKind::NONE || e.final)
    return;
  const char *verb = e.kind == ExpectKind::STOP ? "stop" : "start";
  char text[96];
  if (!online) {
    std::snprintf(text, sizeof(text), "%s cancelled: device offline", verb);
    this->set_expect_text_(d, text, true, false);
    return;
  }
  uint64_t bits = 0;
  const bool have = this->store_ != nullptr && this->store_->get_bits(d.cfg.index, sungrow_datamodel::SIG_DEVICE_STATE, &bits);
  const uint16_t code = static_cast<uint16_t>(bits);
  const StateClass cls = have ? sungrow_datamodel::state_class(code) : StateClass::UNKNOWN;
  e.state_valid = have;
  e.state = code;
  char state[24];
  if (have)
    std::snprintf(state, sizeof(state), "state 0x%04X", code);
  else
    std::snprintf(state, sizeof(state), "state unknown");
  const bool due = static_cast<int32_t>(now - e.deadline_ms) >= 0;
  if (e.kind == ExpectKind::STOP) {
    if (have && cls == StateClass::STOPPED) {
      this->set_expect_text_(d, "stopped", true, true);
    } else if (due) {
      std::snprintf(text, sizeof(text), "stop pending: time-out (%s)", state);
      this->set_expect_text_(d, text, true, false);
    } else {
      this->set_expect_text_(d, "stop pending: output ramping", false, false);
    }
    return;
  }
  if (have && cls == StateClass::RUNNING) {
    this->set_expect_text_(d, "started", true, true);
    return;
  }
  if (have && cls == StateClass::FAULT) {
    std::snprintf(text, sizeof(text), "start failed: fault (%s)", state);
    this->set_expect_text_(d, text, true, false);
    return;
  }
  if (!due) {
    if (e.extensions > 0)
      std::snprintf(text, sizeof(text), "start pending: reconnection delay extended (grid trip %u/%u)", e.extensions,
                    EXPECT_START_MAX_EXTENSIONS);
    else if (have && cls == StateClass::STANDBY)
      std::snprintf(text, sizeof(text), "start pending: standby");
    else if (have && cls == StateClass::STARTING)
      std::snprintf(text, sizeof(text), "start pending: starting");
    else
      std::snprintf(text, sizeof(text), "start pending: reconnection delay (%u s)", e.delay_s);
    this->set_expect_text_(d, text, false, false);
    return;
  }
  if (have && cls == StateClass::STANDBY) {
    this->set_expect_text_(d, "start accepted: standby (low PV)", true, true);
    return;
  }
  if (have && (cls == StateClass::STARTING || cls == StateClass::TRANSIENT) &&
      e.extensions < EXPECT_START_MAX_EXTENSIONS && now - e.started_ms + EXPECT_START_EXTEND_MS <= EXPECT_START_HARD_MS) {
    // The power stage re-runs its reconnection delay after each grid trip.
    e.extensions++;
    e.deadline_ms = now + EXPECT_START_EXTEND_MS;
    std::snprintf(text, sizeof(text), "start pending: reconnection delay extended (grid trip %u/%u)", e.extensions,
                  EXPECT_START_MAX_EXTENSIONS);
    this->set_expect_text_(d, text, false, false);
    return;
  }
  if (have && cls == StateClass::STOPPED) {
    const bool string_model = sungrow_datamodel::inverter_model_family(this->identity(d.cfg.index).dev_type) ==
                              sungrow_datamodel::ModelFamily::STRING;
    if (string_model)
      std::snprintf(text, sizeof(text), "start failed: still stopped (key stop or rapid shutdown)");
    else
      std::snprintf(text, sizeof(text), "start failed: still stopped (%s)", state);
    this->set_expect_text_(d, text, true, false);
    return;
  }
  std::snprintf(text, sizeof(text), "start time-out (%s)", state);
  this->set_expect_text_(d, text, true, false);
}

// ---- power-limit re-assert ------------------------------------------------------------------------

bool ControlCore::power_limit_regs_(const Dev &d, uint16_t *sw, uint16_t *pct) const {
  *sw = *pct = 0;
  for (const Rule &r : d.cfg.rules) {
    if (r.sigid == SIG_POWER_LIMIT_SWITCH)
      *sw = r.address;
    else if (r.sigid == SIG_POWER_LIMIT_PCT)
      *pct = r.address;
  }
  // Written as one fn 16 frame: the setting must follow the switch.
  return *sw != 0 && *pct == static_cast<uint16_t>(*sw + 1);
}

const ControlCore::PowerLimitMemo *ControlCore::power_limit_memo(uint8_t device) const {
  const Dev *d = this->find_(device);
  return d != nullptr ? &d->plimit : nullptr;
}

void ControlCore::capture_power_limit_(Dev &d, uint16_t start, const std::vector<uint16_t> &values,
                                       const std::string &source, bool authorized) {
  uint16_t sw_addr = 0, pct_addr = 0;
  if (!this->power_limit_regs_(d, &sw_addr, &pct_addr))
    return;
  PowerLimitMemo &m = d.plimit;
  // A register the write did not cover keeps its remembered value, else the current read-back.
  auto value_at = [&](uint16_t addr, uint16_t sigid, uint16_t *out) -> bool {
    if (addr >= start && addr < start + values.size()) {
      *out = values[addr - start];
      return true;
    }
    if (m.valid) {
      *out = addr == sw_addr ? m.sw : m.pct;
      return true;
    }
    uint64_t bits = 0;
    if (this->store_ != nullptr && this->store_->get_bits(d.cfg.index, sigid, &bits)) {
      *out = static_cast<uint16_t>(bits);
      return true;
    }
    return false;
  };
  uint16_t sw = 0, pct = 0;
  if (!value_at(sw_addr, SIG_POWER_LIMIT_SWITCH, &sw) || !value_at(pct_addr, SIG_POWER_LIMIT_PCT, &pct))
    return;
  m.valid = true;
  m.sw = sw;
  m.pct = pct;
  m.written_ms = this->now_();
  m.due = false;
  if (std::strncmp(source.c_str(), "reassert(", 9) != 0) {
    m.source = source;
    m.authorized = authorized;
  }
}

void ControlCore::trigger_reassert_(Dev &d, uint32_t now, const char *why) {
  PowerLimitMemo &m = d.plimit;
  uint16_t sw_addr = 0, pct_addr = 0;
  if (!m.valid || !d.cfg.reassert_power_limit || !this->power_limit_regs_(d, &sw_addr, &pct_addr))
    return;
  m.due = true;
  m.due_since_ms = now;
  this->refresh(d.cfg.index, pct_addr, 1);
  this->log_(5, "dev %u power limit check after %s", d.cfg.index, why);
}

void ControlCore::track_state_(Dev &d, uint32_t now) {
  uint64_t bits = 0;
  if (this->store_ == nullptr || !this->store_->get_bits(d.cfg.index, sungrow_datamodel::SIG_DEVICE_STATE, &bits))
    return;
  const uint16_t code = static_cast<uint16_t>(bits);
  if (sungrow_datamodel::state_class(code) == sungrow_datamodel::StateClass::RUNNING) {
    d.seen_running = true;
  } else if (code == STATE_POWER_STAGE_INITIAL && d.seen_running) {
    d.seen_running = false;
    this->trigger_reassert_(d, now, "power-stage restart");
  }
}

void ControlCore::evaluate_reassert_(Dev &d, uint32_t now) {
  PowerLimitMemo &m = d.plimit;
  if (!m.valid || !m.due || this->store_ == nullptr)
    return;
  const sungrow_datamodel::Signal *s = this->store_->get(d.cfg.index, SIG_POWER_LIMIT_PCT);
  if (s == nullptr || !s->valid || static_cast<int32_t>(s->updated_ms - m.due_since_ms) < 0)
    return;  // no read-back since the trigger yet
  m.due = false;
  const uint16_t cur = static_cast<uint16_t>(s->bits());
  if (cur == m.pct) {
    this->log_(5, "dev %u power limit intact (%u)", d.cfg.index, cur);
    return;
  }
  if (!this->enabled_ && !m.authorized) {
    this->log_(2, "dev %u power limit is %u, was set to %u by %s: not re-asserted, inverter writes disabled",
               d.cfg.index, cur, m.pct, m.source.c_str());
    return;
  }
  if (m.count > 0 && now - m.last_reassert_ms < REASSERT_MIN_INTERVAL_MS) {
    this->log_(2, "dev %u power limit is %u, was set to %u: re-asserted %u s ago, not again within %u min",
               d.cfg.index, cur, m.pct, (unsigned) ((now - m.last_reassert_ms) / 1000),
               (unsigned) (REASSERT_MIN_INTERVAL_MS / 60000));
    return;
  }
  uint16_t sw_addr = 0, pct_addr = 0;
  this->power_limit_regs_(d, &sw_addr, &pct_addr);
  const uint16_t vals[2] = {m.sw, m.pct};
  WriteOptions opt;
  opt.fn = sungrow_modbus::FN_WRITE_MULTIPLE;
  const std::string src = "reassert(" + m.source + ")";
  const Verdict v = this->write(d.cfg.index, sw_addr, vals, 2, /*verify=*/true, src.c_str(), nullptr, opt, m.authorized);
  if (v == Verdict::OK) {
    m.last_reassert_ms = now;
    m.count++;
    this->log_(3, "dev %u power limit re-asserted: %u -> %u (set by %s)", d.cfg.index, cur, m.pct, m.source.c_str());
  } else {
    this->log_(2, "dev %u power limit is %u, was set to %u: re-assert not sent, %s", d.cfg.index, cur, m.pct,
               verdict_name(v));
  }
}

// ---- Setup / Installer batch ----------------------------------------------------------------------

bool ControlCore::is_routine_(uint8_t device, uint16_t addr) const {
  const Dev *d = this->find_(device);
  if (d == nullptr)
    return false;
  for (const auto &r : d->cfg.rules)
    if (addr >= r.address && static_cast<uint32_t>(addr) < static_cast<uint32_t>(r.address) + r.count)
      return true;
  return false;
}

Verdict ControlCore::reject_setup_(Verdict v, const char *fmt, ...) {
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  this->setup_detail_ = buf;
  this->rejected_++;
  this->log_(2, "setup rejected: %s (%s)", verdict_name(v), buf);
  return v;
}

std::vector<ControlCore::SetupFrame> ControlCore::frames_for_(uint8_t device, const std::vector<SetupReg> &regs) const {
  // Merge only items adjacent in the given order (never sorted, as stock); a register in
  // both lists uses the routine (value-checked) path.
  std::vector<SetupFrame> frames;
  for (const SetupReg &reg : regs) {
    const bool grid = !this->is_routine_(device, reg.address);
    if (!frames.empty() && frames.back().grid == grid &&
        frames.back().start + frames.back().values.size() == reg.address &&
        frames.back().values.size() < sungrow_modbus::MAX_WRITE_REGS) {
      frames.back().values.push_back(reg.value);
    } else {
      SetupFrame f;
      f.start = reg.address;
      f.values.push_back(reg.value);
      f.grid = grid;
      f.result = frames.size();
      frames.push_back(std::move(f));
    }
  }
  return frames;
}

/// Value of the `count`-word register at `addr` from the batch (U32: low word at addr, high word
/// at addr + 1, both must be in the batch); false when the batch does not hold it.
static bool batch_value(const std::vector<ControlCore::SetupReg> &regs, uint16_t addr, uint8_t count, uint32_t *out) {
  uint32_t v = 0;
  for (uint8_t w = 0; w < count; w++) {
    bool found = false;
    for (const ControlCore::SetupReg &r : regs)
      if (r.address == static_cast<uint16_t>(addr + w)) {
        v |= static_cast<uint32_t>(r.value) << (16 * w);
        found = true;
        break;
      }
    if (!found)
      return false;
  }
  *out = v;
  return true;
}

std::vector<ControlCore::SetupReg> ControlCore::dsp_filter_(uint8_t device, const std::vector<SetupReg> &regs,
                                                            std::string *note) const {
  const DspModel *m = dsp_model(this->identity(device).dev_code);
  if (m == nullptr)
    return regs;
  std::vector<uint16_t> drop;
  for (const SetupReg &r : regs) {
    if (this->is_routine_(device, r.address))
      continue;
    const DspRange *dr = dsp_range(m, r.address);
    uint32_t v = 0;
    if (dr == nullptr || !batch_value(regs, r.address, dr->count, &v) || (v >= dr->min && v <= dr->max))
      continue;
    drop.push_back(r.address);
    if (dr->count == 2)
      drop.push_back(static_cast<uint16_t>(r.address + 1));
    if (note != nullptr) {
      char b[80];
      std::snprintf(b, sizeof(b), "%s%u=%u outside the power-stage range %u-%u", note->empty() ? "" : "; ",
                    r.address, static_cast<unsigned>(v), static_cast<unsigned>(dr->min), static_cast<unsigned>(dr->max));
      *note += b;
    }
  }
  if (drop.empty())
    return regs;
  std::vector<SetupReg> out;
  for (const SetupReg &r : regs)
    if (std::find(drop.begin(), drop.end(), r.address) == drop.end())
      out.push_back(r);
  return out;
}

std::string ControlCore::setup_plan_note(uint8_t device, const std::vector<SetupReg> &regs) const {
  std::string note;
  this->dsp_filter_(device, regs, &note);
  return note;
}

std::vector<ControlCore::SetupFrameResult> ControlCore::plan_frames(uint8_t device,
                                                                    const std::vector<SetupReg> &regs) const {
  std::vector<SetupFrameResult> out;
  for (const SetupFrame &f : this->frames_for_(device, this->dsp_filter_(device, regs, nullptr)))
    out.push_back({f.start, static_cast<uint16_t>(f.values.size()), WriteStatus::QUEUED, 0, false});
  return out;
}

static const char *rel_op_text(RelOp op) {
  switch (op) {
    case RelOp::LT:
      return "<";
    case RelOp::LE:
      return "<=";
    case RelOp::GT:
      return ">";
    case RelOp::GE:
      return ">=";
    case RelOp::EQ:
      return "=";
  }
  return "?";
}

static bool rel_holds(RelOp op, int64_t l, int64_t r) {
  switch (op) {
    case RelOp::LT:
      return l < r;
    case RelOp::LE:
      return l <= r;
    case RelOp::GT:
      return l > r;
    case RelOp::GE:
      return l >= r;
    case RelOp::EQ:
      return l == r;
  }
  return false;
}

Verdict ControlCore::setup_apply(uint8_t device, const std::vector<SetupReg> &regs, const char *source, bool profile) {
  this->setup_detail_.clear();
  this->setup_note_.clear();
  if (this->setup_busy())
    return this->reject_setup_(Verdict::BUSY, "setup batch in progress");
  if (regs.empty() || regs.size() > SETUP_MAX_REGS)
    return this->reject_setup_(Verdict::INVALID, "1..%u registers", static_cast<unsigned>(SETUP_MAX_REGS));
  const DeviceIdentity id = this->identity(device);
  if (!id.known || !this->online_(device))
    return this->reject_setup_(Verdict::OFFLINE, "device %u", device);
  if (!profile) {
    std::vector<uint16_t> a;
    for (const SetupReg &r : regs)
      a.push_back(r.address);
    std::sort(a.begin(), a.end());
    for (size_t i = 1; i < a.size(); i++)
      if (a[i] == a[i - 1])
        return this->reject_setup_(Verdict::INVALID, "duplicate register %u", a[i]);
  }
  // Value of a register: the batch first, then this device's grid-read cache.
  auto value_of = [&](uint16_t addr, uint16_t *v) -> bool {
    for (const SetupReg &r : regs)
      if (r.address == addr) {
        *v = r.value;
        return true;
      }
    if (this->gr_.device == device) {
      const GridReadVal *e = this->gr_.find(sungrow_modbus::FN_READ_HOLDING, addr);
      if (e != nullptr && e->valid) {
        *v = e->value;
        return true;
      }
    }
    return false;
  };
  const DspModel *dsp = dsp_model(id.dev_code);
  // Typed value per the register's installer dtype (U32: low word at addr, high word at addr + 1).
  auto typed = [&](uint16_t addr, int64_t *out) -> bool {
    const InstallerRange *rg = installer_range(id.family, addr);
    const InstallerDtype dt = rg != nullptr ? rg->dtype : InstallerDtype::U16;
    uint16_t lo = 0, hi = 0;
    if (!value_of(addr, &lo))
      return false;
    switch (dt) {
      case InstallerDtype::S16:
        *out = static_cast<int16_t>(lo);
        return true;
      case InstallerDtype::U32:
        if (!value_of(static_cast<uint16_t>(addr + 1), &hi))
          return false;
        *out = static_cast<int64_t>(lo) | (static_cast<int64_t>(hi) << 16);
        return true;
      default:
        *out = lo;
        return true;
    }
  };
  for (const SetupReg &reg : regs) {
    if (this->is_routine_(device, reg.address)) {
      const uint16_t v = reg.value;
      Check rc = check_rules(this->find_(device)->cfg.rules, reg.address, &v, 1);
      if (rc.verdict != Verdict::OK)
        return this->reject_setup_(rc.verdict, "range:%u", reg.address);
      continue;
    }
    if (!installer_addr_allowed(id.family, reg.address))
      return this->reject_setup_(Verdict::NOT_ALLOWED, "register %u", reg.address);
    if (profile)
      continue;  // the XML values are written verbatim; power-stage range misses are skipped below
    const DspRange *dr = dsp_range(dsp, reg.address);
    uint32_t dv = 0;
    if (dr != nullptr && batch_value(regs, reg.address, dr->count, &dv) && (dv < dr->min || dv > dr->max))
      return this->reject_setup_(Verdict::BAD_VALUE, "dsp_range:%u:%u-%u", reg.address, static_cast<unsigned>(dr->min),
                                 static_cast<unsigned>(dr->max));
    const InstallerRange *rg = installer_range(id.family, reg.address);
    int64_t v = 0;
    if (rg != nullptr && typed(reg.address, &v) && (v < rg->raw_min || v > rg->raw_max))
      return this->reject_setup_(Verdict::BAD_VALUE, "range:%u", reg.address);
    const InstallerRel *rels = nullptr;
    size_t nrel = 0;
    if (!installer_rels(id.family, reg.address, &rels, &nrel))
      continue;
    for (size_t i = 0; i < nrel; i++) {
      const InstallerRel &rel = rels[i];
      int64_t l = 0, r = 0;
      if (!typed(rel.lhs, &l))
        return this->reject_setup_(Verdict::BAD_VALUE, "relation:%u", static_cast<unsigned>(rel.lhs + 1));
      if (!typed(rel.rhs, &r))
        return this->reject_setup_(Verdict::BAD_VALUE, "relation:%u", rel.rhs);
      if (!rel_holds(rel.op, l, r))
        return this->reject_setup_(Verdict::BAD_VALUE, "relation:%u%s%u", rel.lhs, rel_op_text(rel.op), rel.rhs);
    }
  }
  std::vector<SetupReg> ordered = regs;
  if (!profile) {
    // Commit registers (country / grid code / 30036 / 30037) first, as the stock SPA orders them.
    std::stable_partition(ordered.begin(), ordered.end(), [&](const SetupReg &r) {
      return sungrow_modbus::is_commit_register(id.desc, r.address);
    });
    this->invalidate_prepare_();
  } else {
    ordered = this->dsp_filter_(device, regs, &this->setup_note_);
    if (!this->setup_note_.empty())
      this->log_(2, "setup dev %u (%s): skipped %s", device, dsp->model, this->setup_note_.c_str());
  }
  this->setup_queue_ = this->frames_for_(device, ordered);
  this->setup_results_.clear();
  for (const SetupFrame &f : this->setup_queue_)
    this->setup_results_.push_back({f.start, static_cast<uint16_t>(f.values.size()), WriteStatus::QUEUED, 0, false});
  this->setup_device_ = device;
  this->setup_country_reg_ = id.desc != nullptr ? id.desc->country_reg : 0;
  this->setup_source_ = source != nullptr ? source : "setup";
  this->setup_inflight_ = false;
  this->setup_accepted_ms_ = this->now_();
  this->setup_total_ = static_cast<uint16_t>(ordered.size());
  this->setup_ok_ = 0;
  this->setup_failed_n_ = 0;
  this->log_(3, "setup dev %u (%s): %u register(s) in %u frame(s) queued (%s, %s)", device,
             sungrow_modbus::xml_family_name(id.family), this->setup_total_,
             static_cast<unsigned>(this->setup_queue_.size()), profile ? "profile" : "installer",
             this->setup_source_.c_str());
  // The first frame goes out from tick() (loop task), never from the caller's task: the web
  // server task is too small for the write path (frame build, verify, logging).
  return Verdict::OK;
}

uint32_t ControlCore::setup_deadline_left() const {
  if (!this->setup_busy())
    return 0;
  const uint32_t el = this->now_() - this->setup_accepted_ms_;
  return el >= SETUP_DEADLINE_MS ? 0 : SETUP_DEADLINE_MS - el;
}

void ControlCore::expire_setup_(uint32_t now) {
  if (this->setup_queue_.empty() || this->setup_inflight_ || now - this->setup_accepted_ms_ < SETUP_DEADLINE_MS)
    return;
  const bool online = this->online_(this->setup_device_);
  uint16_t regs = 0;
  for (const SetupFrame &f : this->setup_queue_) {
    SetupFrameResult &res = this->setup_results_[f.result];
    res.status = online ? WriteStatus::TIMEOUT : WriteStatus::OFFLINE;
    res.expired = true;
    regs += static_cast<uint16_t>(f.values.size());
  }
  this->setup_failed_n_ += regs;
  this->log_(2, "setup dev %u: deadline (%u s) passed, %u frame(s) / %u register(s) dropped unsent",
             this->setup_device_, static_cast<unsigned>(SETUP_DEADLINE_MS / 1000),
             static_cast<unsigned>(this->setup_queue_.size()), regs);
  this->setup_queue_.clear();
}

void ControlCore::drain_setup_() {
  if (this->master_ == nullptr)
    return;
  this->expire_setup_(this->now_());
  if (this->setup_inflight_ || this->setup_queue_.empty())
    return;
  if (!this->online_(this->setup_device_))
    return;  // wait for the device until the deadline
  if (this->master_->writes_pending() >= 2)
    return;  // leave headroom in the 8-deep write queue so routine/verify writes still flow
  const SetupFrame f = this->setup_queue_.front();
  const uint16_t n = static_cast<uint16_t>(f.values.size());
  this->setup_inflight_ = true;
  auto self = this;
  const size_t idx = f.result;
  const bool country = f.grid && this->setup_country_reg_ != 0 && f.start == this->setup_country_reg_;
  const uint16_t first = f.values[0];
  Callback cb = [self, n, idx, country, first](const WriteResult &r) {
    if (idx < self->setup_results_.size()) {
      self->setup_results_[idx].status = r.status;
      self->setup_results_[idx].exception = r.exception;
    }
    if (r.status == WriteStatus::OK) {
      self->setup_ok_ += n;
      if (country && self->country_cb_)
        self->country_cb_(first);
    } else {
      self->setup_failed_n_ += n;
    }
    if (!self->setup_queue_.empty())
      self->setup_queue_.erase(self->setup_queue_.begin());
    self->setup_inflight_ = false;
  };
  Verdict v;
  if (f.grid)
    v = this->write_grid_code_(this->setup_device_, f.start, f.values.data(), n, this->setup_source_.c_str(), cb);
  else
    v = this->write(this->setup_device_, f.start, f.values.data(), n, /*verify=*/true, this->setup_source_.c_str(),
                    cb, {}, /*authorized=*/true);
  if (v != Verdict::OK) {
    // Rejected synchronously (cb not called).
    this->setup_inflight_ = false;
    if (v != Verdict::BUSY && v != Verdict::OFFLINE) {
      // Hard rejection after the up-front validation: count it and move on.
      this->setup_results_[idx].status = WriteStatus::INVALID;
      this->setup_failed_n_ += n;
      this->setup_queue_.erase(this->setup_queue_.begin());
    }
    // BUSY / OFFLINE: the frame stays queued for the next tick (until the deadline).
  }
}

// ---- on-demand reads ------------------------------------------------------------------------------

bool ControlCore::is_diagnostic_read(uint16_t addr) {
  static const uint16_t DIAG[] = {30644, 30051, 32310, 32311, 30036, 30037, 30500, 30502, 30643};
  for (uint16_t a : DIAG)
    if (a == addr)
      return true;
  return false;
}

const ControlCore::GridReadVal *ControlCore::ReadSet::find(uint8_t fn, uint16_t addr) const {
  for (const auto &e : this->vals)
    if (e.fn == fn && e.address == addr)
      return &e;
  return nullptr;
}

void ControlCore::read_start_(ReadSet &rs, uint8_t device) {
  rs.device = device;
  rs.vals.clear();
  rs.runs.clear();
  rs.inflight = 0;
  rs.gen = ++this->read_gen_;
  if (rs.gen == 0)
    rs.gen = ++this->read_gen_;
}

void ControlCore::read_add_(ReadSet &rs, uint8_t fn, std::vector<uint16_t> addrs, bool readable_only) {
  std::sort(addrs.begin(), addrs.end());
  addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
  const XmlFamily fam = readable_only ? this->identity(rs.device).family : XmlFamily::NONE;
  std::vector<uint16_t> rd;
  for (uint16_t a : addrs) {
    if (a == 0 || rs.find(fn, a) != nullptr)
      continue;
    const bool ok = !readable_only || installer_addr_allowed(fam, a) || this->is_routine_(rs.device, a) ||
                    is_diagnostic_read(a);
    rs.vals.push_back({a, 0, false, ok ? ReadReason::PENDING : ReadReason::OFF_LIST, fn});
    if (ok)
      rd.push_back(a);
  }
  std::sort(rs.vals.begin(), rs.vals.end(), [](const GridReadVal &x, const GridReadVal &y) {
    return x.fn != y.fn ? x.fn < y.fn : x.address < y.address;
  });
  for (size_t i = 0; i < rd.size();) {
    const uint16_t start = rd[i];
    uint16_t cnt = 1;
    size_t j = i + 1;
    while (j < rd.size() && rd[j] == static_cast<uint16_t>(start + cnt) && cnt < READ_RUN_MAX) {
      cnt++;
      j++;
    }
    rs.runs.push_back({fn, start, cnt});
    i = j;
  }
}

void ControlCore::drain_reads_(ReadSet &rs) {
  if (this->master_ == nullptr || rs.runs.empty())
    return;
  const uint8_t addr = this->address_of(rs.device);
  if (addr == 0) {
    // Unknown device: nothing can answer.
    for (auto &e : rs.vals)
      if (e.reason == ReadReason::PENDING)
        e.reason = ReadReason::TIMEOUT;
    rs.runs.clear();
    return;
  }
  while (!rs.runs.empty() && rs.inflight < 6 && this->master_->adhoc_pending() < 8) {
    const ReadSet::Run run = rs.runs.front();
    auto self = this;
    ReadSet *set = &rs;
    const uint32_t gen = rs.gen;
    const bool q = this->master_->read_registers(
        addr, run.fn, run.start, run.count, [self, set, gen, run](bool ok, const uint8_t *data, uint16_t ndata) {
          if (set->gen != gen)
            return;  // the set was restarted; this reply belongs to the old one
          if (set->inflight > 0)
            set->inflight--;
          auto mark = [&](uint16_t i, bool valid, uint16_t value, ReadReason why) {
            for (auto &e : set->vals)
              if (e.fn == run.fn && e.address == run.start + i) {
                e.valid = valid;
                e.value = value;
                e.reason = why;
                break;
              }
          };
          if (ok && data != nullptr && ndata == run.count) {
            for (uint16_t i = 0; i < run.count; i++)
              mark(i, true, static_cast<uint16_t>((data[2 * i] << 8) | data[2 * i + 1]), ReadReason::OK);
            return;
          }
          const bool exc = !ok && self->reply_was_exception_(set->device);
          if (exc && run.count > 1) {
            // One unimplemented register rejects the whole run: read the halves separately.
            const uint16_t h = run.count / 2;
            set->runs.insert(set->runs.begin(), {run.fn, static_cast<uint16_t>(run.start + h),
                                                 static_cast<uint16_t>(run.count - h)});
            set->runs.insert(set->runs.begin(), {run.fn, run.start, h});
            return;
          }
          for (uint16_t i = 0; i < run.count; i++)
            mark(i, false, 0, exc ? ReadReason::EXCEPTION : ReadReason::TIMEOUT);
        });
    if (!q)
      break;  // adhoc queue full; resume on the next tick
    rs.runs.erase(rs.runs.begin());
    rs.inflight++;
  }
}

void ControlCore::grid_read_request(uint8_t device, const uint16_t *addrs, uint16_t n) {
  this->read_start_(this->gr_, device);
  if (addrs != nullptr && n > 0)
    this->read_add_(this->gr_, sungrow_modbus::FN_READ_HOLDING, std::vector<uint16_t>(addrs, addrs + n), true);
  this->drain_reads_(this->gr_);
}

// ---- Setup profile flow ---------------------------------------------------------------------------

static constexpr uint16_t REG_5479 = 5479;  // input: PV / ES variant selector of unit options

void ControlCore::setup_info_request(uint8_t device) {
  this->read_start_(this->info_, device);
  this->info_started_ms_ = this->now_();
  this->read_add_(this->info_, sungrow_modbus::FN_READ_INPUT, {REG_5479}, false);
  const DeviceIdentity id = this->identity(device);
  std::vector<uint16_t> a;
  if (id.desc != nullptr) {
    a.push_back(id.desc->country_reg);
    if (id.desc->grid_reg != 0)
      a.push_back(id.desc->grid_reg);
  }
  if (id.family == XmlFamily::UNIT) {
    a.push_back(GP_UNIT_COMPANY_REG);
    a.push_back(GP_UNIT_GRIDTYPE_REG);
  }
  this->read_add_(this->info_, sungrow_modbus::FN_READ_HOLDING, a, false);
  this->drain_reads_(this->info_);
}

bool ControlCore::setup_info_fresh(uint8_t device, uint32_t max_age_ms) const {
  return this->info_.gen != 0 && this->info_.device == device && this->now_() - this->info_started_ms_ < max_age_ms;
}

void ControlCore::invalidate_prepare_() {
  if (this->prep_.active)
    this->log_(5, "setup prepare dev %u option %u discarded", this->prep_.device, this->prep_.option);
  this->prep_ = Prepared{};
}

Verdict ControlCore::setup_prepare(uint8_t device, uint16_t option) {
  this->setup_detail_.clear();
  if (this->setup_busy())
    return this->reject_setup_(Verdict::BUSY, "setup batch in progress");
  const DeviceIdentity id = this->identity(device);
  if (!id.known || !this->online_(device))
    return this->reject_setup_(Verdict::OFFLINE, "device %u", device);
  this->invalidate_prepare_();
  this->prep_.active = true;
  this->prep_.device = device;
  this->prep_.option = option;
  this->prep_.started_ms = this->now_();
  this->read_start_(this->prep_.reads, device);
  const GpOption *opt = find_setup_option(id.family, option);
  if (opt != nullptr && opt->needreadtype == 1)
    this->read_add_(this->prep_.reads, sungrow_modbus::FN_READ_INPUT, {REG_5479}, false);
  this->log_(3, "setup prepare dev %u (%s) option %u", device, sungrow_modbus::xml_family_name(id.family), option);
  this->advance_prepare_();
  this->drain_reads_(this->prep_.reads);
  return Verdict::OK;
}

void ControlCore::advance_prepare_() {
  Prepared &p = this->prep_;
  if (!p.active || p.resolved || !p.reads.complete())
    return;
  const GridReadVal *r5479 = p.reads.find(sungrow_modbus::FN_READ_INPUT, REG_5479);
  const DeviceIdentity id = this->identity(p.device);
  SetupContext ctx;
  ctx.family = id.family;
  ctx.desc = id.desc;
  ctx.w32b5 = this->feature_w32b5(p.device);
  ctx.w29b12 = this->feature_w29b12(p.device);
  ctx.have_5479 = r5479 != nullptr && r5479->valid;
  ctx.reg5479 = ctx.have_5479 ? r5479->value : 0;
  p.have_5479 = ctx.have_5479;
  p.reg5479 = ctx.reg5479;
  // On protocol 2/3 the feature bits come from the capability bitmap (sig 0x2181), which the poll
  // table reads once per cycle. Until it is valid the 30036 item cannot be decided, so the
  // profile is not resolved (stock reads the bitmap in its first round, before any set).
  const bool needs_bitmap = id.desc != nullptr && (id.desc->protocol == 2 || id.desc->protocol == 3);
  if (needs_bitmap && (this->store_ == nullptr ||
                       !this->store_->is_valid(p.device, sungrow_datamodel::SIG_CAPABILITY_BITMAP))) {
    p.items.clear();
    p.status = ResolveStatus::NEED_CAPABILITY;
  } else {
    p.status = resolve_profile(ctx, p.option, &p.items);
  }
  p.resolved = true;
  const GpOption *opt = find_setup_option(id.family, p.option);
  if (opt != nullptr) {
    if (opt->needreadtype != 1) {
      p.variant = opt->nvariants > 0 ? 0 : -1;
    } else if (p.have_5479) {
      for (uint8_t i = 0; i < opt->nvariants && p.variant < 0; i++) {
        const GpVariant &v = GP_VARIANTS[opt->variant_off + i];
        for (uint8_t k = 0; k < v.nref; k++)
          if (GP_REF5479[v.ref_off + k] == p.reg5479)
            p.variant = i;
      }
    }
  }
  this->log_(3, "setup prepare dev %u option %u: %s, %u item(s)%s", p.device, p.option, resolve_status_name(p.status),
             static_cast<unsigned>(p.items.size()), p.have_5479 ? "" : " (5479 unknown)");
  if (p.status != ResolveStatus::OK)
    return;
  std::vector<uint16_t> a;
  for (const ResolvedItem &it : p.items)
    a.push_back(it.address);
  this->read_add_(p.reads, sungrow_modbus::FN_READ_HOLDING, a, false);
}

bool ControlCore::Prepared::current(uint16_t addr, uint16_t *v) const {
  const GridReadVal *e = this->reads.find(sungrow_modbus::FN_READ_HOLDING, addr);
  if (e == nullptr || !e->valid)
    return false;
  *v = e->value;
  return true;
}

size_t ControlCore::Prepared::n_changed() const {
  size_t n = 0;
  for (const ResolvedItem &it : this->items) {
    uint16_t cur = 0;
    n += !this->current(it.address, &cur) || cur != it.value;
  }
  return n;
}

const ControlCore::Prepared *ControlCore::setup_prepared(uint8_t device, uint16_t option) const {
  const Prepared &p = this->prep_;
  if (!p.active || p.device != device || p.option != option || this->now_() - p.started_ms > PREPARE_TTL_MS)
    return nullptr;
  return &p;
}

Verdict ControlCore::setup_apply_profile(uint8_t device, uint16_t option, const char *source) {
  this->setup_detail_.clear();
  const Prepared *p = this->setup_prepared(device, option);
  if (p == nullptr || !p->complete() || p->status != ResolveStatus::OK)
    return this->reject_setup_(Verdict::PREVIEW_REQUIRED, "device %u option %u", device, option);
  std::vector<SetupReg> regs;
  for (const ResolvedItem &it : p->items)
    regs.push_back({it.address, it.value});
  const Verdict v = this->setup_apply(device, regs, source, /*profile=*/true);
  if (v == Verdict::OK)
    this->invalidate_prepare_();  // single use
  return v;
}

Verdict ControlCore::setup_start(uint8_t device, const char *source) {
  this->setup_detail_.clear();
  if (this->setup_busy())
    return this->reject_setup_(Verdict::BUSY, "setup batch in progress");
  const DeviceIdentity id = this->identity(device);
  if (!id.known)
    return this->reject_setup_(Verdict::OFFLINE, "device %u", device);
  if (id.desc == nullptr || id.desc->power_switch_reg == 0)
    return this->reject_setup_(Verdict::NOT_ALLOWED, "no power_switch register for this device");
  this->invalidate_prepare_();
  const uint16_t reg = id.desc->power_switch_reg;
  const uint16_t v = 0xCF;  // start
  // The profile's start/stop rule when it covers the descriptor register, else exactly this write.
  std::vector<Rule> dedicated;
  const Dev *d = this->find_(device);
  if (d == nullptr || !this->is_routine_(device, reg)) {
    Rule r;
    r.address = reg;
    r.min = r.max = v;
    r.key = "power_switch";
    dedicated.push_back(r);
  }
  Check c = check_rules(dedicated.empty() ? d->cfg.rules : dedicated, reg, &v, 1);
  if (c.verdict == Verdict::OK && !this->online_(device))
    c.verdict = Verdict::OFFLINE;
  if (c.verdict != Verdict::OK)
    this->setup_detail_ = "register " + std::to_string(reg);
  WriteOptions opt;
  opt.fn = sungrow_modbus::FN_WRITE_MULTIPLE;  // stock power_switch item: one fn 16 register
  return this->submit_(device, reg, &v, 1, /*verify=*/false, source, nullptr, opt, c);
}

}  // namespace sungrow_control
}  // namespace esphome
