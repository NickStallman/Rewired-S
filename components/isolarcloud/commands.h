#pragma once
// Request parsing / reply building for the cloud commands we implement (SG-Docs
// reference/protocols/cloud-commands.md). Host-buildable; isolarcloud.cpp does the I/O.
//
//   10325 getparam      Modbus read: data{"<key>":{modbusAddress,dataType}}, devCode (247 default),
//                       mode, readOnly (required). FC = mode == 1 ? 0x77 : (readOnly ? 4 : 3).
//   10384 logger set    devCode 247, first data child only: point 4 -> 0x1015 rundata period,
//                       10457 -> server switch "host;port;key" (+ sslFlag), others refused.
//   10324 settimezone   timezoneId (num) + timezone ("UTC±hh:mm", exactly 9 chars).
//   10307 settime       dataTime "YYYYMMDDhhmmss" (local), from the timezone broadcast; no reply.
//   10310 propertydata  devCodeArr [num] -> attribute dump of its first online device.
//   10421 second        Live data: devSwitch 1 + data{"<devCode>":[pointId]} turns per-device
//                       10 s rundata/second on, 0 turns the listed devices off, other values all.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "cloud_time.h"
#include "cloud_types.h"
#include "inbound.h"

namespace esphome::isolarcloud {

// ---- 10325 getparam ---------------------------------------------------------------------------
struct GetParamPoint {
  std::string key;             ///< data key as received
  std::string modbus_address;  ///< as received (echoed in the reply)
  uint16_t reg{0};             ///< document (1-based) register number; wire = reg - 1
  uint8_t data_type{4};
  uint16_t count{1};
};
struct GetParamRequest {
  int64_t dev_code{LOGGER_DEV_CODE};
  int64_t mode{0};
  int64_t read_only{0};
  std::vector<GetParamPoint> points;
  uint8_t fn() const { return this->mode == 1 ? 0x77 : (this->read_only != 0 ? 4 : 3); }
};
/// Registers for a dataType: 1-4 -> 1, 5-6 -> 2, 7-8 -> 4, else 1.
uint16_t getparam_count(uint8_t data_type);
/// Parse the command item. False (no reply, like stock) on a parse error.
bool parse_getparam(const std::string &item_json, GetParamRequest &out, std::string &err);
/// readValue for `n` registers as read (big-endian bytes per register). Multi-register values are
/// low word first (the device byte order the cloud expects): 1 "%d" s8, 2 "%u" u8, 3 "%d" s16,
/// 4 "%u" u16, 5 "%ld" s32, 6 "%lu" u32, 7/8 "%lld", 12 raw string.
std::string getparam_read_value(uint8_t data_type, const uint8_t *regs, uint16_t n);
struct GetParamResult {
  std::string key;
  std::string modbus_address;
  std::string read_value;
  std::string read_result{"4"};  ///< "1" ok, "4" no/invalid reply, "0" exception or refused, "2" busy
};
/// [{batchId,cmdType,dataTime,mode,readOnly,result,data:{key:{modbusAddress,readValue,readResult}}}]
/// (result 0 = device offline, then no data).
std::string build_getparam_reply(const InboundCommand &cmd, const std::string &data_time, const GetParamRequest &req,
                                 int result, const std::vector<GetParamResult> &values);

// ---- 10303 setparam ---------------------------------------------------------------------------
// Every point is written with FC 16 (mode 1 = private FC 0x76, which we refuse), register =
// document number (wire = modbusAddress - 1), values low word first. devCode 247 is handled as a
// 10384 logger-point set.
struct SetParamPoint {
  std::string key;
  std::string modbus_address;
  uint16_t reg{0};  ///< document (1-based) register number
  uint8_t data_type{4};
  std::string data_value;
  bool encodable{false};         ///< dataType 3-8 with a valid number
  std::vector<uint16_t> values;  ///< registers to write, wire order
  std::string echo;              ///< readValue: the value as encoded
};
struct SetParamRequest {
  int64_t dev_code{LOGGER_DEV_CODE};
  int64_t mode{0};
  std::vector<SetParamPoint> points;
};
/// Register ranges (1-based doc numbers) a cloud setparam is not allowed to touch, because a write
/// there can cut the dongle off from the inverter or bypass the inverter's own checks. None of them
/// is a setting the app writes:
///   16000-17500, 29500-29900, 36702-38999  DSP-forward windows (37004 = 0xAA captures the port)
///   19101-20200                           firmware upload (IAP) channel, inverter and batteries
///   21001-21020                           temporary baud rate and its test window
///   30456-30460                           persistent baud rate, parity, stop bits, slave address
///   36099                                 DSP raw relay for slave 0xA5
/// Returns the name of the first range that [reg, reg + count) overlaps, or nullptr.
const char *setparam_protected_range(uint32_t reg, size_t count);
/// Parse the command item. False (no reply, like stock) on a parse error.
bool parse_setparam(const std::string &item_json, SetParamRequest &out, std::string &err);
/// Encode a dataValue: 3/4 one register, 5/6 two, 7/8 four, low word first. Other dataTypes are
/// refused.
bool setparam_encode(uint8_t data_type, const std::string &value, std::vector<uint16_t> &regs, std::string &echo);
/// Reply: the getparam shape with cmdType 10303, readOnly 0, readValue = the value as encoded.
std::string build_setparam_reply(const InboundCommand &cmd, const std::string &data_time, const SetParamRequest &req,
                                 const std::vector<GetParamResult> &values);

// ---- 10384 logger parameter set ---------------------------------------------------------------
struct LoggerParamSet {
  int64_t dev_code{LOGGER_DEV_CODE};
  std::string point;       ///< first data key
  std::string data_type;
  std::string data_value;
  bool has_ssl_flag{false};
  std::string ssl_flag;
};
bool parse_logger_param_set(const std::string &item_json, LoggerParamSet &out, std::string &err);
/// 10457: "host;port;key16", sslFlag absent -> 0, "1" -> 2.
bool parse_server_switch(const LoggerParamSet &p, ServerEndpoint &out);
/// [{batchId,cmdType,dataTime,result:1,data:{"<pt>":{readValue,readResult,sslFlag}}}]
/// readValue echoes the request's dataValue, readResult is a number (1 ok, 5 fail) and sslFlag is
/// always present as a number (the request's value, 0 when it had none).
std::string build_logger_param_reply(const InboundCommand &cmd, const std::string &data_time, const LoggerParamSet &p,
                                     const std::string &read_value, int read_result);

// ---- 10324 / 10307 / 10310 ---------------------------------------------------------------------
struct SetTimezone {
  uint16_t timezone_id{DEFAULT_TIMEZONE_ID};
  std::string timezone;
};
/// False for bad input (no reply).
bool parse_settimezone(const std::string &item_json, SetTimezone &out);
bool parse_settime(const std::string &item_json, CivilTime &out);
/// devCodeArr as numbers. False when the item has no devCodeArr array (no reply).
bool parse_dev_code_arr(const std::string &item_json, std::vector<uint16_t> &out);

// ---- 10421 second (live data) -----------------------------------------------------------------
struct SecondRequest {
  std::vector<int64_t> dev_codes;  ///< devCodeArr (required, numbers)
  int64_t dev_switch{0};           ///< devSwitch (required): 1 on, 0 off listed, other = all off
  bool has_data{false};            ///< `data` object present
  bool null_key{false};            ///< `data` has an empty key (stock: result 0)
  /// data{"<devCode>":[pointId...]} by atoi(key), in key order.
  std::vector<std::pair<int64_t, std::vector<uint32_t>>> data;
};
/// False (no reply, nodes untouched) when devCodeArr / devSwitch are missing.
bool parse_second_request(const std::string &item_json, SecondRequest &out, std::string &err);

struct SecondNode {
  uint16_t dev_code{0};
  bool enabled{false};
  std::vector<uint32_t> point_ids;  ///< request order, duplicates kept
};

/// The live-data state: up to 15 per-device nodes plus the global enable (logger sig 0x101a).
class SecondData {
 public:
  static constexpr size_t MAX_NODES = 15;
  static constexpr size_t MAX_POINTS = 400;
  struct Outcome {
    int result{1};      ///< 1 ok, 0 data missing, 3 > 400 points, 4 no free node
    int dev_switch{0};  ///< echoed devSwitch (0 for the "all off" form)
  };
  Outcome apply(const SecondRequest &r);
  /// Global enable (0x101a) and at least one enabled node.
  bool active() const;
  bool enable_flag() const { return this->enable_; }
  const SecondNode *node(uint16_t dev_code) const;
  const std::vector<SecondNode> &nodes() const { return this->nodes_; }
  void clear();
  /// Compact text form for persistence: "E;<code>:<0|1>:<id>,<id>;..." ("" when cleared).
  std::string serialize() const;
  bool deserialize(const std::string &s);

 protected:
  SecondNode *find_(uint16_t dev_code);
  bool any_enabled_() const;
  std::vector<SecondNode> nodes_;
  bool enable_{false};
};

/// Sync reply on rsp/second: [{batchId,cmdType,dataTime,result,devSwitch,devCodeArr:["<code>"]}].
std::string build_second_reply(const InboundCommand &cmd, const std::string &data_time, const SecondRequest &req,
                               const SecondData::Outcome &o);

}  // namespace esphome::isolarcloud
