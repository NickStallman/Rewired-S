#include "master.h"

#include "../sungrow_datamodel/derived.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome {
namespace sungrow_modbus {

using sungrow_datamodel::Family;
using sungrow_datamodel::RegisterRow;
using sungrow_datamodel::SBR_CELL_BLOCK_COUNT;
using sungrow_datamodel::SBR_CELL_BLOCK_START;
using sungrow_datamodel::TableId;

/// inv_proto1_4: an exception on the node at wire address 8099 (0x3200 @8100) clears 0x3200 and
/// 0x3201 (as stock). The only state an exception changes.
static constexpr uint16_t EXC_CLEAR_WIRE_START = 8099;
static constexpr uint16_t EXC_CLEAR_SIGS[2] = {0x3200, 0x3201};
/// estimate_ms() slack on top of gap + wire time: slave turnaround + loop granularity.
static constexpr uint32_t EST_SLACK_MS = 25;

static inline bool due(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t) >= 0; }
static inline bool before(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) < 0; }

static const char *kind_name(int k) {
  static const char *const names[] = {"poll", "discovery", "probe", "adhoc", "write", "verify", "invtype", "search"};
  return k >= 0 && k < 8 ? names[k] : "?";
}

/// Stock dev_type of a device whose table is fixed (or a fallback) and has no devinfo.
static uint8_t dev_type_for_family(Family f) {
  switch (f) {
    case Family::INVERTER:
      return 1;
    case Family::BATTERY:
      return 44;
    case Family::METER:
      return 18;
    case Family::LOGGER:
      return 34;
    case Family::CHARGER:
      return 46;
    case Family::ENERGY_BRIDGE:
      return 92;
  }
  return 0;
}

/// Protocol type an inverter poll table stands for (inv_proto1_4 -> 1, as stock after a
/// rejected devinfo read); 0 for any other table.
static uint8_t protocol_for_table(const RegisterTable *t) {
  switch (static_cast<TableId>(t->id)) {
    case TableId::INV_PROTO1_4:
      return 1;
    case TableId::INV_PROTO2:
      return 2;
    case TableId::INV_PROTO3:
      return 3;
    case TableId::INV_PROTO5:
      return 5;
    default:
      return 0;
  }
}

const char *write_status_name(WriteStatus s) {
  switch (s) {
    case WriteStatus::QUEUED:
      return "queued";
    case WriteStatus::OK:
      return "ok";
    case WriteStatus::EXCEPTION:
      return "exception";
    case WriteStatus::TIMEOUT:
      return "timeout";
    case WriteStatus::VERIFY_MISMATCH:
      return "verify mismatch";
    case WriteStatus::VERIFY_FAILED:
      return "verify read failed";
    case WriteStatus::OFFLINE:
      return "device offline";
    case WriteStatus::RATE_LIMITED:
      return "rate limited";
    case WriteStatus::QUEUE_FULL:
      return "queue full";
    case WriteStatus::INVALID:
      return "invalid";
  }
  return "?";
}

// ---------------------------------------------------------------------------------------------
const char *Device::dev_type_source() const {
  switch (this->dev_type_src_) {
    case DevTypeSource::DEVINFO:
      return "devinfo";
    case DevTypeSource::YAML:
      return "yaml";
    case DevTypeSource::TABLE:
      return "table";
    default:
      return "none";
  }
}

uint16_t Device::effective_inv_type_() const {
  const uint8_t p = this->devinfo_.protocol;
  return this->inv_type_known_ && (p == 1 || p == 4) ? this->inv_type_ : 0;
}

bool Device::is_inverter_() const {
  return this->table_ != nullptr && this->table_->family == Family::INVERTER;
}

XmlFamily Device::xml_family() const {
  // Grid-code XML families (and so Setup / Installer) exist for inverters only; stock's picker
  // would otherwise map e.g. a battery's protocol 0 to "unit".
  if (!this->is_inverter_())
    return XmlFamily::NONE;
  return sungrow_modbus::xml_family(this->effective_inv_type_(), this->devinfo_.protocol);
}

const FamilyDescriptor *Device::descriptor() const {
  if (!this->is_inverter_())
    return nullptr;
  return family_descriptor(this->effective_inv_type_(), this->devinfo_.protocol);
}

// ---------------------------------------------------------------------------------------------
float LinkStats::loss_pct() const {
  if (this->window_n == 0)
    return __builtin_nanf("");
  uint64_t mask = this->window_n >= 64 ? ~0ULL : ((1ULL << this->window_n) - 1);
  return 100.0f * static_cast<float>(__builtin_popcountll(this->window & mask)) / this->window_n;
}

void LinkStats::record(bool lost) {
  this->window = (this->window << 1) | (lost ? 1ULL : 0ULL);
  if (this->window_n < 64)
    this->window_n++;
}

// ---------------------------------------------------------------------------------------------
void ModbusMaster::log_(int level, const char *fmt, ...) const {
  if (!this->log_fn_)
    return;
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  this->log_fn_(level, buf);
}

Device &ModbusMaster::add_device(const DeviceConfig &cfg) {
  this->devices_.emplace_back();
  Device &d = this->devices_.back();
  d.cfg_ = cfg;
  return d;
}

uint32_t ModbusMaster::job_period(const Device::Job &j) const {
  const uint32_t st = this->tier_ms_[static_cast<size_t>(PollTier::STATIC)];
  return j.unserved && j.interval_ms < st ? st : j.interval_ms;
}

Device *ModbusMaster::find_device_by_address(uint8_t addr) {
  for (auto &d : this->devices_)
    if (d.cfg_.address == addr)
      return &d;
  return nullptr;
}

uint32_t ModbusMaster::char_time_us_() const {
  uint32_t baud = this->timing_.baud ? this->timing_.baud : 9600;
  return (this->timing_.bits_per_char * 1000000u + baud - 1) / baud;
}

uint32_t ModbusMaster::frame_gap(const Device *d) const {
  uint32_t gap = this->timing_.frame_gap_ms;
  if (d != nullptr) {
    if (d->cfg_.frame_gap_ms >= 0) {
      gap = static_cast<uint32_t>(d->cfg_.frame_gap_ms);
    } else {
      const RegisterTable *t = d->table_ != nullptr ? d->table_ : d->cfg_.table;
      if (t != nullptr &&
          (t->family == Family::BATTERY || t->family == Family::CHARGER || t->family == Family::ENERGY_BRIDGE))
        gap = this->timing_.frame_gap_fast_ms;
    }
  }
  // Stock waits only nSendFrameSpace; keep the Modbus t3.5 floor anyway.
  const uint32_t t35 = (35u * this->char_time_us_() + 9999u) / 10000u;
  return gap > t35 ? gap : t35;
}

/// Request + reply characters of a read of `count` registers (8 B request, 5 + 2n B reply).
static uint32_t read_chars(uint16_t count) { return 8u + 5u + 2u * count; }

uint32_t ModbusMaster::estimate_ms(const Device *d, uint8_t /*fn*/, uint16_t count) const {
  return this->frame_gap(d) + (read_chars(count) * this->char_time_us_() + 999u) / 1000u + EST_SLACK_MS;
}

bool ModbusMaster::tunnel_range(Family f, uint16_t start, uint16_t count) {
  const uint32_t a = start, b = static_cast<uint32_t>(start) + (count ? count : 1) - 1;
  auto hit = [&](uint32_t lo, uint32_t hi) { return a <= hi && b >= lo; };
  if (f == Family::BATTERY)
    return hit(19101, 20000);
  if (f == Family::INVERTER)
    return hit(16000, 17500) || hit(29500, 29899) || hit(36450, 38999);
  return false;
}

void ModbusMaster::set_remembered(const std::vector<uint8_t> &addrs) {
  this->remembered_.clear();
  for (uint8_t a : addrs) {
    if (a < 1 || a > 247 || this->remembered_.size() >= MAX_REMEMBERED)
      continue;
    bool dup = false;
    for (uint8_t r : this->remembered_)
      dup |= r == a;
    if (!dup)
      this->remembered_.push_back(a);
  }
}

Device &ModbusMaster::add_inverter_slot_(uint8_t address, bool scan, bool remembered) {
  DeviceConfig c;
  c.index = static_cast<uint8_t>(this->devices_.size());
  c.address = address;
  c.table = nullptr;  // discover via the devinfo block
  // Used if the devinfo read is rejected: stock then runs the device as protocol 1.
  c.fallback_table =
      this->scan_fallback_ != nullptr ? this->scan_fallback_ : sungrow_datamodel::inverter_table_for_protocol(1);
  c.scan = scan;
  c.remembered = remembered;
  return this->add_device(c);
}

