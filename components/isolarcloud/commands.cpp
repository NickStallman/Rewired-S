#include "commands.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "json_compat.h"

namespace esphome::isolarcloud {

namespace {
bool get_num(JsonObjectConst o, const char *k, int64_t &out) {
  JsonVariantConst v = o[k];
  if (v.is<long long>()) {
    out = v.as<long long>();
    return true;
  }
  if (v.is<const char *>()) {  // tolerated: decimal strings
    const char *s = v.as<const char *>();
    char *end = nullptr;
    long long n = strtoll(s, &end, 10);
    if (end != s && *end == '\0') {
      out = n;
      return true;
    }
  }
  return false;
}
std::string get_str(JsonVariantConst v) {
  if (v.is<const char *>())
    return v.as<const char *>();
  if (v.is<long long>())
    return std::to_string(v.as<long long>());
  return "";
}
bool parse_item(const std::string &json, JsonDocument &doc) {
  return deserializeJson(doc, json) == DeserializationError::Ok && doc.is<JsonObject>();
}
std::string print(JsonDocument &doc) {
  std::string s;
  serialize_checked(doc, s);
  return s;
}
}  // namespace

uint16_t getparam_count(uint8_t data_type) {
  if (data_type >= 1 && data_type <= 4)
    return 1;
  if (data_type == 5 || data_type == 6)
    return 2;
  if (data_type == 7 || data_type == 8)
    return 4;
  return 1;
}

bool parse_getparam(const std::string &item_json, GetParamRequest &out, std::string &err) {
  out = GetParamRequest{};
  JsonDocument doc;
  if (!parse_item(item_json, doc)) {
    err = "bad item";
    return false;
  }
  JsonObjectConst o = doc.as<JsonObjectConst>();
  get_num(o, "devCode", out.dev_code);
  get_num(o, "mode", out.mode);
  if (!get_num(o, "readOnly", out.read_only)) {
    err = "readOnly missing";
    return false;
  }
  JsonObjectConst data = o["data"];
  if (data.isNull()) {
    err = "data missing";
    return false;
  }
  for (JsonPairConst kv : data) {
    JsonObjectConst p = kv.value();
    GetParamPoint gp;
    gp.key = kv.key().c_str();
    gp.modbus_address = get_str(p["modbusAddress"]);
    char *end = nullptr;
    unsigned long reg = strtoul(gp.modbus_address.c_str(), &end, 10);
    if (gp.modbus_address.empty() || *end != '\0' || reg == 0 || reg > 0xFFFF) {
      err = "bad modbusAddress";
      return false;
    }
    gp.reg = static_cast<uint16_t>(reg);
    gp.data_type = static_cast<uint8_t>(atoi(get_str(p["dataType"]).c_str()));
    gp.count = getparam_count(gp.data_type);
    out.points.push_back(std::move(gp));
  }
  return true;
}

std::string getparam_read_value(uint8_t data_type, const uint8_t *regs, uint16_t n) {
  char buf[40];
  uint64_t v = 0;
  for (uint16_t i = 0; i < n && i < 4; i++)
    v |= ((static_cast<uint64_t>(regs[2 * i]) << 8) | regs[2 * i + 1]) << (16 * i);
  switch (data_type) {
    case 1:
      snprintf(buf, sizeof(buf), "%d", static_cast<int8_t>(v & 0xFF));
      break;
    case 2:
      snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(v & 0xFF));
      break;
    case 3:
      snprintf(buf, sizeof(buf), "%d", static_cast<int16_t>(v & 0xFFFF));
      break;
    case 4:
      snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(v & 0xFFFF));
      break;
    case 5:
      snprintf(buf, sizeof(buf), "%" PRId32, static_cast<int32_t>(v & 0xFFFFFFFFu));
      break;
    case 6:
      snprintf(buf, sizeof(buf), "%" PRIu32, static_cast<uint32_t>(v & 0xFFFFFFFFu));
      break;
    case 7:
    case 8:
      snprintf(buf, sizeof(buf), "%" PRId64, static_cast<int64_t>(v));
      break;
    case 12: {
      std::string s(reinterpret_cast<const char *>(regs), 2u * n);
      size_t z = s.find('\0');
      return z == std::string::npos ? s : s.substr(0, z);
    }
    default:
      snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(v & 0xFFFF));
      break;
  }
  return buf;
}

