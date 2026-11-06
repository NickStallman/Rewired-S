#pragma once
// sungrow_control core: the write safety gate in front of the RS485 write queue
// (sungrow_modbus::ModbusMaster::submit_write), the fn 3 read-back poller that feeds the
// select / number / sensor entities from real inverter values, and the Setup / Installer batch
// path (family-scoped protection writes, setup profiles, on-demand reads). Pure C++ (no ESPHome
// headers), host-tested in tests/control/.
//
// Safety model (docs/control.md):
//  1. Global enable flag (the restored `writes_enabled` switch, default OFF). It gates write()
//     for unauthorised callers: MQTT, the Modbus-TCP :502 server, the HA action and the native
//     entities. Off -> DISABLED. The web interface (/api/write, /api/setup_write,
//     /api/setup_apply, /api/setup_start) calls with `authorized` = true: the digest login (and
//     the confirm token of the setup endpoints) authorises those writes and the switch does not
//     apply; every check below still does.
//  2. Register allow-list per device: the curated profile (data/write_registers.yaml, register
//     per protocol type) plus YAML extras {address, count, min, max}. Every register of a
//     request must be listed, else NOT_ALLOWED (Modbus exception 2 on :502). Setup / Installer
//     batches may also write the protection registers of the device's own grid-code XML family
//     (installer_allow.gen.h); family none / unit-without-descriptor allows none.
//  3. Value checks: raw min/max and, for enumerations, the exact value set. Installer batches
//     also check the stock parameter ranges and relations (installer_rules.gen.h). Else
//     BAD_VALUE (exception 3).
//  4. Device must be known and online (the master re-checks before sending). Else OFFLINE.
//  5. Rate limit + queue size from the master (BUSY, exception 6).
//  6. Every accepted write and its outcome are logged at INFO.
// The :502 server additionally applies its own source-IP allow-list.
// Follow-ups (docs/control.md): an accepted start/stop command is tracked against the device's
// running state until it took effect or timed out (ExpectStatus); a string inverter's power limit
// is re-asserted after a power-stage restart (PowerLimitMemo); Setup / Installer values outside a
// listed model's power-stage acceptance range (dsp_ranges.gen.h) are refused or skipped.
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../sungrow_datamodel/signal_store.h"
#include "../sungrow_modbus/discovery.h"
#include "../sungrow_modbus/master.h"
#include "dsp_ranges.gen.h"
#include "installer_allow.gen.h"
#include "installer_rules.gen.h"
#include "setup_profile.h"
#include "write_registers.h"

