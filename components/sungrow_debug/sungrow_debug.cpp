#include "sungrow_debug.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/version.h"

#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace sungrow_debug {

static const char *const TAG = "sungrow_debug";
static const char *const TAG_RTU = "sgdbg.rtu";      // TX/RX summaries
static const char *const TAG_HEX = "sgdbg.hex";      // raw RX/TX hex lines
static const char *const TAG_DEC = "sgdbg.decode";   // per-row decode
static const char *const TAG_TCP = "sgdbg.tcp";      // Modbus-TCP frames
static const char *const TAG_DUMP = "sgdbg.dump";    // store / plan / system dumps
static const char *const TAG_DISC = "sgdbg.disc";    // discovery decisions + scan

using sungrow_datamodel::kRowKeys;
using sungrow_datamodel::kUnits;
using sungrow_datamodel::NO_KEY;
using sungrow_datamodel::Signal;
using sungrow_datamodel::SignalStore;
using sungrow_datamodel::ValueKind;
using sungrow_modbus::Device;
using sungrow_modbus::ParseStatus;

static constexpr size_t RECORDS_MAX = 8;
static constexpr size_t JOBS_MAX = 12;
static constexpr size_t RX_KEEP = 300;
static constexpr size_t LOG_CLIENT_BUF = 16384;
static constexpr uint8_t LOG_CLIENTS_MAX = 2;

static inline uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

static const char *outcome_name(int o) {
  static const char *const n[] = {"OK", "EXCEPTION", "TIMEOUT", "CRC-ERROR", "BLOCKED"};
  return o >= 0 && o < 5 ? n[o] : "?";
}

static std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char *f, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

static void emit(int level, const char *tag, const std::string &s) {
  esp_log_printf_(level, tag, __LINE__, "%s", s.c_str());
}

// ---- line jobs ----------------------------------------------------------------------------------
class LinesJob : public LineJob {
 public:
  LinesJob(const char *tag, std::vector<std::string> lines) : tag_(tag), lines_(std::move(lines)) {}
  bool next(std::string *line, const char **tag) override {
    if (this->i_ >= this->lines_.size())
      return false;
    *line = std::move(this->lines_[this->i_++]);
    *tag = this->tag_;
    return true;
  }

 protected:
  const char *tag_;
  std::vector<std::string> lines_;
  size_t i_{0};
};

class DecodeJob : public LineJob {
 public:
  std::string title;
  const RegisterTable *table{nullptr};
  uint8_t device{0};
  bool have_device{false};
  WordOrder wo{WordOrder::LOW_WORD_FIRST};
  bool gating{false};
  const SignalStore *store{nullptr};
  bool forced{false};
  bool only_changed{false};
  TraceFilter filter;
  ChangeTracker *changes{nullptr};
  uint8_t fn{4};
  uint16_t start{0};
  std::vector<uint8_t> data;
  std::vector<uint16_t> rows;

  bool next(std::string *line, const char **tag) override {
    *tag = TAG_DEC;
    if (!this->title_done_) {
      this->title_done_ = true;
      *line = this->title + fmt(" table %s, %u rows in block%s", this->table->name, (unsigned) this->rows.size(),
                                this->forced ? "" : (this->only_changed ? ", only changed" : ""));
      return true;
    }
    while (this->i_ < this->rows.size()) {
      const uint16_t ri = this->rows[this->i_++];
      const RegisterRow &r = this->table->rows[ri];
      const uint8_t *w = this->data.data() + 2 * (r.regaddr - this->start);
      if (!this->forced) {
        if (!this->filter.row_ok(r.regaddr, r.nreg))
          continue;
        if (this->only_changed && this->changes != nullptr) {
          const uint32_t key = (static_cast<uint32_t>(this->device) << 24) |
                               (static_cast<uint32_t>(this->table->id) << 16) | ri;
          if (!this->changes->changed(key, w, 2u * r.nreg))
            continue;
        }
      }
      Gate g = Gate::NONE;
      if (r.dep_word != 0 && this->gating && this->store != nullptr && this->have_device)
        g = this->store->capability_ok(this->device, r.dep_word, r.dep_bit) ? Gate::OK : Gate::GATED;
      const char *key = r.key != NO_KEY ? kRowKeys[r.key] : nullptr;
      const char *unit = r.unit != 0 ? kUnits[r.unit] : nullptr;
      const bool fs = this->store != nullptr && this->store->float_scale();
      *line = format_row(r, w, this->wo, g, key, unit, fs);
      this->shown_++;
      return true;
    }
    if (!this->tail_done_) {
      this->tail_done_ = true;
      if (!this->forced && this->shown_ == 0)
        return false;  // nothing changed: stay quiet
      *line = this->title + fmt(" done: %u of %u rows shown", (unsigned) this->shown_, (unsigned) this->rows.size());
      return true;
    }
    return false;
  }

 protected:
  bool title_done_{false};
  bool tail_done_{false};
  size_t i_{0};
  size_t shown_{0};
};

class StoreDumpJob : public LineJob {
 public:
  struct Entry {
    uint8_t device;
    uint16_t sigid;
  };
  std::vector<Entry> entries;
  const SignalStore *store{nullptr};
  std::function<const RegisterTable *(uint8_t)> table_for;

  bool next(std::string *line, const char **tag) override {
    *tag = TAG_DUMP;
    if (this->i_ >= this->entries.size()) {
      if (this->tail_)
        return false;
      this->tail_ = true;
      *line = fmt("signal store dump done: %u signals", (unsigned) this->entries.size());
      return true;
    }
    const Entry &e = this->entries[this->i_++];
    const Signal *s = this->store->get(e.device, e.sigid);
    if (s == nullptr) {
      *line = fmt("dev %u 0x%04X (gone)", e.device, e.sigid);
      return true;
    }
    const RegisterTable *t = this->table_for ? this->table_for(e.device) : nullptr;
    const char *key = "-";
    const char *unit = "";
    uint16_t reg = 0;
    if (t != nullptr) {
      for (uint16_t i = 0; i < t->count; i++) {
        if (t->rows[i].sigid == e.sigid) {
          reg = t->rows[i].regaddr;
          if (t->rows[i].key != NO_KEY)
            key = kRowKeys[t->rows[i].key];
          unit = kUnits[t->rows[i].unit];
          break;
        }
      }
    }
    const uint32_t age = millis() - s->updated_ms;
    std::string val;
    if (!s->valid) {
      val = "INVALID";
    } else if (s->kind == ValueKind::SCALAR) {
      val = fmt("raw=0x%" PRIX64 " (%s) -> %.7g %s", s->bits(), type_name(s->type), s->number(), unit);
    } else if (s->kind == ValueKind::FLOAT) {
      val = fmt("float -> %.7g %s", s->number(), unit);
    } else if (s->kind == ValueKind::STRING) {
      val = "\"" + s->str() + "\"";
    } else {
      val = fmt("array %u B: ", s->len) + hex(s->data(), s->len, 16);
    }
    *line = fmt("dev %u 0x%04X @%u %s %s%s age %u s", e.device, e.sigid, reg, key, s->valid ? "valid " : "",
                val.c_str(), (unsigned) (age / 1000));
    return true;
  }