std::string build_getparam_reply(const InboundCommand &cmd, const std::string &data_time, const GetParamRequest &req,
                                 int result, const std::vector<GetParamResult> &values) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonArray>().add<JsonObject>();
  o["batchId"] = cmd.batch_id;
  o["cmdType"] = cmd.cmd_type;
  o["dataTime"] = data_time;
  o["mode"] = req.mode;
  o["readOnly"] = req.read_only;
  o["result"] = result;
  if (result != 0) {
    JsonObject data = o["data"].to<JsonObject>();
    for (const auto &v : values) {
      JsonObject e = data[v.key].to<JsonObject>();
      e["modbusAddress"] = v.modbus_address;
      e["readValue"] = v.read_value;
      e["readResult"] = v.read_result;
    }
  }
  return print(doc);
}

bool setparam_encode(uint8_t data_type, const std::string &value, std::vector<uint16_t> &regs, std::string &echo) {
  regs.clear();
  echo.clear();
  if (value.empty())
    return false;
  const char *p = value.c_str();
  char *end = nullptr;
  uint64_t raw = 0;
  unsigned n = 0;
  char buf[32];
  switch (data_type) {
    case 3: {
      long v = strtol(p, &end, 10);
      if (*end != '\0' || v < -32768 || v > 32767)
        return false;
      raw = static_cast<uint16_t>(static_cast<int16_t>(v));
      n = 1;
      snprintf(buf, sizeof(buf), "%ld", v);
      break;
    }
    case 4: {
      unsigned long v = strtoul(p, &end, 10);
      if (*end != '\0' || value[0] == '-' || v > 0xFFFF)
        return false;
      raw = v;
      n = 1;
      snprintf(buf, sizeof(buf), "%lu", v);
      break;
    }
    case 5: {
      long long v = strtoll(p, &end, 10);
      if (*end != '\0' || v < INT32_MIN || v > INT32_MAX)
        return false;
      raw = static_cast<uint32_t>(static_cast<int32_t>(v));
      n = 2;
      snprintf(buf, sizeof(buf), "%lld", v);
      break;
    }
    case 6: {
      unsigned long long v = strtoull(p, &end, 0);  // base 0: "0x..." accepted
      if (*end != '\0' || value[0] == '-' || v > 0xFFFFFFFFull)
        return false;
      raw = v;
      n = 2;
      snprintf(buf, sizeof(buf), "%llu", v);
      break;
    }
    case 7: {
      long long v = strtoll(p, &end, 10);
      if (*end != '\0')
        return false;
      raw = static_cast<uint64_t>(v);
      n = 4;
      snprintf(buf, sizeof(buf), "%lld", v);
      break;
    }
    case 8: {
      unsigned long long v = strtoull(p, &end, 10);
      if (*end != '\0' || value[0] == '-')
        return false;
      raw = v;
      n = 4;
      snprintf(buf, sizeof(buf), "%llu", v);
      break;
    }
    default:
      return false;
  }
  for (unsigned i = 0; i < n; i++)
    regs.push_back(static_cast<uint16_t>((raw >> (16 * i)) & 0xFFFF));
  echo = buf;
  return true;
}

const char *setparam_protected_range(uint32_t reg, size_t count) {
  struct Range {
    uint32_t first, last;
    const char *name;
  };
  static constexpr Range RANGES[] = {
      {16000, 17500, "DSP-forward window"},     {19101, 20200, "firmware upload channel"},
      {21001, 21020, "temporary baud rate"},    {29500, 29900, "DSP-forward window"},
      {30456, 30460, "RS485 baud rate / address"}, {36099, 36099, "DSP raw relay"},
      {36702, 38999, "DSP-forward window"},
  };
  if (count == 0)
    count = 1;
  const uint32_t last = reg + static_cast<uint32_t>(count) - 1;
  for (const auto &r : RANGES) {
    if (reg <= r.last && last >= r.first)
      return r.name;
  }
  return nullptr;
}