void ModbusMaster::init_device_(size_t di, uint32_t now) {
  Device &d = this->devices_[di];
  // inv_proto1_4 polls 30644 into sig 0x1002: keep inv_type (and the family) current.
  if (this->store_ != nullptr)
    this->store_->subscribe(d.cfg_.index, SIG_INV_TYPE, [this, di](const sungrow_datamodel::Signal &s) {
      this->on_inv_type_signal_(di, s.valid, s.bits());
    });
  d.next_probe_ = now;
  if (d.cfg_.table != nullptr) {
    this->activate_table_(d, d.cfg_.table, now);
  } else {
    d.state_ = Device::State::DISCOVER;
    this->log_(3, "dev %u (addr %u): table auto, discovering via devinfo %u+%u", d.cfg_.index, d.cfg_.address,
               DEVINFO_START, DEVINFO_COUNT);
  }
}

void ModbusMaster::start() {
  if (this->started_)
    return;
  this->started_ = true;
  uint32_t now = this->now_();
  const bool range = this->scan_first_ >= 1 && this->scan_last_ >= this->scan_first_;
  const size_t range_slots = range ? this->scan_last_ - this->scan_first_ + 1u : 0u;
  // Devices are append-only; reserve() keeps indices and references valid when the range probe,
  // the search or check_children_() add devices later.
  this->devices_.reserve(this->devices_.size() + this->remembered_.size() + range_slots + MAX_FOUND + MAX_CHILDREN);
  for (uint8_t a : this->remembered_)
    if (this->find_device_by_address(a) == nullptr)
      this->add_inverter_slot_(a, false, true);
  for (size_t di = 0; di < this->devices_.size(); di++)
    this->init_device_(di, now);
  if (!this->remembered_.empty())
    this->log_(3, "%u remembered inverter address(es), polled directly", (unsigned) this->remembered_.size());
  if (this->search_enabled_)
    this->request_search_(now);
  else if (range)
    this->arm_range_probe_(now);
}

void ModbusMaster::arm_range_probe_(uint32_t now) {
  if (this->scan_first_ < 1 || this->scan_last_ < this->scan_first_)
    return;
  if (this->range_created_) {
    for (auto &d : this->devices_) {
      if (d.cfg_.scan && d.table_ == nullptr) {
        d.abandoned_ = false;
        d.discover_fails_ = 0;
        d.state_ = Device::State::DISCOVER;
        d.next_probe_ = now;
      }
    }
    return;
  }
  this->range_created_ = true;
  for (uint16_t addr = this->scan_first_; addr <= this->scan_last_; addr++) {
    if (this->find_device_by_address(static_cast<uint8_t>(addr)) != nullptr)
      continue;
    if (this->devices_.size() >= this->devices_.capacity()) {
      this->log_(2, "range probe: device table full at address %u", addr);
      break;
    }
    this->add_inverter_slot_(static_cast<uint8_t>(addr), true, false);
    this->init_device_(this->devices_.size() - 1, now);
  }
  this->log_(3, "range probe: addresses %u..%u (%u attempts each)", this->scan_first_, this->scan_last_,
             this->scan_attempts_);
}

bool ModbusMaster::inverter_online_() const {
  for (const auto &d : this->devices_)
    if (d.online_ && d.table_ != nullptr && d.table_->family == Family::INVERTER)
      return true;
  return false;
}

void ModbusMaster::request_search_(uint32_t now) {
  if (!this->search_enabled_ || this->cur_search_)
    return;
  if (!this->search_due_)
    this->search_tries_ = 0;
  this->search_due_ = true;
  this->search_at_ = now;
}

void ModbusMaster::search_failed_(uint32_t now) {
  if (++this->search_tries_ < SEARCH_TRIES) {
    this->search_due_ = true;
    this->search_at_ = now;
    return;
  }
  this->search_tries_ = 0;
  this->log_(5, "search: no reply to the broadcast read of %u", SEARCH_REG);
  if (!this->inverter_online_()) {
    this->range_fallback_wanted_ = true;
    this->maybe_range_fallback_(now);
    this->search_due_ = true;
    this->search_at_ = now + SEARCH_REPEAT_MS;
  }
}

void ModbusMaster::maybe_range_fallback_(uint32_t now) {
  // Stock would now run its By-COM arbitration, which writes; the read-only range probe stands in
  // for it. It waits until every remembered address has been asked once, so a remembered
  // inverter that answers is not followed by a probe of the whole range.
  if (!this->range_fallback_wanted_ || this->range_fallback_spent_)
    return;
  if (this->inverter_online_()) {
    this->range_fallback_wanted_ = false;
    return;
  }
  for (const auto &d : this->devices_)
    if (d.cfg_.remembered && d.table_ == nullptr && !d.abandoned_ && d.stats_.consecutive_failures == 0)
      return;  // not asked yet
  this->range_fallback_wanted_ = false;
  this->range_fallback_spent_ = true;
  this->log_(3, "search unanswered and no inverter online: probing the scan range (read-only)");
  this->arm_range_probe_(now);
}

void ModbusMaster::forget_unanswered_remembered_(uint8_t found) {
  bool changed = false;
  for (auto &o : this->devices_) {
    if (!o.cfg_.remembered || o.cfg_.address == found || o.table_ != nullptr || o.abandoned_)
      continue;
    o.abandoned_ = true;
    for (size_t i = 0; i < this->remembered_.size(); i++) {
      if (this->remembered_[i] == o.cfg_.address) {
        this->remembered_.erase(this->remembered_.begin() + static_cast<std::ptrdiff_t>(i));
        changed = true;
        break;
      }
    }
    this->log_(3, "inverter at address %u; dropping remembered address %u", found, o.cfg_.address);
  }
  if (changed && this->listener_ != nullptr)
    this->listener_->on_remembered_changed();
}

void ModbusMaster::search_answered_(uint8_t address, uint32_t now) {
  this->search_tries_ = 0;
  Device *known = this->find_device_by_address(address);
  if (known != nullptr) {
    this->log_(5, "search: inverter answers at address %u (known)", address);
    if (known->table_ == nullptr) {
      known->abandoned_ = false;
      known->discover_fails_ = 0;
      known->state_ = Device::State::DISCOVER;
      known->next_probe_ = now;
    } else if (!known->online_) {
      known->next_probe_ = now;
    }
    return;
  }
  // A new address replaces the remembered ones that have not answered this boot (stock updates
  // or replaces its saved node the same way).
  this->forget_unanswered_remembered_(address);
  if (this->devices_.size() >= this->devices_.capacity() || this->devices_.size() >= 255) {
    this->log_(2, "search: inverter at address %u, but the device table is full", address);
    return;
  }
  this->add_inverter_slot_(address, false, true);
  this->init_device_(this->devices_.size() - 1, now);
  this->log_(3, "search: inverter at address %u", address);
}

void ModbusMaster::remember_(const Device &d) {
  if (!d.cfg_.scan && !d.cfg_.remembered)
    return;  // YAML-configured devices are not remembered
  for (uint8_t a : this->remembered_)
    if (a == d.cfg_.address)
      return;
  if (this->remembered_.size() >= MAX_REMEMBERED)
    this->remembered_.erase(this->remembered_.begin());
  this->remembered_.push_back(d.cfg_.address);
  if (this->listener_ != nullptr)
    this->listener_->on_remembered_changed();
}

void ModbusMaster::scan_miss_(Device &d, uint32_t now) {
  // Bus-scan slot: no reply means no device here. Retry immediately until the attempt budget is
  // spent, then abandon the slot (device_candidate_ skips it until rescan()).
  if (++d.discover_fails_ >= this->scan_attempts_) {
    d.abandoned_ = true;
    this->log_(5, "addr %u: no device, abandoning scan slot", d.cfg_.address);
  } else {
    d.next_probe_ = now;
  }
}

void ModbusMaster::use_fallback_table_(Device &d, uint32_t now) {
  // As stock: a rejected devinfo read means protocol 1, inverter type from the inv_proto1_4
  // poll of 30644. activate_table_ takes the protocol from the fallback table.
  const RegisterTable *tab = d.cfg_.fallback_table;
  this->log_(2, "dev %u: devinfo block rejected; using fallback table %s", d.cfg_.index,
             tab != nullptr ? tab->name : "(none)");
  this->report_unsupported_(d, "devinfo_rejected");
  if (tab == nullptr) {
    d.next_probe_ = now + this->timing_.offline_probe_ms;
    return;
  }
  // An exception means the slave is present (it answered): keep this scan slot alive.
  d.discover_fails_ = 0;
  d.abandoned_ = false;
  d.fallback_used_ = true;
  this->activate_table_(d, tab, now);
  this->set_online_(d, true, now);
  this->remember_(d);
  if (this->listener_ != nullptr)
    this->listener_->on_discovered(d);
}

void ModbusMaster::report_unsupported_(Device &d, const char *why) {
  if (d.unsupported_reported_)
    return;
  d.unsupported_reported_ = true;
  if (this->listener_ != nullptr)
    this->listener_->on_unsupported(d, why);
}