 protected:
  size_t i_{0};
  bool tail_{false};
};

// ---- switches -----------------------------------------------------------------------------------
#ifdef USE_SWITCH
void DebugSwitch::write_state(bool state) {
  this->parent_->set_flag(this->kind_, state);
  this->publish_state(state);
}
#endif

void SungrowDebug::set_flag(SwitchKind k, bool on) {
  switch (k) {
    case SwitchKind::RTU_TRACE:
      this->rtu_trace_ = on;
      break;
    case SwitchKind::DECODE_TRACE:
      this->decode_trace_ = on;
      break;
    case SwitchKind::ONLY_CHANGED:
      this->only_changed_ = on;
      if (on)
        this->changes_.clear();  // next response prints everything once
      break;
    case SwitchKind::TCP_TRACE:
      this->tcp_trace_ = on;
      break;
    case SwitchKind::ALLOW_BOOT_SWITCH:
      this->allow_boot_switch_ = on;
      if (on)
        ESP_LOGW(TAG, "Boot switch ARMED: the next boot_other_slot call rewrites otadata and reboots");
      break;
  }
#ifdef USE_SWITCH
  for (auto *s : this->switches_)
    if (s->kind() == k && s->state != on)
      s->publish_state(on);
#endif
}

bool SungrowDebug::flag(SwitchKind k) const {
  switch (k) {
    case SwitchKind::RTU_TRACE:
      return this->rtu_trace_;
    case SwitchKind::DECODE_TRACE:
      return this->decode_trace_;
    case SwitchKind::ONLY_CHANGED:
      return this->only_changed_;
    case SwitchKind::ALLOW_BOOT_SWITCH:
      return this->allow_boot_switch_;
    case SwitchKind::TCP_TRACE:
    default:
      return this->tcp_trace_;
  }
}

void SungrowDebug::set_initial_filter(const char *addresses, const char *registers) {
  parse_ranges(addresses, 1, 247, &this->filter_.addresses);
  parse_ranges(registers, 1, 65535, &this->filter_.registers);
}

bool SungrowDebug::set_trace_filter(const std::string &addresses, const std::string &registers) {
  TraceFilter f;
  if (!parse_ranges(addresses, 1, 247, &f.addresses) || !parse_ranges(registers, 1, 65535, &f.registers)) {
    ESP_LOGW(TAG, "Bad trace filter: addresses '%s' registers '%s' (use e.g. '1,3-5' and '5000-5100,13000+50')",
             addresses.c_str(), registers.c_str());
    return false;
  }
  this->filter_ = f;
  this->changes_.clear();
  this->rate_.clear();
  ESP_LOGI(TAG, "Trace filter: %s", this->filter_.describe().c_str());
#ifdef USE_TEXT_SENSOR
  if (this->filter_ts_ != nullptr)
    this->filter_ts_->publish_state(this->filter_.describe());
#endif
  return true;
}

bool SungrowDebug::set_tag_level(const std::string &tag, const std::string &level) {
  static const char *const names[] = {"NONE", "ERROR", "WARN", "INFO", "CONFIG", "DEBUG", "VERBOSE", "VERY_VERBOSE"};
  int lv = -1;
  for (int i = 0; i < 8; i++) {
    if (strcasecmp(level.c_str(), names[i]) == 0)
      lv = i;
  }
  if (lv < 0 || tag.empty()) {
    ESP_LOGW(TAG, "set_log_level: unknown level '%s' (NONE..VERY_VERBOSE)", level.c_str());
    return false;
  }
#if defined(USE_LOGGER) && defined(USE_LOGGER_RUNTIME_TAG_LEVELS)
  if (logger::global_logger == nullptr)
    return false;
  // The logger keeps the tag pointer: intern it.
  auto it = this->tag_names_.insert(tag).first;
  logger::global_logger->set_log_level(it->c_str(), static_cast<uint8_t>(lv));
  ESP_LOGI(TAG, "Log level for tag '%s' = %s", tag.c_str(), names[lv]);
  return true;
#else
  ESP_LOGW(TAG, "set_log_level needs logger: runtime_tag_levels: true");
  return false;
#endif
}

void SungrowDebug::add_stats_device(uint8_t address) {
  this->stats_.emplace_back();
  this->stats_.back().address = address;
}

// ---- lifecycle ----------------------------------------------------------------------------------
void SungrowDebug::setup() {
  // Wrap the hub's transport: the hub's setup() (higher priority) already set itself.
  this->hub_->master().set_transport(this);
#ifdef USE_SUNGROW_DEBUG_TCP
  if (this->tcp_ != nullptr)
    this->tcp_->set_frame_observer([this](uint8_t what, const char *peer, const uint8_t *adu, size_t len) {
      this->on_tcp_frame_(what, peer, adu, len);
    });
#endif
#ifdef USE_SUNGROW_DEBUG_LOG_SERVER
  if (logger::global_logger != nullptr)
    logger::global_logger->add_log_callback(this, SungrowDebug::on_log_);
#endif
#ifdef USE_SUNGROW_DEBUG_DUMP
  // Behind the web server's auth (add_handler wraps it in the auth middleware).
  if (web_server_base::global_web_server_base != nullptr)
    web_server_base::global_web_server_base->add_handler(new DumpHandler());  // NOLINT
#endif
#ifdef USE_TEXT_SENSOR
  if (this->idf_ts_ != nullptr)
    this->idf_ts_->publish_state(fmt("IDF %s, ESPHome %s", esp_get_idf_version(), ESPHOME_VERSION));
  if (this->partition_ts_ != nullptr) {
    this->partition_ts_->publish_state(this->partition_summary_());
    // safe_mode marks a freshly installed slot valid boot_is_good_after (30 s) into the boot;
    // publish again after that so the sensor does not keep showing PENDING_VERIFY.
    this->set_timeout("partition_state", 45000,
                      [this]() { this->partition_ts_->publish_state(this->partition_summary_()); });
  }
  if (this->filter_ts_ != nullptr)
    this->filter_ts_->publish_state(this->filter_.describe());
#endif
#ifdef USE_SWITCH
  for (auto *s : this->switches_)
    s->publish_state(this->flag(s->kind()));
#endif
  // Partition table / OTA state / heap at boot (serial), and again after 20 s for API log clients.
  this->dump_flash_layout();
  this->dump_system();
  this->set_timeout("sysdump", 20000, [this]() {
    this->dump_flash_layout();
    this->dump_system();
  });
  ESP_LOGW(TAG, "DEBUG BUILD: RS485 writes %s, ext flash read-only, no cloud uplink",
           this->block_writes_ ? "BLOCKED (fn 3/4 only)" : "NOT blocked");
}

