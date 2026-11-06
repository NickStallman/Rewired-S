#pragma once
// RS485 Modbus-RTU master core: per-device poll plans, tiered slot scheduler, one transaction at
// a time with timeout / inter-frame gap, link statistics, inverter discovery and an ad-hoc request
// queue. Pure C++ on top of the sungrow_datamodel core (no ESPHome headers), so tests/modbus/
// drives it with a fake transport and a fake clock.
//
// Never blocks: tick() does at most one write or one read of what is already buffered.
//
// Polling (docs/update_rates.md):
//  - Poll plan per device: build_poll_plan() (poll_plan.h) splits the table into reads of one
//    refresh tier each: FAST 1 s, LIVE 5 s, SLOW 30 s, STATIC 5 min (set_tier_period()). The SBR
//    cell block is one more read (SLOW tier, sbr_interval_ms).
//  - Slots: each read has fixed slots epoch + phase + k * period. The FAST reads of a device
//    share one phase (one coherent snapshot); the reads of every other tier are spread evenly over
//    their period, so the bus carries a steady trickle instead of bursts. A read sent late keeps
//    its slot grid (no drift); a read more than half a period late skips to the next slot.
//  - Priority: writes, then due FAST reads (earliest slot first), then ad-hoc reads, the inverter
//    search, discovery / probe traffic, the initial pass, and the remaining due reads (lowest
//    tier, then earliest slot). Anything but a FAST read starts only if its expected bus time
//    ends before the next FAST slot, so the FAST set stays on time; after 2 s without such a
//    start the check is waived, so nothing starves.
//  - Initial pass: after start / reconnect every read is sent once (capability bitmap first, then
//    the FAST reads, then the rest); first_round_done() then turns true.
//  - Unserved reads: a read whose answer holds no valid value (every row absent, gated off by the
//    capability bitmap, or not kept by the store), or that the slave rejects with an exception,
//    is polled at the STATIC period until it carries a value again.
//  - Gap before each frame: 18 ms (inverter/meter/logger) or 1 ms (battery/charger/energy
//    bridge), at least 3.5 character times. Stock (nSendFrameSpace): inverter 0 -> 18 ms, meter
//    1 ms; battery/charger/energy bridge fall back to 1 ms, but their DM default is 18 ms. Cyclic reads time out
//    after their wire time + poll_timeout_margin_ms; reads of ranges the inverter tunnels to its
//    DSP or the BCU after tunnel_timeout_ms; other reads after response_timeout_ms. No
//    retransmit. A device goes offline after offline_after consecutive failures spanning at
//    least offline_silence_ms; that clears only tag-bit1 rows. A Modbus exception or a timeout
//    keeps the old values.
//
// Finding inverters (stock auto search, msg 0x72): no address
// range is probed. Remembered inverter addresses (persisted by the ESPHome layer) are polled
// directly. The search is one broadcast read: fn 3 to slave 0 of 30035 (wire 0x7552) x 1; the
// inverter answers from slave id 0 with its own address (1..246), 1.9 s timeout, 2 tries. It runs
// at start, after an inverter goes offline, and every SEARCH_REPEAT_MS while no inverter is
// online. A new address is added as an inverter (and replaces remembered addresses that did not
// answer this boot, as stock replaces the old node). Neither the SH10RS nor the SG5.0RS answers
// this read (their fn 3 handlers have no 30035 case and ignore slave 0), so on our sites the
// remembered address and the range probe find the inverter. Stock falls back to a "By-COM"
// arbitration that WRITES (broadcast fn 16 to 4886 / 4866..); we never do. Instead, when the
// search gets no answer, no inverter is online and every remembered address has been asked at
// least once, the optional scan range is probed read-only, once per boot and once per rescan().
// An inverter that probe finds replaces the remembered addresses that did not answer this boot.
//
// Discovery (devices with table auto): fn 4 devinfo 2515 x 66 -> protocol type and dev_type
// (discovery.h) -> the inv_proto* table, then one fn 3 read of 30644 (inverter type, sig 0x1002;
// an exception or timeout leaves it unknown and costs nothing) before the first round. A Modbus
// exception to the devinfo read activates fallback_table (inv_proto1_4 = protocol 1, as stock).
// sig 0x1002 from the inv_proto1_4 poll keeps the inverter type current afterwards.
//
// Writes (submit_write): a separate queue served before ad-hoc reads and cyclic polls, one
// transaction in flight like everything else. fn 6 for one register, fn 16 for more (or as
// requested), sent once (no retransmit), timeout write_timeout_ms. Only to a device that is
// polling (table active) and online. Optional read-back verify (fn 3 of the same range right
// after the settle gap). A register written less than write_rate_limit_ms ago is rejected. After
// every write (any outcome) the bus stays quiet for write_settle_ms before the next frame.
// Result through the callback; per-master WriteStats.
//
// Exclusive bus (acquire_exclusive): only queued writes and ad-hoc reads go out, nothing cyclic.
// In raw mode (set_raw) the master is silent and leaves the transport to the owner, which runs
// the inverter's YMODEM file transfer on it. Polls resume on release; nothing goes offline
// because of the pause (offline needs failed requests, and none are sent).
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../sungrow_datamodel/register_tables.h"
#include "../sungrow_datamodel/request_builder.h"
#include "../sungrow_datamodel/signal_store.h"
#include "discovery.h"
#include "modbus_rtu.h"
#include "poll_plan.h"