namespace esphome {
namespace sungrow_control {

using sungrow_modbus::FamilyDescriptor;
using sungrow_modbus::ModbusMaster;
using sungrow_modbus::WriteResult;
using sungrow_modbus::WriteStatus;
using sungrow_modbus::XmlFamily;

/// One allow-list entry: `count` consecutive registers from `address` (1-based table address),
/// each raw value (u16 or s16 per `type`) in [min, max] and, if `values` is non-empty, one of them.
struct Rule {
  uint16_t address{0};
  uint16_t count{1};
  uint8_t type{sungrow_datamodel::TYPE_U16};
  int32_t min{0};
  int32_t max{0xFFFF};
  std::vector<uint16_t> values;
  const char *key{""};
  uint16_t sigid{0};  ///< read-back signal of a profile rule; 0 for YAML extras
};

/// Rules of a generated profile with the addresses for protocol type `protocol` (0 = unknown:
/// the default addresses).
std::vector<Rule> profile_rules(const ProfileDef &p, uint8_t protocol = 0);

enum class Verdict : uint8_t {
  OK,
  DISABLED,          ///< writes_enabled is off
  NOT_ALLOWED,       ///< a register outside the allow-list
  BAD_VALUE,         ///< a value outside its range / value set / relation
  OFFLINE,           ///< device unknown, undiscovered or offline
  BUSY,              ///< rate limited, queue full, or a setup batch in progress
  INVALID,           ///< malformed request (count 0, > 123, wraps, duplicates)
  PREVIEW_REQUIRED,  ///< setup apply without a fresh prepare of the same (device, option)
};
/// Human text ("writes disabled", ...).
const char *verdict_name(Verdict v);
/// Stable code ("OK", "DISABLED", "NOT_ALLOWED", ..., "PREVIEW_REQUIRED").
const char *verdict_code(Verdict v);

struct Check {
  Verdict verdict{Verdict::OK};
  uint16_t reg{0};      ///< first offending register (NOT_ALLOWED / BAD_VALUE)
  const char *key{""};  ///< rule key of that register (BAD_VALUE)
};

/// Allow-list + range check of a write of `count` registers at `start` (1-based).
Check check_rules(const std::vector<Rule> &rules, uint16_t start, const uint16_t *values, uint16_t count);

/// Modbus exception for a rejected :502 write (docs/control.md "Path A").
uint8_t exception_for(Verdict v);
/// Modbus exception for a failed RS485 write (EX 0 = success).
uint8_t exception_for(const WriteResult &r);

// ---- value mapping for the select / number entities (pure) ------------------------------------
struct Option {
  uint16_t raw;
  const char *text;
};
/// Option text -> raw; false if unknown.
bool option_to_raw(const std::vector<Option> &opts, const std::string &text, uint16_t *raw);
/// Raw -> option text; nullptr if unknown.
const char *raw_to_option(const std::vector<Option> &opts, uint16_t raw);
/// Display value -> raw register (round(value / scale)), range-checked against [min, max].
bool number_to_raw(float value, float scale, uint8_t type, int32_t min, int32_t max, uint16_t *raw);
/// Raw register -> display value.
float raw_to_number(uint16_t raw, uint8_t type, float scale);

/// Why an on-demand read value is (not) valid.
enum class ReadReason : uint8_t { OK = 0, OFF_LIST = 1, EXCEPTION = 2, TIMEOUT = 3, PENDING = 4 };
/// "ok", "off-list", "exception", "timeout", "pending".
const char *read_reason_name(ReadReason r);
/// Setup frame status for the JSON: "pending", "ok", "exception", "timeout", "expired", ...
const char *frame_status_name(WriteStatus s, bool expired);

/// Identity of a device: its grid-code XML family and parameter descriptor, and the devinfo
/// device code, type and model string (power-stage ranges, model family).
struct DeviceIdentity {
  bool known{false};  ///< the device index exists in the master
  XmlFamily family{XmlFamily::NONE};
  const FamilyDescriptor *desc{nullptr};
  uint16_t dev_code{0};  ///< devinfo device code (0 = unknown)
  uint8_t dev_type{0};   ///< stock dev_type (0 = unknown)
  char model[17]{};      ///< devinfo model string ("" = unknown)
};

// ---- start/stop follow-up ---------------------------------------------------------------------
enum class ExpectKind : uint8_t { NONE, STOP, START };
/// Progress of the start/stop command last written to a device.
struct ExpectStatus {
  ExpectKind kind{ExpectKind::NONE};
  const char *key{""};  ///< rule key of the start/stop register ("start_stop", "hybrid_start_stop", "power_switch")
  std::string text;     ///< "stop pending: output ramping", "started", ...
  bool final{false};    ///< the outcome is decided
  bool ok{false};       ///< final: the command took effect
  uint32_t age_ms{0};   ///< since the command was written (pending) or since the outcome (final)
  bool state_valid{false};
  uint16_t state{0};  ///< last running-state code seen (SIG_DEVICE_STATE)
};

// ---- the gate + read-back poller ------------------------------------------------------------
class ControlCore {
 public:
  using Clock = std::function<uint32_t()>;
  using LogFn = std::function<void(int level, const char *msg)>;
  /// Final outcome of an accepted write (exactly once).
  using Callback = std::function<void(const WriteResult &)>;
  /// Every outcome incl. immediate rejections (entities: last-write text, stats).
  using ResultListener = std::function<void(uint8_t device, const char *text, bool ok)>;
  /// Every change of a device's start/stop follow-up text.
  using ExpectListener = std::function<void(uint8_t device, const ExpectStatus &status)>;
  /// Family / descriptor of a device; the default asks the master's device list.
  using FamilyFn = std::function<DeviceIdentity(uint8_t device)>;
  /// Called after a setup frame starting at the device's country register was written OK.
  using CountryWrittenFn = std::function<void(uint16_t country)>;