void Device::keep_ident_raw_(uint8_t fn, uint16_t start, const uint8_t *data, uint16_t nregs) {
  const uint16_t n = nregs < DEVINFO_COUNT ? nregs : DEVINFO_COUNT;
  std::memcpy(this->ident_raw_, data, 2u * n);
  this->ident_raw_regs_ = n;
  this->ident_raw_start_ = start;
  this->ident_raw_fn_ = fn;
}

void ModbusMaster::check_children_(uint32_t now) {
  this->child_check_due_ = false;
  if (this->store_ == nullptr)
    return;
  const size_t n = this->devices_.size();
  for (size_t pi = 0; pi < n; pi++) {
    const Device &p = this->devices_[pi];
    if (p.table_ == nullptr || p.table_->family != Family::INVERTER || !p.online_)
      continue;
    const uint8_t pidx = p.cfg_.index;
    auto addr_of = [&](uint16_t sig) -> int {
      const sungrow_datamodel::Signal *s = this->store_->get(pidx, sig);
      if (s == nullptr || !s->valid)
        return -1;
      const uint64_t v = s->bits();
      return v >= 1 && v <= 247 ? static_cast<int>(v) : -1;  // 0 / 0xFFFF = empty slot
    };
    int a = addr_of(SIG_SBR_ADDR_FIRST);
    if (a > 0)
      this->add_child_(pi, static_cast<uint8_t>(a), ChildKind::SBR, now);
    for (uint16_t sig : SIG_SBR_ADDR_MORE) {
      a = addr_of(sig);
      if (a > 0)
        this->add_child_(pi, static_cast<uint8_t>(a), ChildKind::SBR, now);
    }
    // 5621: values >= 6 are ignored, bit 0 = EV charger, bit 2 = Energy Bridge.
    const sungrow_datamodel::Signal *f = this->store_->get(pidx, SIG_EXT_DEVICES);
    if (f != nullptr && f->valid && f->bits() < 6) {
      if (f->bits() & 0x1)
        this->add_child_(pi, CHARGER_ADDRESS, ChildKind::CHARGER, now);
      if (f->bits() & 0x4)
        this->add_child_(pi, ENERGY_BRIDGE_ADDRESS, ChildKind::ENERGY_BRIDGE, now);
    }
  }
}

void ModbusMaster::add_child_(size_t parent, uint8_t address, ChildKind kind, uint32_t now) {
  const ChildProbe *cp = child_probe(kind);
  Device *existing = this->find_device_by_address(address);
  if (existing != nullptr) {
    // Known address. An empty scan slot there becomes the reported child; anything else stays.
    if (existing->cfg_.scan && !existing->discovered_ && existing->table_ == nullptr) {
      existing->cfg_.scan = false;
      existing->cfg_.child_kind = kind;
      existing->cfg_.parent = static_cast<int16_t>(parent);
      existing->abandoned_ = false;
      existing->discover_fails_ = 0;
      existing->state_ = Device::State::DISCOVER;
      existing->next_probe_ = now;
      this->log_(3, "dev %u: inverter reports a %s at address %u; probing fn 4 %u+%u", existing->cfg_.index,
                 cp->label, address, cp->start, cp->count);
    }
    return;
  }
  if (this->devices_.size() >= this->devices_.capacity() || this->devices_.size() >= 255) {
    this->log_(2, "inverter reports a %s at address %u, but the device table is full", cp->label, address);
    return;
  }
  DeviceConfig c;
  c.index = static_cast<uint8_t>(this->devices_.size());
  c.address = address;
  c.child_kind = kind;
  c.parent = static_cast<int16_t>(parent);
  Device &d = this->add_device(c);
  d.state_ = Device::State::DISCOVER;
  d.next_probe_ = now;
  this->log_(3, "dev %u: inverter dev %u reports a %s at address %u; probing fn 4 %u+%u", c.index,
             this->devices_[parent].cfg_.index, cp->label, address, cp->start, cp->count);
}

void ModbusMaster::rescan() {
  uint32_t now = this->now_();
  for (auto &d : this->devices_) {
    if ((d.cfg_.scan || d.cfg_.remembered) && d.table_ == nullptr) {
      // Re-arm a silent scan slot or remembered address; leave discovered devices running.
      d.abandoned_ = false;
      d.discover_fails_ = 0;
      d.state_ = Device::State::DISCOVER;
      d.next_probe_ = now;
    }
  }
  this->range_fallback_spent_ = false;
  if (this->search_enabled_)
    this->request_search_(now);
  else
    this->arm_range_probe_(now);
}

void ModbusMaster::activate_table_(Device &d, const RegisterTable *t, uint32_t now) {
  d.table_ = t;
  d.state_ = Device::State::RUN;
  // Identity: YAML dev_type wins, then the devinfo block, then the table family. Without a
  // devinfo block an inverter table stands for its protocol type.
  if (d.cfg_.dev_type != 0) {
    d.devinfo_.dev_type = d.cfg_.dev_type;
    d.dev_type_src_ = Device::DevTypeSource::YAML;
  } else if (d.discovered_) {
    d.dev_type_src_ = Device::DevTypeSource::DEVINFO;
  } else {
    d.devinfo_.dev_type = dev_type_for_family(t->family);
    d.dev_type_src_ = Device::DevTypeSource::TABLE;
  }
  if (!d.discovered_ && t->family == Family::INVERTER)
    d.devinfo_.protocol = protocol_for_table(t);
  if (this->store_ != nullptr)
    this->store_->set_dev_type(d.cfg_.index, d.devinfo_.dev_type);
  if (this->store_ != nullptr && d.devinfo_.protocol != 0)
    this->store_->set_scalar(d.cfg_.index, SIG_PROTOCOL_TYPE, sungrow_datamodel::TYPE_U16, 0.0f,
                             d.devinfo_.protocol, 2);
  // Capability gating applies to protocol 2/3 only (as stock); YAML can force it.
  d.gate_ = d.cfg_.gate_capabilities < 0
                ? (t->id == static_cast<uint8_t>(TableId::INV_PROTO2) ||
                   t->id == static_cast<uint8_t>(TableId::INV_PROTO3))
                : d.cfg_.gate_capabilities != 0;
  // fn 119 (0x77) rows are never polled cyclically (as stock); 0x70/0x73 are SP600 optimizer
  // frames, not needed for a plain inverter.
  PollPlanOptions popts;
  if (d.cfg_.sbr_cells) {
    // The cell block is read whole by its own job, so its rows stay out of the plan.
    popts.filter = [](const RegisterRow &r) {
      return r.regaddr + r.nreg <= SBR_CELL_BLOCK_START || r.regaddr >= SBR_CELL_BLOCK_START + SBR_CELL_BLOCK_COUNT;
    };
  }
  std::vector<PlannedRead> plan = build_poll_plan(*t, popts);
  const size_t sbr_index = plan.size();
  if (d.cfg_.sbr_cells) {
    PlannedRead s{};
    s.req.start = SBR_CELL_BLOCK_START;
    s.req.count = SBR_CELL_BLOCK_COUNT;
    s.req.fn = FN_READ_INPUT;
    s.tier = PollTier::SLOW;
    plan.push_back(s);
  }
  // Initial-pass order: the read carrying the capability bitmap first (gated rows decode against
  // it), then the plan order (FAST first).
  std::vector<size_t> order;
  order.reserve(plan.size());
  for (uint16_t i = 0; i < t->count && order.empty(); i++) {
    const RegisterRow &r = t->rows[i];
    if (r.sigid != sungrow_datamodel::SIG_CAPABILITY_BITMAP)
      continue;
    for (size_t k = 0; k < plan.size(); k++)
      if (plan[k].req.fn == r.fn && r.regaddr >= plan[k].req.start && r.regaddr < plan[k].req.start + plan[k].req.count)
        order.push_back(k);
  }
  for (size_t k = 0; k < plan.size(); k++)
    if (order.empty() || order[0] != k)
      order.push_back(k);
  // Phases: the FAST reads share one phase (one coherent snapshot per period); the reads of every
  // other tier are spread evenly over their period, offset per device so devices interleave.
  size_t per_tier[POLL_TIER_COUNT] = {0, 0, 0, 0};
  for (const auto &p : plan)
    per_tier[static_cast<size_t>(p.tier)]++;
  size_t seen[POLL_TIER_COUNT] = {0, 0, 0, 0};
  const uint32_t dev_off = (static_cast<uint32_t>(d.cfg_.index) * 25033u) & 0xFFFFu;  // ~0.382 per device
  d.jobs_.clear();
  d.jobs_.reserve(plan.size());
  size_t ntier[POLL_TIER_COUNT] = {0, 0, 0, 0};
  for (size_t k : order) {
    const PlannedRead &p = plan[k];
    const size_t ti = static_cast<size_t>(p.tier);
    Device::Job j{};
    j.req = p.req;
    j.tier = p.tier;
    j.interval_ms = k == sbr_index ? d.cfg_.sbr_interval_ms : this->tier_ms_[ti];
    const uint32_t n = static_cast<uint32_t>(per_tier[ti]);
    j.phase_frac = p.tier == PollTier::FAST ? 0
                                            : static_cast<uint16_t>((seen[ti]++ * 65536u + dev_off) / n);
    d.jobs_.push_back(j);
    ntier[ti]++;
  }
  d.plan_gen_++;
  this->restart_pass_(d, now);
  this->log_(3,
             "dev %u (addr %u): table %s, %u reads (fast %u, live %u, slow %u, static %u), capability gating %s, "
             "word order %u, gap %u ms",
             d.cfg_.index, d.cfg_.address, t->name, (unsigned) d.jobs_.size(), (unsigned) ntier[0],
             (unsigned) ntier[1], (unsigned) ntier[2], (unsigned) ntier[3], d.gate_ ? "on" : "off",
             (unsigned) d.cfg_.word_order, (unsigned) this->frame_gap(&d));
}

