#include "capture_core.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

namespace esphome {
namespace sungrow_capture {

using sungrow_modbus::FN_READ_HOLDING;
using sungrow_modbus::FN_READ_INPUT;

namespace {

bool due(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t) >= 0; }

constexpr uint16_t SIG_FUNCTION_ENUM = 0x2173;
constexpr uint16_t IV_COMMAND_REG = 32000;
constexpr uint16_t IV_FORMAT_REG = 31999;
constexpr uint16_t IV_STATE_REG = 32005;
constexpr uint16_t IV_STATE_COUNT = 5;
constexpr uint16_t GRID_START_REG = 32010;
constexpr uint16_t GRID_PROGRESS_REG = 30079;
constexpr uint16_t RECORD_COUNT_REG = 30078;
constexpr uint16_t FAULT_RECORD_REG = 30034;
constexpr uint16_t IV_START = 0x01A0;
constexpr uint16_t IV_SELECT = 0x00A0;
constexpr uint16_t GRID_SELECT = 0x01B0;
constexpr uint16_t GRID_ARM = 0x00AA;
constexpr uint16_t FAULT_RECORD_SCAN = 0x00AA;
constexpr uint16_t FAULT_FETCH = 0x01A1;
constexpr uint16_t FAULT_SELECT = 0x00B0;
constexpr uint16_t SIG_MPPT1_VOLTAGE = 0x2040;
constexpr uint8_t MPPT_MAX = 12;

enum class IvState : uint8_t { IDLE, SCANNING, SUCCESS, FAILED, UNKNOWN };

// Inverter scan-state bits: 17 idle, 16 scanning, 15 success, 18 failed; tested in the order
// success, scanning, failed.
IvState decode_iv(uint32_t s) {
  if (s & 0x8000u)
    return IvState::SUCCESS;
  if (s & 0x10000u)
    return IvState::SCANNING;
  if (s & 0x40000u)
    return IvState::FAILED;
  if (s & 0x20000u)
    return IvState::IDLE;
  return IvState::UNKNOWN;
}

const char *iv_state_name(IvState s) {
  switch (s) {
    case IvState::IDLE:
      return "idle";
    case IvState::SCANNING:
      return "scanning";
    case IvState::SUCCESS:
      return "success";
    case IvState::FAILED:
      return "failed";
    default:
      return "unknown";
  }
}

}  // namespace

// The inverter's I-V scan refusal / abort codes as its power stage assigns them; unknown codes
// keep the number only.
const char *CaptureCore::iv_fault_text(uint16_t code) {
  switch (code) {
    case 1:
      return "bus voltage too high";
    case 4:
      return "PV power too high to sweep";
    case 5:
      return "not grid-tied running";
    case 6:
      return "bus could not be lowered";
    case 7:
      return "PV power too low";
    case 8:
      return "sweep timed out";
    case 10:
      return "reactive power active";
    case 11:
      return "power limit or soft start active";
    case 12:
    case 13:
      return "an external power cap is active";
    case 14:
      return "a protection feature blocks the scan";
    case 21:
      return "the inverter's scan watchdog expired";
    default:
      return nullptr;
  }
}

namespace {

void json_escape_into(std::string *out, const std::string &s) {
  out->push_back('"');
  for (unsigned char ch : s) {
    switch (ch) {
      case '"':
        *out += "\\\"";
        break;
      case '\\':
        *out += "\\\\";
        break;
      case '\n':
        *out += "\\n";
        break;
      case '\r':
        *out += "\\r";
        break;
      case '\t':
        *out += "\\t";
        break;
      default:
        if (ch < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", ch);
          *out += b;
        } else {
          out->push_back(static_cast<char>(ch));
        }
    }
  }
  out->push_back('"');
}

void appendf(std::string *out, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void appendf(std::string *out, const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0)
    out->append(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1);
}

}  // namespace

uint16_t crc16_xmodem(const uint8_t *data, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

const char *kind_name(Kind k) { return k == Kind::IV ? "iv" : k == Kind::GRID ? "grid" : "fault"; }

// ---------------------------------------------------------------------------------------------
// YMODEM receiver
void YmodemReceiver::begin(uint32_t now, size_t max_size) {
  *this = YmodemReceiver();
  this->status_ = Status::RUNNING;
  this->max_size_ = max_size;
  this->last_rx_ = now;
  this->c_at_ = now - C_REPEAT_MS;  // first 'C' on the first tick
}

void YmodemReceiver::fail_(const char *why, std::vector<uint8_t> *tx) {
  if (this->finished())
    return;
  this->status_ = Status::FAILED;
  this->error_ = why;
  tx->push_back(CAN);
  tx->push_back(CAN);
}

void YmodemReceiver::nak_(const char *why, std::vector<uint8_t> *tx) {
  this->buf_.clear();
  this->want_ = 0;
  this->naks_++;
  if (++this->errors_ >= MAX_ERRORS) {
    std::string w = std::string("too many errors (last: ") + why + ")";
    this->status_ = Status::FAILED;
    this->error_ = w;
    tx->push_back(CAN);
    tx->push_back(CAN);
    return;
  }
  tx->push_back(NAK);
}

void YmodemReceiver::tick(uint32_t now, std::vector<uint8_t> *tx) {
  if (this->status_ != Status::RUNNING)
    return;
  if (!this->have_header_ && this->buf_.empty()) {
    if (due(now, this->c_at_ + C_REPEAT_MS)) {
      if (this->c_sent_ >= C_TRIES) {
        this->fail_("no header from the inverter", tx);
        return;
      }
      tx->push_back(C);
      this->c_sent_++;
      this->c_at_ = now;
    }
    return;
  }
  if (!this->buf_.empty() && now - this->last_rx_ >= BLOCK_GAP_MS) {
    this->nak_("block incomplete", tx);
    return;
  }
  if (now - this->last_rx_ >= INACTIVITY_MS)
    this->fail_("no data for 30 s", tx);
}

void YmodemReceiver::feed(const uint8_t *data, size_t n, uint32_t now, std::vector<uint8_t> *tx) {
  if (this->status_ != Status::RUNNING || n == 0)
    return;
  this->last_rx_ = now;
  for (size_t i = 0; i < n && this->status_ == Status::RUNNING; i++) {
    const uint8_t b = data[i];
    if (this->buf_.empty()) {
      if (b == SOH || b == STX) {
        this->want_ = b == SOH ? 133 : 1029;
        this->buf_.push_back(b);
      } else if (b == EOT && this->have_header_) {
        tx->push_back(ACK);
        if (this->size_ == 0) {
          this->status_ = Status::EMPTY;
        } else {
          if (this->data_.size() > this->size_)
            this->data_.resize(this->size_);
          this->status_ = Status::DONE;
        }
      } else if (b == CAN) {
        this->status_ = Status::FAILED;
        this->error_ = "cancelled by the inverter";
      } else {
        this->junk_++;
      }
      continue;
    }
    this->buf_.push_back(b);
    if (this->buf_.size() == this->want_)
      this->block_(tx);
  }
}

void YmodemReceiver::block_(std::vector<uint8_t> *tx) {
  const size_t len = this->want_ - 5;
  const uint8_t *p = this->buf_.data();
  const uint8_t seq = p[1];
  const uint16_t crc = static_cast<uint16_t>(p[3 + len] << 8 | p[4 + len]);
  if (static_cast<uint8_t>(~seq) != p[2]) {
    this->nak_("bad block number", tx);
    return;
  }
  if (crc16_xmodem(p + 3, len) != crc) {
    this->nak_("CRC error", tx);
    return;
  }
  if (!this->have_header_) {
    if (seq != 0) {
      this->nak_("data before the header", tx);
      return;
    }
    // Name NUL, decimal size NUL (the inverter's header builder).
    const char *name = reinterpret_cast<const char *>(p + 3);
    const size_t nl = strnlen(name, len);
    this->name_.assign(name, nl);
    uint32_t size = 0;
    for (size_t k = nl + 1; k < len && p[3 + k] >= '0' && p[3 + k] <= '9'; k++)
      size = size * 10 + (p[3 + k] - '0');
    this->size_ = size;
    this->have_header_ = true;
    this->buf_.clear();
    this->errors_ = 0;
    if (size > this->max_size_) {
      this->fail_("file larger than the capture limit", tx);
      return;
    }
    this->data_.reserve(size + 1024);
    tx->push_back(ACK);
    return;
  }
  if (seq == static_cast<uint8_t>(this->next_seq_ - 1)) {
    // Our ACK was lost: the inverter sent the last block again.
    this->dups_++;
    this->buf_.clear();
    tx->push_back(ACK);
    return;
  }
  if (seq != this->next_seq_) {
    this->nak_("unexpected block number", tx);
    return;
  }
  if (this->data_.size() + len > this->max_size_ + 1024) {
    this->fail_("more data than announced", tx);
    return;
  }
  this->data_.insert(this->data_.end(), p + 3, p + 3 + len);
  this->next_seq_++;
  this->blocks_++;
  this->errors_ = 0;
  this->buf_.clear();
  tx->push_back(ACK);
}

// ---------------------------------------------------------------------------------------------
void CaptureCore::log_(int level, const char *fmt, ...) const {
  if (!this->log_fn_)
    return;
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  this->log_fn_(level, buf);
}

void CaptureCore::note_(const char *fmt, ...) {
  char buf[160];
  const int head = std::snprintf(buf, sizeof(buf), "%u ", (unsigned) (this->now_() - this->cur_.started_ms));
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf + head, sizeof(buf) - head, fmt, ap);
  va_end(ap);
  if (this->cur_.timeline.size() < TIMELINE_MAX)
    this->cur_.timeline.emplace_back(buf);
  this->log_(5, "%s: %s", kind_name(this->cur_.kind), buf + head);
}

bool CaptureCore::iv_supported(uint8_t device) const {
  uint64_t fe = 0;
  return this->store_ != nullptr && this->store_->get_bits(device, SIG_FUNCTION_ENUM, &fe) && (fe & (1u << 3));
}

bool CaptureCore::grid_supported(uint8_t device) const {
  uint64_t fe = 0;
  return this->store_ != nullptr && this->store_->get_bits(device, SIG_FUNCTION_ENUM, &fe) && (fe & (1u << 19));
}

uint8_t CaptureCore::mppt_count(uint8_t device) const {
  uint8_t n = 0;
  if (this->store_ == nullptr)
    return 0;
  for (uint8_t i = 0; i < MPPT_MAX; i++) {
    const sungrow_datamodel::Signal *s = this->store_->get(device, static_cast<uint16_t>(SIG_MPPT1_VOLTAGE + 2 * i));
    if (s != nullptr && s->valid && s->kind == sungrow_datamodel::ValueKind::SCALAR)
      n++;
  }
  return n;
}

uint32_t CaptureCore::scan_ms() const {
  return this->running() && this->cur_.kind == Kind::IV && this->iv_seen_scanning_ ? this->now_() - this->iv_scanning_at_
                                                                                     : 0;
}

uint16_t CaptureCore::iv_fault() const { return this->running() && this->cur_.kind == Kind::IV ? this->cur_.fault : 0; }

size_t CaptureCore::max_size_() const {
  switch (this->cur_.kind) {
    case Kind::IV:
      return IV_MAX_SIZE;
    case Kind::GRID:
      return GRID_MAX_SIZE;
    case Kind::FAULT:
      return FAULT_MAX_SIZE;
  }
  return IV_MAX_SIZE;
}

bool CaptureCore::precheck_(uint8_t device, uint8_t addr, std::string *why) const {
  (void) device;
  if (this->master_ == nullptr || !this->writer_) {
    *why = "not configured";
    return false;
  }
  if (this->running()) {
    *why = "another capture is running";
    return false;
  }
  if (!this->master_->can_write(addr)) {
    *why = "inverter offline";
    return false;
  }
  if (this->master_->exclusive()) {
    *why = "bus busy";
    return false;
  }
  return true;
}

bool CaptureCore::start_iv(uint8_t device, uint8_t addr, const std::string &label, std::string *why) {
  if (!this->precheck_(device, addr, why))
    return false;
  if (!this->iv_supported(device)) {
    *why = "the inverter does not report I-V scan support (functionEnum bit 3)";
    return false;
  }
  const uint32_t now = this->now_();
  if (this->have_iv_start_ && now - this->last_iv_start_ < IV_MIN_INTERVAL_MS) {
    char b[80];
    std::snprintf(b, sizeof(b), "wait %u s before the next I-V scan",
                  (unsigned) ((IV_MIN_INTERVAL_MS - (now - this->last_iv_start_) + 999) / 1000));
    *why = b;
    return false;
  }
  this->begin_(Kind::IV, device, addr, label);
  this->cur_.snapshot_before = this->snapshot(device);
  const uint16_t v = IV_START;
  if (!this->send_write_(IV_COMMAND_REG, &v, 1, Step::IV_POLL)) {
    *why = this->cur_.result;
    return false;
  }
  this->step_ = Step::IV_START;
  this->have_iv_start_ = true;
  this->last_iv_start_ = now;
  this->iv_starts_++;
  return true;
}

bool CaptureCore::start_grid(uint8_t device, uint8_t addr, uint8_t cycles, const std::string &label,
                             std::string *why) {
  if (!this->precheck_(device, addr, why))
    return false;
  if (cycles < 1 || cycles > 8) {
    *why = "cycles must be 1..8";
    return false;
  }
  if (!this->grid_supported(device)) {
    *why = "the inverter does not report grid-waveform support (functionEnum bit 19)";
    return false;
  }
  if (!this->large_ok_) {
    *why = "no PSRAM for the recording";
    return false;
  }
  this->begin_(Kind::GRID, device, addr, label);
  this->cur_.cycles = cycles;
  this->cur_.snapshot_before = this->snapshot(device);
  this->step_ = Step::GRID_PRE;
  this->next_poll_ = this->now_();
  return true;
}

bool CaptureCore::start_fault(uint8_t device, uint8_t addr, const std::string &label, std::string *why) {
  if (!this->precheck_(device, addr, why))
    return false;
  if (!this->fault_supported(device)) {
    *why = "the inverter does not report fault-recording support (functionEnum bit 19)";
    return false;
  }
  if (!this->large_ok_) {
    *why = "no PSRAM for the recordings";
    return false;
  }
  this->begin_(Kind::FAULT, device, addr, label);
  this->cur_.snapshot_before = this->snapshot(device);
  this->step_ = Step::FAULT_PRE;
  this->next_poll_ = this->now_();
  return true;
}

void CaptureCore::begin_(Kind k, uint8_t device, uint8_t addr, const std::string &label) {
  this->cur_ = Capture();
  this->cur_.kind = k;
  this->cur_.id = this->next_id_++;
  this->cur_.inverter = label;
  this->cur_.started_ms = this->now_();
  this->cur_.wall_start = this->wall_ ? this->wall_() : 0;
  this->dev_ = device;
  this->addr_ = addr;
  this->step_at_ = this->cur_.started_ms;
  this->read_in_flight_ = false;
  this->write_in_flight_ = false;
  this->read_fails_ = 0;
  this->cancel_ = false;
  this->iv_seen_scanning_ = false;
  this->iv_scanning_at_ = 0;
  this->grid_progress_ = -1;
  this->grid_seen_below_ = false;
  this->grid_changed_at_ = this->cur_.started_ms;
  this->fault_count_ = 0;
  this->hold_sized_ = false;
  this->log_(3, "%s capture %u started on %s", kind_name(k), (unsigned) this->cur_.id, label.c_str());
}

void CaptureCore::finish_(bool ok, const std::string &result) {
  if (this->step_ == Step::IDLE)
    return;
  if (this->master_ != nullptr && this->master_->exclusive()) {
    this->master_->set_raw(false);
    this->master_->release_exclusive();
  }
  this->cur_.ok = ok;
  this->cur_.result = result;
  this->cur_.finished_ms = this->now_();
  if (ok && this->cur_.kind != Kind::IV && this->cur_.data.size() >= 6) {
    // Record-file prefix: u32 record count, low word first, big-endian words.
    const uint8_t *p = this->cur_.data.data();
    const uint32_t n = static_cast<uint32_t>(p[0] << 8 | p[1]) | static_cast<uint32_t>(p[2] << 8 | p[3]) << 16;
    this->cur_.records = n > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(n);
  }
  if (this->cur_.snapshot_after.empty())
    this->cur_.snapshot_after = this->snapshot(this->dev_);
  note_("finished: %s", result.c_str());
  this->log_(ok ? 3 : 2, "%s capture %u: %s (%u bytes)", kind_name(this->cur_.kind), (unsigned) this->cur_.id,
             result.c_str(), (unsigned) this->cur_.data.size());
  // Per-kind retention (one I-V, GRID_KEEP grid, FAULT_KEEP fault captures): the oldest of the
  // finishing kind goes.
  const Kind k = this->cur_.kind;
  const size_t keep = k == Kind::IV ? 1 : k == Kind::GRID ? GRID_KEEP : FAULT_KEEP;
  size_t same = 0;
  for (const auto &r : this->results_)
    same += r->kind == k;
  for (auto it = this->results_.begin(); it != this->results_.end() && same >= keep;) {
    if ((*it)->kind == k) {
      it = this->results_.erase(it);
      same--;
    } else {
      ++it;
    }
  }
  this->results_.push_back(std::make_shared<const Capture>(std::move(this->cur_)));
  this->cur_ = Capture();
  this->cur_.kind = this->results_.back()->kind;
  this->step_ = Step::IDLE;
  this->gen_++;
}

std::shared_ptr<const Capture> CaptureCore::find(uint32_t id) const {
  for (const auto &r : this->results_)
    if (r->id == id)
      return r;
  return nullptr;
}

void CaptureCore::cancel() {
  if (this->step_ == Step::IDLE)
    return;
  if (this->step_ == Step::TRANSFER && this->master_ != nullptr && this->master_->transport() != nullptr) {
    const uint8_t can[2] = {YmodemReceiver::CAN, YmodemReceiver::CAN};
    this->master_->transport()->send_frame(can, 2);
  }
  this->finish_(false, "cancelled");
}

bool CaptureCore::send_write_(uint16_t start, const uint16_t *v, uint16_t n, Step next_ok) {
  std::string why;
  const uint32_t id = this->cur_.id;
  this->write_in_flight_ = true;
  char vals[40] = "";
  for (size_t i = 0, o = 0; i < n && o + 8 < sizeof(vals); i++)
    o += std::snprintf(vals + o, sizeof(vals) - o, i ? " 0x%04X" : "0x%04X", v[i]);
  note_("write %u = %s", start, vals);
  const bool queued = this->writer_(start, v, n,
                                   [this, id, start, next_ok](bool ok, uint8_t exc) {
                                     if (id != this->cur_.id || this->step_ == Step::IDLE)
                                       return;
                                     this->write_in_flight_ = false;
                                     if (!ok) {
                                       char b[80];
                                       if (exc == 4 && this->step_ == Step::FAULT_FETCH) {
                                         // The catalogue count is still 0: ask again on the next poll.
                                         note_("fetch refused: no records ready");
                                         this->next_poll_ = this->now_() + POLL_MS;
                                         return;
                                       }
                                       if (exc == 4 && start == IV_COMMAND_REG && this->step_ == Step::IV_START)
                                         std::snprintf(b, sizeof(b),
                                                       "inverter refused the scan (busy, or its 1000-scans-per-boot "
                                                       "limit)");
                                       else if (exc != 0)
                                         std::snprintf(b, sizeof(b), "write %u refused (exception %u)", start, exc);
                                       else
                                         std::snprintf(b, sizeof(b), "write %u got no answer", start);
                                       this->finish_(false, b);
                                       return;
                                     }
                                     note_("write %u ok", start);
                                     this->step_ = next_ok;
                                     this->step_at_ = this->now_();
                                     this->next_poll_ = this->step_at_;
                                   },
                                   &why);
  if (!queued) {
    this->write_in_flight_ = false;
    this->cur_.result = "write refused: " + why;
    if (this->step_ != Step::IDLE)
      this->finish_(false, this->cur_.result);
  }
  return queued;
}

// ---------------------------------------------------------------------------------------------
void CaptureCore::tick() {
  if (this->step_ == Step::IDLE)
    return;
  const uint32_t now = this->now_();
  switch (this->step_) {
    case Step::IV_POLL:
      this->poll_iv_(now);
      break;
    case Step::GRID_PRE:
    case Step::GRID_POLL:
    case Step::FAULT_PRE:
    case Step::FAULT_POLL:
      this->poll_progress_(now);
      break;
    case Step::FAULT_FETCH:
      this->fetch_fault_(now);
      break;
    case Step::HOLD:
      if (this->master_->acquire_exclusive(HOLD_PRE_MS)) {
        note_("bus held");
        this->select_();
      } else if (now - this->step_at_ > 30000) {
        this->finish_(false, "bus busy");
      }
      break;
    case Step::SELECT_FILE: {
      // I-V: the format flag is written; now select VI_ALL.
      const uint16_t sel = IV_SELECT;
      if (this->send_write_(IV_COMMAND_REG, &sel, 1, Step::RAW_ENTER))
        this->step_ = Step::SELECT;
      break;
    }
    case Step::RAW_ENTER:
      if (this->master_->set_raw(true)) {
        this->step_ = Step::TRANSFER;
        this->transfer_at_ = now;
        this->ym_.begin(now, this->max_size_());
        note_("raw mode, YMODEM receive");
        this->transfer_(now);
      } else if (now - this->step_at_ > 5000) {
        this->finish_(false, "bus did not go idle");
      }
      break;
    case Step::TRANSFER:
      this->transfer_(now);
      break;
    default:
      break;  // a write is in flight
  }
}

void CaptureCore::read_failed_() {
  this->read_in_flight_ = false;
  if (++this->read_fails_ >= MAX_READ_FAILS)
    this->finish_(false, "inverter stopped answering");
}

void CaptureCore::poll_iv_(uint32_t now) {
  if (now - this->step_at_ > IV_TIMEOUT_MS) {
    this->finish_(false, "scan timed out");
    return;
  }
  if (this->read_in_flight_ || !due(now, this->next_poll_))
    return;
  const uint32_t id = this->cur_.id;
  if (this->master_->read_registers(this->addr_, FN_READ_HOLDING, IV_STATE_REG, IV_STATE_COUNT,
                                    [this, id](bool ok, const uint8_t *data, uint16_t nregs) {
                                      if (id != this->cur_.id || this->step_ != Step::IV_POLL)
                                        return;
                                      if (!ok || nregs < IV_STATE_COUNT) {
                                        note_("state read failed");
                                        this->read_failed_();
                                        return;
                                      }
                                      this->read_in_flight_ = false;
                                      this->read_fails_ = 0;
                                      this->on_iv_state_(data, nregs);
                                    })) {
    this->read_in_flight_ = true;
    this->next_poll_ = now + POLL_MS;
  }
}

void CaptureCore::on_iv_state_(const uint8_t *data, uint16_t nregs) {
  (void) nregs;
  uint16_t r[5];
  for (int i = 0; i < 5; i++)
    r[i] = static_cast<uint16_t>(data[2 * i] << 8 | data[2 * i + 1]);
  std::memcpy(this->cur_.last_regs, r, sizeof(r));
  this->cur_.fault = r[4];
  // Word order of the u32 state is not settled: low word first is Sungrow's usual order; take
  // the other order when only it decodes to a scan state.
  IvState s = decode_iv(static_cast<uint32_t>(r[0]) | static_cast<uint32_t>(r[1]) << 16);
  if (s == IvState::UNKNOWN)
    s = decode_iv(static_cast<uint32_t>(r[1]) | static_cast<uint32_t>(r[0]) << 16);
  note_("state %04X %04X upload %04X %04X fault %u: %s", r[0], r[1], r[2], r[3], r[4], iv_state_name(s));
  const uint32_t since = this->now_() - this->step_at_;
  if (s == IvState::SCANNING && !this->iv_seen_scanning_) {
    this->iv_seen_scanning_ = true;
    this->iv_scanning_at_ = this->now_();
  }
  if (this->iv_seen_scanning_)
    this->cur_.scan_ms = this->now_() - this->iv_scanning_at_;
  switch (s) {
    case IvState::SCANNING:
      break;
    case IvState::SUCCESS:
      this->cur_.snapshot_after = this->snapshot(this->dev_);
      this->step_ = Step::HOLD;
      this->step_at_ = this->now_();
      break;
    case IvState::FAILED: {
      char b[96];
      const char *why = iv_fault_text(r[4]);
      if (why != nullptr)
        std::snprintf(b, sizeof(b), "scan failed (inverter fault code %u: %s)", r[4], why);
      else
        std::snprintf(b, sizeof(b), "scan failed (inverter fault code %u)", r[4]);
      this->finish_(false, b);
      break;
    }
    case IvState::IDLE:
    case IvState::UNKNOWN:
      if (since > IV_START_GRACE_MS)
        this->finish_(false, this->iv_seen_scanning_ ? "scan ended without a result" : "scan did not start");
      break;
  }
}

void CaptureCore::fetch_fault_(uint32_t now) {
  if (this->write_in_flight_)
    return;
  if (now - this->step_at_ > FAULT_START_MS) {
    this->finish_(false, "the inverter has no stored fault recordings (fetch refused for 30 s)");
    return;
  }
  if (!due(now, this->next_poll_))
    return;
  const uint16_t v = FAULT_FETCH;
  this->send_write_(IV_COMMAND_REG, &v, 1, Step::FAULT_POLL);
}

// Progress poller of the grid recording and the fault-record copy: fn 4 doc 30079 (GRID) or
// doc 30078-30079 = record count + progress (FAULT), once per POLL_MS.
void CaptureCore::poll_progress_(uint32_t now) {
  const bool fault = this->cur_.kind == Kind::FAULT;
  if (this->step_ == Step::GRID_POLL || this->step_ == Step::FAULT_POLL) {
    const char *what = fault ? "copy" : "recording";
    if (now - this->step_at_ > GRID_TIMEOUT_MS) {
      this->finish_(false, std::string(what) + " timed out");
      return;
    }
    if (now - this->grid_changed_at_ > GRID_STALL_MS) {
      this->finish_(false, std::string(what) + " stalled");
      return;
    }
    if (!this->grid_seen_below_ && now - this->step_at_ > GRID_STALE_MS) {
      this->finish_(false, std::string(what) + " did not start (progress stayed at 100)");
      return;
    }
  }
  if (this->read_in_flight_ || !due(now, this->next_poll_))
    return;
  const uint32_t id = this->cur_.id;
  const uint16_t reg = fault ? RECORD_COUNT_REG : GRID_PROGRESS_REG;
  const uint16_t n = fault ? 2 : 1;
  if (this->master_->read_registers(this->addr_, FN_READ_INPUT, reg, n,
                                    [this, id, n](bool ok, const uint8_t *data, uint16_t nregs) {
                                      if (id != this->cur_.id ||
                                          (this->step_ != Step::GRID_PRE && this->step_ != Step::GRID_POLL &&
                                           this->step_ != Step::FAULT_PRE && this->step_ != Step::FAULT_POLL))
                                        return;
                                      if (!ok || nregs < n) {
                                        note_("progress read failed");
                                        this->read_failed_();
                                        return;
                                      }
                                      this->read_in_flight_ = false;
                                      this->read_fails_ = 0;
                                      this->on_progress_(data, nregs);
                                    })) {
    this->read_in_flight_ = true;
    this->next_poll_ = now + POLL_MS;
  }
}

void CaptureCore::on_progress_(const uint8_t *data, uint16_t nregs) {
  uint16_t v = static_cast<uint16_t>(data[0] << 8 | data[1]);
  if (this->cur_.kind == Kind::FAULT && nregs >= 2) {
    const uint16_t count = v;
    v = static_cast<uint16_t>(data[2] << 8 | data[3]);
    this->cur_.last_regs[1] = count;
    if (count != this->fault_count_ && this->step_ == Step::FAULT_POLL)
      note_("record count %u", count);
    this->fault_count_ = count;
  }
  this->cur_.last_regs[0] = v;
  if (this->step_ == Step::GRID_PRE) {
    note_("progress before the start: %u", v);
    const uint16_t w[2] = {GRID_ARM, this->cur_.cycles};
    this->send_write_(GRID_START_REG, w, 2, Step::GRID_POLL);
    if (this->step_ != Step::IDLE)
      this->step_ = Step::GRID_START;
    return;
  }
  if (this->step_ == Step::FAULT_PRE) {
    note_("progress before the start: %u, record count %u", v, this->fault_count_);
    const uint16_t w = FAULT_RECORD_SCAN;
    this->send_write_(FAULT_RECORD_REG, &w, 1, Step::FAULT_FETCH);
    if (this->step_ != Step::IDLE)
      this->step_ = Step::FAULT_SCAN;
    return;
  }
  if (static_cast<int>(v) != this->grid_progress_) {
    note_("progress %u", v);
    this->grid_progress_ = v;
    this->grid_changed_at_ = this->now_();
  }
  if (v < 100)
    this->grid_seen_below_ = true;
  if (v == 100 && this->grid_seen_below_) {
    this->cur_.snapshot_after = this->snapshot(this->dev_);
    this->step_ = Step::HOLD;
    this->step_at_ = this->now_();
  } else if (v > 100) {
    char b[48];
    std::snprintf(b, sizeof(b), "unexpected progress value %u", v);
    this->finish_(false, b);
  }
}

void CaptureCore::select_() {
  if (this->cur_.kind == Kind::IV) {
    const uint16_t f = 0;
    if (this->send_write_(IV_FORMAT_REG, &f, 1, Step::SELECT_FILE))
      this->step_ = Step::SELECT_FMT;
    return;
  }
  const uint16_t sel = this->cur_.kind == Kind::GRID ? GRID_SELECT : FAULT_SELECT;
  if (this->send_write_(IV_COMMAND_REG, &sel, 1, Step::RAW_ENTER))
    this->step_ = Step::SELECT;
}

void CaptureCore::transfer_(uint32_t now) {
  sungrow_modbus::Transport *t = this->master_->transport();
  if (t == nullptr) {
    this->finish_(false, "no transport");
    return;
  }
  std::vector<uint8_t> tx;
  uint8_t buf[256];
  for (int i = 0; i < 8; i++) {
    const size_t n = t->receive(buf, sizeof(buf));
    if (n == 0)
      break;
    this->ym_.feed(buf, n, now, &tx);
  }
  this->ym_.tick(now, &tx);
  if (!tx.empty())
    t->send_frame(tx.data(), tx.size());
  if (this->ym_.have_header() && !this->hold_sized_) {
    this->hold_sized_ = true;
    this->cur_.file_name = this->ym_.file_name();
    this->cur_.announced_size = this->ym_.announced_size();
    note_("header: \"%s\" %u bytes", this->cur_.file_name.c_str(), (unsigned) this->cur_.announced_size);
    this->master_->extend_exclusive(this->ym_.announced_size() / BYTES_PER_S * 1000u + HOLD_SLACK_MS);
  }
  if (!this->ym_.finished())
    return;
  this->cur_.transfer_ms = now - this->transfer_at_;
  this->cur_.blocks = this->ym_.blocks();
  this->cur_.naks = this->ym_.naks();
  this->cur_.duplicates = this->ym_.duplicates();
  this->cur_.junk_bytes = this->ym_.junk_bytes();
  switch (this->ym_.status()) {
    case YmodemReceiver::Status::DONE:
      this->cur_.data = std::move(this->ym_.data());
      this->finish_(true, "ok");
      break;
    case YmodemReceiver::Status::EMPTY:
      this->finish_(false, "empty file");
      break;
    default:
      this->cur_.data = std::move(this->ym_.data());  // keep what arrived, for analysis
      this->finish_(false, "transfer failed: " + this->ym_.error());
      break;
  }
}

// ---------------------------------------------------------------------------------------------
std::string CaptureCore::step_text() const {
  char b[64];
  switch (this->step_) {
    case Step::IDLE:
      return "idle";
    case Step::IV_START:
      return "starting scan";
    case Step::IV_POLL:
      return this->iv_seen_scanning_ ? "scanning" : "waiting for the scan";
    case Step::GRID_PRE:
    case Step::GRID_START:
      return "starting recording";
    case Step::GRID_POLL:
      if (this->grid_progress_ < 0 || !this->grid_seen_below_)
        return "recording";
      std::snprintf(b, sizeof(b), "recording %d %%", this->grid_progress_);
      return b;
    case Step::FAULT_PRE:
    case Step::FAULT_SCAN:
      return "asking the inverter for its fault recordings";
    case Step::FAULT_FETCH:
      return "waiting for the record list";
    case Step::FAULT_POLL:
      if (this->grid_progress_ < 0 || !this->grid_seen_below_)
        return "copying the record store";
      std::snprintf(b, sizeof(b), "copying the record store %d %%", this->grid_progress_);
      return b;
    case Step::HOLD:
    case Step::SELECT_FMT:
    case Step::SELECT_FILE:
    case Step::SELECT:
    case Step::RAW_ENTER:
      return "preparing file transfer";
    case Step::TRANSFER:
      if (!this->ym_.have_header())
        return "waiting for the file";
      std::snprintf(b, sizeof(b), "transfer %u / %u bytes", (unsigned) this->ym_.received(),
                    (unsigned) this->ym_.announced_size());
      return b;
  }
  return "?";
}

int CaptureCore::progress() const {
  if ((this->step_ == Step::GRID_POLL || this->step_ == Step::FAULT_POLL) && this->grid_seen_below_ &&
      this->grid_progress_ >= 0)
    return this->grid_progress_;
  if (this->step_ == Step::TRANSFER && this->ym_.have_header() && this->ym_.announced_size() > 0) {
    const size_t got = this->ym_.received();
    const uint32_t pct = static_cast<uint32_t>(got * 100u / this->ym_.announced_size());
    return pct > 100 ? 100 : static_cast<int>(pct);
  }
  return -1;
}

uint32_t CaptureCore::elapsed_ms() const {
  return this->step_ == Step::IDLE ? 0 : this->now_() - this->cur_.started_ms;
}

std::string CaptureCore::snapshot(uint8_t device) const {
  std::string out = "{";
  if (this->store_ == nullptr)
    return out + "}";
  // Register of each signal from the device's poll table.
  std::map<uint16_t, std::pair<uint8_t, uint16_t>> regs;
  if (this->master_ != nullptr) {
    for (const auto &d : this->master_->devices()) {
      if (d.config().index != device || d.table() == nullptr)
        continue;
      for (uint16_t i = 0; i < d.table()->count; i++) {
        const auto &row = d.table()->rows[i];
        if (row.sigid != 0 && regs.find(row.sigid) == regs.end())
          regs[row.sigid] = {row.fn, row.regaddr};
      }
    }
  }
  std::map<uint16_t, double> vals;
  this->store_->for_each([&](const sungrow_datamodel::Signal &s) {
    if (s.device != device || !s.valid || s.kind != sungrow_datamodel::ValueKind::SCALAR)
      return;
    const double v = s.number();
    if (std::isfinite(v))
      vals[s.sigid] = v;
  });
  bool first = true;
  for (const auto &kv : vals) {
    appendf(&out, "%s\"0x%04X\":{\"v\":%.6g", first ? "" : ",", kv.first, kv.second);
    auto it = regs.find(kv.first);
    if (it != regs.end())
      appendf(&out, ",\"fn\":%u,\"reg\":%u", it->second.first, it->second.second);
    out += "}";
    first = false;
  }
  return out + "}";
}

std::string CaptureCore::metadata_json(const Capture &c) {
  std::string o = "{";
  appendf(&o, "\"id\":%u,\"kind\":\"%s\",\"ok\":%s,\"result\":", (unsigned) c.id, kind_name(c.kind),
          c.ok ? "true" : "false");
  json_escape_into(&o, c.result);
  o += ",\"inverter\":";
  json_escape_into(&o, c.inverter);
  appendf(&o, ",\"cycles\":%u,\"records\":%u,\"wall_start\":%lld,\"duration_ms\":%u,\"transfer_ms\":%u,\"scan_ms\":%u",
          c.cycles, c.records, (long long) c.wall_start, (unsigned) (c.finished_ms - c.started_ms),
          (unsigned) c.transfer_ms, (unsigned) c.scan_ms);
  o += ",\"file_name\":";
  json_escape_into(&o, c.file_name);
  appendf(&o, ",\"announced_size\":%u,\"size\":%u,\"blocks\":%u,\"naks\":%u,\"duplicates\":%u,\"junk_bytes\":%u",
          (unsigned) c.announced_size, (unsigned) c.data.size(), (unsigned) c.blocks, (unsigned) c.naks,
          (unsigned) c.duplicates, (unsigned) c.junk_bytes);
  appendf(&o, ",\"last_regs\":[%u,%u,%u,%u,%u],\"fault\":%u", c.last_regs[0], c.last_regs[1], c.last_regs[2],
          c.last_regs[3], c.last_regs[4], c.fault);
  o += ",\"timeline\":[";
  for (size_t i = 0; i < c.timeline.size(); i++) {
    if (i)
      o += ",";
    json_escape_into(&o, c.timeline[i]);
  }
  o += "],\"snapshot_before\":";
  o += c.snapshot_before.empty() ? "{}" : c.snapshot_before;
  o += ",\"snapshot_after\":";
  o += c.snapshot_after.empty() ? "{}" : c.snapshot_after;
  o += "}";
  return o;
}

}  // namespace sungrow_capture
}  // namespace esphome
