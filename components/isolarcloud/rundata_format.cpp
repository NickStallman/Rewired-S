#include "rundata_format.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "json_compat.h"

namespace esphome::isolarcloud {

namespace {
// Our TypeCodes (sungrow_datamodel/dm_types.h), duplicated to stay host-buildable without it.
constexpr uint8_t OUR_S16 = 3, OUR_S32 = 5, OUR_S64 = 7, OUR_F32 = 0x80;

bool stock_signed(uint8_t sig_type) { return sig_type == 1 || sig_type == 3 || sig_type == 5 || sig_type == 7; }
bool stock_float(uint8_t sig_type) { return sig_type == 9 || sig_type == 10; }
bool fmt_hex(uint8_t fmt) { return fmt == 13 || (fmt >= 15 && fmt <= 19) || fmt == 101; }

int64_t sign_extend(uint64_t v, unsigned bytes) {
  if (bytes == 0 || bytes >= 8)
    return static_cast<int64_t>(v);
  const unsigned bits = bytes * 8;
  const uint64_t m = 1ull << (bits - 1);
  v &= (1ull << bits) - 1;
  return static_cast<int64_t>((v ^ m) - m);
}

/// Our signal as an integer (sign-extended per our type) or, for host floats, a double.
int64_t our_integer(const TelemetrySignal &s) {
  if (s.is_float || s.type == OUR_F32)
    return static_cast<int64_t>(std::llround(s.value));
  if (s.type == OUR_S16 || s.type == OUR_S32 || s.type == OUR_S64)
    return sign_extend(s.bits, s.len);
  return static_cast<int64_t>(s.bits);
}

bool is_sentinel(uint8_t sig_type, uint64_t v, const std::string &dm) {
  switch (sig_type) {
    case 1:
    case 2:
      return true;  // 8-bit types are never sent
    case 3:
      return (v & 0xFFFF) == 0x7FFF;
    case 4:
      return (v & 0xFFFF) == 0xFFFF;
    case 5:
      return (v & 0xFFFFFFFFull) == 0x7FFFFFFFull;
    case 6:
      return (v & 0xFFFFFFFFull) == 0xFFFFFFFFull;
    case 7:
      return v == 0x7FFFFFFFFFFFFFFFull;
    case 8:
      return v == ~0ull;
    case 12:
      // All NUL, or unused registers served as 0xFFFF filler.
      for (char c : dm) {
        if (c != '\0' && static_cast<uint8_t>(c) != 0xFF)
          return false;
      }
      return true;
    default:
      return false;
  }
}

std::string fmt_double(double d, unsigned len) {
  char buf[48];
  snprintf(buf, sizeof(buf), len == 2 ? "%.10g" : len == 4 ? "%.13g" : "%.19g", d);
  return buf;
}

void add_header(JsonObject o, const TelemetryDevice &d, const TelemetrySource *src, const std::string &data_time,
                const std::string &batch_id, const std::string *sn = nullptr, const std::string *timestamp = nullptr) {
  if (!batch_id.empty())
    o["batchId"] = batch_id;
  o["devCode"] = d.dev_code;
  o["devType"] = d.dev_type;
  if (d.has_up || d.dev_type == 43 || d.dev_type == 92) {
    // Parent link: devType 43 reads upDevType/upDevCode from sigs 0x2022/0x2028, devType 92 from
    // 0x2030/0x2032. Both keys are always present for these types; an unreadable sig gives 0.
    uint64_t up_type = d.up_dev_type, up_code = d.up_dev_code;
    if (!d.has_up) {
      const uint16_t st = d.dev_type == 43 ? 0x2022 : 0x2030, sc = d.dev_type == 43 ? 0x2028 : 0x2032;
      TelemetrySignal a, b;
      up_type = src != nullptr && src->get_signal(d.index, st, a) && a.valid ? a.bits : 0;
      up_code = src != nullptr && src->get_signal(d.index, sc, b) && b.valid ? b.bits : 0;
    }
    o["upDevType"] = up_type;
    o["upDevCode"] = up_code;
  }
  if (sn != nullptr)
    o["sn"] = *sn;
  if (timestamp != nullptr)
    o["timestamp"] = *timestamp;
  o["dataTime"] = data_time;
}

bool table_has_mask(const MqttTable *t, uint16_t mask) {
  if (t == nullptr)
    return false;
  for (uint16_t i = 0; i < t->count; i++) {
    if (t->points[i].mask & mask)
      return true;
  }
  return false;
}

void add_points(JsonObject data, const MqttTable *t, uint16_t mask, const TelemetryDevice &d,
                const TelemetrySource *src) {
  if (t == nullptr || src == nullptr)
    return;
  std::string v;
  for (uint16_t i = 0; i < t->count; i++) {
    const MqttPoint &p = t->points[i];
    if (!(p.mask & mask))
      continue;
    TelemetrySignal s;
    if (!src->get_signal(d.index, p.sigid, s))
      continue;  // "GET 0x%x FAIL"
    if (!format_point_value(p, s, v))
      continue;  // "CHK 0x%x INVALID"
    data[std::to_string(p.point_id)] = v;
  }
}

std::string raw_string(const TelemetrySignal &s) {
  const std::string &b = s.bytes.empty() ? s.str : s.bytes;
  size_t n = 0;
  while (n < b.size() && b[n] != '\0')
    n++;
  return b.substr(0, n);
}

std::string hex(const std::string &bytes) {
  static const char *const H = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (unsigned char c : bytes) {
    out += H[c >> 4];
    out += H[c & 0xF];
  }
  return out;
}

/// "" when the document overflowed (the caller then publishes nothing).
std::string serialize(JsonDocument &doc) {
  std::string s;
  serialize_checked(doc, s);
  return s;
}
}  // namespace

bool format_point_value(const MqttPoint &p, const TelemetrySignal &s, std::string &out) {
  out.clear();
  if (!s.valid)
    return false;
  const unsigned size = p.size != 0 ? p.size : (s.len != 0 ? s.len : 2);

  // The value as the stock DM would hold it: `size` bytes, little-endian for integers.
  std::string dm;
  const bool bytes_value = s.is_string || !s.bytes.empty();
  if (bytes_value) {
    dm = s.bytes.empty() ? s.str : s.bytes;
    dm.resize(size, '\0');
  } else {
    const uint64_t v = static_cast<uint64_t>(our_integer(s));
    dm.resize(size, '\0');
    for (unsigned i = 0; i < size && i < 8; i++)
      dm[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
  }
  uint64_t raw = 0;
  for (unsigned i = 0; i < size && i < 8; i++)
    raw |= static_cast<uint64_t>(static_cast<uint8_t>(dm[i])) << (8 * i);
  if (is_sentinel(p.sig_type, raw, dm))
    return false;
  const int64_t sraw = sign_extend(raw, size > 8 ? 8 : size);
  const bool sgn = stock_signed(p.sig_type);
  char buf[48];

  switch (p.fmt) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
      if (sgn)
        snprintf(buf, sizeof(buf), "%" PRId64, sraw);
      else
        snprintf(buf, sizeof(buf), "%" PRIu64, raw);
      out = buf;
      return true;
    case 7:
    case 8:
      if (p.sig_type == 7)
        snprintf(buf, sizeof(buf), "%" PRId64, sraw);
      else
        snprintf(buf, sizeof(buf), "%" PRIu64, raw);
      out = buf;
      return true;
    case 9: {
      // Single precision on purpose (the cloud's own rounding above 2^24).
      float f;
      if (stock_float(p.sig_type) && (s.is_float || s.type == OUR_F32))
        f = static_cast<float>(s.value);
      else
        f = sgn ? static_cast<float>(sraw) : static_cast<float>(raw);
      out = fmt_double(static_cast<double>(f) * mqtt_scale(p), size);
      return true;
    }
    case 10: {
      double d;
      if (stock_float(p.sig_type) && (s.is_float || s.type == OUR_F32))
        d = s.value;
      else
        d = sgn ? static_cast<double>(sraw) : static_cast<double>(raw);
      out = fmt_double(d * mqtt_scale(p), size);
      return true;
    }
    case 12: {
      // Up to the first NUL, without trailing 0xFF / space filler (like the store's Signal::str());
      // nothing left -> not sent.
      size_t n = 0;
      while (n < dm.size() && dm[n] != '\0')
        n++;
      while (n > 0 && (dm[n - 1] == ' ' || static_cast<uint8_t>(dm[n - 1]) == 0xFF))
        n--;
      if (n == 0)
        return false;
      out = dm.substr(0, n);
      return true;
    }
    default:
      if (fmt_hex(p.fmt))
        out = hex(dm);
      return true;  // other fmts: "" (the key is still added)
  }
}

TelemetrySignal &LoggerSignals::slot_(uint16_t sigid) {
  for (auto &s : this->sigs_) {
    if (s.sigid == sigid)
      return s;
  }
  this->sigs_.emplace_back();
  this->sigs_.back().sigid = sigid;
  return this->sigs_.back();
}

void LoggerSignals::set_scalar_(uint16_t sigid, uint64_t bits, uint8_t len, uint8_t type) {
  TelemetrySignal &s = this->slot_(sigid);
  s = TelemetrySignal{};
  s.sigid = sigid;
  s.valid = true;
  s.bits = bits;
  s.len = len;
  s.type = type;
  s.value = static_cast<double>(type == 3 ? static_cast<double>(sign_extend(bits, len)) : static_cast<double>(bits));
}

void LoggerSignals::set_str(uint16_t sigid, const std::string &v) {
  TelemetrySignal &s = this->slot_(sigid);
  s = TelemetrySignal{};
  s.sigid = sigid;
  s.valid = true;
  s.is_string = true;
  s.type = 12;
  s.str = v;
  s.bytes = v;
  s.len = static_cast<uint8_t>(v.size() > 255 ? 255 : v.size());
}

void LoggerSignals::set_bytes(uint16_t sigid, const std::string &v) {
  this->set_str(sigid, v);
  this->slot_(sigid).is_string = false;
}

void LoggerSignals::erase(uint16_t sigid) {
  for (auto it = this->sigs_.begin(); it != this->sigs_.end(); ++it) {
    if (it->sigid == sigid) {
      this->sigs_.erase(it);
      return;
    }
  }
}

void LoggerSignals::for_each_signal(uint8_t /*device_index*/, const std::function<void(const TelemetrySignal &)> &fn) const {
  for (const auto &s : this->sigs_)
    fn(s);
}

TelemetryDevice logger_device() {
  TelemetryDevice d;
  d.index = 0;
  d.dev_code = LOGGER_DEV_CODE;
  d.table_type = 0x02;
  d.dev_type = LOGGER_DEV_TYPE;
  d.online = true;
  return d;
}

std::string BusinessFormatter::rundata(const TelemetryDevice &d, const TelemetrySource *src,
                                       const std::string &data_time) const {
  const MqttTable *t = find_mqtt_table(d.table_type);
  if (t == nullptr || d.dev_type == 0)
    return "";
  JsonDocument doc(large_json_allocator());
  JsonObject o = doc.to<JsonArray>().add<JsonObject>();
  add_header(o, d, src, data_time, "");
  JsonObject data = o["data"].to<JsonObject>();
  add_points(data, t, MQTT_MASK_RUNDATA, d, src);
  return serialize(doc);
}

std::string BusinessFormatter::devstatus(const TelemetryDevice &d, const TelemetrySource *src,
                                         const std::string &data_time, const std::string &timestamp) const {
  const MqttTable *t = find_mqtt_table(d.table_type);
  if (d.dev_type == 0 || !table_has_mask(t, MQTT_MASK_STATUS))
    return "";
  JsonDocument doc(large_json_allocator());
  JsonObject o = doc.to<JsonArray>().add<JsonObject>();
  if (d.dev_type == 1 || d.dev_type == 14) {
    TelemetrySignal s;
    const std::string sn =
        src != nullptr && src->get_signal(d.index, 0x0002, s) && s.valid ? raw_string(s) : std::string();
    add_header(o, d, src, data_time, "", &sn, &timestamp);
  } else {
    add_header(o, d, src, data_time, "");
  }
  JsonObject data = o["data"].to<JsonObject>();
  add_points(data, t, MQTT_MASK_STATUS, d, src);
  return serialize(doc);
}

std::string BusinessFormatter::generaldata(const TelemetryDevice &d, const TelemetrySource *src,
                                           const std::string &data_time) const {
  const MqttTable *t = find_mqtt_table(d.table_type);
  if (d.dev_type == 0 || !table_has_mask(t, MQTT_MASK_GENERAL))
    return "";
  JsonDocument doc(large_json_allocator());
  JsonObject o = doc.to<JsonArray>().add<JsonObject>();
  add_header(o, d, src, data_time, "");
  JsonObject data = o["data"].to<JsonObject>();
  add_points(data, t, MQTT_MASK_GENERAL, d, src);
  if (data.size() == 0)
    return "";  // nothing to report
  o["businessType"] = "addBattery";
  return serialize(doc);
}

std::string BusinessFormatter::point_signature(const TelemetryDevice &d, const TelemetrySource *src,
                                               uint16_t mask) const {
  const MqttTable *t = find_mqtt_table(d.table_type);
  std::string sig, v;
  if (t == nullptr || src == nullptr)
    return sig;
  for (uint16_t i = 0; i < t->count; i++) {
    const MqttPoint &p = t->points[i];
    if (!(p.mask & mask))
      continue;
    TelemetrySignal s;
    if (!src->get_signal(d.index, p.sigid, s) || !format_point_value(p, s, v))
      continue;
    sig += std::to_string(p.point_id);
    sig += '=';
    sig += v;
    sig += ';';
  }
  return sig;
}

std::string BusinessFormatter::rundata_second(const TelemetryDevice &d, const TelemetrySource *src,
                                              const std::string &data_time,
                                              const std::vector<uint32_t> &point_ids) const {
  const MqttTable *t = find_mqtt_table(d.table_type);
  if (t == nullptr || d.dev_type == 0)
    return "";
  std::vector<uint16_t> sigs;
  for (uint32_t id : point_ids) {
    for (uint16_t i = 0; i < t->count; i++) {
      if (t->points[i].point_id == static_cast<int32_t>(id)) {
        sigs.push_back(t->points[i].sigid);
        break;
      }
    }
  }
  if (sigs.empty())
    return "";
  JsonDocument doc(large_json_allocator());
  JsonObject o = doc.to<JsonArray>().add<JsonObject>();
  add_header(o, d, src, data_time, "");
  JsonObject data = o["data"].to<JsonObject>();
  std::string v;
  for (uint16_t sig : sigs) {
    const MqttPoint *p = nullptr;
    for (uint16_t i = 0; i < t->count && p == nullptr; i++) {
      if (t->points[i].sigid == sig)
        p = &t->points[i];
    }
    TelemetrySignal s;
    if (p == nullptr || src == nullptr || !src->get_signal(d.index, sig, s))
      continue;
    if (format_point_value(*p, s, v))
      data[std::to_string(p->point_id)] = v;
  }
  return serialize(doc);
}

std::string BusinessFormatter::propertydata(const TelemetryDevice &d, const TelemetrySource *src,
                                            const std::string &data_time, const std::string &batch_id) const {
  const MqttTable *t = find_mqtt_table(d.table_type);
  if (t == nullptr || d.dev_type == 0)
    return "";
  JsonDocument doc(large_json_allocator());
  JsonObject o = doc.to<JsonArray>().add<JsonObject>();
  add_header(o, d, src, data_time, batch_id);
  JsonObject data = o["data"].to<JsonObject>();

  auto get = [&](uint16_t sig, TelemetrySignal &s) { return src != nullptr && src->get_signal(d.index, sig, s) && s.valid; };
  // continuityPoint: the table point ids of these sigs, each only if the sig reads OK; the key is
  // left out when none does.
  static constexpr uint16_t CONT_SIGS[] = {0x2181, 0x3200, 0x221c, 0x221a, 0x202b, 0x202c, 0x231b};
  std::vector<std::string> cont;
  for (uint16_t sig : CONT_SIGS) {
    TelemetrySignal s;
    if (!get(sig, s))
      continue;
    for (uint16_t i = 0; i < t->count; i++) {
      if (t->points[i].sigid == sig && (t->points[i].mask & MQTT_MASK_ATTR)) {
        cont.push_back(std::to_string(t->points[i].point_id));
        break;
      }
    }
  }
  if (!cont.empty()) {
    JsonArray arr = data["continuityPoint"].to<JsonArray>();
    for (const auto &c : cont)
      arr.add(c);
  }
  add_points(data, t, MQTT_MASK_ATTR, d, src);

  // functionEnum: 16 bytes as 32 hex chars. The logger and cloud devType 46 report the logger's
  // bitmap (0xF7 sig 0x104). A device whose table carries the bitmap reports sig 0x2191 (bytes
  // 0-7) + 0x219d (bytes 8-15), each half falling back to the variant default when the store has
  // no such sig. Other tables (meters, ...) have no bitmap: all zeros on the WiNet-S2, and the
  // WiNet-S sends no element at all.
  auto read_bytes = [&](uint16_t sig, size_t n, std::string &b) {
    TelemetrySignal s;
    if (!get(sig, s))
      return false;
    if (s.is_string || !s.bytes.empty()) {
      b = s.bytes.empty() ? s.str : s.bytes;
    } else {
      b.clear();
      for (size_t i = 0; i < 8; i++)
        b += static_cast<char>((s.bits >> (8 * i)) & 0xFF);
    }
    b.resize(n, '\0');
    return true;
  };
  std::string fe = this->device_function_enum_;
  fe.resize(FUNCTION_ENUM_LEN, '\0');
  if (d.dev_type == LOGGER_DEV_TYPE || d.dev_type == FUNCTION_ENUM_LOGGER_DEV_TYPE) {
    std::string own;
    if (d.dev_type == LOGGER_DEV_TYPE && read_bytes(0x0104, FUNCTION_ENUM_LEN, own))
      fe = own;
  } else if (!table_has_function_enum(d.table_type)) {
    if (this->winet_s_)
      return "";
    fe.assign(FUNCTION_ENUM_LEN, '\0');
  } else {
    std::string part;
    if (read_bytes(0x2191, 8, part))
      fe.replace(0, 8, part);
    if (read_bytes(0x219d, 8, part))
      fe.replace(8, 8, part);
    if (d.dev_type == FUNCTION_ENUM_SBR_DEV_TYPE) {
      // Bit 108 follows bit 4 of the first register (big-endian word) of the 8-byte 0x202c block.
      std::string blk;
      const bool on = read_bytes(0x202c, 8, blk) &&
                      ((((static_cast<uint8_t>(blk[0]) << 8) | static_cast<uint8_t>(blk[1])) >> 4) & 1) != 0;
      const uint8_t mask = 1u << (FUNCTION_ENUM_SBR_BIT & 7);
      char &byte = fe[FUNCTION_ENUM_SBR_BIT >> 3];
      byte = static_cast<char>(on ? (byte | mask) : (byte & ~mask));
    }
  }
  o["functionEnum"] = hex(fe);
  o["cmdType"] = "10310";
  TelemetrySignal s;
  o["devName"] = get(0x0001, s) ? raw_string(s) : std::string();
  o["sn"] = get(0x0002, s) ? raw_string(s) : std::string();
  o["devModel"] = get(0x0004, s) ? s.bits : 0;
  const uint16_t ver_sig = (d.dev_type == 1 || d.dev_type == 14) ? 0x5423 : 0x0005;
  o["devVersion"] = get(ver_sig, s) ? raw_string(s) : std::string();
  o["result"] = 1;
  return serialize(doc);
}

std::string BusinessFormatter::devinfo(const DevinfoContext &ctx) const {
  auto endpoint = [](const ServerEndpoint &e) {
    std::string key(reinterpret_cast<const char *>(e.key.data()), strnlen(reinterpret_cast<const char *>(e.key.data()), e.key.size()));
    return e.host + ";" + std::to_string(e.port) + ";" + key;
  };
  JsonDocument doc;
  JsonObject o = doc.to<JsonArray>().add<JsonObject>();
  o["batchId"] = UNSOLICITED_BATCH_ID;
  o["devCode"] = LOGGER_DEV_CODE;
  o["devType"] = LOGGER_DEV_TYPE;
  o["dataTime"] = ctx.data_time;
  JsonObject data = o["data"].to<JsonObject>();
  JsonObject p10461 = data["10461"].to<JsonObject>();
  p10461["10479"] = 2;
  p10461["10480"] = 1;
  p10461["10481"] = 1;
  data["38338"] = endpoint(ctx.server);
  data["38337"] = endpoint(ctx.old_server);
  // 38781 (link-failure bitmask): zeros on both variants; local link errors stay in last_error().
  data["38781"] = "0000000000000000";
  data["5767"] = ctx.sig_1202;
  std::string bits;
  for (int i = 0; i < 32; i++)
    bits += ((1u << i) & 0x7Fu) ? '1' : '0';
  data["5768"] = bits;
  return serialize(doc);
}

std::string BusinessFormatter::beat(const std::vector<TelemetryDevice> &devices) const {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  JsonObject self = arr.add<JsonObject>();
  self["devCode"] = LOGGER_DEV_CODE;
  self["devType"] = LOGGER_DEV_TYPE;
  self["beat"] = 1;  // the logger is always online
  for (const auto &d : devices) {
    if (d.dev_code == LOGGER_DEV_CODE || d.dev_type == 0)
      continue;
    JsonObject o = arr.add<JsonObject>();
    o["devCode"] = d.dev_code;
    o["devType"] = d.dev_type;
    o["beat"] = d.online ? 1 : 0;
  }
  return serialize(doc);
}

}  // namespace esphome::isolarcloud
