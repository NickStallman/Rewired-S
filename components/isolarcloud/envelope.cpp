#include "envelope.h"

#include <cstdio>

#include "json_compat.h"
#include "zcodec.h"

namespace esphome::isolarcloud {

std::string format_send_time(int year, int month, int day, int hour, int minute, int second) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d", year, month, day, hour, minute, second);
  return buf;
}

std::string make_message_id(const std::string &sn, const std::string &send_time, uint16_t counter) {
  char cnt[8];
  snprintf(cnt, sizeof(cnt), "%05u", static_cast<unsigned>(counter));
  return sn + "_" + send_time + "_" + cnt;
}

namespace {
void add_header(JsonDocument &doc, const PublicData &pd, const std::string &message_id, size_t count, size_t id) {
  doc["messageId"] = message_id;
  doc["packgeCount"] = std::to_string(count);  // decimal strings
  doc["packgeId"] = std::to_string(id);
  JsonObject pub = doc["publicData"].to<JsonObject>();
  char pt[8];
  snprintf(pt, sizeof(pt), "%04u", static_cast<unsigned>(pd.product_type));  // 0x720 -> "1824"
  pub["productType"] = pt;
  pub["productName"] = pd.product_name;
  pub["productSN"] = pd.sn;
  pub["sendTime"] = pd.send_time;
}
}  // namespace

std::vector<std::string> build_envelopes(const PublicData &pd, const std::string &message_id,
                                         const std::string &business_json, size_t chunk) {
  std::vector<std::string> out;
  if (chunk == 0)
    chunk = CHUNK_SIZE;
  const size_t len = business_json.size();
  size_t count = len / chunk + (len % chunk != 0 ? 1 : 0);
  if (count == 0)
    count = 1;
  for (size_t i = 1; i <= count; i++) {
    JsonDocument doc;
    add_header(doc, pd, message_id, count, i);
    if (count == 1) {
      doc["businessData"] = serialized(business_json);
    } else {
      // Raw slice of the printed businessData as a JSON string: the receiver concatenates the
      // slices in packgeId order.
      doc["businessData"] = business_json.substr((i - 1) * chunk, chunk);
    }
    std::string s;
    if (!serialize_checked(doc, s))
      return {};  // never send a partial message
    out.push_back(std::move(s));
  }
  return out;
}

bool encode_payload(const std::string &json, uint16_t compress, std::vector<uint8_t> &out) {
  if (compress == 0) {
    out.assign(json.begin(), json.end());
    return true;
  }
  // The compressed input includes the terminating NUL.
  return zlib_compress(reinterpret_cast<const uint8_t *>(json.c_str()), json.size() + 1, out);
}

bool decode_payload(const uint8_t *data, size_t len, uint16_t compress, std::string &out, size_t max_out) {
  if (compress != 0) {
    std::vector<uint8_t> raw;
    if (!zlib_inflate(data, len, raw, max_out))
      return false;
    out.assign(raw.begin(), raw.end());
  } else {
    out.assign(reinterpret_cast<const char *>(data), len);
  }
  while (!out.empty() && out.back() == '\0')
    out.pop_back();
  return true;
}

std::string beat_business(int beat) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonArray>().add<JsonObject>();
  o["devCode"] = LOGGER_DEV_CODE;
  o["devType"] = LOGGER_DEV_TYPE;
  o["beat"] = beat;
  std::string s;
  serialize_checked(doc, s);
  return s;
}

std::string build_will_json(const PublicData &pd, const std::string &message_id) {
  std::vector<std::string> env = build_envelopes(pd, message_id, beat_business(0));
  return env.empty() ? std::string() : std::move(env.front());
}

}  // namespace esphome::isolarcloud