void SungrowDebug::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sungrow debug:\n"
                "  RS485 writes: %s\n"
                "  RTU trace %s, decode trace %s (only changed %s), TCP trace %s\n"
                "  Filter: %s, min interval %" PRIu32 " ms, %u lines/loop\n"
                "  Log server: %s%u",
                this->block_writes_ ? "BLOCKED (only fn 3/4 reach the bus)" : "allowed",
                ONOFF(this->rtu_trace_), ONOFF(this->decode_trace_), ONOFF(this->only_changed_),
                ONOFF(this->tcp_trace_), this->filter_.describe().c_str(), this->trace_min_interval_ms_,
                this->lines_per_loop_, this->log_port_ ? "port " : "off ", this->log_port_);
}

void SungrowDebug::loop() {
  const uint32_t now = millis();
  // A reply that never completed: the master gives up at response_timeout from the send.
  if (this->open_ && now - this->cur_.t_tx > this->hub_->master().timing().response_timeout_ms + 100)
    this->finalize_(now, this->parser_.crc_errors() > 0 ? Outcome::CRC_ERROR : Outcome::TIMEOUT);
  for (int i = 0; i < 4 && !this->records_.empty(); i++) {
    Record r = std::move(this->records_.front());
    this->records_.pop_front();
    this->process_record_(r);
  }
  if (!this->stray_.empty()) {
    ESP_LOGD(TAG_RTU, "%u stray RX bytes outside a transaction: %s", (unsigned) this->stray_.size(),
             hex(this->stray_.data(), this->stray_.size(), 48).c_str());
    this->stray_.clear();
  }
  this->run_jobs_();
  this->scan_tick_(now);
  if (now - this->last_stats_ >= this->stats_interval_ms_) {
    this->last_stats_ = now;
    this->publish_stats_(now);
  }
#ifdef USE_SUNGROW_DEBUG_LOG_SERVER
  this->log_server_loop_();
#endif
}

// ---- Transport wrapper --------------------------------------------------------------------------
void SungrowDebug::send_frame(const uint8_t *data, size_t len) {
  const uint32_t now = millis();
  if (this->open_)
    this->finalize_(now, this->parser_.crc_errors() > 0 ? Outcome::CRC_ERROR : Outcome::TIMEOUT);
  Record &r = this->cur_;
  r = Record();
  r.seq = ++this->seq_;
  r.t_tx = now;
  r.tx.assign(data, data + len);
  r.req = parse_rtu_request(data, len);
  r.dev = this->device_index_for_address_(r.req.addr);
  this->open_ = true;
  sungrow_modbus::ExpectedReply e;
  e.addr = r.req.addr;
  e.fn = r.req.fn;
  e.wire_start = static_cast<uint16_t>(r.req.start - 1);
  e.count = r.req.count;
  e.value = r.req.value;
  this->parser_.begin(e);
  if (this->block_writes_ && !is_read_fn(r.req.fn)) {
    // SAFETY (debug build): nothing but fn 3/4 reaches the bus. The master times out.
    this->blocked_frames_++;
    this->finalize_(now, Outcome::BLOCKED);
    return;
  }
  this->hub_->send_frame(data, len);
}

size_t SungrowDebug::receive(uint8_t *buf, size_t cap) {
  const size_t n = this->hub_->receive(buf, cap);
  if (n == 0)
    return 0;
  if (!this->open_) {
    if (this->stray_.size() < RX_KEEP)
      this->stray_.insert(this->stray_.end(), buf, buf + std::min(n, RX_KEEP - this->stray_.size()));
    return n;
  }
  if (this->cur_.rx.size() < RX_KEEP)
    this->cur_.rx.insert(this->cur_.rx.end(), buf, buf + std::min(n, RX_KEEP - this->cur_.rx.size()));
  ParseStatus st = this->parser_.feed(buf, n);
  if (st == ParseStatus::OK)
    this->finalize_(millis(), Outcome::OK);
  else if (st == ParseStatus::EXCEPTION)
    this->finalize_(millis(), Outcome::EXCEPTION);
  return n;
}

void SungrowDebug::discard_input() {
  uint8_t tmp[64];
  for (int i = 0; i < 16; i++) {
    size_t n = this->hub_->receive(tmp, sizeof(tmp));
    if (n == 0)
      break;
    if (this->stray_.size() < RX_KEEP)
      this->stray_.insert(this->stray_.end(), tmp, tmp + std::min(n, RX_KEEP - this->stray_.size()));
  }
}

void SungrowDebug::finalize_(uint32_t now, Outcome o) {
  if (!this->open_)
    return;
  this->open_ = false;
  Record &r = this->cur_;
  r.outcome = o;
  r.latency = now - r.t_tx;
  r.crc_errors = this->parser_.crc_errors();
  r.dropped = this->parser_.dropped();
  if (o == Outcome::EXCEPTION)
    r.exc = this->parser_.exception_code();
  if (o == Outcome::OK && is_read_fn(r.req.fn))
    r.payload.assign(this->parser_.data(), this->parser_.data() + 2u * this->parser_.nregs());
  if (o == Outcome::OK || o == Outcome::EXCEPTION) {
    LatAcc &a = this->lat_[r.req.addr];
    a.sum += r.latency;
    a.n++;
    a.max = std::max(a.max, r.latency);
  }
  this->last_ = LastOutcome{r.seq, r.req.addr, o, r.exc};
  if (this->records_.size() >= RECORDS_MAX) {
    this->records_.pop_front();
    this->records_dropped_++;
  }
  this->records_.push_back(std::move(r));
}

