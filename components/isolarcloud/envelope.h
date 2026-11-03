#pragma once
// Outbound message envelope, chunking and payload encoding. Host-buildable.
//
// Wire shape of one MQTT message (key order as stock cJSON insertion order):
//   {"messageId":"<SN>_<sendTime>_%05d","packgeCount":"N","packgeId":"i",
//    "publicData":{"productType":"1824","productName":"WiNet-S2","productSN":"<SN>",
//                  "sendTime":"YYYYMMDDhhmmss"},
//    "businessData": <JSON array>                     when the printed businessData <= 2560 B
//                  | "<2560-byte slice of the printed businessData>"   otherwise (string!)}
// then zlib-compressed when the topic enum compress != 0.

#include <cstdint>
#include <string>
#include <vector>

#include "cloud_types.h"

namespace esphome::isolarcloud {

struct PublicData {
  uint16_t product_type{0};
  std::string product_name;
  std::string sn;
  std::string send_time;  ///< "YYYYMMDDhhmmss", local time
};

/// "YYYYMMDDhhmmss" ("%04d%02d%02d%02d%02d%02d").
std::string format_send_time(int year, int month, int day, int hour, int minute, int second);

/// A process-wide u16 counter, pre-incremented, 0 skipped (1..65535, 1, ...), printed as %05d in
/// messageId.
class MessageCounter {
 public:
  uint16_t next() {
    if (++this->n_ == 0)
      this->n_ = 1;
    return this->n_;
  }
  uint16_t peek() const { return this->n_; }

 protected:
  uint16_t n_{0};
};

/// "<SN>_<sendTime>_%05d".
std::string make_message_id(const std::string &sn, const std::string &send_time, uint16_t counter);

/// Split a printed businessData JSON array into envelopes (unencoded JSON strings). One envelope
/// with businessData embedded as JSON if it fits in `chunk` bytes, otherwise ceil(len/chunk)
/// envelopes carrying string slices. All chunks share messageId. Empty if a document overflowed
/// (out of memory): a partial message is never produced.
std::vector<std::string> build_envelopes(const PublicData &pd, const std::string &message_id,
                                         const std::string &business_json, size_t chunk = CHUNK_SIZE);

/// Encode one envelope for the wire. compress != 0: zlib over the JSON *including its
/// terminating NUL*; compress == 0: the JSON bytes without NUL.
bool encode_payload(const std::string &json, uint16_t compress, std::vector<uint8_t> &out);

/// Decode a payload received on a topic with the given compress flag: inflate if compress != 0,
/// strip one trailing NUL either way. Returns false on inflate failure.
bool decode_payload(const uint8_t *data, size_t len, uint16_t compress, std::string &out, size_t max_out = 64 * 1024);

/// Logger-only beat element [{"devCode":247,"devType":22,"beat":<beat>}]: the will uses beat 0.
/// (beatdata itself lists every device: BusinessFormatter::beat.)
std::string beat_business(int beat);

/// The will JSON (single envelope, packgeCount "1", packgeId "1").
std::string build_will_json(const PublicData &pd, const std::string &message_id);

}  // namespace esphome::isolarcloud