namespace esphome {
namespace sungrow_modbus {

using sungrow_datamodel::ReadRequest;
using sungrow_datamodel::RegisterTable;
using sungrow_datamodel::SignalStore;
using sungrow_datamodel::WordOrder;

/// Byte pipe to the RS485 transceiver (half duplex; DE handled below this interface).
class Transport {
 public:
  virtual ~Transport() = default;
  /// Send one frame. May block for the time the UART needs to accept it (FIFO-sized frames
  /// return at once). DE/RTS turnaround is the transport's job.
  virtual void send_frame(const uint8_t *data, size_t len) = 0;
  /// Copy up to `cap` already-received bytes into buf; never waits. Returns the count.
  virtual size_t receive(uint8_t *buf, size_t cap) = 0;
  /// Discard anything pending in the RX buffer.
  virtual void discard_input() = 0;
};

struct MasterTiming {
  uint32_t baud{9600};
  uint8_t bits_per_char{10};          ///< 8N1 = 10; 8E1/8N2 = 11
  /// Silence before each frame (stock nSendFrameSpace; 0 there means 18 ms in COM_Master) for
  /// inverters, meters and loggers. The effective gap is never below 3.5 character times.
  uint32_t frame_gap_ms{18};
  /// Same for battery, charger and energy bridge devices (stock set-default fallback
  /// nSendFrameSpace = 1; their DM sig-config default is 18).
  uint32_t frame_gap_fast_ms{1};
  /// Reply timeout from the send for the reads that are not cyclic (discovery, inverter type,
  /// ad-hoc, write verify). Stock uses 2000 ms for every frame.
  uint32_t response_timeout_ms{1000};
  /// Cyclic reads (and offline probes) time out after the request + reply wire time plus this.
  /// The SH10RS answers in 4..10 ms (docs/update_rates.md §7).
  uint32_t poll_timeout_margin_ms{200};
  /// Reads of the ranges the inverter forwards to its DSP or tunnels to the BCU (up to ~1.5 s
  /// inside the inverter; tunnel_range()).
  uint32_t tunnel_timeout_ms{2000};
  /// Retransmits after a timeout / CRC failure. Stock sends a Modbus request exactly once.
  uint8_t retries{0};
  /// Consecutive failed transactions before a device is declared offline (stock nReSendTimes)...
  uint8_t offline_after{10};
  /// ...that also span at least this long since the device last answered.
  uint32_t offline_silence_ms{10000};
  /// While offline (or undiscovered after failures): one probe this often.
  /// (Stock meters probe every 100 s.)
  uint32_t offline_probe_ms{15000};
  /// Write reply timeout from the send.
  uint32_t write_timeout_ms{2000};
  /// Bus silence after a write before the next frame (incl. its verify read).
  /// Differs from stock, which pauses 100 ms only after a broadcast write.
  uint32_t write_settle_ms{100};
  /// Minimum time between two writes touching the same register of one slave; 0 = off.
  uint32_t write_rate_limit_ms{0};
};

struct DeviceConfig {
  uint8_t index{0};      ///< SignalStore device index (matches `device:` of the entities)
  uint8_t address{1};    ///< Modbus slave address
  /// Poll table; nullptr = auto (inverter discovery via the devinfo block, see discovery.h).
  const RegisterTable *table{nullptr};
  /// Table used when discovery gets a Modbus exception or an unknown protocol type.
  const RegisterTable *fallback_table{nullptr};
  /// Stock dev_type (1, 21, 35, 18, 34, 44, 46, 92); 0 = from the devinfo block, else inferred
  /// from the table family. Non-zero overrides both.
  uint8_t dev_type{0};
  WordOrder word_order{WordOrder::LOW_WORD_FIRST};
  /// -1 = auto (on for inv_proto2/3, which carry the 0x2181 bitmap), 0 = off, 1 = on.
  int8_t gate_capabilities{-1};
  /// Read the SBR cell block 10757..10780 as one block (a SLOW-tier read with its own period).
  bool sbr_cells{false};
  uint32_t sbr_interval_ms{30000};
  /// Frame gap for this device in ms; -1 = by table family (MasterTiming).
  int32_t frame_gap_ms{-1};
  /// True for a device created by the fallback range probe (abandonable when nothing answers).
  bool scan{false};
  /// True for an inverter at a remembered address or one the search found: discovered via the
  /// devinfo block, retried while it is silent (an inverter is dark at night), never abandoned
  /// unless the search finds the inverter at a different address.
  bool remembered{false};
  /// Non-NONE for a device an inverter reported (battery / charger / Energy Bridge): discovered by
  /// its class identity read instead of the inverter devinfo block (discovery.h ChildProbe).
  ChildKind child_kind{ChildKind::NONE};
  /// Device index of the reporting inverter, -1 = none.
  int16_t parent{-1};
};

struct LinkStats {
  uint32_t sent{0};        ///< frames sent (incl. retries)
  uint32_t received{0};    ///< valid replies (incl. exceptions)
  uint32_t timeouts{0};    ///< attempts with no valid reply and no CRC error
  uint32_t crc_errors{0};  ///< attempts that ended with only CRC-bad candidates
  uint32_t exceptions{0};  ///< Modbus exception replies
  uint16_t consecutive_failures{0};
  /// Sliding window of the last 64 attempts (bit = 1: lost).
  uint64_t window{0};
  uint8_t window_n{0};
  /// Packet loss over the window in percent; NaN before the first attempt.
  float loss_pct() const;
  void record(bool lost);
};

using AdhocCallback = std::function<void(bool ok, const uint8_t *data, uint16_t nregs)>;

enum class WriteStatus : uint8_t {
  QUEUED,           ///< submit_write(): accepted, the callback follows
  OK,               ///< echo received (and the read-back matched, with verify)
  EXCEPTION,        ///< the slave answered with a Modbus exception (WriteResult::exception)
  TIMEOUT,          ///< no valid reply within the write timeout (or only CRC-bad frames)
  VERIFY_MISMATCH,  ///< echo received, read-back differs (WriteResult::readback)
  VERIFY_FAILED,    ///< echo received, read-back failed (timeout / exception)
  OFFLINE,          ///< unknown address, not discovered yet, or offline
  RATE_LIMITED,     ///< a register of the range was written less than write_rate_limit_ms ago
  QUEUE_FULL,
  INVALID,          ///< bad arguments
};
const char *write_status_name(WriteStatus s);
/// True for the outcomes after which the slave holds the written values.
inline bool write_succeeded(WriteStatus s) { return s == WriteStatus::OK; }

struct WriteOptions {
  /// 0 = auto (fn 6 for one register, fn 16 for more), FN_WRITE_SINGLE or FN_WRITE_MULTIPLE.
  uint8_t fn{0};
  /// Read the range back (fn 3) after the echo and compare.
  bool verify{false};
};

struct WriteResult {
  WriteStatus status{WriteStatus::OK};
  uint8_t exception{0};  ///< EXCEPTION / VERIFY_FAILED by exception: the slave's code
  uint8_t addr{0};
  uint8_t fn{0};         ///< function code that was sent
  uint16_t start{0};     ///< 1-based table address
  uint16_t count{0};
  /// Registers read back (verify; set on OK with verify and on VERIFY_MISMATCH).
  std::vector<uint16_t> readback;
};
using WriteCallback = std::function<void(const WriteResult &)>;

struct WriteStats {
  uint32_t ok{0};
  uint32_t failed{0};    ///< exception, timeout, verify mismatch/failure, offline at send time
  uint32_t rejected{0};  ///< refused by submit_write (offline, rate limit, queue full, invalid)
  uint32_t verify_mismatch{0};
};

class ModbusMaster;

class Device {
 public:
  enum class State : uint8_t { DISCOVER, RUN };
  struct Job {
    ReadRequest req;
    PollTier tier;         ///< configured refresh tier
    uint32_t interval_ms;  ///< period at that tier
    /// Slot offset as a fraction of the period (x / 65536): spreads the reads of one tier.
    uint16_t phase_frac;
    uint32_t next_due;     ///< next slot
    uint32_t sent_at;      ///< last send
    uint32_t last_ok;      ///< last answer with data (0 = never)
    bool pending;          ///< not sent since start / reconnect (initial pass)
    bool in_flight;
    /// The last answer held no valid value or was an exception: polled at the STATIC period.
    bool unserved;
    uint8_t last_exc;    ///< Modbus exception code of the last exception reply (0 = none yet)
    uint32_t exc_count;  ///< exception replies to this read since start
    /// Tier the scheduler uses now (STATIC while unserved).
    PollTier effective_tier() const { return this->unserved ? PollTier::STATIC : this->tier; }
  };
  /// The raw reply that identified the device: the inverter devinfo block or the child identity
  /// read (discovery.h). nregs = 0 until one was parsed.
  struct IdentRaw {
    const uint8_t *data;  ///< big-endian registers
    uint16_t nregs;
    uint16_t start;  ///< 1-based table address of the read
    uint8_t fn;
  };