// ---- trace output -------------------------------------------------------------------------------
void SungrowDebug::process_record_(Record &r) {
  log_discovery_(r);
  if (r.outcome == Outcome::BLOCKED) {
    ESP_LOGW(TAG_RTU, "#%" PRIu32 " BLOCKED write/non-read frame (debug build): %s | %s", r.seq,
             describe_request(r.req).c_str(), hex(r.tx.data(), r.tx.size(), 32).c_str());
    return;
  }
  const bool sel = this->filter_.address_ok(r.req.addr) && this->filter_.block_ok(r.req.start, r.req.count);
  const uint32_t key = (static_cast<uint32_t>(r.req.addr) << 24) | (static_cast<uint32_t>(r.req.fn) << 16) | r.req.start;
  const bool failed = r.outcome != Outcome::OK;
  // Failures are never rate-limited.
  const bool allowed = sel && (failed || this->rate_.allow(key, r.t_tx, this->trace_min_interval_ms_));
  if (this->rtu_trace_ && allowed) {
    ESP_LOGD(TAG_RTU, "#%" PRIu32 " t=%" PRIu32 " TX %s | %s", r.seq, r.t_tx, describe_request(r.req).c_str(),
             hex(r.tx.data(), r.tx.size(), 16).c_str());
    if (r.outcome == Outcome::OK || r.outcome == Outcome::EXCEPTION) {
      // The accepted frame is the tail of what we received (the parser drops leading junk).
      const size_t flen = r.rx.size() >= r.dropped ? r.rx.size() - r.dropped : r.rx.size();
      const uint8_t *frame = r.rx.data() + (r.rx.size() - flen);
      RtuInfo a = parse_rtu_response(frame, flen);
      if (r.outcome == Outcome::EXCEPTION) {
        ESP_LOGW(TAG_RTU, "#%" PRIu32 " RX %u B in %" PRIu32 " ms: %s", r.seq, (unsigned) flen, r.latency,
                 describe_response(a).c_str());
      } else {
        ESP_LOGD(TAG_RTU, "#%" PRIu32 " RX %u B in %" PRIu32 " ms: %s%s", r.seq, (unsigned) flen, r.latency,
                 describe_response(a).c_str(),
                 r.dropped ? fmt(" (%u junk bytes skipped)", r.dropped).c_str() : "");
      }
      for (const auto &l : hex_lines(r.rx.data(), r.rx.size(), 32))
        ESP_LOGV(TAG_HEX, "#%" PRIu32 " RX %s", r.seq, l.c_str());
    } else {
      ESP_LOGW(TAG_RTU, "#%" PRIu32 " %s after %" PRIu32 " ms: %s; %u B received, %u CRC-bad candidates%s%s", r.seq,
               outcome_name(static_cast<int>(r.outcome)), r.latency, describe_request(r.req).c_str(),
               (unsigned) r.rx.size(), r.crc_errors, r.rx.empty() ? "" : ": ",
               r.rx.empty() ? "" : hex(r.rx.data(), r.rx.size(), 48).c_str());
    }
  }
  if (r.outcome == Outcome::OK && this->decode_trace_ && allowed && is_read_fn(r.req.fn) && r.dev >= 0) {
    const Device &d = this->hub_->master().devices()[r.dev];
    if (d.table() != nullptr)
      this->queue_decode_(r.dev, d.table(), r.req.fn, r.req.start, r.payload, false,
                          fmt("#%" PRIu32 " decode addr %u fn %u %u+%u:", r.seq, r.req.addr, r.req.fn, r.req.start,
                              r.req.count)
                              .c_str());
  }
}

void SungrowDebug::log_discovery_(const Record &r) {
  using namespace sungrow_modbus;
  if (r.req.fn != FN_READ_INPUT || r.req.start != DEVINFO_START || r.req.count != DEVINFO_COUNT)
    return;
  const Device *d = r.dev >= 0 ? &this->hub_->master().devices()[r.dev] : nullptr;
  if (r.outcome == Outcome::OK) {
    DevInfo info;
    if (!parse_devinfo(r.payload.data(), static_cast<uint16_t>(r.payload.size() / 2), &info)) {
      ESP_LOGW(TAG_DISC, "addr %u devinfo reply too short (%u regs)", r.req.addr, (unsigned) (r.payload.size() / 2));
      return;
    }
    const RegisterTable *t = sungrow_datamodel::inverter_table_for_protocol(info.protocol);
    ESP_LOGI(TAG_DISC,
             "addr %u devinfo %u+%u: dev_code 0x%04X model '%s' applyProtocolCode '%s' tag(2578) '%s' -> protocol "
             "type %u, dev_type %u (%s) -> table %s",
             r.req.addr, DEVINFO_START, DEVINFO_COUNT, info.dev_code, info.model, info.proto_code, info.tag,
             info.protocol, info.dev_type,
             info.dev_type == 35 ? "hybrid/storage" : info.dev_type == 21 ? "string/HOME" : "other",
             t != nullptr ? t->name
                          : (d != nullptr && d->config().fallback_table != nullptr
                                 ? (std::string(d->config().fallback_table->name) + " (fallback)").c_str()
                                 : "(none)"));
    ESP_LOGI(TAG_DISC, "addr %u devinfo raw: %s", r.req.addr,
             hex_words(r.payload.data(), r.payload.size() / 2, 66).c_str());
  } else if (r.outcome == Outcome::EXCEPTION) {
    ESP_LOGW(TAG_DISC, "addr %u devinfo %u+%u rejected with exception %u (%s) -> fallback table %s", r.req.addr,
             DEVINFO_START, DEVINFO_COUNT, r.exc, exception_name(r.exc),
             d != nullptr && d->config().fallback_table != nullptr ? d->config().fallback_table->name : "(none)");
  } else {
    ESP_LOGD(TAG_DISC, "addr %u devinfo probe: %s", r.req.addr, outcome_name(static_cast<int>(r.outcome)));
  }
}

void SungrowDebug::queue_decode_(int dev_index, const RegisterTable *table, uint8_t fn, uint16_t start,
                                 const std::vector<uint8_t> &payload, bool forced, const char *title) {
  auto job = make_unique<DecodeJob>();
  job->title = title;
  job->table = table;
  job->fn = fn;
  job->start = start;
  job->data = payload;
  job->forced = forced;
  job->only_changed = this->only_changed_;
  job->filter = this->filter_;
  job->changes = &this->changes_;
  job->store = &this->dm_->store();
  if (dev_index >= 0) {
    const Device &d = this->hub_->master().devices()[dev_index];
    job->device = d.config().index;
    job->have_device = true;
    job->wo = d.config().word_order;
    job->gating = d.gating();
  }
  job->rows = rows_in_block(*table, fn, start, static_cast<uint16_t>(payload.size() / 2));
  this->push_job_(std::move(job));
}

void SungrowDebug::push_job_(std::unique_ptr<LineJob> job) {
  if (this->jobs_.size() >= JOBS_MAX) {
    this->jobs_dropped_++;
    ESP_LOGW(TAG, "Output queue full: dropped a trace/dump (%" PRIu32 " so far). Narrow the filter.",
             this->jobs_dropped_);
    return;
  }
  this->jobs_.push_back(std::move(job));
}

void SungrowDebug::run_jobs_() {
  std::string line;
  const char *tag = TAG;
  for (uint8_t n = 0; n < this->lines_per_loop_ && !this->jobs_.empty();) {
    if (!this->jobs_.front()->next(&line, &tag)) {
      this->jobs_.pop_front();
      continue;
    }
    emit(tag == TAG_DEC ? ESPHOME_LOG_LEVEL_DEBUG : ESPHOME_LOG_LEVEL_INFO, tag, line);
    n++;
  }
}