void ModbusMaster::restart_pass_(Device &d, uint32_t now) {
  d.first_round_done_ = d.jobs_.empty();
  d.pending_ = d.jobs_.size();
  for (auto &j : d.jobs_) {
    j.pending = true;
    j.in_flight = false;
    j.next_due = now;
  }
}

// ---------------------------------------------------------------------------------------------
bool ModbusMaster::read_registers(uint8_t addr, uint8_t fn, uint16_t start, uint16_t count, AdhocCallback cb) {
  if ((fn != FN_READ_HOLDING && fn != FN_READ_INPUT) || start == 0 || count == 0 || count > MAX_READ_REGS ||
      this->adhoc_.size() >= ADHOC_QUEUE_MAX)
    return false;
  Txn t;
  t.kind = Kind::ADHOC;
  t.addr = addr;
  t.fn = fn;
  t.start = start;
  t.count = count;
  t.cb = std::move(cb);
  this->adhoc_.push_back(std::move(t));
  return true;
}

const Device *ModbusMaster::find_device_(uint8_t addr) const {
  for (const auto &d : this->devices_)
    if (d.cfg_.address == addr)
      return &d;
  return nullptr;
}

bool ModbusMaster::can_write(uint8_t addr) const {
  const Device *d = this->find_device_(addr);
  return d != nullptr && d->state_ == Device::State::RUN && d->table_ != nullptr && d->online_;
}

WriteStatus ModbusMaster::submit_write(uint8_t addr, uint16_t start, const uint16_t *values, uint16_t count,
                                       const WriteOptions &opt, WriteCallback cb) {
  // Writes need the device online. They interleave with the polls (stock pauses new poll rounds
  // during a write). A write starting at the country register triggers the :502 country rule
  // through ControlCore's country-written callback (sungrow_modbus_tcp).
  WriteStatus rej = WriteStatus::QUEUED;
  uint8_t fn = opt.fn != 0 ? opt.fn : (count == 1 ? FN_WRITE_SINGLE : FN_WRITE_MULTIPLE);
  if (start == 0 || count == 0 || count > MAX_WRITE_REGS || values == nullptr || !is_write_fn(fn) ||
      (fn == FN_WRITE_SINGLE && count != 1) || static_cast<uint32_t>(start) + count > 0x10000u || addr == 0 ||
      (opt.verify && count > MAX_READ_REGS)) {
    rej = WriteStatus::INVALID;
  } else if (!this->can_write(addr)) {
    rej = WriteStatus::OFFLINE;
  } else if (this->writes_.size() >= WRITE_QUEUE_MAX) {
    rej = WriteStatus::QUEUE_FULL;
  }
  const uint32_t now = this->now_();
  if (rej == WriteStatus::QUEUED && this->timing_.write_rate_limit_ms != 0) {
    for (const auto &e : this->rate_) {
      if (e.addr == addr && e.reg >= start && e.reg < start + count &&
          now - e.t < this->timing_.write_rate_limit_ms) {
        rej = WriteStatus::RATE_LIMITED;
        break;
      }
    }
  }
  if (rej != WriteStatus::QUEUED) {
    this->wstats_.rejected++;
    this->log_(2, "write addr %u start %u count %u rejected: %s", addr, start, count, write_status_name(rej));
    return rej;
  }
  if (this->timing_.write_rate_limit_ms != 0) {
    for (uint16_t i = 0; i < count; i++) {
      const uint16_t reg = static_cast<uint16_t>(start + i);
      RateEntry *slot = nullptr;
      for (auto &e : this->rate_)
        if (e.addr == addr && e.reg == reg)
          slot = &e;
      if (slot == nullptr) {
        if (this->rate_.size() < RATE_TABLE_MAX) {
          this->rate_.push_back({addr, reg, now});
          continue;
        }
        slot = &this->rate_[0];  // evict the oldest
        for (auto &e : this->rate_)
          if (now - e.t > now - slot->t)
            slot = &e;
      }
      *slot = {addr, reg, now};
    }
  }
  Txn t;
  t.kind = Kind::WRITE;
  t.addr = addr;
  t.fn = fn;
  t.start = start;
  t.count = count;
  t.write_values.assign(values, values + count);
  t.wcb = std::move(cb);
  t.verify = opt.verify;
  this->writes_.push_back(std::move(t));
  return WriteStatus::QUEUED;
}

void ModbusMaster::finish_write_(Txn &t, WriteStatus st, uint8_t exception, const uint8_t *data, uint16_t nregs) {
  WriteResult r;
  r.status = st;
  r.exception = exception;
  r.addr = t.addr;
  r.fn = t.kind == Kind::VERIFY ? (t.count == 1 ? FN_WRITE_SINGLE : FN_WRITE_MULTIPLE) : t.fn;
  r.start = t.start;
  r.count = t.count;
  if (data != nullptr)
    for (uint16_t i = 0; i < nregs; i++)
      r.readback.push_back(static_cast<uint16_t>(data[2 * i] << 8 | data[2 * i + 1]));
  if (st == WriteStatus::OK)
    this->wstats_.ok++;
  else
    this->wstats_.failed++;
  if (st == WriteStatus::VERIFY_MISMATCH)
    this->wstats_.verify_mismatch++;
  if (t.wcb) {
    WriteCallback cb = std::move(t.wcb);
    t.wcb = nullptr;
    cb(r);
  }
}

bool ModbusMaster::wants_fast_loop() const {
  // settle_active_ stays set until the next frame is about to go out; only the gap itself counts.
  return this->state_ == TxState::WAIT || this->retry_pending_ || !this->writes_.empty() || !this->adhoc_.empty() ||
         (this->settle_active_ && !due(this->now_(), this->settle_until_));
}

void ModbusMaster::write_done_(uint32_t now) {
  if (this->timing_.write_settle_ms != 0) {
    this->settle_until_ = now + this->timing_.write_settle_ms;
    this->settle_active_ = true;
  }
}

// ---------------------------------------------------------------------------------------------
// Scheduler: slot grid per read, FAST reads first, everything else in the gaps between FAST slots.
uint32_t ModbusMaster::next_slot_(const Device::Job &j, uint32_t after) const {
  const uint32_t p = this->job_period(j);
  const uint32_t phase = static_cast<uint32_t>((static_cast<uint64_t>(j.phase_frac) * p) >> 16);
  const uint32_t into = (after - phase) % p;  // position inside the period
  return after + (p - into);
}

bool ModbusMaster::runnable_(const Device &d) const {
  return !d.abandoned_ && d.state_ == Device::State::RUN && d.online_ && d.table_ != nullptr && !d.inv_type_pending_;
}

bool ModbusMaster::next_fast_due_(uint32_t *out) const {
  bool have = false;
  uint32_t best = 0;
  for (const auto &d : this->devices_) {
    if (!this->runnable_(d))
      continue;
    for (const auto &j : d.jobs_) {
      if (j.in_flight || j.pending || j.effective_tier() != PollTier::FAST)
        continue;
      if (!have || before(j.next_due, best)) {
        best = j.next_due;
        have = true;
      }
    }
  }
  *out = best;
  return have;
}

void ModbusMaster::take_job_(int di, int ji, uint32_t now, Txn *out) {
  Device &d = this->devices_[di];
  Device::Job &j = d.jobs_[ji];
  out->kind = Kind::POLL;
  out->dev = di;
  out->job = ji;
  out->addr = d.cfg_.address;
  out->fn = j.req.fn;
  out->start = j.req.start;
  out->count = j.req.count;
  out->attempts = 0;
  out->plan_gen = d.plan_gen_;
  out->cb = nullptr;
  out->write_values.clear();
  if (j.pending) {
    j.pending = false;
    d.pending_--;
  }
  j.in_flight = true;
  j.sent_at = now;
  // Next slot more than half a period away: a read sent late keeps its grid, one sent very late
  // (or in the initial pass) skips the slot it would otherwise repeat at once.
  j.next_due = this->next_slot_(j, now + this->job_period(j) / 2);
}

