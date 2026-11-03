#pragma once
// Modbus-TCP slave core: MBAP framing, PDU handling and register assembly from the stock
// tcpslave maps + the signal store. Pure C++ (no ESPHome / lwIP headers) so tests/modbus_tcp/
// builds it on the host. The ESPHome glue (sockets, entities, unit map) is sungrow_modbus_tcp.h.
//
// Behaviour (matches the stock dongle unless marked "differs from stock"):
//  - MBAP: transaction id + protocol id echoed (protocol id not checked), length = unit + PDU.
//    Several frames per segment are split. Differs from stock: a frame cut across two TCP
//    segments is reassembled (stock drops it).
//  - Wire address + 1 = 1-based table address.
//  - fn 3 / fn 4: count > 125 -> exception 2. Each register is looked up by an exact row start
//    (row.regaddr == address and row.fn == request fn, first match in table order). A hit with
//    sigid != 0 is "resolved" and advances by row.nreg; a miss (or sigid 0) is "unresolved" and
//    advances by 1. fn 3 and fn 4 therefore serve different rows of the same table (rows carry
//    their own fn).
//       nothing resolved, fn 4      -> exception 2
//       some resolved,    fn 4      -> served from the store, holes = 0x0000
//       all resolved,     fn 3      -> served from the store
//       any unresolved,   fn 3      -> live read of the whole range from the device;
//                                      failure / 5 s timeout -> exception 4
//  - Values are re-packed from the signal store (low word first by default).
//  - fn 6 / fn 16: success -> reply rebuilt from the request (fn 6: echo addr + value, fn 16:
//    addr + count); any failure -> exception 4. Differs from stock (which forwards any register
//    to the device): a write is first gated by Backend::check_write() (sungrow_control: enable
//    switch, allow-list, ranges, device online, client IP) and only then forwarded
//    (Decision::WRITE). fn 5 / 0x0F / 0x60 answer exception 1.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../sungrow_datamodel/decoder.h"
#include "../sungrow_datamodel/dm_types.h"
#include "../sungrow_datamodel/signal_store.h"

namespace esphome {
namespace sungrow_modbus_tcp {

using sungrow_datamodel::RegisterRow;
using sungrow_datamodel::RegisterTable;
using sungrow_datamodel::Signal;
using sungrow_datamodel::WordOrder;

static constexpr size_t MBAP_HEADER_LEN = 7;     ///< tid(2) pid(2) len(2) unit(1)
static constexpr size_t MAX_PDU_LEN = 253;       ///< Modbus spec
static constexpr size_t MAX_ADU_LEN = MBAP_HEADER_LEN + MAX_PDU_LEN;  // 260
static constexpr uint16_t MAX_READ_REGS = 125;   ///< Modbus spec limit for fn 3/4
static constexpr uint16_t MAX_WRITE_REGS = 123;  ///< Modbus spec limit for fn 16
/// Live-read / write request timeout (as stock).
static constexpr uint32_t PASSTHROUGH_TIMEOUT_MS = 5000;

enum FunctionCode : uint8_t {
  FC_READ_HOLDING = 0x03,
  FC_READ_INPUT = 0x04,
  FC_WRITE_SINGLE_COIL = 0x05,
  FC_WRITE_SINGLE = 0x06,
  FC_WRITE_MULTIPLE = 0x10,
  FC_SUNGROW_WRITE = 0x60,  ///< stock proprietary write-multiple variant
};

enum ExceptionCode : uint8_t {
  EX_NONE = 0,
  EX_ILLEGAL_FUNCTION = 0x01,
  EX_ILLEGAL_ADDRESS = 0x02,
  EX_ILLEGAL_VALUE = 0x03,
  EX_DEVICE_FAILURE = 0x04,
  EX_BUSY = 0x06,
  EX_GATEWAY_PATH = 0x0A,
  EX_GATEWAY_TARGET = 0x0B,
};

/// The part of a request needed to answer it later (after the frame left the buffer).
struct ReqHeader {
  uint16_t tid{0};
  uint16_t pid{0};
  uint8_t unit{0};
  uint8_t fc{0};
};

/// One complete MBAP frame inside a FrameAssembler buffer (valid until pop()).
struct Frame {
  ReqHeader hdr;
  const uint8_t *pdu{nullptr};  ///< starts at the function code
  uint16_t pdu_len{0};          ///< MBAP length - 1
  uint16_t total_len{0};        ///< bytes this frame occupies in the stream
};

/// Reassembles MBAP frames from a TCP byte stream (split and pipelined requests).
class FrameAssembler {
 public:
  enum class Status : uint8_t { NEED_MORE, FRAME, BAD };
  static constexpr size_t CAPACITY = 2 * MAX_ADU_LEN;