  const DeviceConfig &config() const { return this->cfg_; }
  State state() const { return this->state_; }
  IdentRaw ident_raw() const {
    return {this->ident_raw_, this->ident_raw_regs_, this->ident_raw_start_, this->ident_raw_fn_};
  }
  /// Name of the active poll table, "" while discovering.
  const char *table_name() const { return this->table_ != nullptr ? this->table_->name : ""; }
  /// The active table is the configured fallback: the devinfo read was rejected or named a
  /// protocol type without a table.
  bool fallback_used() const { return this->fallback_used_; }
  bool online() const { return this->online_; }
  const LinkStats &stats() const { return this->stats_; }
  /// Active poll table (nullptr while discovering).
  const RegisterTable *table() const { return this->table_; }
  const std::vector<Job> &jobs() const { return this->jobs_; }
  /// The devinfo block was read and parsed.
  bool discovered() const { return this->discovered_; }
  /// DevInfo of the device. dev_type is set once a table is active (devinfo, YAML or table
  /// family); protocol comes from the devinfo block, or from the inverter table when the block
  /// was rejected or the table is fixed.
  const DevInfo &devinfo() const { return this->devinfo_; }
  /// Discovered, or a dev_type is known from YAML / the table family.
  bool identified() const { return this->discovered_ || this->devinfo_.dev_type != 0; }
  /// Where devinfo().dev_type comes from: "devinfo", "yaml", "table" or "none".
  const char *dev_type_source() const;
  /// Inverter type (holding 30644 / sig 0x1002); meaningful only when inv_type_known().
  uint16_t inv_type() const { return this->inv_type_; }
  /// 30644 answered with a value other than the 0xFFFF sentinel.
  bool inv_type_known() const { return this->inv_type_known_; }
  /// Grid-code XML family (stock tree on inverter type and protocol type). The inverter type
  /// counts only for protocol 1/4, the protocols whose table reads 30644 in stock; otherwise,
  /// and while unknown, it is 0.
  /// Grid-code XML family; NONE for anything that is not an inverter.
  XmlFamily xml_family() const;
  /// Parameter descriptor for the same (inverter type, protocol) pair; nullptr = none.
  const FamilyDescriptor *descriptor() const;
  bool gating() const { return this->gate_; }
  /// Initial pass complete (every read answered or timed out once) since start/reconnect.
  bool first_round_done() const { return this->first_round_done_; }
  /// When a read of this device's fastest tier last answered (ms clock; 0 = never): the age of
  /// its most frequently refreshed values.
  uint32_t fresh_ms() const { return this->fresh_ms_; }
  /// Answered reads of the fastest tier since start (a refresh counter).
  uint32_t fresh_count() const { return this->fresh_count_; }
  /// Not polled any more: a scan slot that never answered, or a remembered address the search
  /// replaced. Hidden from device lists.
  bool abandoned() const { return this->abandoned_; }