#ifdef USE_SUNGROW_DEBUG_TCP
void SungrowDebug::on_tcp_frame_(uint8_t what, const char *peer, const uint8_t *adu, size_t len) {
  if (!this->tcp_trace_)
    return;
  const bool reply = what == 1;
  MbapInfo m = parse_mbap(adu, len, reply);
  const char *dir = what == 0 ? "REQ" : what == 1 ? "RSP" : "DROPPED (no reply)";
  if (what == 1 && m.exception)
    ESP_LOGW(TAG_TCP, "%s %s %s", peer, dir, describe_mbap(m, true).c_str());
  else
    ESP_LOGD(TAG_TCP, "%s %s %s", peer, dir, describe_mbap(m, reply).c_str());
  for (const auto &l : hex_lines(adu, len, 32))
    ESP_LOGV(TAG_HEX, "tcp %s %s", what == 0 ? "req" : "rsp", l.c_str());
}
#endif

// ---- on-demand tools ----------------------------------------------------------------------------
int SungrowDebug::device_index_for_address_(uint8_t addr) const {
  const auto &devs = this->hub_->master().devices();
  for (size_t i = 0; i < devs.size(); i++)
    if (devs[i].config().address == addr)
      return static_cast<int>(i);
  return -1;
}

void SungrowDebug::set_last_result_(const std::string &s) {
#ifdef USE_TEXT_SENSOR
  if (this->last_result_ts_ != nullptr)
    this->last_result_ts_->publish_state(s.size() > 250 ? s.substr(0, 250) : s);
#endif
}

bool SungrowDebug::debug_read(int address, int fn, int start, int count, const std::string &table) {
  if (address < 1 || address > 247 || (fn != 3 && fn != 4) || start < 1 || start > 65535 || count < 1 ||
      count > 125) {
    ESP_LOGW(TAG, "debug_read: need address 1..247, fn 3|4, start 1..65535 (1-based), count 1..125");
    this->set_last_result_("bad arguments");
    return false;
  }
  const RegisterTable *forced_table = nullptr;
  if (!table.empty()) {
    forced_table = sungrow_datamodel::find_table(table.c_str());
    if (forced_table == nullptr)
      ESP_LOGW(TAG, "debug_read: unknown table '%s', using the device's", table.c_str());
  }
  const uint8_t a = address, f = fn;
  const uint16_t s = start, c = count;
  ESP_LOGI(TAG, "debug_read addr %u fn %u start %u count %u queued", a, f, s, c);
  bool q = this->hub_->read_registers(a, f, s, c, [this, a, f, s, c, forced_table](bool ok, const uint8_t *data,
                                                                                       uint16_t nregs) {
    if (!ok) {
      if (this->open_ && this->cur_.req.addr == a)
        this->finalize_(millis(), this->parser_.crc_errors() > 0 ? Outcome::CRC_ERROR : Outcome::TIMEOUT);
      std::string why = this->last_.addr == a ? outcome_name(static_cast<int>(this->last_.outcome)) : "failed";
      if (this->last_.addr == a && this->last_.outcome == Outcome::EXCEPTION)
        why += fmt(" %u (%s)", this->last_.exc, exception_name(this->last_.exc));
      std::string res = fmt("addr %u fn %u %u+%u: %s", a, f, s, c, why.c_str());
      ESP_LOGW(TAG, "debug_read %s", res.c_str());
      this->set_last_result_(res);
      return;
    }
    std::string words = hex_words(data, nregs);
    ESP_LOGI(TAG, "debug_read addr %u fn %u %u+%u OK: %u regs", a, f, s, c, nregs);
    for (uint16_t off = 0; off < nregs; off += 16) {
      const uint16_t n = std::min<uint16_t>(16, nregs - off);
      ESP_LOGI(TAG, "  %5u: %s", s + off, hex_words(data + 2 * off, n).c_str());
    }
    this->set_last_result_(fmt("addr %u fn %u %u+%u: ", a, f, s, c) + words);
    const int di = this->device_index_for_address_(a);
    const RegisterTable *t = forced_table;
    if (t == nullptr && di >= 0)
      t = this->hub_->master().devices()[di].table();
    if (t != nullptr) {
      std::vector<uint8_t> payload(data, data + 2u * nregs);
      this->queue_decode_(di, t, f, s, payload, true, fmt("debug_read addr %u fn %u %u+%u:", a, f, s, c).c_str());
    } else {
      ESP_LOGI(TAG, "  (no table for addr %u: pass table: e.g. inv_proto2 to decode)", a);
    }
  });
  if (!q) {
    ESP_LOGW(TAG, "debug_read: ad-hoc queue full or invalid");
    this->set_last_result_("queue full");
  }
  return q;
}

bool SungrowDebug::debug_scan(int first, int last, int start, int count, int interval_ms, int timeout_ms) {
  if (this->scan_.active()) {
    ESP_LOGW(TAG_DISC, "Scan already running (%s); call debug_scan_stop first", this->scan_.summary().c_str());
    return false;
  }
  if (start < 1 || start > 65535 || count < 1 || count > 125) {
    ESP_LOGW(TAG_DISC, "debug_scan: start 1..65535, count 1..125");
    return false;
  }
  if (first < 1)
    first = 1;
  if (last > 247)
    last = 247;
  if (first > last) {
    ESP_LOGW(TAG_DISC, "debug_scan: empty range");
    return false;
  }
  this->scan_start_ = start;
  this->scan_count_ = count;
  if (timeout_ms > 0) {
    auto t = this->hub_->master().timing();
    this->scan_saved_timeout_ = t.response_timeout_ms;
    t.response_timeout_ms = std::max(50, timeout_ms);
    this->hub_->master().set_timing(t);
    this->scan_timeout_override_ = true;
  }
  this->scan_.begin(first, last, interval_ms < 0 ? 0 : interval_ms, millis());
  ESP_LOGI(TAG_DISC, "Scan of addresses %d..%d with fn 4 %u+%u, %d ms between probes, timeout %" PRIu32 " ms%s",
           first, last, this->scan_start_, this->scan_count_, interval_ms,
           this->hub_->master().timing().response_timeout_ms,
           this->scan_timeout_override_ ? " (temporary, applies to the whole bus)" : "");
#ifdef USE_TEXT_SENSOR
  if (this->scan_result_ts_ != nullptr)
    this->scan_result_ts_->publish_state("running");
#endif
  return true;
}

void SungrowDebug::stop_scan() {
  if (!this->scan_.active())
    return;
  this->scan_.stop();
  this->scan_finish_();
}

