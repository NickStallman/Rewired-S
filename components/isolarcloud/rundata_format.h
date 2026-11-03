#pragma once
// businessData builders for rundata / propertydata / devstatusdata / generaldata / devinfo /
// beatdata (SG-Docs reference/protocols/cloud-messages.md).
//
// Table driven: each device's *table type* selects its mqtt point table (mqtt_point_table.h). For
// every table row with the wanted attr mask (0x2 rundata, 0x1 attributes), in table order, the
// device's DM signal is read and emitted as  "<pointId>":"<value string>":
//
//   fmt 1-6     raw integer, no scale ("%u", or "%d" for a signed sig type)
//   fmt 7,8     raw 64-bit ("%lld" for sig type 7, else "%llu")
//   fmt 9       (double)(float)raw * scale   "%.10g" (len 2) / "%.13g" (len 4)
//   fmt 10      (double)raw * scale          "%.10g" / "%.13g" / "%.19g" (len 2 / 4 / 8)
//   fmt 12      string up to the first NUL
//   fmt 13,15-19,101  lowercase hex of every DM byte
//   other       ""
//
// "raw" is the DM value converted to the DM type of the sig (size/sig_type columns), NOT our
// display-scaled number. A signal is dropped when we do not have it, it is invalid, or it holds
// the sentinel of the sig type (s16 0x7fff, u16 0xffff, s32 0x7fffffff, u32 0xffffffff, s64/u64
// max, all-NUL string; 8-bit types always). An element whose signals are all dropped is still
// sent, with "data":{}.
//
// Element, keys in this order:
//   {["batchId":"…",]"devCode":N,"devType":T,["upDevType":U,"upDevCode":C,]"dataTime":"YYYYMMDDhhmmss",
//    "data":{"<pointId>":"<value>",…}}
// devCode / devType / upDev* are JSON numbers. upDevType/upDevCode are always present for cloud
// devTypes 43 and 92 (0 when the parent sigs cannot be read). rundata carries one element per
// message (one message per device, the logger 247/22 included).

#include <cstdint>
#include <string>
#include <vector>

#include "cloud_types.h"
#include "interfaces.h"
#include "mqtt_point_table.h"

namespace esphome::isolarcloud {

/// Format one point. Returns false when the signal must be dropped (missing / invalid / sentinel).
bool format_point_value(const MqttPoint &p, const TelemetrySignal &s, std::string &out);

/// Logger-only devinfo inputs.
struct DevinfoContext {
  std::string data_time;
  ServerEndpoint server;      ///< 0x1071 / 0x1072 / 0x1013 -> "38338"
  ServerEndpoint old_server;  ///< 0x1957 / 0x1958 / 0x1959 -> "38337"
  std::string sig_1202;       ///< 0xF7/0x1202 (20 B string) -> "5767"
};

/// The logger as a telemetry device: devCode 247, table type 2, devType 22.
TelemetryDevice logger_device();

class BusinessFormatter {
 public:
  virtual ~BusinessFormatter() = default;
  /// rundata/5min businessData for ONE device: a one-element array. `src` may be nullptr (then the
  /// element has "data":{}). Returns "" if the device has no mqtt table / devType.
  virtual std::string rundata(const TelemetryDevice &d, const TelemetrySource *src, const std::string &data_time) const;
  /// rundata/second element for ONE device (live data, 10421): the requested point ids in request
  /// order, each mapped to its sig and keyed by the first table point of that sig. Unknown ids
  /// are dropped; "" when none is known.
  virtual std::string rundata_second(const TelemetryDevice &d, const TelemetrySource *src, const std::string &data_time,
                                     const std::vector<uint32_t> &point_ids) const;
  /// rsp/propertydata attribute element (cmdType 10310) for one device, one-element array. "" for
  /// a device the variant sends no propertydata for (WiNet-S: tables without a functionEnum).
  virtual std::string propertydata(const TelemetryDevice &d, const TelemetrySource *src, const std::string &data_time,
                                   const std::string &batch_id) const;
  /// pub/devstatusdata element for ONE device (mask 0x40 points). devType 1/14
  /// also carry "sn" (sig 2) and "timestamp" (local time with milliseconds, 17 digits) before
  /// dataTime. "" when the device's table has no status rows.
  virtual std::string devstatus(const TelemetryDevice &d, const TelemetrySource *src, const std::string &data_time,
                                const std::string &timestamp) const;
  /// pub/generaldata element (mask 0x400 points, then "businessType":"addBattery"). "" when the
  /// table has no such rows or none of them has a value.
  virtual std::string generaldata(const TelemetryDevice &d, const TelemetrySource *src,
                                  const std::string &data_time) const;
  /// "<pointId>=<value>;" for every point of `mask` that would be sent, in table order: compared
  /// between ticks to detect a change.
  std::string point_signature(const TelemetryDevice &d, const TelemetrySource *src, uint16_t mask) const;
  /// pub/devinfo (logger only).
  virtual std::string devinfo(const DevinfoContext &ctx) const;
  /// pub/beatdata: one {"devCode","devType","beat"} element per device; the logger (beat 1) first.
  virtual std::string beat(const std::vector<TelemetryDevice> &devices) const;
  /// The variant's functionEnum (16 B): the logger's bitmap, and the default of every device whose
  /// table carries one (table_has_function_enum) when the store has no sig 0x2191 / 0x219d.
  void set_device_function_enum(const std::string &b) { this->device_function_enum_ = b; }
  /// WiNet-S variant: no propertydata for devices whose table carries no functionEnum.
  void set_winet_s(bool v) { this->winet_s_ = v; }

 protected:
  std::string device_function_enum_;
  bool winet_s_{false};
};

using DefaultFormatter = BusinessFormatter;

/// A TelemetrySource for the logger's own (0xF7) signals, filled by the component (product
/// constants, firmware version, fixed defaults). Devices: none.
class LoggerSignals : public TelemetrySource {
 public:
  void set_u16(uint16_t sigid, uint16_t v) { this->set_scalar_(sigid, v, 2, 4); }
  void set_u32(uint16_t sigid, uint32_t v) { this->set_scalar_(sigid, v, 4, 6); }
  void set_s16(uint16_t sigid, int16_t v) { this->set_scalar_(sigid, static_cast<uint16_t>(v), 2, 3); }
  void set_str(uint16_t sigid, const std::string &v);
  void set_bytes(uint16_t sigid, const std::string &v);
  void erase(uint16_t sigid);
  void list_devices(std::vector<TelemetryDevice> &out) const override { out.clear(); }
  void for_each_signal(uint8_t device_index, const std::function<void(const TelemetrySignal &)> &fn) const override;

 protected:
  void set_scalar_(uint16_t sigid, uint64_t bits, uint8_t len, uint8_t type);
  TelemetrySignal &slot_(uint16_t sigid);
  std::vector<TelemetrySignal> sigs_;
};

}  // namespace esphome::isolarcloud
