#pragma once
// Topic builder / parser (SG-Docs reference/protocols/cloud-mqtt.md). Host-buildable.
//
//   pub/rsp: isolarcloud/devicedata/<pub|rsp>/<family>/<SN>/enum_<encrypt>_<keynum>_<compress>
//   cmd:     isolarcloud/devicedata/cmd/<SN>/<family>/enum_<e>_<k>_<c>
//   group:   isolarcloud/devicedata/cmd/<timezoneId>/enum_0_0_0   (sig 0x107a: per-timezone broadcast)
//   will:    isolarcloud/devicedata/pub/willdata/<SN>/enum_<e>_<k>_<c>

#include <cstdint>
#include <string>
#include <vector>

namespace esphome::isolarcloud {

static constexpr const char *TOPIC_ROOT = "isolarcloud/devicedata/";
static constexpr const char *CMD_PREFIX = "isolarcloud/devicedata/cmd/";

struct TopicEnum {
  uint16_t encrypt{0};
  uint16_t keynum{0};
  uint16_t compress{1};
};

class TopicBuilder {
 public:
  TopicBuilder() = default;
  TopicBuilder(std::string sn, TopicEnum e) : sn_(std::move(sn)), enum_(e) {}

  const std::string &sn() const { return this->sn_; }
  const TopicEnum &topic_enum() const { return this->enum_; }

  /// "enum_<e>_<k>_<c>"
  static std::string enum_suffix(const TopicEnum &e);
  std::string pub(const std::string &family) const;
  std::string rsp(const std::string &family) const { return this->rsp(family, this->enum_); }
  /// rsp with an explicit enum (we answer with the enum of the command topic).
  std::string rsp(const std::string &family, const TopicEnum &e) const;
  std::string cmd(const std::string &family) const;
  std::string will() const { return this->pub("willdata"); }
  /// Per-timezone broadcast (10307 settime), always subscribed, never answered.
  static std::string group_cmd(uint16_t timezone_id);

  /// The cmd/<SN>/<family> topics subscribed under the remote-control mask (sig 0x6492): always
  /// settimezone, plus each group's families when (mask bit1 && group bit). The group topic is
  /// separate (group_cmd). Only our own enum (compress) variant is subscribed, QoS 1.
  std::vector<std::string> subscriptions(uint32_t remote_ctl_mask) const;

 protected:
  std::string sn_;
  TopicEnum enum_;
};

struct ParsedTopic {
  enum class Kind { UNKNOWN, CMD, GROUP_CMD, PUB, RSP };
  Kind kind{Kind::UNKNOWN};
  std::string family;  ///< cmd/pub/rsp family; empty for GROUP_CMD
  std::string sn;      ///< SN (cmd/pub/rsp) or the timezoneId (GROUP_CMD)
  bool has_enum{false};
  TopicEnum topic_enum{0, 0, 0};
};

/// Parse an incoming topic. Returns false for anything outside isolarcloud/devicedata/.
/// The enum suffix is the trailing "/enum_<e>_<k>_<c>"; a missing or malformed suffix leaves
/// has_enum false.
bool parse_topic(const std::string &topic, ParsedTopic &out);

}  // namespace esphome::isolarcloud
