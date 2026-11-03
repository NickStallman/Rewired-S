#pragma once
// Binding points to the components built in parallel. Host-buildable, header-only.
//
//   IdentitySource   <- sungrow_extflash (device 0xF7 sigs from /usrdataout sigdata)
//   TelemetrySource  <- sungrow_datamodel SignalStore
//   ParamReader      <- sungrow_modbus ad-hoc read API (cloud getparam)
//
// Bound in adapters.h/.cpp (+ ExtflashIdentitySource / bind_* in isolarcloud.h) when the sibling
// component is configured; isolarcloud's codegen wires them. Without them the uplink still works
// (YAML identity, logger-only beat/rundata/devinfo, getparam answered with result 0).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cloud_types.h"

namespace esphome::isolarcloud {

// ---------------------------------------------------------------------------------------------
// Identity (sungrow_extflash)
// ---------------------------------------------------------------------------------------------
/// Bound: ExtflashIdentitySource (isolarcloud.h) + map_extflash_identity() (adapters.h), YAML
/// `isolarcloud: identity_source: <sungrow_extflash id>`. Only values present in the sigdata file
/// are reported (Identity::is_from_file), never extflash's defaults.
class IdentitySource {
 public:
  virtual ~IdentitySource() = default;
  /// True once the source has been read (successfully or not).
  virtual bool identity_ready() const = 0;
  /// Fill `out` with what the source knows and return the mask of IdentityField bits it set.
  /// Fields not in the mask are left untouched. 0 = nothing usable.
  virtual uint32_t read_identity(CloudIdentity &out) const = 0;
  /// Register a callback fired once the identity has been loaded. Optional.
  virtual void add_on_loaded_callback(std::function<void()> && /*cb*/) {}
  /// Read a file of the dongle's ext flash (the broker CA /usrdataout/tlsca/cafile). Optional.
  virtual bool read_file(const std::string & /*path*/, std::vector<uint8_t> & /*out*/) { return false; }
};

// ---------------------------------------------------------------------------------------------
// Telemetry (sungrow_datamodel)
// ---------------------------------------------------------------------------------------------
struct TelemetryDevice {
  uint8_t index{0};          ///< SignalStore device index
  /// businessData "devCode": stock's device id (DM_AddDevice hands out the next free id as devices
  /// are found: inverter 1, its SBR stacks 2, 3, ...; the logger is 247). iSolarCloud keys its
  /// devices on it, so it must match what the stock dongle used for the same device.
  uint16_t dev_code{0};
  uint8_t address{0};        ///< RS485 slave address (cloud reads go here)
  /// Stock *table type* of the device model (0x23 = 35 hybrid, 0x15 = 21 string inverter,
  /// 0x2c = 44 SBR battery, 0x12 = 18 meter, ...): selects the mqtt point table.
  uint16_t table_type{0};
  /// businessData "devType" the cloud expects (cloud_dev_type_for_table(table_type): 14, 1, 43, 7,
  /// ...). 0 = unknown: the device is not reported.
  uint16_t dev_type{0};
  bool online{true};         ///< stock skips offline devices ("DEV %d OFFLINE"); beat = online
  // Upstream device for sub-devices (stock adds upDevType/upDevCode for devType 43 / 92).
  uint16_t up_dev_code{0};
  uint16_t up_dev_type{0};
  bool has_up{false};
};

/// One DM signal as stored. The cloud formatter works on the RAW value (`bits` / `bytes`) and
/// applies the mqtt-table scale; `value` / `str` are the display forms (getparam, logs).
struct TelemetrySignal {
  uint16_t sigid{0};
  bool valid{false};
  bool is_string{false};
  double value{0.0};  ///< scaled numeric value (SignalStore Signal::number())
  std::string str;    ///< string signals (Signal::str(), trimmed)
  // Raw DM value.
  uint64_t bits{0};    ///< integer bits of a scalar, zero-extended (Signal::bits())
  uint8_t len{0};      ///< value bytes (2/4/8 scalar, n for strings)
  uint8_t type{0};     ///< our TypeCode (3 s16, 4 u16, 5 s32, 6 u32, 7 s64, 8 u64, 9 u16?, 12 str, 0x80 f32)
  bool is_float{false};  ///< value is a host float (derived values), `value` holds it unscaled
  std::string bytes;   ///< raw bytes for strings / arrays (wire order, untrimmed)
};

/// Bound: StoreTelemetrySource (adapters.h) over sungrow_datamodel::SignalStore, device list from
/// the sungrow_modbus master (devCode = Modbus address, table type from discovery / table family).
class TelemetrySource {
 public:
  virtual ~TelemetrySource() = default;
  virtual void list_devices(std::vector<TelemetryDevice> &out) const = 0;
  virtual void for_each_signal(uint8_t device_index, const std::function<void(const TelemetrySignal &)> &fn) const = 0;
  /// One signal; false if the device never stored it. Default: linear scan of for_each_signal.
  virtual bool get_signal(uint8_t device_index, uint16_t sigid, TelemetrySignal &out) const {
    bool found = false;
    this->for_each_signal(device_index, [&](const TelemetrySignal &s) {
      if (!found && s.sigid == sigid) {
        out = s;
        found = true;
      }
    });
    return found;
  }
};

// ---------------------------------------------------------------------------------------------
// Parameter reads for cloud getparam (sungrow_modbus)
// ---------------------------------------------------------------------------------------------
/// One Modbus read for cloud getparam (10325).
struct ParamReadRequest {
  uint8_t addr{1};    ///< Modbus slave address (= devCode)
  uint8_t fn{4};      ///< 3 / 4 (mode == 1 -> 0x77, which we refuse)
  uint16_t reg{0};    ///< modbusAddress: document (1-based) number; wire = reg - 1
  uint16_t count{1};  ///< registers
};
/// done(ok, regs, n): regs = big-endian register bytes, valid only during the callback.
using ParamReadDone = std::function<void(bool ok, const uint8_t *regs, uint16_t nregs)>;
/// Bound: make_modbus_param_reader() (adapters.h) -> sungrow_modbus read_registers(), completed
/// from the master's tick on the main loop. Returns false if the read could not be queued.
using ParamReader = std::function<bool(const ParamReadRequest &, ParamReadDone)>;

// ---------------------------------------------------------------------------------------------
// Parameter writes for cloud setparam (sungrow_modbus)
// ---------------------------------------------------------------------------------------------
/// One Modbus write for cloud setparam (10303). Stock always writes with FC 16.
struct ParamWriteRequest {
  uint8_t addr{1};                ///< Modbus slave address (= devCode)
  uint16_t reg{0};                ///< modbusAddress: document (1-based) number; wire = reg - 1
  std::vector<uint16_t> values;   ///< register values, in wire order
};
/// done(ok, exception): exception = the slave's Modbus exception code, 0 for none / timeout.
using ParamWriteDone = std::function<void(bool ok, uint8_t exception)>;
/// Bound: make_modbus_param_writer() (adapters.h) -> sungrow_modbus submit_write() (FC 16).
/// Returns false if the write could not be queued.
using ParamWriter = std::function<bool(const ParamWriteRequest &, ParamWriteDone)>;

}  // namespace esphome::isolarcloud