  static constexpr uint32_t SETUP_DEADLINE_MS = 60000;  ///< batch accept -> remaining frames dropped
  static constexpr uint32_t PREPARE_TTL_MS = 60000;     ///< a prepare is usable this long
  static constexpr size_t SETUP_MAX_REGS = 128;
  static constexpr uint16_t READ_RUN_MAX = 64;

  struct DeviceCfg {
    uint8_t index{0};  ///< SignalStore device index (= sungrow_modbus device index)
    const ProfileDef *profile{nullptr};
    std::vector<Rule> rules;  ///< profile rules + YAML extras
    bool readback{true};
    uint8_t protocol{0};  ///< protocol type the profile addresses are bound to (0 = default)
    /// Re-write the last accepted power limit when the power stage restarts (string profile).
    bool reassert_power_limit{true};
  };

  void set_master(ModbusMaster *m) { this->master_ = m; }
  void set_store(sungrow_datamodel::SignalStore *s) { this->store_ = s; }
  void set_clock(Clock c) { this->clock_ = std::move(c); }
  void set_log(LogFn f) { this->log_fn_ = std::move(f); }
  void set_result_listener(ResultListener l) { this->listener_ = std::move(l); }
  void add_expect_listener(ExpectListener l) { this->expect_listeners_.push_back(std::move(l)); }
  void set_family_fn(FamilyFn f) { this->family_fn_ = std::move(f); }
  void set_country_written_cb(CountryWrittenFn f) { this->country_cb_ = std::move(f); }
  void add_device(DeviceCfg cfg);
  DeviceCfg *device(uint8_t index);
  /// Re-point the profile rules and read-back of `index` to protocol type `protocol`.
  void bind_protocol(uint8_t index, uint8_t protocol);
  /// Address of the profile rule with read-back signal `sigid` on `device`, else `fallback`.
  uint16_t address_for_sigid(uint8_t device, uint16_t sigid, uint16_t fallback) const;
  /// Address of the rule `key` ("start_stop", "ems_mode", ...) on `device`, bound to its protocol
  /// type; false when the device has no control entry or no such rule.
  bool address_for_key(uint8_t device, const char *key, uint16_t *address) const;
  /// Name of the control profile attached to `device` ("hybrid", "string"); nullptr without one.
  const char *profile_name(uint8_t device) const;
  DeviceIdentity identity(uint8_t device) const;

  void set_enabled(bool on);
  bool enabled() const { return this->enabled_; }