void SungrowDebug::scan_finish_() {
  if (this->scan_timeout_override_) {
    auto t = this->hub_->master().timing();
    t.response_timeout_ms = this->scan_saved_timeout_;
    this->hub_->master().set_timing(t);
    this->scan_timeout_override_ = false;
  }
  const std::string s = this->scan_.summary();
  ESP_LOGI(TAG_DISC, "%s", s.c_str());
#ifdef USE_TEXT_SENSOR
  if (this->scan_result_ts_ != nullptr)
    this->scan_result_ts_->publish_state(s.size() > 250 ? s.substr(0, 250) : s);
#endif
}

void SungrowDebug::scan_tick_(uint32_t now) {
  uint8_t addr = 0;
  if (!this->scan_.next(now, &addr))
    return;
  const uint16_t start = this->scan_start_, count = this->scan_count_;
  bool q = this->hub_->read_registers(addr, 4, start, count, [this, addr, start](bool ok, const uint8_t *data,
                                                                                    uint16_t nregs) {
    const uint32_t t = millis();
    ProbeResult res = ProbeResult::OK;
    uint8_t exc = 0;
    uint16_t value = 0;
    if (ok) {
      value = nregs > 0 ? be16(data) : 0;
    } else {
      if (this->open_ && this->cur_.req.addr == addr)
        this->finalize_(t, this->parser_.crc_errors() > 0 ? Outcome::CRC_ERROR : Outcome::TIMEOUT);
      res = ProbeResult::TIMEOUT;
      if (this->last_.addr == addr) {
        if (this->last_.outcome == Outcome::EXCEPTION) {
          res = ProbeResult::EXCEPTION;
          exc = this->last_.exc;
        } else if (this->last_.outcome == Outcome::CRC_ERROR) {
          res = ProbeResult::CRC_ERROR;
        }
      }
    }
    if (res == ProbeResult::OK)
      ESP_LOGI(TAG_DISC, "scan: addr %u answered fn 4 %u: 0x%04X (%u)", addr, start, value, value);
    else if (res == ProbeResult::EXCEPTION)
      ESP_LOGI(TAG_DISC, "scan: addr %u present, exception %u (%s)", addr, exc, exception_name(exc));
    else if (res == ProbeResult::CRC_ERROR)
      ESP_LOGW(TAG_DISC, "scan: addr %u: CRC-bad reply (something is there, or line noise)", addr);
    else
      ESP_LOGV(TAG_DISC, "scan: addr %u: no reply", addr);
    // Follow a responder up with the devinfo block (protocol type / dev_type classification).
    if (res == ProbeResult::OK && start != sungrow_modbus::DEVINFO_START)
      this->hub_->read_registers(addr, 4, sungrow_modbus::DEVINFO_START, sungrow_modbus::DEVINFO_COUNT,
                                 [addr](bool ok2, const uint8_t *d2, uint16_t n2) {
                                   sungrow_modbus::DevInfo info;
                                   if (ok2 && sungrow_modbus::parse_devinfo(d2, n2, &info)) {
                                     const RegisterTable *tab =
                                         sungrow_datamodel::inverter_table_for_protocol(info.protocol);
                                     ESP_LOGI(TAG_DISC,
                                              "scan: addr %u devinfo: dev_code 0x%04X model '%s' proto '%s' -> "
                                              "protocol type %u dev_type %u table %s",
                                              addr, info.dev_code, info.model, info.proto_code, info.protocol,
                                              info.dev_type, tab != nullptr ? tab->name : "(none)");
                                   } else {
                                     ESP_LOGI(TAG_DISC, "scan: addr %u: devinfo 2515+66 not readable (not an inverter?)",
                                              addr);
                                   }
                                 });
    this->scan_.done(addr, res, exc, value, t);
    if (this->scan_.probed() % 16 == 0 && this->scan_.active())
      ESP_LOGI(TAG_DISC, "%s", this->scan_.summary().c_str());
    if (!this->scan_.active())
      this->scan_finish_();
  });
  if (!q)
    this->scan_.done(addr, ProbeResult::NOT_QUEUED, 0, 0, now);
}

void SungrowDebug::dump_signal_store(int device) {
  auto job = make_unique<StoreDumpJob>();
  job->store = &this->dm_->store();
  this->dm_->store().for_each([&](const Signal &s) {
    if (device < 0 || s.device == device)
      job->entries.push_back({s.device, s.sigid});
  });
  std::sort(job->entries.begin(), job->entries.end(), [](const StoreDumpJob::Entry &a, const StoreDumpJob::Entry &b) {
    return a.device != b.device ? a.device < b.device : a.sigid < b.sigid;
  });
  auto *hub = this->hub_;
  job->table_for = [hub](uint8_t dev) -> const RegisterTable * {
    for (const auto &d : hub->master().devices())
      if (d.config().index == dev)
        return d.table();
    return nullptr;
  };
  ESP_LOGI(TAG_DUMP, "signal store dump (device %d): %u signals, store total %u", device,
           (unsigned) job->entries.size(), (unsigned) this->dm_->store().size());
  this->push_job_(std::move(job));
}

void SungrowDebug::dump_poll_plan(int device) {
  std::vector<std::string> lines;
  const uint32_t now = millis();
  for (const auto &d : this->hub_->master().devices()) {
    const auto &c = d.config();
    if (device >= 0 && c.index != device)
      continue;
    lines.push_back(fmt("dev %u addr %u: %s, table %s, %u reads, initial pass %s, gating %s, gap %" PRIu32
                        " ms, freshest read %" PRId32 " ms ago",
                        c.index, c.address, d.online() ? "online" : "OFFLINE",
                        d.table() != nullptr ? d.table()->name : "(discovering)", (unsigned) d.jobs().size(),
                        d.first_round_done() ? "done" : "pending", d.gating() ? "on" : "off",
                        this->hub_->master().frame_gap(&d),
                        d.fresh_ms() != 0 ? static_cast<int32_t>(now - d.fresh_ms()) : -1));
    for (size_t i = 0; i < d.jobs().size(); i++) {
      const auto &j = d.jobs()[i];
      lines.push_back(fmt("  #%02u fn%u %5u..%5u (%3u) %-6s%s every %" PRIu32 " ms, next in %" PRId32
                          " ms, last answer %" PRId32 " ms ago",
                          (unsigned) i, j.req.fn, j.req.start, (unsigned) (j.req.start + j.req.count - 1), j.req.count,
                          sungrow_modbus::poll_tier_name(j.tier), j.unserved ? " (no values: static)" : "",
                          this->hub_->master().job_period(j), static_cast<int32_t>(j.next_due - now),
                          j.last_ok != 0 ? static_cast<int32_t>(now - j.last_ok) : -1));
    }
  }
  if (lines.empty())
    lines.push_back(fmt("no device with index %d", device));
  this->push_job_(make_unique<LinesJob>(TAG_DUMP, std::move(lines)));
}