bool ModbusMaster::control_candidate_(int di, uint32_t now, Txn *out) {
  Device &d = this->devices_[di];
  if (d.abandoned_)
    return false;  // scan slot with no device: never send to it again (until rescan())
  out->dev = di;
  out->addr = d.cfg_.address;
  out->attempts = 0;
  out->job = -1;
  out->cb = nullptr;
  out->write_values.clear();
  if (d.state_ == Device::State::DISCOVER) {
    if (!due(now, d.next_probe_))
      return false;
    out->kind = Kind::DISCOVERY;
    out->fn = FN_READ_INPUT;
    const ChildProbe *cp = child_probe(d.cfg_.child_kind);
    out->start = cp != nullptr ? cp->start : DEVINFO_START;
    out->count = cp != nullptr ? cp->count : DEVINFO_COUNT;
    return true;
  }
  if (!d.online_) {
    if (!due(now, d.next_probe_))
      return false;
    // Probe with the most frequently polled read (cheapest proof of life that is useful).
    int best = -1;
    for (size_t i = 0; i < d.jobs_.size(); i++) {
      const auto &j = d.jobs_[i];
      if (best < 0 || j.tier < d.jobs_[best].tier ||
          (j.tier == d.jobs_[best].tier && j.interval_ms < d.jobs_[best].interval_ms))
        best = static_cast<int>(i);
    }
    if (best < 0)
      return false;
    out->kind = Kind::PROBE;
    out->job = best;
    out->fn = d.jobs_[best].req.fn;
    out->start = d.jobs_[best].req.start;
    out->count = d.jobs_[best].req.count;
    return true;
  }
  if (d.inv_type_pending_) {
    out->kind = Kind::INVTYPE;
    out->fn = FN_READ_HOLDING;
    out->start = INV_TYPE_REG;
    out->count = 1;
    return true;
  }
  return false;
}

bool ModbusMaster::pick_(Txn *out, uint32_t now) {
  while (!this->writes_.empty()) {
    Txn w = std::move(this->writes_.front());
    this->writes_.pop_front();
    if (!this->can_write(w.addr)) {
      // Went offline (or was re-discovered) while queued: never send it.
      this->log_(2, "%s addr %u start %u dropped: device offline", kind_name(static_cast<int>(w.kind)), w.addr,
                 w.start);
      this->finish_write_(w, w.kind == Kind::VERIFY ? WriteStatus::VERIFY_FAILED : WriteStatus::OFFLINE, 0,
                          nullptr, 0);
      continue;
    }
    *out = std::move(w);
    Device *d = this->find_device_by_address(out->addr);
    out->dev = d == nullptr ? -1 : static_cast<int>(d - this->devices_.data());
    out->attempts = 0;
    return true;
  }
  if (this->excl_) {
    // Exclusive: the owner's ad-hoc reads only, nothing cyclic.
    if (this->adhoc_.empty())
      return false;
    *out = std::move(this->adhoc_.front());
    this->adhoc_.pop_front();
    Device *dd = this->find_device_by_address(out->addr);
    out->dev = dd == nullptr ? -1 : static_cast<int>(dd - this->devices_.data());
    out->attempts = 0;
    return true;
  }
  const size_t n = this->devices_.size();
  // 1. Due FAST reads, earliest slot first.
  int bd = -1, bj = -1;
  uint32_t bdue = 0;
  for (size_t di = 0; di < n; di++) {
    const Device &d = this->devices_[di];
    if (!this->runnable_(d))
      continue;
    for (size_t ji = 0; ji < d.jobs_.size(); ji++) {
      const Device::Job &j = d.jobs_[ji];
      if (j.in_flight || j.pending || j.effective_tier() != PollTier::FAST || !due(now, j.next_due))
        continue;
      if (bd < 0 || before(j.next_due, bdue)) {
        bd = static_cast<int>(di);
        bj = static_cast<int>(ji);
        bdue = j.next_due;
      }
    }
  }
  if (bd >= 0) {
    this->take_job_(bd, bj, now, out);
    return true;
  }
  // Everything else must end before the next FAST slot (waived after FAST_RESERVE_MAX_WAIT_MS of
  // holding back, so nothing starves when the FAST set fills the bus).
  uint32_t fast_next = 0;
  const bool have_fast = this->next_fast_due_(&fast_next);
  const bool waived = this->blocked_ && now - this->blocked_since_ >= FAST_RESERVE_MAX_WAIT_MS;
  bool held = false;
  auto fits = [&](const Device *d, uint16_t count) {
    if (!have_fast || waived ||
        static_cast<int32_t>(fast_next - now) >= static_cast<int32_t>(this->estimate_ms(d, 4, count)))
      return true;
    held = true;
    return false;
  };
  auto taken = [&]() {
    this->blocked_ = false;
    return true;
  };
  auto hold = [&]() {
    if (held && !this->blocked_) {
      this->blocked_ = true;
      this->blocked_since_ = now;
    }
    return false;
  };
  // 2. Ad-hoc reads (Modbus-TCP forwards, web UI, cloud), in arrival order.
  if (!this->adhoc_.empty()) {
    const Device *d = this->find_device_(this->adhoc_.front().addr);
    if (fits(d, this->adhoc_.front().count)) {
      *out = std::move(this->adhoc_.front());
      this->adhoc_.pop_front();
      Device *dd = this->find_device_by_address(out->addr);
      out->dev = dd == nullptr ? -1 : static_cast<int>(dd - this->devices_.data());
      out->attempts = 0;
      return taken();
    }
    return hold();  // keep arrival order: nothing else jumps a waiting ad-hoc read
  }
  // 3. The broadcast inverter search.
  if (this->search_due_ && due(now, this->search_at_) && fits(nullptr, 1)) {
    this->search_due_ = false;
    this->cur_search_ = true;
    Txn t;
    t.kind = Kind::SEARCH;
    t.dev = -1;
    t.addr = 0;  // broadcast; the inverter replies from slave id 0
    t.fn = FN_READ_HOLDING;
    t.start = SEARCH_REG;
    t.count = 1;
    *out = std::move(t);
    return taken();
  }
  // 4. Discovery, offline probes and inverter-type reads, devices in turn. A device with a table
  // whose initial pass is still open goes first, so its values (and everything that waits for
  // them, e.g. the MQTT publish) are not held up behind the discovery timeouts of empty scan
  // slots.
  for (int pass = 0; pass < 2; pass++) {
    for (size_t k = 0; k < n; k++) {
      const size_t di = (this->rr_ + k) % n;
      const Device &d = this->devices_[di];
      if (pass == 0 && (d.table_ == nullptr || d.first_round_done_))
        continue;
      Txn c;
      if (this->control_candidate_(static_cast<int>(di), now, &c) && fits(&d, c.count)) {
        *out = std::move(c);
        this->rr_ = (di + 1) % n;
        return taken();
      }
    }
  }
  // 5. Initial pass: each device's unsent reads in order, devices in turn.
  for (size_t k = 0; k < n; k++) {
    const size_t di = (this->rr_ + k) % n;
    const Device &d = this->devices_[di];
    if (!this->runnable_(d) || d.pending_ == 0)
      continue;
    for (size_t ji = 0; ji < d.jobs_.size(); ji++) {
      if (!d.jobs_[ji].pending)
        continue;
      if (!fits(&d, d.jobs_[ji].req.count))
        break;
      this->take_job_(static_cast<int>(di), static_cast<int>(ji), now, out);
      this->rr_ = (di + 1) % n;
      return taken();
    }
  }
  // 6. Other due reads: lowest tier, then earliest slot; a read that does not fit before the next
  // FAST slot lets a smaller one go first.
  bd = -1;
  PollTier btier = PollTier::STATIC;
  for (size_t di = 0; di < n; di++) {
    const Device &d = this->devices_[di];
    if (!this->runnable_(d))
      continue;
    for (size_t ji = 0; ji < d.jobs_.size(); ji++) {
      const Device::Job &j = d.jobs_[ji];
      if (j.in_flight || j.pending || !due(now, j.next_due))
        continue;
      const PollTier tier = j.effective_tier();
      if (bd >= 0 && (tier > btier || (tier == btier && !before(j.next_due, bdue))))
        continue;
      if (!fits(&d, j.req.count))
        continue;
      bd = static_cast<int>(di);
      bj = static_cast<int>(ji);
      btier = tier;
      bdue = j.next_due;
    }
  }
  if (bd >= 0) {
    this->take_job_(bd, bj, now, out);
    return taken();
  }
  return hold();
}

