#include "modbus_rtu.h"

#include <cstring>

namespace esphome {
namespace sungrow_modbus {

uint16_t crc16(const uint8_t *data, size_t len, uint16_t crc) {
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
  }
  return crc;
}

bool crc_ok(const uint8_t *frame, size_t len) {
  if (len < 3)
    return false;
  uint16_t c = crc16(frame, len - 2);
  return frame[len - 2] == static_cast<uint8_t>(c) && frame[len - 1] == static_cast<uint8_t>(c >> 8);
}

size_t append_crc(uint8_t *out, size_t len) {
  uint16_t c = crc16(out, len);
  out[len] = static_cast<uint8_t>(c);
  out[len + 1] = static_cast<uint8_t>(c >> 8);
  return len + 2;
}

size_t build_read_request(uint8_t addr, uint8_t fn, uint16_t wire_start, uint16_t count, uint8_t *out) {
  if ((fn != FN_READ_HOLDING && fn != FN_READ_INPUT) || count == 0 || count > MAX_READ_REGS)
    return 0;
  out[0] = addr;
  out[1] = fn;
  out[2] = static_cast<uint8_t>(wire_start >> 8);
  out[3] = static_cast<uint8_t>(wire_start);
  out[4] = static_cast<uint8_t>(count >> 8);
  out[5] = static_cast<uint8_t>(count);
  return append_crc(out, 6);
}

size_t build_write_request(uint8_t addr, uint16_t wire_start, const uint16_t *values, uint16_t count,
                           uint8_t *out, size_t cap) {
  if (count == 0 || count > MAX_WRITE_REGS || values == nullptr || cap < 9u + 2u * count)
    return 0;
  out[0] = addr;
  out[1] = FN_WRITE_MULTIPLE;
  out[2] = static_cast<uint8_t>(wire_start >> 8);
  out[3] = static_cast<uint8_t>(wire_start);
  out[4] = static_cast<uint8_t>(count >> 8);
  out[5] = static_cast<uint8_t>(count);
  out[6] = static_cast<uint8_t>(2 * count);
  for (uint16_t i = 0; i < count; i++) {
    out[7 + 2 * i] = static_cast<uint8_t>(values[i] >> 8);
    out[8 + 2 * i] = static_cast<uint8_t>(values[i]);
  }
  return append_crc(out, 7 + 2u * count);
}

size_t build_write_single_request(uint8_t addr, uint16_t wire_reg, uint16_t value, uint8_t *out) {
  out[0] = addr;
  out[1] = FN_WRITE_SINGLE;
  out[2] = static_cast<uint8_t>(wire_reg >> 8);
  out[3] = static_cast<uint8_t>(wire_reg);
  out[4] = static_cast<uint8_t>(value >> 8);
  out[5] = static_cast<uint8_t>(value);
  return append_crc(out, 6);
}

void ResponseParser::begin(const ExpectedReply &e) {
  this->exp_ = e;
  this->status_ = ParseStatus::INCOMPLETE;
  this->have_ = 0;
  this->nregs_ = 0;
  this->exception_ = 0;
  this->crc_errors_ = 0;
  this->dropped_ = 0;
}

void ResponseParser::drop_(size_t n) {
  if (n >= this->have_) {
    this->dropped_ += static_cast<uint16_t>(this->have_);
    this->have_ = 0;
    return;
  }
  std::memmove(this->buf_, this->buf_ + n, this->have_ - n);
  this->have_ -= n;
  this->dropped_ += static_cast<uint16_t>(n);
}

ParseStatus ResponseParser::feed(const uint8_t *data, size_t len) {
  if (this->status_ != ParseStatus::INCOMPLETE)
    return this->status_;  // late bytes after a complete frame are the caller's to discard
  while (len > 0) {
    size_t space = sizeof(this->buf_) - this->have_;
    if (space == 0) {
      this->drop_(1);  // cannot happen with a sane scan_, but never overflow
      continue;
    }
    size_t n = len < space ? len : space;
    std::memcpy(this->buf_ + this->have_, data, n);
    this->have_ += n;
    data += n;
    len -= n;
    if (this->scan_() != ParseStatus::INCOMPLETE)
      return this->status_;
  }
  return this->status_;
}

ParseStatus ResponseParser::scan_() {
  const uint8_t fn = this->exp_.fn;
  while (this->have_ > 0) {
    if (this->buf_[0] != this->exp_.addr) {
      this->drop_(1);
      continue;
    }
    if (this->have_ < 2)
      return ParseStatus::INCOMPLETE;
    size_t total;
    const uint8_t rfn = this->buf_[1];
    if (rfn == (fn | FN_EXCEPTION_FLAG)) {
      total = 5;
    } else if (rfn == fn) {
      if (is_write_fn(fn)) {
        total = 8;
      } else {
        if (this->have_ < 3)
          return ParseStatus::INCOMPLETE;
        if (this->buf_[2] != 2u * this->exp_.count) {
          this->drop_(1);  // wrong byte count: not our reply (or garbage); resync
          continue;
        }
        total = 5u + this->buf_[2];
      }
    } else {
      this->drop_(1);
      continue;
    }
    if (this->have_ < total)
      return ParseStatus::INCOMPLETE;
    if (!crc_ok(this->buf_, total)) {
      this->crc_errors_++;
      this->drop_(1);
      continue;
    }
    if (rfn & FN_EXCEPTION_FLAG) {
      this->exception_ = this->buf_[2];
      this->status_ = ParseStatus::EXCEPTION;
    } else if (fn == FN_WRITE_MULTIPLE) {
      uint16_t s = static_cast<uint16_t>(this->buf_[2] << 8 | this->buf_[3]);
      uint16_t c = static_cast<uint16_t>(this->buf_[4] << 8 | this->buf_[5]);
      if (s != this->exp_.wire_start || c != this->exp_.count) {
        this->drop_(1);
        continue;
      }
      this->nregs_ = c;
      this->status_ = ParseStatus::OK;
    } else if (fn == FN_WRITE_SINGLE) {
      // fn 6 reply = echo of register and value.
      uint16_t r = static_cast<uint16_t>(this->buf_[2] << 8 | this->buf_[3]);
      uint16_t v = static_cast<uint16_t>(this->buf_[4] << 8 | this->buf_[5]);
      if (r != this->exp_.wire_start || v != this->exp_.value) {
        this->drop_(1);
        continue;
      }
      this->nregs_ = 1;
      this->status_ = ParseStatus::OK;
    } else {
      this->nregs_ = this->exp_.count;
      this->status_ = ParseStatus::OK;
    }
    // Keep the frame at buf_[0..total); anything after it is ignored (late noise).
    this->have_ = total;
    return this->status_;
  }
  return ParseStatus::INCOMPLETE;
}

}  // namespace sungrow_modbus
}  // namespace esphome