void SungrowDebug::dump_stats() {
  std::vector<std::string> lines;
  for (const auto &d : this->hub_->master().devices()) {
    const auto &s = d.stats();
    auto it = this->lat_.find(d.config().address);
    const uint32_t avg = it != this->lat_.end() && it->second.n ? it->second.sum / it->second.n : 0;
    const uint32_t mx = it != this->lat_.end() ? it->second.max : 0;
    lines.push_back(fmt("dev %u addr %u %s: sent %" PRIu32 " recv %" PRIu32 " timeouts %" PRIu32 " crc %" PRIu32
                        " exc %" PRIu32 " loss %.1f%% consecutive fails %u, latency avg %" PRIu32 " max %" PRIu32
                        " ms (since last stats)",
                        d.config().index, d.config().address, d.online() ? "online" : "OFFLINE", s.sent, s.received,
                        s.timeouts, s.crc_errors, s.exceptions, s.loss_pct(), s.consecutive_failures, avg, mx));
  }
  lines.push_back(fmt("trace: %" PRIu32 " frames seen, %" PRIu32 " records dropped, %" PRIu32 " frames blocked, %" PRIu32
                      " outputs dropped, ad-hoc pending %u",
                      this->seq_, this->records_dropped_, this->blocked_frames_, this->jobs_dropped_,
                      (unsigned) this->hub_->master().adhoc_pending()));
  this->push_job_(make_unique<LinesJob>(TAG_DUMP, std::move(lines)));
}

void SungrowDebug::publish_stats_(uint32_t now) {
  for (auto &sd : this->stats_) {
    Device *d = this->hub_->master().find_device_by_address(sd.address);
    if (d == nullptr)
      continue;
    LatAcc &a = this->lat_[sd.address];
    const float age_ms = d->fresh_ms() != 0 ? static_cast<float>(now - d->fresh_ms()) : NAN;
#ifdef USE_SENSOR
    const auto &s = d->stats();
    const float vals[ST_COUNT] = {static_cast<float>(s.sent),
                                  static_cast<float>(s.received),
                                  static_cast<float>(s.timeouts),
                                  static_cast<float>(s.crc_errors),
                                  static_cast<float>(s.exceptions),
                                  a.n ? static_cast<float>(a.sum) / a.n : NAN,
                                  a.n ? static_cast<float>(a.max) : NAN,
                                  age_ms,
                                  s.loss_pct()};
    for (int k = 0; k < ST_COUNT; k++)
      if (sd.sensors[k] != nullptr)
        sd.sensors[k]->publish_state(vals[k]);
#endif
    a = LatAcc();  // latency figures are per stats interval
  }
}

// ---- ext flash (read-only) ----------------------------------------------------------------------
bool SungrowDebug::dump_file(const std::string &path) {
#ifdef USE_SUNGROW_DEBUG_EXTFLASH
  if (this->extflash_ == nullptr || !this->extflash_->is_mounted()) {
    ESP_LOGW(TAG, "dump_file: ext flash not mounted");
    return false;
  }
  if (path.find("..") != std::string::npos) {
    ESP_LOGW(TAG, "dump_file: '..' not allowed");
    return false;
  }
  const std::string &mp = this->extflash_->mount_path();
  std::string full = path.rfind(mp, 0) == 0 ? path : mp + (path.empty() || path[0] != '/' ? "/" : "") + path;
  ESP_LOGI(TAG, "dump_file %s (hex at VERY_VERBOSE, tag sungrow_extflash; decode with tools/read_identity.py)",
           full.c_str());
  return this->extflash_->dump_file(full);
#else
  ESP_LOGW(TAG, "dump_file: no sungrow_extflash configured");
  return false;
#endif
}

void SungrowDebug::list_files(const std::string &path) {
#ifdef USE_SUNGROW_DEBUG_EXTFLASH
  if (this->extflash_ != nullptr)
    this->extflash_->list_dir(path);
#endif
}

void SungrowDebug::dump_identity() {
#ifdef USE_SUNGROW_DEBUG_EXTFLASH
  if (this->extflash_ != nullptr)
    this->extflash_->dump_identity();
#endif
}

void SungrowDebug::dump_flash_layout() { this->push_job_(make_unique<LinesJob>(TAG_DUMP, flash_report_lines())); }

// ---- system diagnostics -------------------------------------------------------------------------
static const char *ota_state_name(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW:
      return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
      return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID:
      return "valid";
    case ESP_OTA_IMG_INVALID:
      return "INVALID";
    case ESP_OTA_IMG_ABORTED:
      return "ABORTED";
    case ESP_OTA_IMG_UNDEFINED:
    default:
      return "undefined";
  }
}

static const char *reset_reason_name(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "external pin";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "PANIC";
    case ESP_RST_INT_WDT:
      return "INTERRUPT WDT";
    case ESP_RST_TASK_WDT:
      return "TASK WDT";
    case ESP_RST_WDT:
      return "other WDT";
    case ESP_RST_DEEPSLEEP:
      return "deep sleep";
    case ESP_RST_BROWNOUT:
      return "BROWNOUT";
    case ESP_RST_SDIO:
      return "SDIO";
    default:
      return "unknown";
  }
}

std::string SungrowDebug::partition_summary_() const {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
  if (run != nullptr)
    esp_ota_get_state_partition(run, &st);
  return fmt("running %s @0x%" PRIX32 " (0x%" PRIX32 ", %s), boot %s", run ? run->label : "?",
             run ? run->address : 0, run ? run->size : 0, ota_state_name(st), boot ? boot->label : "?");
}

