// Host tests for components/isolarcloud/adapters.{h,cpp}: the extflash identity mask logic, the
// SignalStore TelemetrySource (raw values, table type -> devType) and the getparam Modbus reader. Build/run:
//   make -C tests/isolarcloud bindings
#include <cstring>
#include <string>
#include <vector>

#include "../test_harness.h"
#include "isolarcloud/adapters.h"
#include "isolarcloud/rundata_format.h"

using namespace esphome::isolarcloud;
namespace ef = esphome::sungrow_extflash;
namespace dm = esphome::sungrow_datamodel;
namespace mb = esphome::sungrow_modbus;

namespace {
struct Builder {
  std::vector<uint8_t> b;
  Builder &rec(uint16_t id, const std::vector<uint8_t> &data) {
    b.push_back(id & 0xFF);
    b.push_back(id >> 8);
    b.push_back(data.size() & 0xFF);
    b.push_back((data.size() >> 8) & 0xFF);
    b.insert(b.end(), data.begin(), data.end());
    return *this;
  }
  Builder &str(uint16_t id, const std::string &s, size_t padded) {
    std::vector<uint8_t> d(padded, 0);
    std::memcpy(d.data(), s.data(), std::min(s.size(), padded));
    return this->rec(id, d);
  }
  Builder &u16(uint16_t id, uint16_t v) { return this->rec(id, {uint8_t(v & 0xFF), uint8_t(v >> 8)}); }
  Builder &u32(uint16_t id, uint32_t v) {
    return this->rec(id, {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
  }
  ef::Identity identity(uint16_t default_pt = 0x720) const {
    ef::SigdataFile f;
    f.parse(b.data(), b.size());
    return ef::decode_identity(f, default_pt);
  }
};

/// An identity as resolve_identity_() would hand it in: non-default values everywhere, so a
/// leaked extflash default would be visible.
CloudIdentity prefilled() {
  CloudIdentity c;
  c.sn = "YAMLSN";
  c.product_type = 0x1234;
  c.encrypt = 7;
  c.keynum = 7;
  c.compress = 7;
  c.server.host = "persisted.example";
  c.server.port = 1111;
  c.server.key = key_from_string("PERSISTEDKEY0000");
  c.server.ssl_flag = 7;
  c.old_server.host = "old.persisted";
  c.old_server.port = 2222;
  c.old_server.ssl_flag = 7;
  c.timezone_id = 77;
  c.timezone = "UTC-3:00";
  c.rundata_period_min = 77;
  c.silence_enable = true;
  c.silence_start_hour = 3;
  c.silence_end_hour = 4;
  c.remote_ctl_mask = 0xABCD;
  return c;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
TEST(extflash_empty_file_sets_nothing) {
  ef::Identity id = Builder{}.identity();  // all stock defaults, no SN
  CloudIdentity c = prefilled();
  const CloudIdentity before = c;
  CHECK_EQ(map_extflash_identity(id, c), 0u);
  CHECK(c.sn == before.sn);
  CHECK(c.server == before.server);
  CHECK(c.old_server == before.old_server);
  CHECK_EQ(c.product_type, before.product_type);  // default_product_type is not "from file"
  CHECK_EQ(c.timezone_id, 77);
  CHECK(c.timezone == "UTC-3:00");
  CHECK_EQ(c.remote_ctl_mask, 0xABCDu);
}

TEST(extflash_sn_only) {
  ef::Identity id = Builder{}.str(ef::sig::SN, "B2101234567", 20).identity();
  CloudIdentity c = prefilled();
  CHECK_EQ(map_extflash_identity(id, c), static_cast<uint32_t>(FIELD_SN));
  CHECK(c.sn == "B2101234567");
  CHECK(c.server.host == "persisted.example");  // default iot.isolarcloud.com NOT copied
  CHECK_EQ(c.compress, 7);
}

TEST(extflash_implausible_sn_not_copied) {
  ef::Identity id = Builder{}.str(ef::sig::SN, "bad sn", 20).u16(ef::sig::TIMEZONE_ID, 9).identity();
  CloudIdentity c = prefilled();
  CHECK_EQ(map_extflash_identity(id, c), static_cast<uint32_t>(FIELD_TIMEZONE_ID));
  CHECK(c.sn == "YAMLSN");
  CHECK_EQ(c.timezone_id, 9);
}

TEST(extflash_full_file) {
  Builder b;
  b.str(ef::sig::SN, "B2101234567", 20)
      .u16(ef::sig::PRODUCT_TYPE, 1824)
      .u16(ef::sig::ENCRYPT_MODE, 1)
      .u16(ef::sig::KEY_NUMBER, 3)
      .u16(ef::sig::COMPRESS_MODE, 0)
      .u16(ef::sig::SSL_FLAG, 1)
      .u16(ef::sig::OLD_SSL_FLAG, 0)
      .str(ef::sig::BROKER_HOST, "gateway.isolarcloud.eu", 64)
      .u16(ef::sig::BROKER_PORT, 19999)
      .str(ef::sig::MQTT_KEY, "ABCDEFGHIJKLMNOP", 16)
      .str(ef::sig::OLD_BROKER_HOST, "old.example", 64)
      .u16(ef::sig::OLD_BROKER_PORT, 1883)
      .str(ef::sig::OLD_MQTT_KEY, "QRSTUVWXYZ012345", 16)
      .u16(ef::sig::TIMEZONE_ID, 12)
      .str(ef::sig::TIMEZONE, "UTC+10:00", 16)
      .u16(ef::sig::RUNDATA_PERIOD, 15)
      .u16(ef::sig::SILENCE_ENABLE, 1)
      .u16(ef::sig::SILENCE_START, 22)
      .u16(ef::sig::SILENCE_END, 6)
      .u32(ef::sig::REMOTE_CTL_MASK, 0x2);
  CloudIdentity c = prefilled();
  uint32_t mask = map_extflash_identity(b.identity(), c);
  // productType (0x0004) is never taken from the file: the board constant wins.
  CHECK_EQ(mask, static_cast<uint32_t>(FIELD_ALL & ~(FIELD_PRODUCT_NAME | FIELD_PRODUCT_TYPE)));
  CHECK(c.sn == "B2101234567");
  CHECK_EQ(c.product_type, 0x1234);
  CHECK_EQ(c.encrypt, 1);
  CHECK_EQ(c.keynum, 3);
  CHECK_EQ(c.compress, 0);
  CHECK(c.server.host == "gateway.isolarcloud.eu");
  CHECK_EQ(c.server.port, 19999);
  CHECK(c.server.key == key_from_string("ABCDEFGHIJKLMNOP"));
  CHECK_EQ(c.server.ssl_flag, 1);
  CHECK(c.server.transport() == Transport::TCP);  // port 19999 forces plain TCP (else 1 = invalid)
  CHECK(c.old_server.host == "old.example");
  CHECK_EQ(c.old_server.port, 1883);
  CHECK(c.old_server.key == key_from_string("QRSTUVWXYZ012345"));
  CHECK_EQ(c.old_server.ssl_flag, 0);
  CHECK_EQ(c.timezone_id, 12);
  CHECK(c.timezone == "UTC+10:00");
  CHECK_EQ(c.rundata_period_min, 15);
  CHECK(c.silence_enable);
  CHECK_EQ(c.silence_start_hour, 22);
  CHECK_EQ(c.silence_end_hour, 6);
  CHECK_EQ(c.remote_ctl_mask, 0x2u);
}

TEST(extflash_product_type_and_bad_timezone_not_copied) {
  ef::Identity id = Builder{}.u16(ef::sig::PRODUCT_TYPE, 0x715).str(ef::sig::TIMEZONE, "CET", 16).identity();
  CloudIdentity c = prefilled();
  CHECK_EQ(map_extflash_identity(id, c), 0u);
  CHECK_EQ(c.product_type, 0x1234);
  CHECK(c.timezone == "UTC-3:00");
}

TEST(extflash_partial_groups_keep_other_subfields) {
  ef::Identity id = Builder{}.u16(ef::sig::OLD_BROKER_PORT, 1883).u16(ef::sig::SILENCE_END, 5).identity();
  CloudIdentity c = prefilled();
  CHECK_EQ(map_extflash_identity(id, c), static_cast<uint32_t>(FIELD_OLD_SERVER | FIELD_SILENCE));
  CHECK(c.old_server.host == "old.persisted");
  CHECK_EQ(c.old_server.port, 1883);
  CHECK_EQ(c.old_server.ssl_flag, 7);
  CHECK(c.silence_enable);
  CHECK_EQ(c.silence_start_hour, 3);
  CHECK_EQ(c.silence_end_hour, 5);
}

TEST(extflash_implausible_values_skipped) {
  ef::Identity id = Builder{}
                        .str(ef::sig::BROKER_HOST, "", 64)
                        .u16(ef::sig::BROKER_PORT, 0)
                        .u16(ef::sig::RUNDATA_PERIOD, 0)
                        .u16(ef::sig::SILENCE_START, 99)
                        .u16(ef::sig::PRODUCT_TYPE, 0)
                        .identity();
  CloudIdentity c = prefilled();
  CHECK_EQ(map_extflash_identity(id, c), 0u);
  CHECK(c.server.host == "persisted.example");
  CHECK_EQ(c.server.port, 1111);
  CHECK_EQ(c.rundata_period_min, 77);
  CHECK_EQ(c.silence_start_hour, 3);
}

TEST(extflash_mask_composes_with_apply_identity_fields) {
  // As resolve_identity_: source over defaults, then YAML over the source.
  ef::Identity id =
      Builder{}.str(ef::sig::SN, "B2101234567", 20).str(ef::sig::BROKER_HOST, "eu.example", 64).identity();
  CloudIdentity eff;
  CloudIdentity s = eff;
  uint32_t mask = map_extflash_identity(id, s);
  apply_identity_fields(eff, s, mask);
  CloudIdentity yaml;
  yaml.sn = "OVERRIDE1";
  apply_identity_fields(eff, yaml, FIELD_SN);
  CHECK(eff.sn == "OVERRIDE1");
  CHECK(eff.server.host == "eu.example");
  CHECK_EQ(eff.server.port, DEFAULT_BROKER_PORT);
}

// ---------------------------------------------------------------------------------------------
TEST(telemetry_store_only) {
  dm::SignalStore store;
  store.set_scalar(0, 0x2080, dm::TYPE_U32, 0.1f, 12345, 4);
  store.set_scalar(2, 0x1000, dm::TYPE_U16, 0.0f, 7, 2);
  StoreTelemetrySource src(&store);
  std::vector<TelemetryDevice> devs;
  src.list_devices(devs);
  CHECK_EQ(devs.size(), 2u);
  CHECK_EQ(devs[0].index, 0);
  CHECK_EQ(devs[1].index, 2);
  CHECK(devs[1].online);
}

TEST(telemetry_signals_sorted_typed_and_validity) {
  dm::SignalStore store;
  store.set_scalar(0, 0x3000, dm::TYPE_U16, 0.0f, 5, 2);
  store.set_scalar(0, 0x2080, dm::TYPE_U32, 0.1f, 12345, 4);
  const char sn[] = "A2101234567\0\0\0";
  store.set(0, 0x1001, dm::TYPE_STRING, 0.0f, dm::ValueKind::STRING, reinterpret_cast<const uint8_t *>(sn), 14);
  store.set_scalar(0, 0x2090, dm::TYPE_U16, 0.0f, 1, 2);
  store.clear(0, 0x2090);
  store.set_scalar(1, 0x2080, dm::TYPE_U16, 0.0f, 1, 2);  // other device: not listed
  StoreTelemetrySource src(&store);
  std::vector<TelemetrySignal> got;
  src.for_each_signal(0, [&](const TelemetrySignal &s) { got.push_back(s); });
  CHECK_EQ(got.size(), 4u);
  CHECK_EQ(got[0].sigid, 0x1001);
  CHECK(got[0].is_string);
  CHECK(got[0].valid);
  CHECK(got[0].str == "A2101234567");
  CHECK_EQ(got[1].sigid, 0x2080);
  CHECK_NEAR(got[1].value, 1234.5, 1e-3);
  CHECK(got[1].valid);
  // Raw DM value for the cloud formatter (scaled by the mqtt table, not by our 0.1).
  CHECK_EQ(got[1].bits, 12345u);
  CHECK_EQ(got[1].len, 4);
  CHECK_EQ(got[1].type, dm::TYPE_U32);
  CHECK(got[0].bytes == std::string("A2101234567\0\0\0", 14));
  TelemetrySignal one;
  CHECK(src.get_signal(0, 0x2080, one) && one.bits == 12345u);
  CHECK(!src.get_signal(0, 0x4444, one));
  CHECK_EQ(got[2].sigid, 0x2090);
  CHECK(!got[2].valid);
  CHECK_EQ(got[3].sigid, 0x3000);
  CHECK_NEAR(got[3].value, 5.0, 1e-9);
}

TEST(telemetry_master_devices_and_formatter) {
  dm::SignalStore store;
  mb::ModbusMaster master;
  master.set_store(&store);
  mb::DeviceConfig inv;  // auto table: devType unknown until discovery
  inv.index = 0;
  inv.address = 1;
  mb::DeviceConfig bat;
  bat.index = 1;
  bat.address = 2;
  bat.table = dm::find_table("battery");
  mb::DeviceConfig meter;
  meter.index = 2;
  meter.address = 3;
  meter.table = dm::find_table("other_dev18_st6100");
  master.add_device(inv);
  master.add_device(bat);
  master.add_device(meter);
  master.start();
  StoreTelemetrySource src(&store);
  src.set_master(&master);
  std::vector<TelemetryDevice> devs;
  src.list_devices(devs);
  CHECK_EQ(devs.size(), 3u);
  // devCode = stock device id, handed out as devices are identified (address is separate).
  CHECK_EQ(devs[0].dev_code, 0);    // auto table, not identified yet: no id
  CHECK_EQ(devs[0].address, 1);
  CHECK_EQ(devs[0].table_type, 0);  // auto table, not discovered yet: not reported
  CHECK_EQ(devs[0].dev_type, 0);
  CHECK(!devs[0].online);  // no traffic yet
  CHECK_EQ(devs[1].index, 1);
  CHECK_EQ(devs[1].dev_code, 1);
  CHECK_EQ(devs[1].address, 2);
  CHECK_EQ(devs[2].dev_code, 2);
  CHECK_EQ(devs[1].table_type, 0x2c);  // table type 44 (SBR) ...
  CHECK_EQ(devs[1].dev_type, 43);      // ... is cloud devType 43
  CHECK_EQ(devs[2].table_type, 0x12);
  CHECK_EQ(devs[2].dev_type, 7);
  CHECK(!devs[1].has_up);
  // Ids are stable across listings.
  std::vector<TelemetryDevice> again;
  src.list_devices(again);
  CHECK_EQ(again[1].dev_code, 1);
  CHECK_EQ(again[2].dev_code, 2);

  // Formatter: table-driven, raw value x mqtt scale (0x201a -> point 58601, u16 x0.1).
  store.set_scalar(1, 0x201a, dm::TYPE_U16, 0.0f, 2345, 2);
  BusinessFormatter f;
  std::string j = f.rundata(devs[1], &src, "20261018120000");
  CHECK(j.find("\"devCode\":1,\"devType\":43,") != std::string::npos);
  CHECK(j.find("\"58601\":\"234.5\"") != std::string::npos);
  // A sentinel cleared by the store is dropped.
  store.clear(1, 0x201a);
  CHECK(f.rundata(devs[1], &src, "t").find("58601") == std::string::npos);
}

// ---------------------------------------------------------------------------------------------
TEST(getparam_reader) {
  struct Pending {
    uint8_t addr, fn;
    uint16_t start, count;
    mb::AdhocCallback cb;
  };
  std::vector<Pending> q;
  ParamReader reader = make_modbus_param_reader([&](uint8_t addr, uint8_t fn, uint16_t start, uint16_t count,
                                                    mb::AdhocCallback cb) {
    q.push_back({addr, fn, start, count, std::move(cb)});
    return true;
  });
  int calls = 0;
  bool last_ok = false;
  uint16_t last_n = 0;
  ParamReadRequest r;
  r.addr = 1;
  r.fn = 4;
  r.reg = 5000;  // modbusAddress: document number, as the cloud sends it
  r.count = 2;
  CHECK(reader(r, [&](bool ok, const uint8_t *, uint16_t n) {
    calls++;
    last_ok = ok;
    last_n = n;
  }));
  CHECK_EQ(q.size(), 1u);
  CHECK_EQ(q[0].start, 5000);  // same numbering as read_registers (wire 4999, as stock)
  CHECK_EQ(q[0].fn, 4);
  CHECK_EQ(calls, 0);
  const uint8_t regs[] = {0x12, 0x34, 0xFF, 0xFE};
  q[0].cb(true, regs, 2);
  CHECK(calls == 1 && last_ok && last_n == 2);
  q.clear();
  // mode 1 (FC 0x77) and oversize reads are refused.
  r.fn = 0x77;
  CHECK(!reader(r, [](bool, const uint8_t *, uint16_t) {}));
  r.fn = 3;
  r.count = 200;
  CHECK(!reader(r, [](bool, const uint8_t *, uint16_t) {}));
  CHECK(q.empty());
}

int main() { return th::run_all(); }
