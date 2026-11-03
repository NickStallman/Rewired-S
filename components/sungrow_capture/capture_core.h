#pragma once
// Diagnostic captures from the inverter: I-V string scan, grid-waveform recording and the export
// of the stored fault recordings. Each one makes the inverter offer a file and fetches it with the
// inverter's YMODEM-1K sender, raw on the RS485 line. Pure C++ on top of the Modbus master core
// (no ESPHome headers), so tests/capture drives it with a fake inverter and a fake clock.
//
// I-V scan (both SG5.0RS and SH10RS; functionEnum sig 0x2173 bit 3 = the 32000/32005 set):
//   fn 16 doc 32000 = 0x01A0 (start; exception 04 = busy or the inverter's 1000-starts-per-boot
//   limit), poll fn 3 doc 32005 x5 every POLL_MS until success / failure, then hold the bus,
//   fn 16 doc 31999 = 0 (format flag), fn 16 doc 32000 = 0x00A0 (select VI_ALL), YMODEM.
// Grid waveform (SH10RS only; functionEnum bit 19):
//   read fn 4 doc 30079 (progress %), fn 16 doc 32010-32011 = [0xAA, cycles 1..8], poll 30079
//   until 100 after at least one reading below 100 (a stale 100 would export an old store; the
//   inverter never raises an exception for a bad start and never reports a failure value), then
//   hold the bus, fn 16 doc 32000 = 0x01B0 (select GridWaveRecord), YMODEM.
// Fault recordings (same inverters as the grid waveform): the inverter keeps up to 10 recordings,
//   one per trip, in the grid-waveform record format with N = 1 cycle (19 956 bytes each).
//   Nothing new is recorded; the stored ones are copied out:
//   read fn 4 doc 30079 (progress %), fn 16 doc 30034 = 0xAA (the control board re-reads its
//   record catalogue; only when idle), fn 16 doc 32000 = 0x01A1 (copy the records into the
//   control board's flash; exception 04 while the catalogue count is still 0, so it is repeated
//   every POLL_MS and given up after FAULT_START_MS), poll 30078 (record count) + 30079
//   (progress, the grid guards) until 100, hold the bus, fn 16 doc 32000 = 0x00B0 (select
//   FaultUpload_packet_01: 6-byte prefix = u32 count low word first, u16 record length, then
//   the records back to back), YMODEM. The copy takes about 20 s per record and the inverter's
//   readings freeze meanwhile.
// Normal polling continues while the inverter works; the bus is held only for the file transfer.
//
// YMODEM (inverter sender): we send 'C'; header block 0 (SOH, name NUL, decimal size NUL); data
// blocks STX 1024 / SOH 128 from block 1, wrapping at 256, CRC-16/XMODEM over the data; ACK each,
// NAK on error (the inverter resends up to 4 times); EOT, ACK. No closing empty header.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../sungrow_datamodel/signal_store.h"
#include "../sungrow_modbus/master.h"

namespace esphome {
namespace sungrow_capture {

/// CRC-16/XMODEM (poly 0x1021, init 0).
uint16_t crc16_xmodem(const uint8_t *data, size_t len);

class YmodemReceiver {
 public:
  enum class Status : uint8_t { IDLE, RUNNING, DONE, EMPTY, FAILED };
  static constexpr uint8_t SOH = 0x01, STX = 0x02, EOT = 0x04, ACK = 0x06, NAK = 0x15, CAN = 0x18, C = 0x43;
  /// No byte for this long: the transfer failed.
  static constexpr uint32_t INACTIVITY_MS = 30000;
  /// A block that stops arriving for this long is dropped and NAKed.
  static constexpr uint32_t BLOCK_GAP_MS = 1500;
  /// 'C' is repeated this often until the header starts (the inverter waits ~10 s for it).
  static constexpr uint32_t C_REPEAT_MS = 3000;
  static constexpr uint8_t C_TRIES = 3;
  /// Errors (NAKs) before the transfer is cancelled (stock: more than 9).
  static constexpr uint8_t MAX_ERRORS = 10;