  /// Gate a write without sending it (the :502 server uses it before forwarding).
  Check check(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count) const;
  /// Gate and queue a write. Returns OK if queued (cb follows exactly once), else the verdict
  /// (cb not called). source: "api", "select", "number", "mbtcp 192.0.2.10", ...
  /// opt.fn: 0 = auto (fn 6 for one register), or the :502 client's function code.
  /// `authorized` (web interface) skips the writes_enabled gate; allow-list, value and online
  /// checks still apply.
  Verdict write(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count, bool verify,
                const char *source, Callback cb, sungrow_modbus::WriteOptions opt = {}, bool authorized = false);
  /// Inverter clock: one fn 16 of doc 5000-5005 = year (four digits), month, day, hour, minute,
  /// second, to the device's own address. The clock is in no allow-list, so only this call can
  /// write it; the value ranges are the inverter's own (year 2000-2099, month 1-12, day 1-31,
  /// hour 0-23, minute and second 0-59). The clock is not a system parameter, so the writes-enabled
  /// switch does not gate it; the device must have a control profile (inverters only) and be
  /// online. No read-back: the seconds have moved on by then; the echo confirms the write.
  static constexpr uint16_t CLOCK_ADDRESS = 5000;
  Verdict write_clock(uint8_t device, const uint16_t fields[6], const char *source, Callback cb);
  /// Diagnostic capture triggers (sungrow_capture, started from the web UI with a confirm token),
  /// one fn 16 to the device's own address. Only these registers and values pass:
  ///   doc 30034 = 0xAA                      re-read the stored fault-recording catalogue
  ///   doc 31999 = 0                         export format flag (text records)
  ///   doc 32000 = 0x01A0 / 0x00A0 / 0x01B0  I-V scan start / I-V file select / grid-wave file select
  ///   doc 32000 = 0x01A1 / 0x00B0           fault-recording fetch / fault-recording file select
  ///   doc 32010 = 0xAA, doc 32011 = 1..8    grid-wave recording start [arm, cycle count]
  /// They are in no allow-list, so cloud / MQTT / :502 never reach them. The writes-enabled
  /// switch does not gate them (the web confirmation is the authorisation); the device must have
  /// a control profile (inverters only) and be online. No read-back.
  static constexpr uint16_t DIAG_FAULT_RECORD_REG = 30034;
  static constexpr uint16_t DIAG_FORMAT_REG = 31999;
  static constexpr uint16_t DIAG_COMMAND_REG = 32000;
  static constexpr uint16_t DIAG_GRID_WAVE_REG = 32010;
  Verdict write_diag(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count, const char *source,
                     Callback cb);
  /// Read-back polling, start/stop follow-up, power-limit re-assert, setup batch drain,
  /// on-demand reads; call from loop().
  void tick();
  /// Ask for a read-back of the blocks overlapping [start, start + count) as soon as possible.
  void refresh(uint8_t device, uint16_t start, uint16_t count);

  // ---- start/stop follow-up ------------------------------------------------------------------
  // An accepted 0xCE / 0xCF to the device's start/stop register (its profile rule, or the
  // descriptor's power switch for setup_start) starts an expectation that tick() evaluates
  // against the running state (SIG_DEVICE_STATE, classified by state_class()):
  //   STOP   "stop pending: output ramping" until a stopped-class state ("stopped"), or
  //          "stop pending: time-out (state 0xXXXX)" after EXPECT_STOP_TIMEOUT_MS.
  //   START  one fn 3 read of the reconnection delay (RECONNECT_DELAY_REG, clamped to
  //          RECONNECT_DELAY_MIN_S..MAX_S, DEFAULT_S until it answers) sets the deadline; texts
  //          "start pending: reconnection delay (N s)" / "start pending: standby" /
  //          "start pending: starting"; a running-class state ends it ("started"), a fault
  //          ("start failed: fault (state ...)"). At the deadline: standby -> "start accepted:
  //          standby (low PV)"; starting / transient -> the deadline extends by
  //          EXPECT_START_EXTEND_MS up to EXPECT_START_MAX_EXTENSIONS times ("start pending:
  //          reconnection delay extended (grid trip k/3)"), never past EXPECT_START_HARD_MS;
  //          still stopped -> "start failed: still stopped (state ...)", on a string-family
  //          model "start failed: still stopped (key stop or rapid shutdown)"; else
  //          "start time-out (state ...)".
  // A device going offline cancels the expectation. Every text change goes to the result
  // listener (as "dev N <text>") and the expect listeners. The status stays readable after the
  // outcome until the next start/stop command.
  static constexpr uint32_t EXPECT_STOP_TIMEOUT_MS = 15000;
  static constexpr uint32_t EXPECT_START_GRACE_MS = 15000;  ///< added to the reconnection delay
  static constexpr uint32_t EXPECT_START_EXTEND_MS = 600000;
  static constexpr uint8_t EXPECT_START_MAX_EXTENSIONS = 3;
  static constexpr uint32_t EXPECT_START_HARD_MS = 3600000;
  static constexpr uint16_t RECONNECT_DELAY_REG = 30091;
  static constexpr uint16_t RECONNECT_DELAY_MIN_S = 10;
  static constexpr uint16_t RECONNECT_DELAY_MAX_S = 900;
  static constexpr uint16_t RECONNECT_DELAY_DEFAULT_S = 60;
  /// Follow-up of the last start/stop command of `device`; false when none was written.
  bool expect_status(uint8_t device, ExpectStatus *out) const;