  /// Free space for a direct socket read; commit() what was written.
  uint8_t *write_ptr() { return this->buf_ + this->len_; }
  size_t space() const { return CAPACITY - this->len_; }
  void commit(size_t n) { this->len_ += n; }
  /// Copy in as much as fits; returns the number of bytes taken.
  size_t feed(const uint8_t *data, size_t n);
  /// Look at the frame at the head of the buffer. BAD = unusable MBAP length (the stream cannot
  /// be resynchronised: close the connection).
  Status peek(Frame *out) const;
  /// Drop the head frame (after peek() returned FRAME).
  void pop(const Frame &f);
  size_t buffered() const { return this->len_; }
  void clear() { this->len_ = 0; }

 protected:
  uint8_t buf_[CAPACITY];
  size_t len_{0};
};

/// Where a unit id's requests go. Filled by the Backend per request (dev_type may be discovered
/// late, so this is resolved every time).
struct UnitTarget {
  uint8_t device{0};                    ///< SignalStore device index
  const RegisterTable *table{nullptr};  ///< tcpslave map (or the device's poll table fallback)
  uint8_t rtu_address{0};               ///< RS485 slave address for fn 3 live reads, 0 = none
  /// Device's active poll table, for Options::poll_fallback (nullptr = none).
  const RegisterTable *poll_table{nullptr};
};

class Backend {
 public:
  virtual ~Backend() = default;
  enum class Resolve : uint8_t {
    OK,
    UNKNOWN,     ///< unit not configured
    NOT_READY,   ///< configured, but the device's table is not known yet (discovery pending)
  };
  virtual Resolve resolve_unit(uint8_t unit, UnitTarget *out) = 0;
  /// Stored signal, nullptr if never stored.
  virtual const Signal *get_signal(uint8_t device, uint16_t sigid) = 0;
  /// Gate a write (fn 6/16, start = 1-based, values host order). Returns 0 to forward it, or the
  /// Modbus exception to answer. Default: read-only server (exception 1).
  virtual uint8_t check_write(const UnitTarget & /*t*/, uint8_t /*fc*/, uint16_t /*start*/,
                              const uint16_t * /*values*/, uint16_t /*count*/) {
    return 0x01;
  }
};

/// Backend over a SignalStore with a static unit list (host tests, or a fixed YAML map).
class StaticBackend : public Backend {
 public:
  explicit StaticBackend(const sungrow_datamodel::SignalStore *store) : store_(store) {}
  void add_unit(uint8_t unit, const UnitTarget &t) { this->units_.push_back({unit, t}); }
  Resolve resolve_unit(uint8_t unit, UnitTarget *out) override;
  const Signal *get_signal(uint8_t device, uint16_t sigid) override {
    return this->store_->get(device, sigid);
  }