  /// Start: queue the first 'C'. Files larger than max_size fail.
  void begin(uint32_t now, size_t max_size);
  /// Feed received bytes; bytes to send are appended to tx.
  void feed(const uint8_t *data, size_t n, uint32_t now, std::vector<uint8_t> *tx);
  /// Timeouts ('C' repeat, partial block, inactivity).
  void tick(uint32_t now, std::vector<uint8_t> *tx);

  Status status() const { return this->status_; }
  bool finished() const { return this->status_ == Status::DONE || this->status_ == Status::EMPTY || this->status_ == Status::FAILED; }
  /// The header block arrived (file name and size known).
  bool have_header() const { return this->have_header_; }
  const std::string &file_name() const { return this->name_; }
  /// Size from the header (bytes without padding).
  uint32_t announced_size() const { return this->size_; }
  /// File content, trimmed to the announced size on DONE.
  std::vector<uint8_t> &data() { return this->data_; }
  size_t received() const { return this->data_.size(); }
  const std::string &error() const { return this->error_; }
  uint32_t blocks() const { return this->blocks_; }
  uint32_t naks() const { return this->naks_; }
  uint32_t duplicates() const { return this->dups_; }
  uint32_t junk_bytes() const { return this->junk_; }

 protected:
  void fail_(const char *why, std::vector<uint8_t> *tx);
  void nak_(const char *why, std::vector<uint8_t> *tx);
  void block_(std::vector<uint8_t> *tx);
  Status status_{Status::IDLE};
  size_t max_size_{0};
  std::vector<uint8_t> buf_;  ///< block being assembled
  size_t want_{0};            ///< full length of that block (133 / 1029)
  bool have_header_{false};
  std::string name_;
  uint32_t size_{0};
  uint8_t next_seq_{1};
  std::vector<uint8_t> data_;
  std::string error_;
  uint32_t last_rx_{0};
  uint32_t c_at_{0};
  uint8_t c_sent_{0};
  uint8_t errors_{0};
  uint32_t blocks_{0}, naks_{0}, dups_{0}, junk_{0};
};

enum class Kind : uint8_t { IV, GRID, FAULT };
/// "iv", "grid", "fault".
const char *kind_name(Kind k);

/// One finished (or failed) capture.
struct Capture {
  Kind kind{Kind::IV};
  uint32_t id{0};           ///< 1, 2, ... per boot
  bool ok{false};           ///< a file was received
  std::string result;       ///< "ok", "empty", or the failure text
  uint8_t cycles{0};        ///< GRID: cycle count sent
  uint16_t records{0};      ///< GRID / FAULT: records in the received file (its prefix count; GRID = 1)
  int64_t wall_start{0};    ///< unix time at the start (0 = clock not set)
  uint32_t started_ms{0};
  uint32_t finished_ms{0};
  uint32_t transfer_ms{0};  ///< YMODEM duration
  uint32_t scan_ms{0};      ///< IV: ms from the first SCANNING poll to the last state poll
  std::string inverter;     ///< "SH10RS SN ..." as given by the ESPHome layer
  std::string file_name;    ///< name in the YMODEM header
  uint32_t announced_size{0};
  std::vector<uint8_t> data;
  uint32_t blocks{0}, naks{0}, duplicates{0}, junk_bytes{0};
  /// IV: last state block {state lo, state hi, upload lo, upload hi, fault}; GRID: [0] = progress;
  /// FAULT: [0] = progress, [1] = record count (doc 30078).
  uint16_t last_regs[5]{};
  uint16_t fault{0};        ///< IV: fault word (doc 32009)
  /// "t_ms text" lines: every poll answer and step, relative to started_ms.
  std::vector<std::string> timeline;
  /// JSON objects (signal snapshot of the inverter) taken before the start and after the scan.
  std::string snapshot_before;
  std::string snapshot_after;
};

class CaptureCore {
 public:
  using Clock = std::function<uint32_t()>;
  using WallClock = std::function<int64_t()>;
  using LogFn = std::function<void(int level, const char *msg)>;
  /// Gated diagnostic write (ControlCore::write_diag). Returns false with `why` set if refused at
  /// once; otherwise done(ok, exception) follows exactly once.
  using DoneFn = std::function<void(bool ok, uint8_t exception)>;
  using WriteFn = std::function<bool(uint16_t start, const uint16_t *values, uint16_t count, DoneFn done,
                                     std::string *why)>;

