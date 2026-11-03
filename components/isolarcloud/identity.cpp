#include "cloud_types.h"

namespace esphome::isolarcloud {

void apply_identity_fields(CloudIdentity &dst, const CloudIdentity &src, uint32_t mask) {
  if (mask & FIELD_SN)
    dst.sn = src.sn;
  if (mask & FIELD_PRODUCT_TYPE)
    dst.product_type = src.product_type;
  if (mask & FIELD_PRODUCT_NAME)
    dst.product_name = src.product_name;
  if (mask & FIELD_ENCRYPT)
    dst.encrypt = src.encrypt;
  if (mask & FIELD_KEYNUM)
    dst.keynum = src.keynum;
  if (mask & FIELD_COMPRESS)
    dst.compress = src.compress;
  if (mask & FIELD_SERVER_HOST)
    dst.server.host = src.server.host;
  if (mask & FIELD_SERVER_PORT)
    dst.server.port = src.server.port;
  if (mask & FIELD_SERVER_KEY)
    dst.server.key = src.server.key;
  if (mask & FIELD_SERVER_SSL)
    dst.server.ssl_flag = src.server.ssl_flag;
  if (mask & FIELD_OLD_SERVER)
    dst.old_server = src.old_server;
  if (mask & FIELD_TIMEZONE_ID)
    dst.timezone_id = src.timezone_id;
  if (mask & FIELD_TIMEZONE)
    dst.timezone = src.timezone;
  if (mask & FIELD_RUNDATA_PERIOD)
    dst.rundata_period_min = src.rundata_period_min;
  if (mask & FIELD_SILENCE) {
    dst.silence_enable = src.silence_enable;
    dst.silence_start_hour = src.silence_start_hour;
    dst.silence_end_hour = src.silence_end_hour;
  }
  if (mask & FIELD_REMOTE_CTL_MASK)
    dst.remote_ctl_mask = src.remote_ctl_mask;
}

uint16_t cloud_dev_type_for_table(uint16_t table_type) {
  // {table type, cloud devType}, first match wins.
  static constexpr uint16_t MAP[][2] = {
      {0x01, 1},  {0x15, 1},  {0x04, 1},  {0x07, 1},  {0x18, 1},  {0x19, 1},  {0x02, 22}, {0x23, 14},
      {0x05, 7},  {0x06, 7},  {0x12, 7},  {0x08, 4},  {0x09, 21}, {0x0b, 5},  {0x14, 5},  {0x0c, 19},
      {0x0d, 6},  {0x0e, 27}, {0x10, 10}, {0x1d, 10}, {0x16, 32}, {0x1b, 26}, {0x1c, 31}, {0x1e, 23},
      {0x1f, 25}, {0x22, 46}, {0x2c, 43}, {0x2e, 51}, {0x5c, 92},
  };
  for (const auto &m : MAP) {
    if (m[0] == table_type)
      return m[1];
  }
  return 0;
}

namespace {
// Remote-control groups: the cmd/<SN>/<family> topics and the cmdTypes each mask bit gates.
// rsp/apidata/<SN> (also in group 1) is not subscribed: nothing acts on it.
const char *const G1_F[] = {"apidata", "fcas", "propertydata", "generalcmd", nullptr};
const char *const G1_C[] = {"10369", "10310", "10375", nullptr};
const char *const G2_F[] = {"setparam", "getparam", "parammultiset", "parammultiget", nullptr};
const char *const G2_C[] = {"10303", "10325", "10451", "10453", "10384", nullptr};
const char *const G3_F[] = {"second", nullptr};
const char *const G3_C[] = {"10421", nullptr};
const char *const G4_F[] = {"structurefileset", nullptr};
const char *const G4_C[] = {"10452", nullptr};
const char *const G5_F[] = {"upgrade", nullptr};
const char *const G5_C[] = {"10308", "10376", nullptr};
const char *const G6_F[] = {"logexport", nullptr};
const char *const G6_C[] = {"10329", nullptr};
const char *const G7_F[] = {"iv", nullptr};
const char *const G7_C[] = {"10306", "10326", nullptr};
const RemoteCtlGroup GROUPS[] = {
    {1, G1_F, G1_C}, {2, G2_F, G2_C}, {3, G3_F, G3_C}, {4, G4_F, G4_C},
    {5, G5_F, G5_C}, {6, G6_F, G6_C}, {7, G7_F, G7_C},
};
}  // namespace

const RemoteCtlGroup *remote_ctl_groups(size_t *count) {
  *count = sizeof(GROUPS) / sizeof(GROUPS[0]);
  return GROUPS;
}

bool remote_ctl_allows(uint32_t mask, const std::string &cmd_type) {
  for (const auto &g : GROUPS) {
    for (const char *const *c = g.cmd_types; *c != nullptr; c++) {
      if (cmd_type.find(*c) != std::string::npos)  // substring match
        return ((mask >> g.bit) & 1) != 0;
    }
  }
  return true;
}

}  // namespace esphome::isolarcloud