 protected:
  struct Entry {
    uint8_t unit;
    UnitTarget target;
  };
  const sungrow_datamodel::SignalStore *store_;
  std::vector<Entry> units_;
};

struct Options {
  /// Multi-register value order on the wire. Low word first, as in the public Sungrow
  /// Modbus documentation and common client configurations (`swap: word`).
  WordOrder word_order{WordOrder::LOW_WORD_FIRST};
  /// Unknown unit id: true = exception 0x0B, false = no reply (stock: silent drop).
  bool unknown_unit_exception{true};
  /// fn 3 with unresolved registers: live read from the device (stock). false = serve the
  /// store with holes = 0 (exception 2 if nothing resolved).
  bool passthrough{true};
  /// A tcpslave row whose sigid was never stored: serve the signal of the poll-table row at the
  /// same address + fn instead (e.g. tcpslave_dev35 13036/13045/13017/13001/5214/13010 vs
  /// inv_proto5). false = sentinel, as stock.
  bool poll_fallback{false};
};

/// Result of register assembly.
struct AssembleStats {
  uint16_t resolved{0};    ///< rows found (sigid != 0)
  uint16_t unresolved{0};  ///< registers with no row start
  uint16_t absent{0};      ///< resolved rows whose signal was never stored (after fallback)
  uint16_t fallback{0};    ///< rows served from the poll-table sigid at the same address
};

/// Encode one row's value into row.nreg registers (big-endian registers, `wo` for multi-register
/// scalars). Raw store bits are rescaled when the row's scale differs from the signal's scale
/// (both non-zero). nullptr / invalid signal -> stock sentinel (0xFFFF, 0x7FFF, 0xFFFFFFFF, 0x7FFFFFFF,
/// ...); strings/arrays -> zeros. Float32 rows (type 9 x2) carry the signal's number() in
/// engineering units as IEEE bits per word order (the row scale is only a decimals hint);
/// absent/invalid -> quiet NaN 0x7FC00000.
void encode_row(const RegisterRow &row, const Signal *sig, WordOrder wo, uint8_t *out);
/// Sentinel bits for a numeric type of width nreg registers (inverse of is_sentinel()).
uint64_t sentinel_bits(uint8_t type, uint8_t nreg);

/// Fill `count` registers starting at the 1-based table address `start` (out = 2*count bytes)
/// from the rows of `table` with row.fn == fn, walking the rows as stock does.
/// fallback: optional poll table searched for rows whose tcpslave signal was never stored.
AssembleStats assemble_registers(const RegisterTable &table, uint8_t fn, uint16_t start, uint16_t count,
                                 Backend &backend, uint8_t device, WordOrder wo, uint8_t *out,
                                 const RegisterTable *fallback = nullptr);

/// What the transport must do with a request.
struct Decision {
  enum Kind : uint8_t { REPLY, DROP, PASSTHROUGH, WRITE } kind{DROP};
  uint8_t exception{EX_NONE};  ///< REPLY: exception code sent (EX_NONE = normal reply)
  // PASSTHROUGH: live read the transport performs, then build_passthrough_reply().
  // WRITE: write the transport performs (values[0..count)), then build_write_reply().
  uint8_t rtu_address{0};
  uint8_t device{0};  ///< WRITE: SignalStore device index of the unit
  uint8_t fn{0};
  uint16_t start{0};  ///< 1-based table address
  uint16_t count{0};
  uint16_t values[MAX_WRITE_REGS]{};  ///< WRITE only
};

/// Handle one frame. On REPLY, the reply ADU is written to out (>= MAX_ADU_LEN bytes) and
/// *out_len set.
Decision handle_frame(const Frame &f, Backend &backend, const Options &opt, uint8_t *out, size_t *out_len);

size_t build_exception(const ReqHeader &h, uint8_t code, uint8_t *out);
/// Normal fn 3/4 reply with `nregs` registers of big-endian data.
size_t build_read_reply(const ReqHeader &h, const uint8_t *data, uint16_t nregs, uint8_t *out);
/// Reply to a finished write: exception 0 -> fn 6 echo (addr, value) / fn 16 (addr, count);
/// else that exception. `start` is the 1-based address (wire = start - 1).
size_t build_write_reply(const ReqHeader &h, uint16_t start, uint16_t value_or_count, uint8_t exception,
                         uint8_t *out);
/// Reply to a finished live read: ok -> data, else exception 4 (as stock).
size_t build_passthrough_reply(const ReqHeader &h, bool ok, const uint8_t *data, uint16_t nregs,
                               uint16_t expected, uint8_t *out);

// ---- source-IP allow-list ------------------------------------------------------------------
struct IpRule {
  uint32_t addr;  ///< IPv4, host order
  uint32_t mask;  ///< host order
};
/// Parse "a.b.c.d" or "a.b.c.d/n". Returns false on syntax error.
bool parse_ip_rule(const char *s, IpRule *out);
/// Empty list = allow all (as the stock whitelist).
bool ip_allowed(const std::vector<IpRule> &rules, uint32_t addr);

// ---- :502 country rule -----------------------------------------------------------------------
/// Inverter region / country code signal (stock boot check reads it from the first inverter).
static constexpr uint16_t SIG_COUNTRY_CODE = 0x2001;
/// :502 default by country (as stock, at boot and after a country write): enabled for
/// 6 (Australia), 99 and 114..117, disabled otherwise.
inline bool tcp_enabled_for_country(uint16_t country) {
  return country == 6 || country == 99 || (country >= 114 && country <= 117);
}

// ---- statistics ------------------------------------------------------------------------------
struct Stats {
  uint32_t requests{0};     ///< frames answered normally
  uint32_t exceptions{0};   ///< exception replies
  uint32_t dropped{0};      ///< frames not answered (unknown unit with drop, framing errors)
  uint32_t passthrough{0};  ///< live reads started
  uint32_t writes{0};       ///< writes forwarded to RS485
  uint32_t writes_rejected{0};  ///< writes refused by the gate
  uint32_t rejected{0};     ///< connections refused (allow-list, max clients)
  uint32_t errors() const { return this->exceptions + this->dropped; }
};

}  // namespace sungrow_modbus_tcp
}  // namespace esphome