bool parse_setparam(const std::string &item_json, SetParamRequest &out, std::string &err) {
  out = SetParamRequest{};
  JsonDocument doc;
  if (!parse_item(item_json, doc)) {
    err = "bad item";
    return false;
  }
  JsonObjectConst o = doc.as<JsonObjectConst>();
  get_num(o, "devCode", out.dev_code);
  get_num(o, "mode", out.mode);
  JsonObjectConst data = o["data"];
  if (data.isNull()) {
    err = "data missing";
    return false;
  }
  for (JsonPairConst kv : data) {
    JsonObjectConst p = kv.value();
    SetParamPoint sp;
    sp.key = kv.key().c_str();
    sp.modbus_address = get_str(p["modbusAddress"]);
    char *end = nullptr;
    unsigned long reg = strtoul(sp.modbus_address.c_str(), &end, 10);
    if (sp.modbus_address.empty() || *end != '\0' || reg == 0 || reg > 0xFFFF) {
      err = "bad modbusAddress";
      return false;
    }
    sp.reg = static_cast<uint16_t>(reg);
    const std::string dt = get_str(p["dataType"]);
    if (dt.empty()) {
      err = "dataType missing";
      return false;
    }
    sp.data_type = static_cast<uint8_t>(atoi(dt.c_str()));
    sp.data_value = get_str(p["dataValue"]);
    sp.encodable = setparam_encode(sp.data_type, sp.data_value, sp.values, sp.echo);
    out.points.push_back(std::move(sp));
  }
  if (out.points.empty()) {
    err = "no points";
    return false;
  }
  return true;
}

std::string build_setparam_reply(const InboundCommand &cmd, const std::string &data_time, const SetParamRequest &req,
                                 const std::vector<GetParamResult> &values) {
  GetParamRequest g;
  g.dev_code = req.dev_code;
  g.mode = req.mode;
  g.read_only = 0;
  return build_getparam_reply(cmd, data_time, g, 1, values);
}

bool parse_logger_param_set(const std::string &item_json, LoggerParamSet &out, std::string &err) {
  out = LoggerParamSet{};
  JsonDocument doc;
  if (!parse_item(item_json, doc)) {
    err = "bad item";
    return false;
  }
  JsonObjectConst o = doc.as<JsonObjectConst>();
  get_num(o, "devCode", out.dev_code);
  if (out.dev_code != LOGGER_DEV_CODE) {
    err = "devCode must be 247";
    return false;
  }
  JsonObjectConst data = o["data"];
  if (data.isNull() || data.size() == 0) {
    err = "data missing";
    return false;
  }
  JsonPairConst first = *data.begin();  // first child only
  out.point = first.key().c_str();
  JsonObjectConst p = first.value();
  out.data_type = get_str(p["dataType"]);
  out.data_value = get_str(p["dataValue"]);
  out.has_ssl_flag = !p["sslFlag"].isNull();
  out.ssl_flag = get_str(p["sslFlag"]);
  return true;
}

bool parse_server_switch(const LoggerParamSet &p, ServerEndpoint &out) {
  const std::string &v = p.data_value;
  size_t a = v.find(';');
  if (a == std::string::npos || a == 0 || a > 0x40)
    return false;
  size_t b = v.find(';', a + 1);
  if (b == std::string::npos)
    return false;
  ServerEndpoint e;
  e.host = v.substr(0, a);
  int port = atoi(v.substr(a + 1, b - a - 1).c_str());
  if (port <= 0 || port > 0xFFFF)
    return false;
  e.port = static_cast<uint16_t>(port);
  e.key = key_from_string(v.substr(b + 1).c_str());
  int ssl = p.has_ssl_flag ? atoi(p.ssl_flag.c_str()) : 0;
  if (ssl == 1)
    ssl = 2;
  e.ssl_flag = static_cast<uint16_t>(ssl);
  out = e;
  return true;
}

std::string build_logger_param_reply(const InboundCommand &cmd, const std::string &data_time, const LoggerParamSet &p,
                                     const std::string &read_value, int read_result) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonArray>().add<JsonObject>();
  o["batchId"] = cmd.batch_id;
  o["cmdType"] = cmd.cmd_type;
  o["dataTime"] = data_time;
  o["result"] = 1;
  JsonObject e = o["data"].to<JsonObject>()[p.point].to<JsonObject>();
  e["readValue"] = read_value;
  e["readResult"] = read_result;
  e["sslFlag"] = p.has_ssl_flag ? atoi(p.ssl_flag.c_str()) : 0;
  return print(doc);
}