  static constexpr uint32_t POLL_MS = 1000;
  static constexpr uint32_t IV_TIMEOUT_MS = 180000;    ///< stock per-device limit
  static constexpr uint32_t IV_START_GRACE_MS = 10000;  ///< idle state after the start for this long: not started
  static constexpr uint32_t IV_MIN_INTERVAL_MS = 120000;
  static constexpr uint32_t GRID_STALL_MS = 240000;    ///< progress unchanged this long: failed
  static constexpr uint32_t GRID_TIMEOUT_MS = 600000;
  static constexpr uint32_t GRID_STALE_MS = 15000;     ///< still 100 this long after the start: not started
  static constexpr uint32_t FAULT_START_MS = 30000;    ///< fetch refused (no records) this long: give up
  static constexpr uint8_t MAX_READ_FAILS = 10;
  /// Bus hold limit until the YMODEM header arrives; then size / BYTES_PER_S + HOLD_SLACK_MS.
  static constexpr uint32_t HOLD_PRE_MS = 60000;
  static constexpr uint32_t BYTES_PER_S = 900;
  static constexpr uint32_t HOLD_SLACK_MS = 30000;
  static constexpr size_t IV_MAX_SIZE = 64 * 1024;
  static constexpr size_t GRID_MAX_SIZE = 256 * 1024 + 64;
  static constexpr size_t FAULT_MAX_SIZE = GRID_MAX_SIZE;
  static constexpr size_t GRID_KEEP = 4;
  static constexpr size_t FAULT_KEEP = 1;
  static constexpr size_t TIMELINE_MAX = 800;

  void set_master(sungrow_modbus::ModbusMaster *m) { this->master_ = m; }
  void set_store(sungrow_datamodel::SignalStore *s) { this->store_ = s; }
  void set_clock(Clock c) { this->clock_ = std::move(c); }
  void set_wall_clock(WallClock c) { this->wall_ = std::move(c); }
  void set_log(LogFn f) { this->log_fn_ = std::move(f); }
  void set_writer(WriteFn w) { this->writer_ = std::move(w); }
  /// Large (grid) captures need PSRAM; the ESPHome layer says whether it is there.
  void set_large_ok(bool ok) { this->large_ok_ = ok; }

  /// Start a capture on the inverter at store index `device` / slave `addr`. `label` names the
  /// inverter in the result. False with `why` if refused (busy, offline, unsupported...).
  bool start_iv(uint8_t device, uint8_t addr, const std::string &label, std::string *why);
  bool start_grid(uint8_t device, uint8_t addr, uint8_t cycles, const std::string &label, std::string *why);
  bool start_fault(uint8_t device, uint8_t addr, const std::string &label, std::string *why);
  /// Ask a running capture to stop (releases the bus; the inverter finishes on its own).
  void cancel();
  void tick();

  /// I-V supported (functionEnum bit 3) / grid waveform and fault recordings supported (bit 19);
  /// false while unknown.
  bool iv_supported(uint8_t device) const;
  bool grid_supported(uint8_t device) const;
  bool fault_supported(uint8_t device) const { return this->grid_supported(device); }
  /// Number of PV inputs the store knows for `device`: valid MPPT voltage signals 0x2040 + 2 i,
  /// i < 12.
  uint8_t mppt_count(uint8_t device) const;
  bool running() const { return this->step_ != Step::IDLE; }
  Kind running_kind() const { return this->cur_.kind; }
  /// Running I-V scan: ms since the first poll that reported SCANNING (0 before and otherwise).
  uint32_t scan_ms() const;
  /// Running I-V scan: fault word of the latest state poll (0 when none).
  uint16_t iv_fault() const;
  /// Text for an I-V scan refusal / abort code; nullptr for unknown codes.
  static const char *iv_fault_text(uint16_t code);
  /// Human step text ("scanning", "recording 40 %", "transfer 12/40 KiB", ...).
  std::string step_text() const;
  /// 0..100 where it is known, -1 otherwise.
  int progress() const;
  uint32_t elapsed_ms() const;
  /// Bumped whenever a capture finishes (the stored results changed).
  uint32_t generation() const { return this->gen_; }
  /// Finished captures, newest last: at most one I-V, GRID_KEEP grid and FAULT_KEEP fault captures.
  const std::deque<std::shared_ptr<const Capture>> &results() const { return this->results_; }
  std::shared_ptr<const Capture> find(uint32_t id) const;
  uint32_t iv_starts() const { return this->iv_starts_; }

