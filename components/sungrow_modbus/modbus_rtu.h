#pragma once
// Modbus RTU framing for the RS485 master: CRC16, request frames, incremental response parser.
// Pure C++ (no ESPHome headers) so tests/modbus/ can build it on the host.
//
// Frame layouts (all multi-byte fields big-endian except the CRC, which is appended LE):
//   read  req  (fn 3/4): addr fn start_hi start_lo cnt_hi cnt_lo crc_lo crc_hi        (8 B)
//   read  resp (fn 3/4): addr fn bytecount data[bytecount] crc_lo crc_hi              (5+2n B)
//   write req  (fn 6)  : addr 0x06 reg(2) value(2) crc                                 (8 B)
//   write resp (fn 6)  : echo of the request                                          (8 B)
//   write req  (fn 16) : addr 0x10 start(2) cnt(2) bytecount data[2n] crc             (9+2n B)
//   write resp (fn 16) : addr 0x10 start(2) cnt(2) crc                                (8 B)
//   exception          : addr fn|0x80 code crc                                        (5 B)
// CRC check and frame-length rules match the stock dongle.
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace sungrow_modbus {

static constexpr uint8_t FN_READ_HOLDING = 0x03;
static constexpr uint8_t FN_READ_INPUT = 0x04;
static constexpr uint8_t FN_WRITE_SINGLE = 0x06;
static constexpr uint8_t FN_WRITE_MULTIPLE = 0x10;
static constexpr uint8_t FN_EXCEPTION_FLAG = 0x80;

/// Max registers per read the protocol allows (spec 125). The request builder caps at 120.
static constexpr uint16_t MAX_READ_REGS = 125;
/// Max registers per write-multiple (spec 123).
static constexpr uint16_t MAX_WRITE_REGS = 123;
/// Largest frame we ever expect: read response with 125 registers = 255 B.
static constexpr size_t MAX_FRAME = 5 + 2 * MAX_READ_REGS;
static constexpr size_t READ_REQUEST_LEN = 8;

/// Modbus CRC16: init 0xFFFF, reflected poly 0xA001. Transmit low byte first.
uint16_t crc16(const uint8_t *data, size_t len, uint16_t crc = 0xFFFF);
/// True if the last two bytes of `frame` are the LE CRC of the bytes before them.
bool crc_ok(const uint8_t *frame, size_t len);
/// Append the CRC of out[0..len) at out[len], out[len+1]; returns len + 2.
size_t append_crc(uint8_t *out, size_t len);

/// Build a fn 3/4 read request. `wire_start` is the 0-based PDU address (table addr - 1).
/// Writes READ_REQUEST_LEN bytes; returns that length (0 on invalid args).
size_t build_read_request(uint8_t addr, uint8_t fn, uint16_t wire_start, uint16_t count, uint8_t *out);
/// Build a fn 16 write-multiple request. Needs cap >= 9 + 2*count. Returns length or 0.
size_t build_write_request(uint8_t addr, uint16_t wire_start, const uint16_t *values, uint16_t count,
                           uint8_t *out, size_t cap);

/// Build a fn 6 write-single request (8 B). Returns the length.
size_t build_write_single_request(uint8_t addr, uint16_t wire_reg, uint16_t value, uint8_t *out);

inline bool is_write_fn(uint8_t fn) { return fn == FN_WRITE_SINGLE || fn == FN_WRITE_MULTIPLE; }

/// Expected length of a complete response to a request with this fn/count.
inline size_t expected_response_len(uint8_t fn, uint16_t count) {
  return is_write_fn(fn) ? 8 : 5 + 2u * count;
}

/// What the master is waiting for.
struct ExpectedReply {
  uint8_t addr{1};
  uint8_t fn{FN_READ_INPUT};
  uint16_t wire_start{0};  ///< checked against the fn 6 / fn 16 echo
  uint16_t count{0};       ///< registers requested (fn 6: 1)
  uint16_t value{0};       ///< fn 6: value checked against the echo
};

enum class ParseStatus : uint8_t {
  INCOMPLETE,  ///< need more bytes
  OK,          ///< valid response; data()/nregs() are set
  EXCEPTION,   ///< valid exception response; exception_code() is set
};

/// Incremental response parser with stray-byte resync. Feed it whatever the UART delivered;
/// it drops leading bytes that cannot start the expected reply (wrong address, wrong function,
/// wrong byte count, bad CRC) one at a time and rescans, so line noise, a late reply to an
/// earlier request or our own echo do not break framing. Only a CRC-valid frame from the
/// expected slave with the expected function (or its exception) is accepted.
class ResponseParser {
 public:
  void begin(const ExpectedReply &e);
  ParseStatus feed(const uint8_t *data, size_t len);
  ParseStatus status() const { return this->status_; }

  /// Register payload of an OK fn 3/4 reply (big-endian registers, 2*nregs bytes).
  const uint8_t *data() const { return this->buf_ + 3; }
  uint16_t nregs() const { return this->nregs_; }
  uint8_t exception_code() const { return this->exception_; }
  /// CRC failures seen while scanning in this transaction (a candidate frame of the right
  /// shape whose CRC did not match).
  uint16_t crc_errors() const { return this->crc_errors_; }
  /// Bytes discarded while resyncing.
  uint16_t dropped() const { return this->dropped_; }
  /// Bytes currently buffered (the frame length once a frame is complete).
  size_t buffered() const { return this->have_; }

 protected:
  void drop_(size_t n);
  ParseStatus scan_();

  ExpectedReply exp_{};
  ParseStatus status_{ParseStatus::INCOMPLETE};
  uint8_t buf_[MAX_FRAME + 8]{};
  size_t have_{0};
  uint16_t nregs_{0};
  uint8_t exception_{0};
  uint16_t crc_errors_{0};
  uint16_t dropped_{0};
};

}  // namespace sungrow_modbus
}  // namespace esphome