 protected:
  friend class ModbusMaster;
  bool is_inverter_() const;
  void keep_ident_raw_(uint8_t fn, uint16_t start, const uint8_t *data, uint16_t nregs);
  DeviceConfig cfg_;
  State state_{State::DISCOVER};
  bool online_{false};
  bool discovered_{false};
  bool gate_{false};
  const RegisterTable *table_{nullptr};
  std::vector<Job> jobs_;
  LinkStats stats_;
  DevInfo devinfo_;
  enum class DevTypeSource : uint8_t { NONE, DEVINFO, YAML, TABLE };
  DevTypeSource dev_type_src_{DevTypeSource::NONE};
  uint16_t inv_type_{0};
  bool inv_type_known_{false};
  /// Boot read of 30644 still to be sent (after a successful devinfo probe).
  bool inv_type_pending_{false};
  uint16_t effective_inv_type_() const;
  uint32_t next_probe_{0};
  // Scheduler state.
  uint32_t plan_gen_{0};       ///< bumped when the job list is rebuilt
  size_t pending_{0};          ///< reads still to send in the initial pass
  bool first_round_done_{false};
  bool fallback_used_{false};
  bool unsupported_reported_{false};  ///< MasterListener::on_unsupported fired for this device
  uint8_t ident_raw_[2 * DEVINFO_COUNT]{};
  uint16_t ident_raw_regs_{0};
  uint16_t ident_raw_start_{0};
  uint8_t ident_raw_fn_{0};
  uint32_t last_ok_ms_{0};     ///< last answered frame (offline_silence_ms)
  uint32_t fresh_ms_{0};
  uint32_t fresh_count_{0};
  // Bus-scan slot bookkeeping (only meaningful when cfg_.scan).
  uint8_t discover_fails_{0};
  bool abandoned_{false};
};

/// Notifications for the ESPHome layer (entities, logs).
class MasterListener {
 public:
  virtual ~MasterListener() = default;
  virtual void on_online_changed(const Device &/*dev*/) {}
  /// Discovery finished: devinfo parsed and 30644 read, or the fallback table activated.
  virtual void on_discovered(const Device &/*dev*/) {}
  /// inv_type() or xml_family() changed after on_discovered (sig 0x1002 from the poll).
  virtual void on_identity_changed(const Device &/*dev*/) {}
  /// The remembered inverter addresses changed (persist ModbusMaster::remembered()).
  virtual void on_remembered_changed() {}
  /// Discovery could not classify the device the way a known model is classified (once per
  /// device): the devinfo read was rejected or named a protocol type without a table, or a
  /// child identity read did not parse. `why` is a short reason without addresses.
  virtual void on_unsupported(const Device &/*dev*/, const char * /*why*/) {}
};

class ModbusMaster {
 public:
  using Clock = std::function<uint32_t()>;
  /// level: 1 error, 2 warn, 3 info, 5 debug, 6 verbose (ESPHome numbering).
  using LogFn = std::function<void(int level, const char *msg)>;