  // ---- power-limit re-assert ---------------------------------------------------------------------
  // The last accepted write to the profile's power-limit rules (SIG_POWER_LIMIT_SWITCH and
  // SIG_POWER_LIMIT_PCT, adjacent registers) is remembered. When the device comes back online or
  // its running state drops from a running class to STATE_POWER_STAGE_INITIAL, tick() waits for
  // a read-back of the limit setting newer than that moment: equal -> nothing (debug log); else
  // one fn 16 of [switch, setting] with verify, source "reassert(<original source>)", INFO log,
  // provided writes are enabled or the original write was web-authorised and the last re-assert
  // is at least REASSERT_MIN_INTERVAL_MS ago; otherwise one WARN. DeviceCfg::reassert_power_limit
  // = false turns it off; a device without read-back never has a fresh value and never re-asserts.
  struct PowerLimitMemo {
    bool valid{false};
    uint16_t sw{0};
    uint16_t pct{0};
    bool authorized{false};  ///< the original write was web-authorised
    std::string source;      ///< source of the original write
    uint32_t written_ms{0};
    uint32_t last_reassert_ms{0};
    uint32_t due_since_ms{0};  ///< trigger time while `due`
    bool due{false};
    uint32_t count{0};  ///< re-asserts sent
  };
  static constexpr uint32_t REASSERT_MIN_INTERVAL_MS = 600000;
  /// Running-state code the power stage reports right after a restart (its RAM re-initialised).
  static constexpr uint16_t STATE_POWER_STAGE_INITIAL = 0x1200;
  /// The remembered power limit of `device`, nullptr for a device without a control entry.
  const PowerLimitMemo *power_limit_memo(uint8_t device) const;

  // ---- grid-code (protection) registers ------------------------------------------------------
  /// Every register of the write is in the installer allow-list of the device's XML family.
  Check check_grid_code(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count) const;

  // ---- Setup / Installer batch apply ----------------------------------------------------------
  // One confirmation (web confirm token, consumed in spa_handler) authorises a whole batch. The
  // batch is validated here, coalesced into contiguous same-class fn 16 frames (routine
  // allow-list registers through write(), protection registers through the family allow-list)
  // and drained into the master's write queue a frame at a time from tick(), each frame with a
  // read-back verify. Installer path (profile = false): family allow-list, stock ranges and
  // relations (over the batch values on top of this device's grid-read cache), commit
  // registers (descriptor country / grid code, 30036, 30037) first. Profile path (profile =
  // true): family allow-list only, items in the given order (stock writes the XML verbatim).
  // On a model with known power-stage ranges (dsp_ranges.gen.h, by devinfo device code) a
  // protection value outside its range is refused on the installer path (BAD_VALUE
  // "dsp_range:ADDR:MIN-MAX") and skipped on the profile path (WARN log, listed in
  // setup_note()). An offline or unknown device is rejected (OFFLINE). Frames still queued
  // SETUP_DEADLINE_MS after acceptance are dropped as failed.
  struct SetupReg {
    uint16_t address;  ///< 1-based document holding register
    uint16_t value;    ///< raw register value (U32 settings are two SetupRegs, low word first)
  };
  struct SetupFrameResult {
    uint16_t start;
    uint16_t count;
    WriteStatus status;  ///< QUEUED while pending
    uint8_t exception;   ///< Modbus exception code (EXCEPTION / VERIFY_FAILED)
    bool expired;        ///< dropped unsent at the deadline
  };
  /// Validate + queue a batch. Returns OK if accepted (drains asynchronously), else the first
  /// offending verdict (nothing is queued; setup_detail() says why). A batch in progress -> BUSY.
  Verdict setup_apply(uint8_t device, const std::vector<SetupReg> &regs, const char *source, bool profile = false);
  /// Why the last setup call was rejected: "register 30646", "range:32323", "relation:32319"
  /// (that peer's value is unknown), "relation:32323<=32327" (rule violated), "" when accepted.
  const std::string &setup_detail() const { return this->setup_detail_; }
  /// Items the accepted profile batch skipped ("32550=1300 outside the power-stage range
  /// 1000-1200"; "; "-separated), "" when none.
  const std::string &setup_note() const { return this->setup_note_; }
  /// The note a profile apply of `regs` on `device` would produce (the preview shows it).
  std::string setup_plan_note(uint8_t device, const std::vector<SetupReg> &regs) const;
  bool setup_busy() const { return !this->setup_queue_.empty() || this->setup_inflight_; }
  uint8_t setup_device() const { return this->setup_device_; }
  uint16_t setup_total() const { return this->setup_total_; }
  uint16_t setup_done() const { return this->setup_ok_ + this->setup_failed_n_; }
  uint16_t setup_ok() const { return this->setup_ok_; }
  uint16_t setup_failed_n() const { return this->setup_failed_n_; }
  /// ms left until the batch deadline (0 when idle).
  uint32_t setup_deadline_left() const;
  /// Per-frame results of the current / last batch, in send order.
  const std::vector<SetupFrameResult> &setup_frames() const { return this->setup_results_; }
  /// Frames (start, count) a profile batch would be sent as (same merge rule and power-stage
  /// range skips as setup_apply).
  std::vector<SetupFrameResult> plan_frames(uint8_t device, const std::vector<SetupReg> &regs) const;

