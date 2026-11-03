#pragma once
// Inbound cloud messages (SG-Docs reference/protocols/cloud-envelope.md). Host-buildable.
//
// Branch B (topic contains isolarcloud/devicedata/cmd/): parse /enum_e_k_c of the TOPIC, inflate
// if its compress != 0, parse JSON, take businessData[0] (businessData should be an array; an
// object or a JSON string is tolerated), read cmdType / batchId, drop an exact repeat of the
// previous command's businessData ("recv same cmd", depth 1, only when batchId is present), gate
// by the remote-control mask, dispatch by cmdType only, then clear the retained command with an
// empty QoS1 retained publish to the incoming topic. Payloads > 4096 B (raw or inflated) are
// dropped. No messageId/packgeId handling: every PUBLISH stands alone.
// Branch A (any other topic): binary TLV, not reachable from iSolarCloud -> dropped.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "topics.h"

namespace esphome::isolarcloud {

enum class InboundStatus {
  OK,
  NOT_OURS,        ///< outside isolarcloud/devicedata/
  BRANCH_A,        ///< non-cmd topic: binary TLV channel, dropped
  WRONG_SN,        ///< cmd topic for another SN
  EMPTY,           ///< zero-length payload (e.g. our own retained clear coming back)
  TOO_LARGE,       ///< > 4096 B raw or inflated
  INFLATE_FAILED,  ///< "mqtt get data uncompress failed!!!"
  BAD_JSON,
  NO_BUSINESS,     ///< no businessData / item / cmdType ("nFuncData err", "nSubFuncData err")
  DUPLICATE,       ///< same businessData as the previous command ("recv same cmd:%s")
};
const char *inbound_status_str(InboundStatus s);

struct InboundCommand {
  std::string raw_topic;      ///< the incoming topic (for the retained clear)
  ParsedTopic topic;
  std::string json;           ///< decoded payload
  std::string business_json;  ///< printed businessData[0] (the command item)
  std::string business_all;   ///< printed businessData as received (de-dup key)
  std::string cmd_type;       ///< cmdType as string
  std::string batch_id;
  bool has_batch_id{false};
};

/// Depth-1 de-dup: the printed businessData is compared with the previous command's, only when
/// the item has a batchId.
class CommandDeduper {
 public:
  /// true = identical to the previous command (drop); otherwise it becomes the new "previous".
  bool check_and_add(const InboundCommand &cmd);
  void clear() { this->last_.clear(); }

 protected:
  std::string last_;
};

/// Decode one inbound message. `own_sn` filters cmd/<SN>/... topics; group topics always pass.
InboundStatus decode_inbound(const std::string &topic, const uint8_t *payload, size_t len, const std::string &own_sn,
                             InboundCommand &out);

// ---- responses -------------------------------------------------------------------------------
/// Reply businessData: [{"batchId":"…","cmdType":"…","dataTime":"YYYYMMDDhhmmss","result":<n>, …}]
/// with the members of `extra_json` (a JSON object, may be empty) appended. No devCode/devType.
std::string build_reply_business(const InboundCommand &cmd, const std::string &data_time, int result,
                                 const std::string &extra_json = "");

/// Reply topic: rsp/<family of the incoming topic>/<SN>/enum_<own encrypt>_<own keynum>_<own
/// compress>. Group-topic (and malformed) commands get "" = no reply.
std::string reply_topic(const TopicBuilder &tb, const InboundCommand &cmd);
/// Replies are encoded with our own 0x22.
inline uint16_t reply_compress(const TopicBuilder &tb, const InboundCommand & /*cmd*/) { return tb.topic_enum().compress; }

// ---- dispatch -------------------------------------------------------------------------------
/// A handler replies itself (synchronously or later) through the component; nothing is sent for
/// commands without a handler.
using CommandHandler = std::function<void(const InboundCommand &)>;

/// Routing by cmdType only, in registration order, first entry whose code is a substring of
/// cmdType. The topic family is not looked at.
class CommandRouter {
 public:
  void on_cmd_type(const std::string &cmd_type, CommandHandler h) { this->table_.emplace_back(cmd_type, std::move(h)); }
  /// True if a handler ran.
  bool dispatch(const InboundCommand &cmd) const;
  /// The entry that would handle `cmd_type`, or nullptr.
  const CommandHandler *find(const std::string &cmd_type) const;

 protected:
  std::vector<std::pair<std::string, CommandHandler>> table_;
};

}  // namespace esphome::isolarcloud