  void set_transport(Transport *t) { this->transport_ = t; }
  void set_store(SignalStore *s) { this->store_ = s; }
  void set_clock(Clock c) { this->clock_ = std::move(c); }
  void set_log(LogFn f) { this->log_fn_ = std::move(f); }
  void set_listener(MasterListener *l) { this->listener_ = l; }
  void set_timing(const MasterTiming &t) { this->timing_ = t; }
  const MasterTiming &timing() const { return this->timing_; }
  /// Period of a refresh tier (default DEFAULT_TIER_PERIOD_MS). Call before start().
  void set_tier_period(PollTier t, uint32_t ms) {
    this->tier_ms_[static_cast<size_t>(t)] = ms < 100 ? 100 : ms;
  }
  uint32_t tier_period(PollTier t) const { return this->tier_ms_[static_cast<size_t>(t)]; }
  /// Current period of a read (its tier's, or the STATIC period while it is unserved).
  uint32_t job_period(const Device::Job &j) const;
  /// Range probe: every address in [first, last] (1-based, <= 247) gets the devinfo read, each
  /// slot up to `attempts` misses before it is abandoned. With the search on it is only the
  /// fallback (search unanswered, no inverter online, remembered addresses asked and silent),
  /// once per boot and per rescan(); with the search off it runs at start.
  /// first == 0 disables it. Batteries, chargers and Energy Bridges are never probed this way:
  /// their addresses come from the inverter (check_children_()).
  void set_scan(uint8_t first, uint8_t last, uint8_t attempts) {
    this->scan_first_ = first;
    this->scan_last_ = last;
    this->scan_attempts_ = attempts < 1 ? 1 : attempts;
  }
  /// Broadcast inverter search (stock HOME search); off unless set (the YAML inverter_search
  /// option defaults to on).
  void set_search(bool on) { this->search_enabled_ = on; }
  /// Inverter addresses from an earlier boot; polled from the start. Call before start().
  void set_remembered(const std::vector<uint8_t> &addrs);
  /// Addresses of the inverters found by the search / range probe / remembered, oldest first.
  const std::vector<uint8_t> &remembered() const { return this->remembered_; }
  static constexpr uint16_t SEARCH_REG = 30035;  ///< 1-based; wire 0x7552
  static constexpr uint32_t SEARCH_TIMEOUT_MS = 1900;
  static constexpr uint8_t SEARCH_TRIES = 2;
  /// Search period while no inverter is online.
  static constexpr uint32_t SEARCH_REPEAT_MS = 60000;
  static constexpr size_t MAX_REMEMBERED = 8;
  /// Inverters the search can add after start().
  static constexpr size_t MAX_FOUND = 4;
  /// Add the children inverters report (check_children_()); on by default.
  void set_child_discovery(bool on) { this->child_discovery_ = on; }
  /// Child devices (batteries, charger, Energy Bridge) the master can add after start().
  static constexpr size_t MAX_CHILDREN = 10;
  /// Retry period of a reported child that did not answer its identity read.
  static constexpr uint32_t CHILD_RETRY_MS = 60000;
  /// Table given to every scan device when the devinfo read is rejected / unclassifiable, so a
  /// scanned device that answers but has no usable devinfo still polls (like a per-device
  /// fallback_table). nullptr = inv_proto1_4 (stock treats a rejected devinfo read as protocol 1).
  void set_scan_fallback(const RegisterTable *t) { this->scan_fallback_ = t; }
  /// Search again now, re-arm every not-yet-discovered scan slot and remembered address, and
  /// allow the range-probe fallback to run once more.
  void rescan();
  Device &add_device(const DeviceConfig &cfg);

