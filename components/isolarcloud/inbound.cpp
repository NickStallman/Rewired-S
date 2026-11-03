#include "inbound.h"

#include <cstdlib>
#include <cstring>

#include "cloud_types.h"
#include "envelope.h"
#include "json_compat.h"

namespace esphome::isolarcloud {

const char *inbound_status_str(InboundStatus s) {
  switch (s) {
    case InboundStatus::OK:
      return "ok";
    case InboundStatus::NOT_OURS:
      return "not an isolarcloud topic";
    case InboundStatus::BRANCH_A:
      return "binary TLV (branch A) dropped";
    case InboundStatus::WRONG_SN:
      return "cmd for another SN";
    case InboundStatus::EMPTY:
      return "empty payload";
    case InboundStatus::TOO_LARGE:
      return "payload > 4096 B";
    case InboundStatus::INFLATE_FAILED:
      return "uncompress failed";
    case InboundStatus::BAD_JSON:
      return "bad JSON";
    case InboundStatus::NO_BUSINESS:
      return "no businessData/cmdType";
    case InboundStatus::DUPLICATE:
      return "same cmd as the previous one";
  }
  return "?";
}

bool CommandDeduper::check_and_add(const InboundCommand &cmd) {
  if (!cmd.has_batch_id)
    return false;
  if (cmd.business_all == this->last_)
    return true;
  this->last_ = cmd.business_all;
  return false;
}

namespace {
// cmdType/batchId should be JSON strings; numbers are tolerated.
std::string as_string(JsonVariantConst v) {
  if (v.isNull())
    return "";
  if (v.is<const char *>())
    return v.as<const char *>();
  if (v.is<long long>())
    return std::to_string(v.as<long long>());
  if (v.is<double>()) {
    std::string s;
    serializeJson(v, s);
    return s;
  }
  return "";
}
}  // namespace

InboundStatus decode_inbound(const std::string &topic, const uint8_t *payload, size_t len, const std::string &own_sn,
                             InboundCommand &out) {
  out = InboundCommand{};
  out.raw_topic = topic;
  parse_topic(topic, out.topic);
  // Topics without the cmd prefix belong to the binary TLV framing (branch A).
  if (topic.find(CMD_PREFIX) == std::string::npos)
    return InboundStatus::BRANCH_A;
  if (out.topic.kind == ParsedTopic::Kind::CMD && !own_sn.empty() && out.topic.sn != own_sn)
    return InboundStatus::WRONG_SN;
  if (len == 0)
    return InboundStatus::EMPTY;
  if (len > MAX_INBOUND_PAYLOAD)
    return InboundStatus::TOO_LARGE;

  // No enum suffix -> uncompressed.
  uint16_t compress = out.topic.has_enum ? out.topic.topic_enum.compress : 0;
  if (!decode_payload(payload, len, compress, out.json, MAX_INBOUND_PAYLOAD))
    return InboundStatus::INFLATE_FAILED;  // (also: inflated > 4096 B)

  JsonDocument doc;
  if (deserializeJson(doc, out.json) != DeserializationError::Ok)
    return InboundStatus::BAD_JSON;

  JsonDocument inner;
  JsonVariantConst bd = doc["businessData"];
  if (bd.is<const char *>()) {  // tolerated: businessData as a JSON string
    if (deserializeJson(inner, bd.as<const char *>()) != DeserializationError::Ok)
      return InboundStatus::NO_BUSINESS;
    bd = inner.as<JsonVariantConst>();
  }
  if (bd.isNull())
    return InboundStatus::NO_BUSINESS;
  serializeJson(bd, out.business_all);
  if (bd.is<JsonArrayConst>()) {
    JsonArrayConst a = bd.as<JsonArrayConst>();
    if (a.size() == 0)
      return InboundStatus::NO_BUSINESS;
    bd = a[0];
  }
  if (!bd.is<JsonObjectConst>())
    return InboundStatus::NO_BUSINESS;
  serializeJson(bd, out.business_json);

  out.cmd_type = as_string(bd["cmdType"]);
  out.has_batch_id = !bd["batchId"].isNull();
  out.batch_id = as_string(bd["batchId"]);
  if (out.cmd_type.empty())  // "mqtt get param set cmdType error"
    return InboundStatus::NO_BUSINESS;
  return InboundStatus::OK;
}

std::string build_reply_business(const InboundCommand &cmd, const std::string &data_time, int result,
                                 const std::string &extra_json) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonArray>().add<JsonObject>();
  o["batchId"] = cmd.batch_id;
  o["cmdType"] = cmd.cmd_type;
  o["dataTime"] = data_time;
  o["result"] = result;
  if (!extra_json.empty()) {
    JsonDocument extra;
    if (deserializeJson(extra, extra_json) == DeserializationError::Ok && extra.is<JsonObject>()) {
      for (JsonPair kv : extra.as<JsonObject>())
        o[kv.key()] = kv.value();
    }
  }
  std::string s;
  serialize_checked(doc, s);
  return s;
}

std::string reply_topic(const TopicBuilder &tb, const InboundCommand &cmd) {
  if (cmd.topic.kind != ParsedTopic::Kind::CMD || cmd.topic.family.empty())
    return "";
  return tb.rsp(cmd.topic.family, tb.topic_enum());
}

const CommandHandler *CommandRouter::find(const std::string &cmd_type) const {
  if (cmd_type.empty())
    return nullptr;
  for (const auto &e : this->table_) {
    if (cmd_type.find(e.first) != std::string::npos)
      return &e.second;
  }
  return nullptr;
}

bool CommandRouter::dispatch(const InboundCommand &cmd) const {
  const CommandHandler *h = this->find(cmd.cmd_type);
  if (h == nullptr)
    return false;
  (*h)(cmd);
  return true;
}

}  // namespace esphome::isolarcloud
