#include "topics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cloud_types.h"

namespace esphome::isolarcloud {

std::string TopicBuilder::enum_suffix(const TopicEnum &e) {
  char buf[40];
  snprintf(buf, sizeof(buf), "enum_%u_%u_%u", e.encrypt, e.keynum, e.compress);
  return buf;
}

std::string TopicBuilder::pub(const std::string &family) const {
  return std::string(TOPIC_ROOT) + "pub/" + family + "/" + this->sn_ + "/" + enum_suffix(this->enum_);
}

std::string TopicBuilder::rsp(const std::string &family, const TopicEnum &e) const {
  return std::string(TOPIC_ROOT) + "rsp/" + family + "/" + this->sn_ + "/" + enum_suffix(e);
}

std::string TopicBuilder::cmd(const std::string &family) const {
  return std::string(CMD_PREFIX) + this->sn_ + "/" + family + "/" + enum_suffix(this->enum_);
}

std::string TopicBuilder::group_cmd(uint16_t timezone_id) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%s%u/enum_0_0_0", CMD_PREFIX, timezone_id);
  return buf;
}

std::vector<std::string> TopicBuilder::subscriptions(uint32_t remote_ctl_mask) const {
  std::vector<std::string> out;
  out.push_back(this->cmd("settimezone"));
  size_t n = 0;
  const RemoteCtlGroup *groups = remote_ctl_groups(&n);
  for (size_t i = 0; i < n; i++) {
    if (!remote_ctl_group_on(remote_ctl_mask, groups[i].bit))
      continue;
    for (const char *const *f = groups[i].families; *f != nullptr; f++)
      out.push_back(this->cmd(*f));
  }
  return out;
}

namespace {
// Parse "<digits>" into v; returns pointer past the digits or nullptr.
const char *parse_u16(const char *p, uint16_t &v) {
  if (*p < '0' || *p > '9')
    return nullptr;
  unsigned long n = 0;
  while (*p >= '0' && *p <= '9') {
    n = n * 10 + static_cast<unsigned long>(*p - '0');
    if (n > 0xFFFF)
      return nullptr;
    p++;
  }
  v = static_cast<uint16_t>(n);
  return p;
}

bool all_digits(const std::string &s) {
  if (s.empty())
    return false;
  for (char c : s) {
    if (c < '0' || c > '9')
      return false;
  }
  return true;
}
}  // namespace

bool parse_topic(const std::string &topic, ParsedTopic &out) {
  out = ParsedTopic{};
  const size_t root_len = strlen(TOPIC_ROOT);
  if (topic.compare(0, root_len, TOPIC_ROOT) != 0)
    return false;

  // Enum suffix "/enum_<e>_<k>_<c>".
  std::string body = topic.substr(root_len);  // "<dir>/..."
  size_t epos = body.rfind("/enum_");
  if (epos != std::string::npos) {
    const char *p = body.c_str() + epos + 6;
    TopicEnum e;
    if ((p = parse_u16(p, e.encrypt)) != nullptr && *p == '_' && (p = parse_u16(p + 1, e.keynum)) != nullptr &&
        *p == '_' && (p = parse_u16(p + 1, e.compress)) != nullptr && *p == '\0') {
      out.has_enum = true;
      out.topic_enum = e;
    }
    body.resize(epos);  // strip suffix (even if malformed) for the path split below
  }

  size_t slash = body.find('/');
  if (slash == std::string::npos)
    return true;  // e.g. "isolarcloud/devicedata/foo" - inside the tree but unknown
  std::string dir = body.substr(0, slash);
  std::string rest = body.substr(slash + 1);

  if (dir == "cmd") {
    // cmd/<SN>/<family>  or  cmd/<timezoneId>
    size_t s2 = rest.find('/');
    if (s2 == std::string::npos) {
      if (all_digits(rest)) {
        out.kind = ParsedTopic::Kind::GROUP_CMD;
        out.sn = rest;
      }
      return true;
    }
    out.kind = ParsedTopic::Kind::CMD;
    out.sn = rest.substr(0, s2);
    out.family = rest.substr(s2 + 1);
    return true;
  }
  if (dir == "pub" || dir == "rsp") {
    // <family may contain '/'>/<SN>: SN is the last path element.
    size_t s2 = rest.rfind('/');
    if (s2 == std::string::npos)
      return true;
    out.kind = dir == "pub" ? ParsedTopic::Kind::PUB : ParsedTopic::Kind::RSP;
    out.family = rest.substr(0, s2);
    out.sn = rest.substr(s2 + 1);
    return true;
  }
  return true;
}

}  // namespace esphome::isolarcloud