void ModbusMaster::send_(uint32_t now) {
  Txn &t = this->cur_;
  if (t.fn == FN_WRITE_SINGLE) {
    this->frame_len_ =
        build_write_single_request(t.addr, static_cast<uint16_t>(t.start - 1), t.write_values[0], this->frame_);
  } else if (t.fn == FN_WRITE_MULTIPLE) {
    this->frame_len_ = build_write_request(t.addr, static_cast<uint16_t>(t.start - 1), t.write_values.data(),
                                           t.count, this->frame_, sizeof(this->frame_));
  } else {
    this->frame_len_ = build_read_request(t.addr, t.fn, static_cast<uint16_t>(t.start - 1), t.count, this->frame_);
  }
  ExpectedReply e;
  e.addr = t.addr;
  e.fn = t.fn;
  e.wire_start = static_cast<uint16_t>(t.start - 1);
  e.count = t.count;
  e.value = t.fn == FN_WRITE_SINGLE ? t.write_values[0] : 0;
  this->parser_.begin(e);
  if (this->transport_ != nullptr) {
    this->transport_->discard_input();
    this->transport_->send_frame(this->frame_, this->frame_len_);
  }
  if (t.dev >= 0)
    this->devices_[t.dev].stats_.sent++;
  // The timeout runs from the send (as stock COM_Master) and covers our frame, the slave's
  // latency and the whole reply (a 120-register reply is ~255 ms at 9600).
  const bool is_write = t.kind == Kind::WRITE;
  uint32_t timeout = this->timing_.response_timeout_ms;
  switch (t.kind) {
    case Kind::WRITE:
      timeout = this->timing_.write_timeout_ms;
      break;
    case Kind::SEARCH:
      timeout = SEARCH_TIMEOUT_MS;
      break;
    case Kind::POLL:
    case Kind::PROBE:
    case Kind::DISCOVERY:
      timeout = (read_chars(t.count) * this->char_time_us_() + 999u) / 1000u + this->timing_.poll_timeout_margin_ms;
      break;
    default:
      break;
  }
  if (!is_write && t.kind != Kind::SEARCH) {
    const Device *dv = t.dev >= 0 ? &this->devices_[t.dev] : nullptr;
    const RegisterTable *tab = dv == nullptr ? nullptr : (dv->table_ != nullptr ? dv->table_ : dv->cfg_.table);
    const Family fam = tab != nullptr ? tab->family : Family::INVERTER;
    if (tunnel_range(fam, t.start, t.count) && timeout < this->timing_.tunnel_timeout_ms)
      timeout = this->timing_.tunnel_timeout_ms;
  }
  this->deadline_ = now + timeout;
  this->state_ = TxState::WAIT;
  if (is_write)
    this->log_(5, "TX write addr %u fn %u start %u count %u first 0x%04X", t.addr, t.fn, t.start, t.count,
               t.write_values.empty() ? 0 : t.write_values[0]);
  this->log_(6, "TX %s addr %u fn %u start %u count %u (try %u)", kind_name(static_cast<int>(t.kind)), t.addr,
             t.fn, t.start, t.count, t.attempts + 1);
}

void ModbusMaster::tick() {
  if (!this->started_)
    this->start();
  uint32_t now = this->now_();
  if (this->excl_ && due(now, this->excl_until_)) {
    this->log_(2, "exclusive bus not released by its owner: released after the time limit");
    this->release_exclusive();
  }
  if (this->raw_)
    return;  // the owner runs a non-Modbus exchange on the transport
  if (this->child_check_due_)
    this->check_children_(now);
  if (this->state_ == TxState::WAIT) {
    uint8_t buf[64];
    // Bounded drain: a full frame is at most 255 B.
    for (int i = 0; i < 8 && this->transport_ != nullptr; i++) {
      size_t n = this->transport_->receive(buf, sizeof(buf));
      if (n == 0)
        break;
      ParseStatus st = this->parser_.feed(buf, n);
      if (st != ParseStatus::INCOMPLETE)
        break;
    }
    switch (this->parser_.status()) {
      case ParseStatus::OK:
        this->finish_ok_(now);
        return;
      case ParseStatus::EXCEPTION:
        this->finish_exception_(now, this->parser_.exception_code());
        return;
      default:
        break;
    }
    if (due(now, this->deadline_))
      this->attempt_failed_(now, this->parser_.crc_errors() > 0);
    return;
  }
  if (this->retry_pending_) {
    const Device *d = this->cur_.dev >= 0 ? &this->devices_[this->cur_.dev] : nullptr;
    if (!due(now, this->last_bus_end_ + this->frame_gap(d)))
      return;
    this->retry_pending_ = false;
    this->send_(now);
    return;
  }
  if (!this->have_next_) {
    if (!this->pick_(&this->next_, now))
      return;
    this->have_next_ = true;
  }
  // Gap before each frame (stock vTaskDelay(send_delay) in COM_Master), per target device.
  const Device *d = this->next_.dev >= 0 ? &this->devices_[this->next_.dev] : nullptr;
  if (!due(now, this->last_bus_end_ + this->frame_gap(d)))
    return;
  if (this->settle_active_) {
    if (!due(now, this->settle_until_))
      return;
    this->settle_active_ = false;
  }
  this->cur_ = std::move(this->next_);
  this->have_next_ = false;
  this->send_(now);
}

// ---------------------------------------------------------------------------------------------
bool ModbusMaster::acquire_exclusive(uint32_t max_ms) {
  if (this->excl_)
    return false;
  this->excl_ = true;
  this->raw_ = false;
  this->excl_until_ = this->now_() + max_ms;
  this->blocked_ = false;
  this->log_(3, "bus held exclusively (polling paused, limit %u s)", (unsigned) (max_ms / 1000));
  return true;
}

void ModbusMaster::extend_exclusive(uint32_t max_ms) {
  if (this->excl_)
    this->excl_until_ = this->now_() + max_ms;
}

void ModbusMaster::release_exclusive() {
  if (!this->excl_)
    return;
  if (this->raw_ && this->transport_ != nullptr)
    this->transport_->discard_input();
  this->excl_ = false;
  this->raw_ = false;
  // The bus was silent for the master: the next frame needs only its normal gap from now.
  this->last_bus_end_ = this->now_();
  this->log_(3, "bus released (polling resumes)");
}

bool ModbusMaster::set_raw(bool on) {
  if (!on) {
    if (this->raw_ && this->transport_ != nullptr)
      this->transport_->discard_input();
    if (this->raw_)
      this->last_bus_end_ = this->now_();
    this->raw_ = false;
    return true;
  }
  if (!this->excl_ || !this->idle())
    return false;
  this->raw_ = true;
  return true;
}

void ModbusMaster::count_ok_(Device &d, uint32_t now) {
  d.stats_.received++;
  d.stats_.record(false);
  d.stats_.consecutive_failures = 0;
  d.last_ok_ms_ = now;
}

void ModbusMaster::count_fail_(Device &d, uint32_t now) {
  if (d.stats_.consecutive_failures < 0xFFFF)
    d.stats_.consecutive_failures++;
  if (d.online_ && d.stats_.consecutive_failures >= this->timing_.offline_after &&
      now - d.last_ok_ms_ >= this->timing_.offline_silence_ms)
    this->set_online_(d, false, now);
}

bool ModbusMaster::read_served_(const Device &d, const Device::Job &j) const {
  if (this->store_ == nullptr || d.table_ == nullptr)
    return true;
  const uint32_t end = static_cast<uint32_t>(j.req.start) + j.req.count;
  for (uint16_t i = 0; i < d.table_->count; i++) {
    const RegisterRow &r = d.table_->rows[i];
    if (r.sigid == 0 || r.fn != j.req.fn || r.regaddr < j.req.start || static_cast<uint32_t>(r.regaddr) + r.nreg > end)
      continue;
    // An empty fault list is information too ("no active faults").
    if (r.sigid == sungrow_datamodel::SIG_FAULT_LIST || this->store_->is_valid(d.cfg_.index, r.sigid))
      return true;
  }
  return false;
}

void ModbusMaster::job_done_(Device &d, int ji, uint32_t now, bool answered, bool exception) {
  Device::Job &j = d.jobs_[ji];
  j.in_flight = false;
  if (answered) {
    if (!exception)
      j.last_ok = now;
    // Judge "unserved" only once gated rows can decode (the capability bitmap is stored).
    const bool judge = exception || !d.gate_ || this->store_ == nullptr ||
                       this->store_->is_valid(d.cfg_.index, sungrow_datamodel::SIG_CAPABILITY_BITMAP);
    if (judge) {
      const bool unserved = exception || !this->read_served_(d, j);
      if (unserved != j.unserved) {
        j.unserved = unserved;
        j.next_due = this->next_slot_(j, j.sent_at + this->job_period(j) / 2);
        this->log_(unserved ? 5 : 3, "dev %u: fn %u %u+%u %s; polled every %u s", d.cfg_.index, j.req.fn,
                   j.req.start, j.req.count,
                   exception ? "rejected (exception)" : (unserved ? "holds no values" : "holds values again"),
                   (unsigned) (this->job_period(j) / 1000));
      }
    }
    if (!exception && !j.unserved) {
      // Freshness = answers of the device's fastest tier that carries values.
      PollTier best = PollTier::STATIC;
      for (const auto &o : d.jobs_)
        if (!o.unserved && o.tier < best)
          best = o.tier;
      if (j.tier == best) {
        d.fresh_ms_ = now;
        d.fresh_count_++;
      }
    }
  }
  if (!d.first_round_done_ && d.pending_ == 0) {
    bool busy = false;
    for (const auto &o : d.jobs_)
      busy |= o.in_flight;
    if (!busy) {
      d.first_round_done_ = true;
      this->log_(5, "dev %u: initial pass done (%u reads)", d.cfg_.index, (unsigned) d.jobs_.size());
    }
  }
}