  /// Prepare plans for fixed-table devices. Call once after configuration.
  void start();
  /// Advance the state machine. Call from loop(); returns quickly.
  void tick();

  /// Queue an ad-hoc read (served before cyclic polls). start is the 1-based table address.
  /// cb(ok, data, nregs): data = big-endian registers, valid only during the callback.
  /// Returns false if the queue is full or the arguments are invalid.
  bool read_registers(uint8_t addr, uint8_t fn, uint16_t start, uint16_t count, AdhocCallback cb);
  /// Queue a register write (start = 1-based table address, wire = start - 1). Returns QUEUED
  /// (cb follows exactly once) or the rejection (cb is not called). Writes go out before
  /// ad-hoc reads and cyclic polls. Callers gate what may be written (sungrow_control).
  WriteStatus submit_write(uint8_t addr, uint16_t start, const uint16_t *values, uint16_t count,
                           const WriteOptions &opt, WriteCallback cb);
  /// Whether a write to this slave would be accepted now (known, table active, online).
  bool can_write(uint8_t addr) const;
  const WriteStats &write_stats() const { return this->wstats_; }
  size_t writes_pending() const { return this->writes_.size(); }

  std::vector<Device> &devices() { return this->devices_; }
  const std::vector<Device> &devices() const { return this->devices_; }
  /// Stock DM device id (the cloud's devCode and the Modbus-TCP unit id): 1, 2, 3 … in the order
  /// devices are first identified or given a table (DM_AddDevice), skipping 247 (the logger). So
  /// the inverter found first is 1 and the SBR stacks it reports follow. 0 until the device is
  /// identified. Ids are kept for the rest of the boot.
  uint16_t device_id(uint8_t index) const;
  Device *find_device_by_address(uint8_t addr);
  /// Gap before a frame to device `d` (nullptr: unknown address) incl. the 3.5-char floor.
  uint32_t frame_gap(const Device *d) const;
  /// Expected bus time of a read of `count` registers to `d`: gap + request + reply + slack.
  uint32_t estimate_ms(const Device *d, uint8_t fn, uint16_t count) const;
  /// Ranges the inverter forwards to its DSP (16000..17500, 29500..29899, 36450..38999) or, on
  /// a battery, tunnels to the BCU over CAN (19101..20000): answered after up to ~1.5 s. The third
  /// DSP range is the widest case: the SH10RS forwards only 36702..38999 and the SG5.0RS none of it;
  /// reads it answers itself just get the longer timeout.
  static bool tunnel_range(sungrow_datamodel::Family f, uint16_t start, uint16_t count);
  bool idle() const { return this->state_ == TxState::IDLE && !this->retry_pending_ && !this->have_next_; }
  size_t adhoc_pending() const { return this->adhoc_.size(); }
  /// The bus benefits from ticks faster than the main loop right now: a reply is awaited, a retry
  /// is due after the frame gap, a write or an ad-hoc read is queued, or the post-write settle gap
  /// runs. SungrowModbus holds a high-frequency loop request while this is true (`fast_loop:`).
  bool wants_fast_loop() const;