  // ---- on-demand reads ------------------------------------------------------------------------
  struct GridReadVal {
    uint16_t address;
    uint16_t value;
    bool valid;
    ReadReason reason;
    uint8_t fn;  ///< 3 holding, 4 input
  };
  /// A set of on-demand reads drained from tick(): runs of <= READ_RUN_MAX registers; a Modbus
  /// exception on a run of more than one register splits it in halves and re-reads them.
  struct ReadSet {
    uint8_t device{0};
    std::vector<GridReadVal> vals;  ///< address-sorted per fn
    const GridReadVal *find(uint8_t fn, uint16_t addr) const;
    bool complete() const { return this->runs.empty() && this->inflight == 0; }
    // internal
    struct Run {
      uint8_t fn;
      uint16_t start;
      uint16_t count;
    };
    std::vector<Run> runs;
    uint16_t inflight{0};
    uint32_t gen{0};
  };
  /// Read `n` holding registers of `device` into the grid-read cache (replaces it). Readable:
  /// the device's family allow-list, its routine rules and the diagnostic set; any other
  /// address stays in the cache as OFF_LIST.
  void grid_read_request(uint8_t device, const uint16_t *addrs, uint16_t n);
  const std::vector<GridReadVal> &grid_read_results() const { return this->gr_.vals; }
  uint8_t grid_read_device() const { return this->gr_.device; }
  bool grid_read_complete() const { return this->gr_.complete(); }
  /// Identity / feature registers readable on every device (30644, 30051, 32310, 32311, 30036,
  /// 30037, 30500, 30502, 30643).
  static bool is_diagnostic_read(uint16_t addr);