void ModbusMaster::set_online_(Device &d, bool online, uint32_t now) {
  if (d.online_ == online)
    return;
  d.online_ = online;
  if (online) {
    // Reconnect: every read once (initial pass), then the slot grid.
    d.last_ok_ms_ = now;
    this->restart_pass_(d, now);
    this->log_(3, "dev %u (addr %u): online", d.cfg_.index, d.cfg_.address);
    // An inverter is back: a queued repeat search is dropped.
    if (d.table_ != nullptr && d.table_->family == Family::INVERTER && !this->cur_search_) {
      this->search_due_ = false;
      this->search_tries_ = 0;
    }
  } else {
    for (auto &j : d.jobs_)
      j.in_flight = false;
    d.next_probe_ = now + this->timing_.offline_probe_ms;
    size_t n = 0;
    if (this->store_ != nullptr && d.table_ != nullptr)
      n = this->store_->clear_offline(d.cfg_.index, *d.table_);
    this->log_(2, "dev %u (addr %u): offline after %u failed requests; %u clear-on-offline signals cleared",
               d.cfg_.index, d.cfg_.address, d.stats_.consecutive_failures, (unsigned) n);
    // Stock searches again whenever an inverter drops out (it may have a new address).
    if (d.table_ != nullptr && d.table_->family == Family::INVERTER)
      this->request_search_(now);
  }
  if (this->listener_ != nullptr)
    this->listener_->on_online_changed(d);
}

void ModbusMaster::attempt_failed_(uint32_t now, bool crc) {
  Txn &t = this->cur_;
  if (t.dev >= 0) {
    LinkStats &s = this->devices_[t.dev].stats_;
    if (crc)
      s.crc_errors++;
    else
      s.timeouts++;
    // A silent 30644 is normal (stock reads it on protocol 1/4 only): not a lost frame.
    if (t.kind != Kind::INVTYPE)
      s.record(true);
  }
  this->log_(crc ? 2 : 5, "%s addr %u fn %u start %u: %s (try %u, %u B dropped)", kind_name(static_cast<int>(t.kind)),
             t.addr, t.fn, t.start, crc ? "CRC error" : "timeout", t.attempts + 1, this->parser_.dropped());
  this->state_ = TxState::IDLE;
  this->last_bus_end_ = now;
  t.attempts++;
  // Writes are sent once regardless of `retries` (stock sends every request once).
  if (t.kind != Kind::WRITE && t.attempts <= this->timing_.retries) {
    this->retry_pending_ = true;
    return;
  }
  this->finish_fail_(now, crc);
}

void ModbusMaster::finish_ok_(uint32_t now) {
  Txn &t = this->cur_;
  this->state_ = TxState::IDLE;
  this->last_bus_end_ = now;
  const uint8_t *data = this->parser_.data();
  const uint16_t nregs = this->parser_.nregs();
  Device *d = t.dev >= 0 ? &this->devices_[t.dev] : nullptr;
  if (d != nullptr)
    this->count_ok_(*d, now);

  switch (t.kind) {
    case Kind::ADHOC:
      if (d != nullptr && !d->online_ && d->state_ == Device::State::RUN)
        this->set_online_(*d, true, now);
      if (t.cb)
        t.cb(true, data, nregs);
      break;
    case Kind::WRITE:
      this->write_done_(now);
      if (t.verify) {
        Txn v;
        v.kind = Kind::VERIFY;
        v.addr = t.addr;
        v.fn = FN_READ_HOLDING;
        v.start = t.start;
        v.count = t.count;
        v.write_values = std::move(t.write_values);
        v.wcb = std::move(t.wcb);
        this->writes_.push_front(std::move(v));  // right after the settle gap
      } else {
        this->finish_write_(t, WriteStatus::OK, 0, nullptr, 0);
      }
      break;
    case Kind::VERIFY: {
      bool same = nregs == t.write_values.size();
      for (uint16_t i = 0; same && i < nregs; i++)
        same = static_cast<uint16_t>(data[2 * i] << 8 | data[2 * i + 1]) == t.write_values[i];
      if (!same)
        this->log_(2, "write addr %u start %u: read-back differs (first 0x%04X, wrote 0x%04X)", t.addr, t.start,
                   nregs > 0 ? static_cast<uint16_t>(data[0] << 8 | data[1]) : 0,
                   t.write_values.empty() ? 0 : t.write_values[0]);
      this->finish_write_(t, same ? WriteStatus::OK : WriteStatus::VERIFY_MISMATCH, 0, data, nregs);
      break;
    }
    case Kind::DISCOVERY: {
      if (d->cfg_.child_kind != ChildKind::NONE) {
        // Reported battery / charger / Energy Bridge: identified by its class identity read.
        const ChildProbe *cp = child_probe(d->cfg_.child_kind);
        const RegisterTable *tab = sungrow_datamodel::find_table(cp->table);
        DevInfo info;
        d->keep_ident_raw_(FN_READ_INPUT, cp->start, data, nregs);
        if (tab == nullptr || !parse_child_identity(d->cfg_.child_kind, data, nregs, &info)) {
          this->report_unsupported_(*d, tab == nullptr ? "child_no_table" : "child_unparsed");
          d->next_probe_ = now + CHILD_RETRY_MS;
          break;
        }
        d->devinfo_ = info;
        d->discovered_ = true;
        if (d->cfg_.child_kind == ChildKind::SBR) {
          // Stock reads the cell block of every SBR in every round; a SLOW read every 30 s
          // keeps the module cell spreads current enough to follow balancing.
          d->cfg_.sbr_cells = true;
          d->cfg_.sbr_interval_ms = 30000;
        }
        this->log_(3, "dev %u (addr %u): %s, dev_code 0x%04X (%s)", d->cfg_.index, d->cfg_.address, cp->label,
                   info.dev_code, info.model);
        this->activate_table_(*d, tab, now);
        if (this->store_ != nullptr)
          this->store_->ingest_block(d->cfg_.index, *tab, FN_READ_INPUT, cp->start, data, nregs, d->cfg_.word_order,
                                     d->gate_);
        this->set_online_(*d, true, now);
        if (this->listener_ != nullptr)
          this->listener_->on_discovered(*d);
        break;
      }
      // The app version at 2579 does not affect which table is polled.
      DevInfo info;
      const RegisterTable *tab = nullptr;
      d->keep_ident_raw_(FN_READ_INPUT, DEVINFO_START, data, nregs);
      if (parse_devinfo(data, nregs, &info)) {
        d->devinfo_ = info;
        d->discovered_ = true;
        tab = sungrow_datamodel::inverter_table_for_protocol(info.protocol);
      }
      if (tab == nullptr) {
        tab = d->cfg_.fallback_table;
        this->log_(2, "dev %u: no table for protocol type %u; using fallback %s", d->cfg_.index, info.protocol,
                   tab != nullptr ? tab->name : "(none)");
        this->report_unsupported_(*d, d->discovered_ ? "no_table" : "devinfo_unparsed");
        d->fallback_used_ = tab != nullptr;
      }
      if (tab == nullptr) {
        d->next_probe_ = now + this->timing_.offline_probe_ms;
        break;
      }
      // The slave answered the devinfo read: it exists, so never abandon this scan slot.
      d->discover_fails_ = 0;
      d->abandoned_ = false;
      this->activate_table_(*d, tab, now);
      // The devinfo block is the table's own group-36000 request: store it now.
      if (this->store_ != nullptr)
        this->store_->ingest_block(d->cfg_.index, *tab, FN_READ_INPUT, DEVINFO_START, data, nregs,
                                   d->cfg_.word_order, d->gate_);
      this->set_online_(*d, true, now);
      this->remember_(*d);
      // The search fallback found the inverter: it replaces remembered addresses that stayed
      // silent (the inverter was re-addressed).
      if (d->cfg_.scan && this->range_fallback_spent_ && tab->family == Family::INVERTER)
        this->forget_unanswered_remembered_(d->cfg_.address);
      if (d->discovered_) {
        // Inverter type next (before the initial pass); on_discovered follows its answer.
        d->inv_type_pending_ = true;
      } else if (this->listener_ != nullptr) {
        this->listener_->on_discovered(*d);
      }
      break;
    }
    case Kind::SEARCH: {
      this->cur_search_ = false;
      const uint16_t v = nregs >= 1 ? static_cast<uint16_t>(data[0] << 8 | data[1]) : 0;
      if (v >= 1 && v <= 246) {
        this->search_answered_(static_cast<uint8_t>(v), now);
      } else {
        this->log_(2, "search: reply with invalid address %u", v);
        this->search_failed_(now);
      }
      break;
    }
    case Kind::INVTYPE: {
      const uint16_t v = nregs >= 1 ? static_cast<uint16_t>(data[0] << 8 | data[1]) : 0xFFFF;
      // The 0xFFFF sentinel is stored as inverter type 0, as stock (an SH10RS reports 30644 =
      // 0xFFFF -> inv_type 0, family unit).
      this->inv_type_done_(*d, true, v == 0xFFFF ? 0 : v);
      break;
    }
    case Kind::POLL:
    case Kind::PROBE: {
      if (this->store_ != nullptr && d->table_ != nullptr)
        this->store_->ingest_block(d->cfg_.index, *d->table_, t.fn, t.start, data, nregs, d->cfg_.word_order,
                                   d->gate_);
      if (t.kind == Kind::POLL && t.plan_gen == d->plan_gen_ && t.job >= 0 &&
          static_cast<size_t>(t.job) < d->jobs_.size())
        this->job_done_(*d, t.job, now, true, false);
      this->set_online_(*d, true, now);  // a PROBE answer brings the device back (initial pass)
      if (this->child_discovery_ && d->table_ != nullptr && d->table_->family == Family::INVERTER)
        this->child_check_due_ = true;
      break;
    }
  }
}