  // ---- Exclusive bus (diagnostic file transfers, sungrow_capture) ---------------------------
  /// Take the bus: cyclic polls, the search, discovery and probes stop; queued writes and ad-hoc
  /// reads still go out (the owner's own Modbus steps). Released by release_exclusive() or, as a
  /// safety net, `max_ms` after the last acquire/extend. False if already held.
  bool acquire_exclusive(uint32_t max_ms);
  /// Move the automatic release to `max_ms` from now.
  void extend_exclusive(uint32_t max_ms);
  void release_exclusive();
  bool exclusive() const { return this->excl_; }
  /// Raw mode, only while exclusive and idle(): the master sends nothing and reads nothing, so
  /// the owner can run a non-Modbus exchange (YMODEM) on transport(). False if not allowed now.
  bool set_raw(bool on);
  bool raw() const { return this->raw_; }
  Transport *transport() const { return this->transport_; }

  static constexpr size_t ADHOC_QUEUE_MAX = 16;
  static constexpr size_t WRITE_QUEUE_MAX = 8;
  static constexpr size_t RATE_TABLE_MAX = 32;
  /// Non-FAST traffic waits for a gap before the next FAST slot at most this long.
  static constexpr uint32_t FAST_RESERVE_MAX_WAIT_MS = 2000;

 protected:
  enum class TxState : uint8_t { IDLE, WAIT };
  /// INVTYPE: the one fn 3 read of 30644 after a successful devinfo probe.
  /// SEARCH: the broadcast inverter search (slave 0, 30035).
  enum class Kind : uint8_t { POLL, DISCOVERY, PROBE, ADHOC, WRITE, VERIFY, INVTYPE, SEARCH };
  struct Txn {
    Kind kind{Kind::POLL};
    int dev{-1};       ///< index into devices_ (-1: ad-hoc to an unknown address)
    int job{-1};       ///< POLL/PROBE: job index
    uint8_t addr{1};
    uint8_t fn{4};
    uint16_t start{0}; ///< 1-based
    uint16_t count{0};
    uint8_t attempts{0};
    uint32_t plan_gen{0};   ///< POLL: Device::plan_gen_ when it was taken
    std::vector<uint16_t> write_values;  ///< WRITE: values sent; VERIFY: values expected
    AdhocCallback cb;
    WriteCallback wcb;   ///< WRITE / VERIFY
    bool verify{false};  ///< WRITE: queue a VERIFY after the echo
  };
  struct RateEntry {
    uint8_t addr;
    uint16_t reg;
    uint32_t t;
  };

  uint32_t now_() const { return this->clock_ ? this->clock_() : 0; }
  void log_(int level, const char *fmt, ...) const
#if defined(__GNUC__)
      __attribute__((format(printf, 3, 4)))
#endif
      ;
  uint32_t char_time_us_() const;
  bool pick_(Txn *out, uint32_t now);
  /// Discovery / offline probe / inverter-type read of device `di`, if one is due.
  bool control_candidate_(int di, uint32_t now, Txn *out);
  /// Fill `out` for job `ji` of device `di` and mark it in flight (next slot computed).
  void take_job_(int di, int ji, uint32_t now, Txn *out);
  /// Earliest slot of a FAST read of an online device not yet in flight; false if none.
  bool next_fast_due_(uint32_t *out) const;
  /// First slot of `j` strictly after `after`. Slots are phase + k * period on the ms clock.
  uint32_t next_slot_(const Device::Job &j, uint32_t after) const;
  /// The device polls (table active, online, not waiting for its inverter-type read).
  bool runnable_(const Device &d) const;
  /// The rows of the read hold at least one valid value (the fault list always counts).
  bool read_served_(const Device &d, const Device::Job &j) const;
  /// A read of `d` answered (ok) or failed: initial-pass bookkeeping, unserved flag, freshness.
  void job_done_(Device &d, int ji, uint32_t now, bool answered, bool exception);
  /// Restart the initial pass (start, reconnect).
  void restart_pass_(Device &d, uint32_t now);
  void send_(uint32_t now);
  void finish_ok_(uint32_t now);
  void finish_exception_(uint32_t now, uint8_t code);
  void finish_fail_(uint32_t now, bool crc);
  void attempt_failed_(uint32_t now, bool crc);
  void set_online_(Device &d, bool online, uint32_t now);
  void activate_table_(Device &d, const RegisterTable *t, uint32_t now);
  void inv_type_done_(Device &d, bool known, uint16_t value);
  void on_inv_type_signal_(size_t di, bool valid, uint64_t bits);
  void count_ok_(Device &d, uint32_t now);
  void count_fail_(Device &d, uint32_t now);
  const Device *find_device_(uint8_t addr) const;
  void finish_write_(Txn &t, WriteStatus st, uint8_t exception, const uint8_t *data, uint16_t nregs);
  /// Scan slot: a discovery probe got no answer; abandons the slot after scan_attempts_ misses.
  void scan_miss_(Device &d, uint32_t now);
  /// Add children the inverters report (battery address list 10451.., 5621 charger/EB bits).
  /// Runs from tick(), never while a Device reference is held (it appends to devices_).
  void check_children_(uint32_t now);
  void add_child_(size_t parent, uint8_t address, ChildKind kind, uint32_t now);
  /// Run a device whose devinfo read was rejected on its fallback table (stock: protocol 1).
  void use_fallback_table_(Device &d, uint32_t now);
  void report_unsupported_(Device &d, const char *why);
  /// Per-device start state (store subscription, timers); `di` indexes devices_.
  void init_device_(size_t di, uint32_t now);
  /// Append an auto-discovered inverter slot (table auto, inv_proto1_4 fallback).
  Device &add_inverter_slot_(uint8_t address, bool scan, bool remembered);
  /// Create (first time) or re-arm the fallback range probe.
  void arm_range_probe_(uint32_t now);
  void search_answered_(uint8_t address, uint32_t now);
  void search_failed_(uint32_t now);
  /// Arm the range probe if the search fallback is wanted, not yet spent, no inverter is online
  /// and every remembered address has been asked at least once.
  void maybe_range_fallback_(uint32_t now);
  /// An inverter answered at `found`: drop the remembered addresses that have not answered this
  /// boot (stock replaces its saved node).
  void forget_unanswered_remembered_(uint8_t found);
  /// Queue a search now (no-op when the search is off).
  void request_search_(uint32_t now);
  bool inverter_online_() const;
  /// An auto-discovered inverter answered: remember its address.
  void remember_(const Device &d);
  void write_done_(uint32_t now);