void SungrowDebug::dump_system() {
  std::vector<std::string> L;
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flash = 0;
  esp_flash_get_size(nullptr, &flash);
  L.push_back(fmt("system: ESPHome %s, IDF %s, chip model %d rev v%d.%d, %d cores, features 0x%" PRIX32
                  ", internal flash %" PRIu32 " MB",
                  ESPHOME_VERSION, esp_get_idf_version(), (int) chip.model, chip.revision / 100, chip.revision % 100,
                  chip.cores, chip.features, flash >> 20));
  const esp_app_desc_t *app = esp_app_get_description();
  L.push_back(fmt("app: %s %s built %s %s (idf %s), secure_version %" PRIu32, app->project_name, app->version,
                  app->date, app->time, app->idf_ver, app->secure_version));
  L.push_back(fmt("reset reason: %s, uptime %" PRIu32 " s", reset_reason_name(esp_reset_reason()), millis() / 1000));
  L.push_back(fmt("heap internal: free %u, min free %u, largest block %u",
                  (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  const size_t ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  L.push_back(ps == 0 ? std::string("PSRAM: none detected (check psram mode / H3)")
                      : fmt("PSRAM: total %u, free %u, min free %u, largest block %u", (unsigned) ps,
                            (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                            (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                            (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  // Every partition the running app can see (internal table + the ext flash one sungrow_extflash
  // registers at runtime). This is the confirmation of the real stock table on first boot.
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
  L.push_back(fmt("partitions: running %s, boot %s, next OTA %s, last invalid %s", run ? run->label : "?",
                  boot ? boot->label : "?", next ? next->label : "-", bad ? bad->label : "-"));
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  int n = 0;
  for (; it != nullptr; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    std::string extra;
    if (p->type == ESP_PARTITION_TYPE_APP) {
      esp_ota_img_states_t st;
      if (esp_ota_get_state_partition(p, &st) == ESP_OK)
        extra += fmt(" ota-state %s", ota_state_name(st));
      esp_app_desc_t d;
      if (esp_ota_get_partition_description(p, &d) == ESP_OK)
        extra += fmt(" app '%s' %s (%s %s, idf %s)", d.project_name, d.version, d.date, d.time, d.idf_ver);
      else
        extra += " (no valid app image)";
    }
    L.push_back(fmt("  part %-10s type %u sub 0x%02X @0x%06" PRIX32 " size 0x%06" PRIX32 "%s%s%s%s", p->label,
                    (unsigned) p->type, (unsigned) p->subtype, p->address, p->size,
                    p->flash_chip != esp_flash_default_chip ? " [EXT FLASH]" : "", p->readonly ? " ro" : "",
                    p == run ? " <running>" : "", extra.c_str()));
    n++;
  }
  esp_partition_iterator_release(it);
  L.push_back(fmt("  %d partitions", n));
  L.push_back(fmt("main loop task stack free (high-water) %u B",
                  (unsigned) uxTaskGetStackHighWaterMark(nullptr)));
#if configUSE_TRACE_FACILITY
  {
    const UBaseType_t cnt = uxTaskGetNumberOfTasks();
    std::vector<TaskStatus_t> ts(cnt + 4);
    const UBaseType_t got = uxTaskGetSystemState(ts.data(), ts.size(), nullptr);
    for (UBaseType_t i = 0; i < got; i++)
      L.push_back(fmt("  task %-16s prio %2u state %d stack free %5u B", ts[i].pcTaskName,
                      (unsigned) ts[i].uxCurrentPriority, (int) ts[i].eCurrentState,
                      (unsigned) ts[i].usStackHighWaterMark));
  }
#else
  L.push_back("  (task list needs CONFIG_FREERTOS_USE_TRACE_FACILITY)");
#endif
  L.push_back(fmt("RS485 writes %s; trace frames %" PRIu32 ", blocked %" PRIu32, this->block_writes_ ? "BLOCKED" : "allowed",
                  this->seq_, this->blocked_frames_));
#ifdef USE_TEXT_SENSOR
  if (this->partition_ts_ != nullptr)
    this->partition_ts_->publish_state(this->partition_summary_());
#endif
  this->push_job_(make_unique<LinesJob>(TAG_DUMP, std::move(L)));
}

// ---- plain TCP log server -----------------------------------------------------------------------
#ifdef USE_SUNGROW_DEBUG_LOG_SERVER
void SungrowDebug::on_log_(void *self, uint8_t level, const char *tag, const char *msg, size_t len) {
  auto *d = static_cast<SungrowDebug *>(self);
  (void) level;
  (void) tag;
  if (d->in_log_cb_ || d->log_clients_.empty())
    return;
  d->in_log_cb_ = true;
  for (auto &c : d->log_clients_) {
    if (c->out.size() + len + 2 > LOG_CLIENT_BUF) {
      c->dropped++;
      continue;
    }
    c->out.append(msg, len);
    c->out.append("\r\n");
  }
  d->in_log_cb_ = false;
}

void SungrowDebug::log_server_loop_() {
  if (this->log_port_ == 0)
    return;
  const uint32_t now = millis();
  if (this->log_listener_ == nullptr) {
    if (static_cast<int32_t>(now - this->log_retry_) < 0)
      return;
    this->log_retry_ = now + 10000;
    if (!network::is_connected())
      return;
    auto l = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
    if (l == nullptr)
      return;
    int one = 1;
    l->setsockopt(SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_storage sa;
    socklen_t sl = socket::set_sockaddr_any((struct sockaddr *) &sa, sizeof(sa), this->log_port_);
    if (l->setblocking(false) != 0 || sl == 0 || l->bind((struct sockaddr *) &sa, sl) != 0 || l->listen(1) != 0) {
      ESP_LOGW(TAG, "Log server: listen on :%u failed (errno %d)", this->log_port_, errno);
      return;
    }
    this->log_listener_ = std::move(l);
    ESP_LOGI(TAG, "Log server listening on :%u (plain TCP, LAN only: `nc <ip> %u`)", this->log_port_, this->log_port_);
  }
  while (true) {
    struct sockaddr_storage addr;
    socklen_t alen = sizeof(addr);
    auto s = this->log_listener_->accept_loop_monitored((struct sockaddr *) &addr, &alen);
    if (!s)
      break;
    if (this->log_clients_.size() >= LOG_CLIENTS_MAX || s->setblocking(false) != 0)
      continue;  // destructor closes
    auto c = make_unique<LogClient>();
    s->getpeername_to(c->peer);
    c->sock = std::move(s);
    c->out = "WiNet debug log stream (read-only). Level follows the logger.\r\n";
    ESP_LOGI(TAG, "Log client %s connected", c->peer);
    this->log_clients_.push_back(std::move(c));
  }
  for (size_t i = 0; i < this->log_clients_.size();) {
    LogClient &c = *this->log_clients_[i];
    bool alive = true;
    uint8_t junk[32];
    ssize_t rn = c.sock->read(junk, sizeof(junk));  // input is ignored; detects close
    if (rn == 0 || (rn < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
      alive = false;
    while (alive && !c.out.empty()) {
      ssize_t n = c.sock->write(c.out.data(), std::min<size_t>(c.out.size(), 1024));
      if (n > 0) {
        c.out.erase(0, static_cast<size_t>(n));
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        break;
      alive = false;
    }
    if (alive && c.dropped != 0 && c.out.size() < LOG_CLIENT_BUF / 2) {
      c.out += fmt("\r\n[%" PRIu32 " log lines dropped: client too slow]\r\n", c.dropped);
      c.dropped = 0;
    }
    if (!alive) {
      ESP_LOGI(TAG, "Log client %s disconnected", c.peer);
      this->log_clients_.erase(this->log_clients_.begin() + i);
    } else {
      i++;
    }
  }
}
#endif

}  // namespace sungrow_debug
}  // namespace esphome