void ModbusMaster::finish_exception_(uint32_t now, uint8_t code) {
  Txn &t = this->cur_;
  this->state_ = TxState::IDLE;
  this->last_bus_end_ = now;
  Device *d = t.dev >= 0 ? &this->devices_[t.dev] : nullptr;
  if (d != nullptr) {
    this->count_ok_(*d, now);  // the slave answered: the link is fine (stock: no nLostFrame++)
    d->stats_.exceptions++;
  }
  this->log_(t.kind == Kind::POLL || t.kind == Kind::INVTYPE ? 5 : 2,
             "%s addr %u fn %u start %u count %u: Modbus exception %u",
             kind_name(static_cast<int>(t.kind)), t.addr, t.fn, t.start, t.count, code);
  switch (t.kind) {
    case Kind::SEARCH:
      this->cur_search_ = false;
      this->search_failed_(now);
      break;
    case Kind::ADHOC:
      if (t.cb)
        t.cb(false, nullptr, 0);
      break;
    case Kind::WRITE:
      this->write_done_(now);
      this->finish_write_(t, WriteStatus::EXCEPTION, code, nullptr, 0);
      break;
    case Kind::VERIFY:
      this->finish_write_(t, WriteStatus::VERIFY_FAILED, code, nullptr, 0);
      break;
    case Kind::INVTYPE:
      this->inv_type_done_(*d, false, 0);
      break;
    case Kind::DISCOVERY:
      if (d->cfg_.child_kind != ChildKind::NONE) {
        d->next_probe_ = now + CHILD_RETRY_MS;  // identity read rejected: ask again later
        break;
      }
      this->use_fallback_table_(*d, now);
      break;
    case Kind::POLL:
    case Kind::PROBE: {
      // Old values are kept (no decode, no invalidation; as stock). The read drops to
      // the STATIC period until it answers with data (job_done_).
      if (this->store_ != nullptr && d->table_ == &sungrow_datamodel::get_table(TableId::INV_PROTO1_4) &&
          static_cast<uint16_t>(t.start - 1) == EXC_CLEAR_WIRE_START) {
        for (uint16_t sig : EXC_CLEAR_SIGS)
          if (this->store_->get(d->cfg_.index, sig) != nullptr)
            this->store_->clear(d->cfg_.index, sig);
      }
      if (t.kind == Kind::POLL && t.plan_gen == d->plan_gen_ && t.job >= 0 &&
          static_cast<size_t>(t.job) < d->jobs_.size()) {
        Device::Job &j = d->jobs_[t.job];
        j.last_exc = code;
        j.exc_count++;
        this->job_done_(*d, t.job, now, true, true);
      }
      this->set_online_(*d, true, now);
      break;
    }
  }
}

void ModbusMaster::finish_fail_(uint32_t now, bool crc) {
  Txn &t = this->cur_;
  Device *d = t.dev >= 0 ? &this->devices_[t.dev] : nullptr;
  if (d != nullptr && t.kind != Kind::INVTYPE)
    this->count_fail_(*d, now);  // may take the device offline
  const uint32_t retry_ms =
      d != nullptr && d->stats_.consecutive_failures < this->timing_.offline_after ? 2000 : this->timing_.offline_probe_ms;
  switch (t.kind) {
    case Kind::SEARCH:
      this->cur_search_ = false;
      this->search_failed_(now);
      break;
    case Kind::ADHOC:
      if (t.cb)
        t.cb(false, nullptr, 0);
      break;
    case Kind::WRITE:
      this->write_done_(now);
      this->finish_write_(t, WriteStatus::TIMEOUT, 0, nullptr, 0);
      break;
    case Kind::VERIFY:
      this->finish_write_(t, WriteStatus::VERIFY_FAILED, 0, nullptr, 0);
      break;
    case Kind::DISCOVERY:
      if (d->cfg_.scan) {
        this->scan_miss_(*d, now);
        break;
      }
      if (d->cfg_.child_kind != ChildKind::NONE) {
        // A reported child that is silent does not hold up the others (stock waits for all of
        // them, so one silent battery hides every battery); it is asked again later.
        d->next_probe_ = now + CHILD_RETRY_MS;
        break;
      }
      d->next_probe_ = now + retry_ms;
      if (d->cfg_.remembered)
        this->maybe_range_fallback_(now);  // the last silent remembered address may gate it
      break;
    case Kind::PROBE:
      d->next_probe_ = now + retry_ms;
      break;
    case Kind::INVTYPE:
      this->inv_type_done_(*d, false, 0);
      break;
    case Kind::POLL:
      // Lost frame: values are kept (stock only counts nLostFrame); the slot grid goes on.
      if (t.plan_gen == d->plan_gen_ && t.job >= 0 && static_cast<size_t>(t.job) < d->jobs_.size())
        this->job_done_(*d, t.job, now, false, false);
      if (!d->online_)
        d->next_probe_ = now + retry_ms;
      break;
  }
  (void) crc;
}

void ModbusMaster::inv_type_done_(Device &d, bool known, uint16_t value) {
  d.inv_type_pending_ = false;
  d.inv_type_known_ = known;
  d.inv_type_ = known ? value : 0;
  // Fields first: the sig 0x1002 subscription then sees no change.
  if (known && this->store_ != nullptr)
    this->store_->set_scalar(d.cfg_.index, SIG_INV_TYPE, sungrow_datamodel::TYPE_U16, 0.0f, value, 2);
  char inv[8];
  if (known)
    std::snprintf(inv, sizeof(inv), "%u", value);
  else
    std::snprintf(inv, sizeof(inv), "unknown");
  const DevInfo &i = d.devinfo_;
  this->log_(3,
             "addr %u: dev_code 0x%04X model '%s' proto '%s' tag '%s' -> protocol %u, dev_type %u, inv_type %s, "
             "xml family %s",
             d.cfg_.address, i.dev_code, i.model, i.proto_code, i.tag, i.protocol, i.dev_type, inv,
             xml_family_name(d.xml_family()));
  if (this->listener_ != nullptr)
    this->listener_->on_discovered(d);
}

void ModbusMaster::on_inv_type_signal_(size_t di, bool valid, uint64_t bits) {
  Device &d = this->devices_[di];
  const bool known = valid && bits != 0xFFFF;
  const uint16_t value = known ? static_cast<uint16_t>(bits) : 0;
  if (known == d.inv_type_known_ && value == d.inv_type_)
    return;
  d.inv_type_known_ = known;
  d.inv_type_ = value;
  if (known)
    this->log_(3, "addr %u: inv_type %u -> xml family %s", d.cfg_.address, value, xml_family_name(d.xml_family()));
  else
    this->log_(3, "addr %u: inv_type unknown -> xml family %s", d.cfg_.address, xml_family_name(d.xml_family()));
  if (!d.inv_type_pending_ && this->listener_ != nullptr)
    this->listener_->on_identity_changed(d);
}

uint16_t ModbusMaster::device_id(uint8_t index) const {
  // Assign in device order, so the result does not depend on which caller asks first.
  for (const auto &d : this->devices_) {
    if (!d.identified() && d.table() == nullptr)
      continue;
    const uint8_t i = d.config().index;
    bool known = false;
    for (const auto &e : this->device_ids_)
      known = known || e.first == i;
    if (known)
      continue;
    if (this->next_device_id_ == 247)
      this->next_device_id_++;
    this->device_ids_.emplace_back(i, this->next_device_id_++);
  }
  for (const auto &e : this->device_ids_) {
    if (e.first == index)
      return e.second;
  }
  return 0;
}

}  // namespace sungrow_modbus
}  // namespace esphome
