#pragma once
// Adapters that bind the interfaces.h hooks to the sibling components. Host-buildable: only the
// host-testable cores of those components are used here (sigdata.h, signal_store.h, master.h);
// the ESPHome-facing glue that owns these objects is in isolarcloud.h/.cpp.
//
// Each section is compiled only when the sibling component is part of the build. isolarcloud's
// codegen defines ISOLARCLOUD_BIND_{EXTFLASH,DATAMODEL,MODBUS}; host tests pass them with -D.
//
//   extflash  -> IdentitySource   map_extflash_identity() (+ ExtflashIdentitySource in isolarcloud.h)
//   datamodel -> TelemetrySource  StoreTelemetrySource (SignalStore + optional ModbusMaster devices)
//   modbus    -> ParamReader      make_modbus_param_reader() (getparam 10325 -> ad-hoc read_registers)
//             -> ParamWriter      make_modbus_param_writer() (setparam 10303 -> submit_write, FC 16)

#ifndef ISOLARCLOUD_HOST
#include "esphome/core/defines.h"
#endif

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "cloud_types.h"
#include "interfaces.h"

#ifdef ISOLARCLOUD_BIND_EXTFLASH
#include "../sungrow_extflash/sigdata.h"
#endif
#ifdef ISOLARCLOUD_BIND_DATAMODEL
#include "../sungrow_datamodel/signal_store.h"
#endif
#ifdef ISOLARCLOUD_BIND_MODBUS
#include "../sungrow_modbus/master.h"
#endif

namespace esphome::isolarcloud {

#ifdef ISOLARCLOUD_BIND_EXTFLASH
// ---------------------------------------------------------------------------------------------
// extflash -> identity
// ---------------------------------------------------------------------------------------------
/// Copy into `out` only the fields that came from the sigdata file (Identity::is_from_file), plus
/// the SN when it is plausible. sungrow_extflash fills missing records with stock defaults; those
/// must not override YAML or persisted values, so they are never copied. Sub-fields of the
/// grouped bits (old server, silence window) that are not in the file keep `out`'s value.
/// Implausible file values (empty host, port 0, rundata period 0 or > 1440 min, hours > 24, a
/// timezone string that does not parse) are skipped like missing records. productType (0x0004) is
/// never taken from the file: the board's compile-time constant is used.
/// Returns the IdentityField mask of what was set.
uint32_t map_extflash_identity(const sungrow_extflash::Identity &src, CloudIdentity &out);
#endif

#ifdef ISOLARCLOUD_BIND_DATAMODEL
// ---------------------------------------------------------------------------------------------
// datamodel -> telemetry
// ---------------------------------------------------------------------------------------------
/// Stock table type for a poll table family when discovery gave none: battery 0x2c (44, SBR),
/// meter 0x12 (18), 0x22 (34) for the "other_dev34" family. These are TABLE types; the cloud devType
/// comes from cloud_dev_type_for_table() (44 -> 43, 18 -> 7, 34 -> 46). Charger / energy bridge: 0
/// (unknown, not reported).
uint16_t table_type_for_family(sungrow_datamodel::Family family);

/// TelemetrySource over a SignalStore. With a ModbusMaster the device list is the master's
/// configured devices (index, Modbus address, table type from discovery (21/35/1) or the table
/// family, cloud devType from the map, link state); without one it is every device index present in
/// the store, reported online with table type / devType 0 (= not reported to the cloud).
/// devCode is the Modbus slave address.
class StoreTelemetrySource : public TelemetrySource {
 public:
  explicit StoreTelemetrySource(const sungrow_datamodel::SignalStore *store) : store_(store) {}
#ifdef ISOLARCLOUD_BIND_MODBUS
  void set_master(const sungrow_modbus::ModbusMaster *master) { this->master_ = master; }
#endif
  void list_devices(std::vector<TelemetryDevice> &out) const override;
  /// Signals of one device in ascending sigid order (SignalStore iteration order is unspecified;
  /// sorted keeps payloads stable). Raw DM value included (bits/len/type/bytes).
  void for_each_signal(uint8_t device_index, const std::function<void(const TelemetrySignal &)> &fn) const override;
  bool get_signal(uint8_t device_index, uint16_t sigid, TelemetrySignal &out) const override;

 protected:
  /// Battery parent link: 0x2022 (doc 10751, the inverter's model class) reads as 1 if < 3 else
  /// 14 (upDevType), and 0x2028 is the devCode of the device at the Modbus address in 0x2021
  /// (doc 10750, upDevCode).
  bool battery_link_(uint8_t device_index, uint16_t sigid, TelemetrySignal &out) const;
  /// Inverter software versions: the version block (sig 0x2011, doc 2582, 30-byte slots) is
  /// searched for the slot containing the target sig's marker ("_01" -> 0x2012, "MDSP"/"_03" ->
  /// 0x2013, …).
  bool inverter_version_(uint8_t device_index, uint16_t sigid, TelemetrySignal &out) const;
  const sungrow_datamodel::SignalStore *store_;
#ifdef ISOLARCLOUD_BIND_MODBUS
  const sungrow_modbus::ModbusMaster *master_{nullptr};
  /// Stock-style device ids, handed out in the order devices are identified (device index -> id).
#endif
};
#endif

#ifdef ISOLARCLOUD_BIND_MODBUS
// ---------------------------------------------------------------------------------------------
// modbus -> getparam
// ---------------------------------------------------------------------------------------------
/// Issues an ad-hoc read; the shape of sungrow_modbus::SungrowModbus::read_registers() (1-based
/// table address).
using RegisterReadFn = std::function<bool(uint8_t addr, uint8_t fn, uint16_t start, uint16_t count,
                                          sungrow_modbus::AdhocCallback cb)>;

/// ParamReader over sungrow_modbus for getparam 10325: FC 3/4 only (mode 1 = FC 0x77 is refused),
/// register = the cloud's modbusAddress, a document (1-based) number like the master's (wire =
/// modbusAddress - 1, as stock). Completes from the master's tick (main loop).
ParamReader make_modbus_param_reader(RegisterReadFn read);

/// Queues a write; the shape of sungrow_modbus::SungrowModbus::submit_write() (1-based table
/// address).
using RegisterWriteFn = std::function<sungrow_modbus::WriteStatus(
    uint8_t addr, uint16_t start, const uint16_t *values, uint16_t count, const sungrow_modbus::WriteOptions &opt,
    sungrow_modbus::WriteCallback cb)>;

/// ParamWriter over sungrow_modbus for setparam 10303: FC 16 like stock, register = the cloud's
/// modbusAddress (document number; wire = modbusAddress - 1). Completes from the master's tick.
ParamWriter make_modbus_param_writer(RegisterWriteFn write);
#endif

}  // namespace esphome::isolarcloud