  // ---- Setup profile flow ---------------------------------------------------------------------
  // setup_info_request: input 5479 + the descriptor's country / grid-code registers and (unit)
  // 30036 / 30037, for the option picker. setup_prepare: 5479 (when the option needs it) ->
  // resolve_profile -> fn 3 read of every resolved register (the preview's current values).
  // setup_apply_profile: queues the resolved items in order through setup_apply(profile =
  // true); needs a complete prepare of the same (device, option) younger than PREPARE_TTL_MS.
  // setup_start: 0xCF to the descriptor's power_switch register. An installer batch, a start or
  // a prepare of another (device, option) discards the prepared context.
  struct Prepared {
    uint8_t device{0};
    uint16_t option{0};
    uint32_t started_ms{0};
    bool active{false};
    bool resolved{false};
    sungrow_control::ResolveStatus status{ResolveStatus::OK};
    bool have_5479{false};
    uint16_t reg5479{0};
    int variant{-1};  ///< index of the chosen variant inside the option, -1 = none
    std::vector<ResolvedItem> items;
    ReadSet reads;  ///< fn 4 5479 and fn 3 of every item
    bool complete() const { return this->resolved && this->reads.complete(); }
    /// Current (read) value of a holding register of the profile; false when unknown.
    bool current(uint16_t addr, uint16_t *v) const;
    /// Items whose current value is unknown or differs from the profile value.
    size_t n_changed() const;
  };
  void setup_info_request(uint8_t device);
  const ReadSet &setup_info() const { return this->info_; }
  bool setup_info_fresh(uint8_t device, uint32_t max_age_ms) const;
  Verdict setup_prepare(uint8_t device, uint16_t option);
  /// The prepared context of (device, option) while it is younger than PREPARE_TTL_MS, else nullptr.
  const Prepared *setup_prepared(uint8_t device, uint16_t option) const;
  Verdict setup_apply_profile(uint8_t device, uint16_t option, const char *source);
  Verdict setup_start(uint8_t device, const char *source);
  /// Feature bits from the device's sig 0x2181 (1-based word).
  bool feature_w32b5(uint8_t device) const;
  bool feature_w29b12(uint8_t device) const;

  uint32_t writes_ok() const { return this->ok_; }
  uint32_t writes_failed() const { return this->failed_; }
  uint32_t writes_rejected() const { return this->rejected_; }
  uint32_t readbacks() const { return this->readbacks_; }

  /// Device address on the bus (0 = not configured in the master).
  uint8_t address_of(uint8_t device) const;

 protected:
  struct BlockState {
    uint32_t next{0};
    bool pending{false};
  };
  struct Expectation {
    ExpectKind kind{ExpectKind::NONE};
    const char *key{""};
    uint32_t gen{0};  ///< distinguishes the reconnection-delay reply of an older expectation
    uint32_t started_ms{0};
    uint32_t deadline_ms{0};
    uint32_t final_ms{0};
    uint16_t delay_s{RECONNECT_DELAY_DEFAULT_S};
    uint8_t extensions{0};
    bool final{false};
    bool ok{false};
    bool state_valid{false};
    uint16_t state{0};
    std::string text;
  };
  struct Dev {
    DeviceCfg cfg;
    std::vector<BlockState> blocks;
    bool was_online{false};
    bool seen_running{false};  ///< a running-class state since the last power-stage restart
    Expectation expect;
    PowerLimitMemo plimit;
  };
  const Dev *find_(uint8_t index) const;
  Dev *find_(uint8_t index);
  bool online_(uint8_t index) const;
  /// Kind of start/stop command a write is (NONE for any other write); *key = its rule key.
  ExpectKind expect_kind_for_(const Dev &d, uint16_t start, const uint16_t *values, uint16_t count,
                              const char **key) const;
  void begin_expect_(uint8_t device, ExpectKind kind, const char *key);
  void evaluate_expect_(Dev &d, uint32_t now, bool online);
  void set_expect_text_(Dev &d, const char *text, bool final, bool ok);
  /// Addresses of the profile's power-limit switch and setting rules (0, 0 when absent).
  bool power_limit_regs_(const Dev &d, uint16_t *sw, uint16_t *pct) const;
  void capture_power_limit_(Dev &d, uint16_t start, const std::vector<uint16_t> &values, const std::string &source,
                            bool authorized);
  void trigger_reassert_(Dev &d, uint32_t now, const char *why);
  void evaluate_reassert_(Dev &d, uint32_t now);
  /// Running-state bookkeeping of one tick (seen_running, power-stage restart trigger).
  void track_state_(Dev &d, uint32_t now);
  void log_(int level, const char *fmt, ...) const
#if defined(__GNUC__)
      __attribute__((format(printf, 3, 4)))
#endif
      ;
  uint32_t now_() const { return this->clock_ ? this->clock_() : 0; }
  void report_(uint8_t device, const char *text, bool ok);
  void ingest_(uint8_t device, uint16_t start, const uint16_t *regs, uint16_t n);
  const ReadbackDef *readback_(const Dev &d) const;
  /// Queue a write that passed `c` (rejection bookkeeping when c is not OK). `authorized` is
  /// remembered with a power-limit write for its later re-assert.
  Verdict submit_(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count, bool verify,
                  const char *source, Callback cb, sungrow_modbus::WriteOptions opt, const Check &c,
                  bool authorized = false);
  /// One protection-register frame (fn 16, verify) of a setup batch.
  Verdict write_grid_code_(uint8_t device, uint16_t start, const uint16_t *values, uint16_t count,
                           const char *source, Callback cb);
  Verdict reject_setup_(Verdict v, const char *fmt, ...)
#if defined(__GNUC__)
      __attribute__((format(printf, 3, 4)))
#endif
      ;

