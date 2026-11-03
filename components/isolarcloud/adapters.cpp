#include "adapters.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <set>

#include "cloud_time.h"

namespace esphome::isolarcloud {

#ifdef ISOLARCLOUD_BIND_EXTFLASH
uint32_t map_extflash_identity(const sungrow_extflash::Identity &src, CloudIdentity &out) {
  namespace sig = sungrow_extflash::sig;
  auto has = [&src](uint16_t id) { return src.is_from_file(id); };
  uint32_t mask = 0;

  if (src.valid()) {
    out.sn = src.sn;
    mask |= FIELD_SN;
  }
  // 0x0004 productType is NOT copied: the cloud publicData uses the board constant (C27).
  if (has(sig::ENCRYPT_MODE)) {
    out.encrypt = src.encrypt_mode;
    mask |= FIELD_ENCRYPT;
  }
  if (has(sig::KEY_NUMBER)) {
    out.keynum = src.key_number;
    mask |= FIELD_KEYNUM;
  }
  if (has(sig::COMPRESS_MODE)) {
    out.compress = src.compress_mode;
    mask |= FIELD_COMPRESS;
  }

  // Current broker (0x1071 / 0x1072 / 0x1013 / 0x0023).
  if (has(sig::BROKER_HOST) && !src.broker_host.empty()) {
    out.server.host = src.broker_host;
    mask |= FIELD_SERVER_HOST;
  }
  if (has(sig::BROKER_PORT) && src.broker_port != 0) {
    out.server.port = src.broker_port;
    mask |= FIELD_SERVER_PORT;
  }
  if (has(sig::MQTT_KEY)) {
    out.server.key = src.mqtt_key;
    mask |= FIELD_SERVER_KEY;
  }
  if (has(sig::SSL_FLAG)) {
    out.server.ssl_flag = src.ssl_flag;
    mask |= FIELD_SERVER_SSL;
  }

  // Backup ("old") broker (0x1957 / 0x1958 / 0x1959 / 0x0024): one mask bit for the endpoint.
  bool old_any = false;
  if (has(sig::OLD_BROKER_HOST) && !src.old_broker_host.empty()) {
    out.old_server.host = src.old_broker_host;
    old_any = true;
  }
  if (has(sig::OLD_BROKER_PORT) && src.old_broker_port != 0) {
    out.old_server.port = src.old_broker_port;
    old_any = true;
  }
  if (has(sig::OLD_MQTT_KEY)) {
    out.old_server.key = src.old_mqtt_key;
    old_any = true;
  }
  if (has(sig::OLD_SSL_FLAG)) {
    out.old_server.ssl_flag = src.old_ssl_flag;
    old_any = true;
  }
  if (old_any)
    mask |= FIELD_OLD_SERVER;

  // Silence window (0x1017 / 0x1018 / 0x1019): one mask bit for the three values.
  bool silence_any = false;
  if (has(sig::SILENCE_ENABLE)) {
    out.silence_enable = src.silence_enable != 0;
    silence_any = true;
  }
  if (has(sig::SILENCE_START) && src.silence_start <= 23) {
    out.silence_start_hour = static_cast<uint8_t>(src.silence_start);
    silence_any = true;
  }
  if (has(sig::SILENCE_END) && src.silence_end <= 24) {
    out.silence_end_hour = static_cast<uint8_t>(src.silence_end);
    silence_any = true;
  }
  if (silence_any)
    mask |= FIELD_SILENCE;

  if (has(sig::TIMEZONE_ID)) {
    out.timezone_id = src.timezone_id;
    mask |= FIELD_TIMEZONE_ID;
  }
  int32_t tz_off = 0;
  if (has(sig::TIMEZONE) && parse_cloud_timezone(src.timezone, tz_off)) {
    out.timezone = src.timezone;
    mask |= FIELD_TIMEZONE;
  }
  if (has(sig::RUNDATA_PERIOD) && src.rundata_period >= 1 && src.rundata_period <= 1440) {
    out.rundata_period_min = src.rundata_period;
    mask |= FIELD_RUNDATA_PERIOD;
  }
  if (has(sig::REMOTE_CTL_MASK)) {
    out.remote_ctl_mask = src.remote_ctl_mask;
    mask |= FIELD_REMOTE_CTL_MASK;
  }
  return mask;
}
#endif  // ISOLARCLOUD_BIND_EXTFLASH

#ifdef ISOLARCLOUD_BIND_DATAMODEL
uint16_t table_type_for_family(sungrow_datamodel::Family family) {
  using sungrow_datamodel::Family;
  switch (family) {
    case Family::BATTERY:
      return 0x2c;  // tcpslave_dev44 (SBR) -> cloud devType 43
    case Family::METER:
      return 0x12;  // other_dev18_* -> cloud devType 7
    case Family::LOGGER:
      return 0x22;  // other_dev34_* -> cloud devType 46
    case Family::CHARGER:
      return 0x2E;  // charger (dev_type 46)
    case Family::ENERGY_BRIDGE:
      return 0x5C;  // energy bridge (dev_type 92)
    default:
      return 0;  // inverter: dev_type from the device identity
  }
}

void StoreTelemetrySource::list_devices(std::vector<TelemetryDevice> &out) const {
  out.clear();
#ifdef ISOLARCLOUD_BIND_MODBUS
  if (this->master_ != nullptr) {
    for (const auto &d : this->master_->devices()) {
      TelemetryDevice td;
      td.index = d.config().index;
      td.address = d.config().address;
      // Stock device id (DM_AddDevice order): inverter 1, then the SBR stacks it reports.
      td.dev_code = this->master_->device_id(td.index);
      td.online = d.online();
      if (d.identified()) {
        td.table_type = d.devinfo().dev_type;  // devinfo, YAML dev_type or the table family
      } else {
        const sungrow_datamodel::RegisterTable *t = d.table() != nullptr ? d.table() : d.config().table;
        td.table_type = t != nullptr ? table_type_for_family(t->family) : 0;
      }
      td.dev_type = cloud_dev_type_for_table(td.table_type);
      out.push_back(td);
    }
    return;
  }
#endif
  if (this->store_ == nullptr)
    return;
  std::set<uint8_t> seen;
  this->store_->for_each([&seen](const sungrow_datamodel::Signal &s) { seen.insert(s.device); });
  for (uint8_t idx : seen) {
    TelemetryDevice td;
    td.index = idx;
    td.dev_code = idx;
    out.push_back(td);
  }
}

namespace {
TelemetrySignal to_telemetry(const sungrow_datamodel::Signal &s) {
  TelemetrySignal ts;
  ts.sigid = s.sigid;
  ts.valid = s.valid;
  ts.type = s.type;
  ts.len = s.len;
  if (s.kind == sungrow_datamodel::ValueKind::STRING || s.kind == sungrow_datamodel::ValueKind::ARRAY) {
    ts.is_string = s.kind == sungrow_datamodel::ValueKind::STRING;
    ts.bytes.assign(reinterpret_cast<const char *>(s.data()), s.len);
    if (s.valid && ts.is_string)
      ts.str = s.str();
  } else if (s.kind == sungrow_datamodel::ValueKind::FLOAT) {
    ts.is_float = true;
    ts.value = s.number();
    if (!std::isfinite(ts.value))
      ts.valid = false;
  } else {
    ts.bits = s.bits();
    ts.value = s.number();
  }
  return ts;
}
}  // namespace

void StoreTelemetrySource::for_each_signal(uint8_t device_index,
                                           const std::function<void(const TelemetrySignal &)> &fn) const {
  if (this->store_ == nullptr)
    return;
  std::vector<const sungrow_datamodel::Signal *> sigs;
  this->store_->for_each([&](const sungrow_datamodel::Signal &s) {
    if (s.device == device_index)
      sigs.push_back(&s);
  });
  std::sort(sigs.begin(), sigs.end(), [](const auto *a, const auto *b) { return a->sigid < b->sigid; });
  for (const auto *s : sigs)
    fn(to_telemetry(*s));
}

bool StoreTelemetrySource::battery_link_(uint8_t device_index, uint16_t sigid, TelemetrySignal &out) const {
#ifdef ISOLARCLOUD_BIND_MODBUS
  // Only SBR batteries (these sigids mean other things in other tables).
  if (this->master_ == nullptr)
    return false;
  bool battery = false;
  for (const auto &d : this->master_->devices()) {
    if (d.config().index != device_index)
      continue;
    const sungrow_datamodel::RegisterTable *t = d.table() != nullptr ? d.table() : d.config().table;
    battery = t != nullptr && t->family == sungrow_datamodel::Family::BATTERY;
    break;
  }
  if (!battery)
    return false;
#else
  return false;
#endif
  const sungrow_datamodel::Signal *cls = this->store_->get(device_index, 0x2022);
  const sungrow_datamodel::Signal *addr = this->store_->get(device_index, 0x2021);
  if (cls == nullptr || addr == nullptr)
    return false;
  const TelemetrySignal raw_cls = to_telemetry(*cls), raw_addr = to_telemetry(*addr);
  if (!raw_cls.valid || !raw_addr.valid)
    return false;
  uint64_t v = 0;
  if (sigid == 0x2022) {
    v = raw_cls.bits < 3 ? 1 : 14;
  } else {
#ifdef ISOLARCLOUD_BIND_MODBUS
    bool found = false;
    for (const auto &d : this->master_->devices()) {
      if (d.config().address != raw_addr.bits)
        continue;
      v = this->master_->device_id(d.config().index);
      if (v == 0)
        return false;
      found = true;
      break;
    }
    if (!found)
      return false;
#endif
  }
  out = TelemetrySignal{};
  out.sigid = sigid;
  out.valid = true;
  out.bits = v;
  out.len = 2;
  out.type = 4;
  out.value = static_cast<double>(v);
  return true;
}

namespace {
struct VersionMarker {
  uint16_t sigid;
  const char *a;
  const char *b;
};
// Version-slot markers, searched in this order.
constexpr VersionMarker VERSION_MARKERS[] = {
    {0x2012, "LCD", "_01"},  {0x2013, "MDSP", "_03"},     {0x2014, "SDSP", "_04"},      {0x2015, "PVD", "_05"},
    {0x2016, "CPLD", "_08"}, {0x2017, "MCU", "_18"},      {0x2018, "TEMP", "_20"},      {0x2019, "BAT", "_22"},
    {0x2034, "AFD", "_06"},  {0x203a, "KUROYOROCK", "RSD"}, {0x5853, "MLPE_CSU", "_38"}, {0x5854, "MLPE_MPLC", "_39"},
    {0x5855, "MLPE_SPLC", "_40"},
};
constexpr size_t VERSION_SLOT = 30;
}  // namespace

bool StoreTelemetrySource::inverter_version_(uint8_t device_index, uint16_t sigid, TelemetrySignal &out) const {
  const VersionMarker *m = nullptr;
  for (const auto &v : VERSION_MARKERS) {
    if (v.sigid == sigid)
      m = &v;
  }
  if (m == nullptr)
    return false;
#ifdef ISOLARCLOUD_BIND_MODBUS
  if (this->master_ == nullptr)
    return false;
  bool inverter = false;
  for (const auto &d : this->master_->devices()) {
    if (d.config().index != device_index)
      continue;
    const sungrow_datamodel::RegisterTable *t = d.table() != nullptr ? d.table() : d.config().table;
    inverter = t != nullptr && t->family == sungrow_datamodel::Family::INVERTER;
    break;
  }
  if (!inverter)
    return false;
#else
  return false;
#endif
  const sungrow_datamodel::Signal *blk = this->store_->get(device_index, 0x2011);
  if (blk == nullptr || !blk->valid)
    return false;
  const char *p = reinterpret_cast<const char *>(blk->data());
  for (size_t off = 0; off + VERSION_SLOT <= blk->len; off += VERSION_SLOT) {
    size_t n = 0;
    while (n < VERSION_SLOT && p[off + n] != '\0' && static_cast<uint8_t>(p[off + n]) != 0xFF)
      n++;
    const std::string slot(p + off, n);
    if (slot.empty())
      continue;
    // Stock skips a slot that ends in "V00" (no version present).
    if (slot.size() >= 3 && slot.compare(slot.size() - 3, 3, "V00") == 0)
      continue;
    if (slot.find(m->a) == std::string::npos && slot.find(m->b) == std::string::npos)
      continue;
    out = TelemetrySignal{};
    out.sigid = sigid;
    out.valid = true;
    out.is_string = true;
    out.type = 12;
    out.str = slot;
    out.bytes = slot;
    out.len = static_cast<uint8_t>(slot.size());
    return true;
  }
  return false;
}

bool StoreTelemetrySource::get_signal(uint8_t device_index, uint16_t sigid, TelemetrySignal &out) const {
  if (this->store_ == nullptr)
    return false;
  if (this->inverter_version_(device_index, sigid, out))
    return true;
  if (sigid == 0x2022 || sigid == 0x2028) {
    if (this->battery_link_(device_index, sigid, out))
      return true;
    if (sigid == 0x2028)
      return false;
  }
  const sungrow_datamodel::Signal *s = this->store_->get(device_index, sigid);
  if (s == nullptr)
    return false;
  out = to_telemetry(*s);
  return true;
}
#endif  // ISOLARCLOUD_BIND_DATAMODEL

#ifdef ISOLARCLOUD_BIND_MODBUS
ParamReader make_modbus_param_reader(RegisterReadFn read) {
  return [read = std::move(read)](const ParamReadRequest &req, ParamReadDone done) -> bool {
    if (req.fn != sungrow_modbus::FN_READ_HOLDING && req.fn != sungrow_modbus::FN_READ_INPUT)
      return false;  // 0x77 (mode 1) is not supported by our master
    if (req.count == 0 || req.count > sungrow_modbus::MAX_READ_REGS || req.reg == 0)
      return false;
    // modbusAddress is the document (1-based) number, the same numbering read_registers takes.
    return read(req.addr, req.fn, req.reg, req.count,
                [done = std::move(done)](bool ok, const uint8_t *data, uint16_t nregs) {
                  done(ok && data != nullptr, data, nregs);
                });
  };
}

ParamWriter make_modbus_param_writer(RegisterWriteFn write) {
  return [write = std::move(write)](const ParamWriteRequest &req, ParamWriteDone done) -> bool {
    if (req.values.empty() || req.values.size() > 123 || req.reg == 0)
      return false;
    sungrow_modbus::WriteOptions opt;
    opt.fn = sungrow_modbus::FN_WRITE_MULTIPLE;
    const auto st = write(req.addr, req.reg, req.values.data(),
                          static_cast<uint16_t>(req.values.size()), opt,
                          [done = std::move(done)](const sungrow_modbus::WriteResult &r) {
                            done(sungrow_modbus::write_succeeded(r.status), r.exception);
                          });
    return st == sungrow_modbus::WriteStatus::QUEUED;
  };
}
#endif  // ISOLARCLOUD_BIND_MODBUS

}  // namespace esphome::isolarcloud