bool parse_settimezone(const std::string &item_json, SetTimezone &out) {
  JsonDocument doc;
  if (!parse_item(item_json, doc))
    return false;
  JsonObjectConst o = doc.as<JsonObjectConst>();
  int64_t id = 0;
  if (!get_num(o, "timezoneId", id) || id < 0 || id > 0xFFFF)
    return false;
  std::string tz = get_str(o["timezone"]);
  if (!settimezone_string_ok(tz))
    return false;
  out.timezone_id = static_cast<uint16_t>(id);
  out.timezone = tz;
  return true;
}

bool parse_settime(const std::string &item_json, CivilTime &out) {
  JsonDocument doc;
  if (!parse_item(item_json, doc))
    return false;
  return parse_cloud_time(get_str(doc["dataTime"]), out);
}

bool parse_dev_code_arr(const std::string &item_json, std::vector<uint16_t> &out) {
  out.clear();
  JsonDocument doc;
  if (!parse_item(item_json, doc))
    return false;
  JsonArrayConst arr = doc["devCodeArr"];
  if (arr.isNull())
    return false;
  for (JsonVariantConst v : arr) {
    if (v.is<long long>())
      out.push_back(static_cast<uint16_t>(v.as<long long>()));
    else if (v.is<const char *>())
      out.push_back(static_cast<uint16_t>(atoi(v.as<const char *>())));
  }
  return true;
}

// ---- 10421 second ----------------------------------------------------------------------------
bool parse_second_request(const std::string &item_json, SecondRequest &out, std::string &err) {
  out = SecondRequest{};
  JsonDocument doc;
  if (!parse_item(item_json, doc)) {
    err = "bad item";
    return false;
  }
  JsonObjectConst o = doc.as<JsonObjectConst>();
  JsonArrayConst arr = o["devCodeArr"];
  if (arr.isNull()) {
    err = "devCodeArr missing";
    return false;
  }
  for (JsonVariantConst v : arr) {
    int64_t code = 0;
    if (v.is<long long>())
      code = v.as<long long>();
    else if (v.is<const char *>())
      code = strtoll(v.as<const char *>(), nullptr, 10);
    else
      continue;
    out.dev_codes.push_back(code);
  }
  if (!get_num(o, "devSwitch", out.dev_switch)) {
    err = "devSwitch missing";
    return false;
  }
  JsonObjectConst data = o["data"];
  out.has_data = !data.isNull();
  if (out.has_data) {
    for (JsonPairConst kv : data) {
      const char *key = kv.key().c_str();
      if (key == nullptr || *key == '\0') {
        out.null_key = true;
        continue;
      }
      std::vector<uint32_t> ids;
      JsonArrayConst list = kv.value();
      for (JsonVariantConst v : list) {
        if (v.is<long long>())
          ids.push_back(static_cast<uint32_t>(v.as<long long>()));
        else if (v.is<const char *>())
          ids.push_back(static_cast<uint32_t>(strtoul(v.as<const char *>(), nullptr, 10)));
      }
      out.data.emplace_back(strtoll(key, nullptr, 10), std::move(ids));
    }
  }
  return true;
}

SecondNode *SecondData::find_(uint16_t dev_code) {
  for (auto &n : this->nodes_) {
    if (n.dev_code == dev_code)
      return &n;
  }
  return nullptr;
}

const SecondNode *SecondData::node(uint16_t dev_code) const {
  for (const auto &n : this->nodes_) {
    if (n.dev_code == dev_code)
      return &n;
  }
  return nullptr;
}

bool SecondData::any_enabled_() const {
  for (const auto &n : this->nodes_) {
    if (n.enabled)
      return true;
  }
  return false;
}

bool SecondData::active() const { return this->enable_ && this->any_enabled_(); }

void SecondData::clear() {
  this->nodes_.clear();
  this->enable_ = false;
}