  Transport *transport_{nullptr};
  SignalStore *store_{nullptr};
  Clock clock_;
  LogFn log_fn_;
  MasterListener *listener_{nullptr};
  MasterTiming timing_;
  uint32_t tier_ms_[POLL_TIER_COUNT]{DEFAULT_TIER_PERIOD_MS[0], DEFAULT_TIER_PERIOD_MS[1], DEFAULT_TIER_PERIOD_MS[2],
                                     DEFAULT_TIER_PERIOD_MS[3]};
  /// Non-FAST traffic has been held back for the FAST reservation since blocked_since_.
  bool blocked_{false};
  uint32_t blocked_since_{0};
  // Range probe (0 = disabled); scan_attempts_ discovery misses abandon a slot.
  uint8_t scan_first_{0};
  uint8_t scan_last_{0};
  uint8_t scan_attempts_{2};
  bool range_created_{false};
  bool range_fallback_wanted_{false};  ///< the search went unanswered with no inverter online
  bool range_fallback_spent_{false};   ///< the fallback ran this boot / since the last rescan()
  // Broadcast search.
  bool search_enabled_{false};
  bool search_due_{false};
  bool cur_search_{false};  ///< the SEARCH transaction is queued or in flight
  uint32_t search_at_{0};
  uint8_t search_tries_{0};
  std::vector<uint8_t> remembered_;
  bool child_discovery_{true};
  bool child_check_due_{false};  ///< an inverter finished a poll round: re-read its child list
  const RegisterTable *scan_fallback_{nullptr};
  std::vector<Device> devices_;
  mutable std::vector<std::pair<uint8_t, uint16_t>> device_ids_;  ///< config index -> device_id()
  mutable uint16_t next_device_id_{1};
  std::deque<Txn> adhoc_;
  std::deque<Txn> writes_;  ///< WRITE / VERIFY, served first
  std::vector<RateEntry> rate_;
  WriteStats wstats_;
  uint32_t settle_until_{0};
  bool settle_active_{false};

  TxState state_{TxState::IDLE};
  Txn cur_;
  bool retry_pending_{false};
  ResponseParser parser_;
  uint8_t frame_[9 + 2 * MAX_WRITE_REGS];
  size_t frame_len_{0};
  uint32_t deadline_{0};
  uint32_t last_bus_end_{0};  ///< end of the previous transaction (reply / timeout)
  Txn next_;                  ///< picked, waiting for its gap
  bool have_next_{false};
  size_t rr_{0};
  bool started_{false};
  bool excl_{false};
  bool raw_{false};
  uint32_t excl_until_{0};
};

}  // namespace sungrow_modbus
}  // namespace esphome