  /// Capture metadata as JSON (everything but the file bytes).
  static std::string metadata_json(const Capture &c);
  /// Snapshot of every valid numeric signal of `device` as a JSON object
  /// {"0x2080":{"v":-2.5,"fn":4,"reg":5031},...}; register from the device's poll table.
  std::string snapshot(uint8_t device) const;

 protected:
  enum class Step : uint8_t {
    IDLE,
    IV_START,     ///< start write in flight
    IV_POLL,      ///< polling the state block
    GRID_PRE,     ///< progress read before the start
    GRID_START,   ///< start write in flight
    GRID_POLL,    ///< polling progress
    FAULT_PRE,    ///< progress read before the start
    FAULT_SCAN,   ///< catalogue re-read write in flight
    FAULT_FETCH,  ///< fetch write due or in flight (repeated while refused)
    FAULT_POLL,   ///< polling record count and progress
    HOLD,         ///< waiting for the bus to be ours and idle
    SELECT_FMT,   ///< format-flag write in flight (I-V)
    SELECT_FILE,  ///< format flag written (I-V): file-select write to send
    SELECT,       ///< file-select write in flight
    RAW_ENTER,    ///< waiting for the master to go idle before raw mode
    TRANSFER,     ///< YMODEM
  };
  uint32_t now_() const { return this->clock_ ? this->clock_() : 0; }
  void log_(int level, const char *fmt, ...) const
#if defined(__GNUC__)
      __attribute__((format(printf, 3, 4)))
#endif
      ;
  void note_(const char *fmt, ...)
#if defined(__GNUC__)
      __attribute__((format(printf, 2, 3)))
#endif
      ;
  bool precheck_(uint8_t device, uint8_t addr, std::string *why) const;
  void begin_(Kind k, uint8_t device, uint8_t addr, const std::string &label);
  void finish_(bool ok, const std::string &result);
  bool send_write_(uint16_t start, const uint16_t *v, uint16_t n, Step next_ok);
  void poll_iv_(uint32_t now);
  void on_iv_state_(const uint8_t *data, uint16_t nregs);
  void poll_progress_(uint32_t now);
  void on_progress_(const uint8_t *data, uint16_t nregs);
  void fetch_fault_(uint32_t now);
  size_t max_size_() const;
  void read_failed_();
  void select_();
  void transfer_(uint32_t now);

  sungrow_modbus::ModbusMaster *master_{nullptr};
  sungrow_datamodel::SignalStore *store_{nullptr};
  Clock clock_;
  WallClock wall_;
  LogFn log_fn_;
  WriteFn writer_;
  bool large_ok_{true};

  Step step_{Step::IDLE};
  Capture cur_;
  uint8_t dev_{0};
  uint8_t addr_{1};
  uint32_t step_at_{0};      ///< when the current step started
  uint32_t next_poll_{0};
  bool read_in_flight_{false};
  bool write_in_flight_{false};
  uint8_t read_fails_{0};
  bool cancel_{false};
  // I-V
  bool iv_seen_scanning_{false};
  uint32_t iv_scanning_at_{0};
  uint32_t last_iv_start_{0};
  bool have_iv_start_{false};
  uint32_t iv_starts_{0};
  // Grid / fault progress
  int grid_progress_{-1};
  bool grid_seen_below_{false};
  uint32_t grid_changed_at_{0};
  uint16_t fault_count_{0};
  // Transfer
  YmodemReceiver ym_;
  uint32_t transfer_at_{0};
  bool hold_sized_{false};
  // Results
  std::deque<std::shared_ptr<const Capture>> results_;
  uint32_t next_id_{1};
  uint32_t gen_{0};
};

}  // namespace sungrow_capture
}  // namespace esphome