SecondData::Outcome SecondData::apply(const SecondRequest &r) {
  Outcome o;
  if (r.dev_switch == 1) {
    o.dev_switch = 1;
    for (int64_t code64 : r.dev_codes) {
      const uint16_t code = static_cast<uint16_t>(code64 & 0xFF);  // devCodes are kept as bytes
      SecondNode *n = this->find_(code);
      if (n == nullptr) {
        if (this->nodes_.size() >= MAX_NODES) {
          o.result = 4;
          break;
        }
        this->nodes_.push_back(SecondNode{code, false, {}});
        n = &this->nodes_.back();
      }
      if (!r.has_data || r.null_key) {
        o.result = 0;  // the enable flag is left as it was
        return o;
      }
      const std::vector<uint32_t> *ids = nullptr;
      for (const auto &kv : r.data) {
        if (static_cast<uint16_t>(kv.first & 0xFF) == code) {
          ids = &kv.second;
          break;
        }
      }
      if (ids == nullptr)
        continue;  // node kept, unchanged
      if (ids->size() > MAX_POINTS) {
        o.result = 3;
        break;
      }
      n->enabled = true;
      n->point_ids = *ids;
    }
    this->enable_ = true;  // on for results 1, 3 and 4
    return o;
  }
  o.dev_switch = 0;
  if (r.dev_switch == 0) {
    for (int64_t code64 : r.dev_codes) {
      const uint16_t code = static_cast<uint16_t>(code64 & 0xFF);
      for (auto it = this->nodes_.begin(); it != this->nodes_.end(); ++it) {
        if (it->dev_code == code) {
          this->nodes_.erase(it);
          break;
        }
      }
    }
    // The flag follows whether any node is still on.
    if (!this->any_enabled_())
      this->enable_ = false;
    return o;
  }
  this->clear();  // "timeout cmd": everything off
  return o;
}

std::string SecondData::serialize() const {
  if (this->nodes_.empty() && !this->enable_)
    return "";
  std::string s = this->enable_ ? "E1" : "E0";
  for (const auto &n : this->nodes_) {
    s += ";" + std::to_string(n.dev_code) + ":" + (n.enabled ? "1" : "0") + ":";
    for (size_t i = 0; i < n.point_ids.size(); i++) {
      if (i != 0)
        s += ",";
      s += std::to_string(n.point_ids[i]);
    }
  }
  return s;
}

bool SecondData::deserialize(const std::string &s) {
  this->clear();
  if (s.empty())
    return true;
  if (s.size() < 2 || s[0] != 'E')
    return false;
  bool enable = s[1] == '1';
  size_t pos = 2;
  std::vector<SecondNode> nodes;
  while (pos < s.size()) {
    if (s[pos] != ';')
      return false;
    size_t end = s.find(';', pos + 1);
    if (end == std::string::npos)
      end = s.size();
    const std::string part = s.substr(pos + 1, end - pos - 1);
    const size_t c1 = part.find(':');
    const size_t c2 = c1 == std::string::npos ? std::string::npos : part.find(':', c1 + 1);
    if (c2 == std::string::npos || nodes.size() >= MAX_NODES)
      return false;
    SecondNode n;
    n.dev_code = static_cast<uint16_t>(strtoul(part.substr(0, c1).c_str(), nullptr, 10));
    n.enabled = part[c1 + 1] == '1';
    const std::string ids = part.substr(c2 + 1);
    size_t p = 0;
    while (p < ids.size() && n.point_ids.size() < MAX_POINTS) {
      size_t q = ids.find(',', p);
      if (q == std::string::npos)
        q = ids.size();
      n.point_ids.push_back(static_cast<uint32_t>(strtoul(ids.substr(p, q - p).c_str(), nullptr, 10)));
      p = q + 1;
    }
    nodes.push_back(std::move(n));
    pos = end;
  }
  this->nodes_ = std::move(nodes);
  this->enable_ = enable;
  return true;
}

std::string build_second_reply(const InboundCommand &cmd, const std::string &data_time, const SecondRequest &req,
                               const SecondData::Outcome &o) {
  JsonDocument doc;
  JsonObject e = doc.to<JsonArray>().add<JsonObject>();
  e["batchId"] = cmd.batch_id;
  e["cmdType"] = cmd.cmd_type;
  e["dataTime"] = data_time;
  e["result"] = o.result;
  e["devSwitch"] = o.dev_switch;
  JsonArray arr = e["devCodeArr"].to<JsonArray>();
  for (int64_t c : req.dev_codes)
    arr.add(std::to_string(c));
  return print(doc);
}

}  // namespace esphome::isolarcloud