  /// True if `addr` is covered by `device`'s routine allow-list (a single-register rule check).
  bool is_routine_(uint8_t device, uint16_t addr) const;
  void drain_setup_();  ///< submit the next queued setup frame when the write queue has room
  void expire_setup_(uint32_t now);
  void invalidate_prepare_();
  void advance_prepare_();

  // on-demand read machinery
  void read_start_(ReadSet &rs, uint8_t device);
  void read_add_(ReadSet &rs, uint8_t fn, std::vector<uint16_t> addrs, bool readable_only);
  void drain_reads_(ReadSet &rs);
  bool reply_was_exception_(uint8_t device) const;

  /// One contiguous same-class run of a setup batch (written as one fn 16 frame).
  struct SetupFrame {
    uint16_t start;
    std::vector<uint16_t> values;
    bool grid;  ///< true: family protection allow-list; false: routine allow-list
    size_t result;  ///< index into setup_results_
  };
  std::vector<SetupFrame> frames_for_(uint8_t device, const std::vector<SetupReg> &regs) const;
  /// `regs` without the non-routine items outside the device model's power-stage range; `note`
  /// (optional) collects one "ADDR=VALUE outside the power-stage range MIN-MAX" per skipped item.
  std::vector<SetupReg> dsp_filter_(uint8_t device, const std::vector<SetupReg> &regs, std::string *note) const;

  ModbusMaster *master_{nullptr};
  sungrow_datamodel::SignalStore *store_{nullptr};
  Clock clock_;
  LogFn log_fn_;
  ResultListener listener_;
  std::vector<ExpectListener> expect_listeners_;
  FamilyFn family_fn_;
  CountryWrittenFn country_cb_;
  std::vector<Dev> devs_;
  bool enabled_{false};
  uint32_t ok_{0}, failed_{0}, rejected_{0}, readbacks_{0};

  // setup-apply queue (drained from tick())
  std::vector<SetupFrame> setup_queue_;  ///< remaining frames (front = next to submit)
  std::vector<SetupFrameResult> setup_results_;
  uint8_t setup_device_{0};
  uint16_t setup_country_reg_{0};  ///< descriptor country register of the batch's device (0 = none)
  std::string setup_source_;
  std::string setup_detail_;
  std::string setup_note_;
  bool setup_inflight_{false};  ///< a frame is in the master's queue awaiting its cb
  uint32_t setup_accepted_ms_{0};
  uint16_t setup_total_{0}, setup_ok_{0}, setup_failed_n_{0};

  // on-demand reads (drained from tick())
  ReadSet gr_;    ///< grid-read cache (/api/grid_read)
  ReadSet info_;  ///< setup option picker reads
  uint32_t info_started_ms_{0};
  Prepared prep_;
  uint32_t read_gen_{0};
};

/// "13050=[2, 0xAA]" style summary for logs (at most 8 values).
std::string format_write(uint16_t start, const uint16_t *values, uint16_t count);

}  // namespace sungrow_control
}  // namespace esphome
