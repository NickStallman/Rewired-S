// Host tests for components/sungrow_control (allow-list, range checks, value mapping, gate +
// read-back poller on top of the real ModbusMaster with a fake inverter).
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../test_harness.h"
#include "control_core.h"
#include "mbtcp_core.h"
#include "write_registers.h"

using namespace esphome::sungrow_control;
namespace mb = esphome::sungrow_modbus;
namespace dm = esphome::sungrow_datamodel;

static uint32_t g_now = 0;

// Fake inverter: holding + input registers, echoes fn 6 / fn 16, answers fn 3 / fn 4. Reads
// covering an address in `rexc` and writes covering one in `wexc` get a Modbus exception.
struct FakeInv : mb::Transport {
  uint8_t addr{1};
  std::map<uint16_t, uint16_t> hold;   // 1-based
  std::map<uint16_t, uint16_t> input;  // 1-based (fn 4)
  std::set<uint16_t> rexc;
  std::map<uint16_t, uint8_t> wexc;  // address -> exception code
  std::deque<uint8_t> rx;
  std::vector<std::vector<uint8_t>> tx;
  bool mute{false};
  int writes{0};
  void send_frame(const uint8_t *d, size_t n) override {
    tx.emplace_back(d, d + n);
    if (mute || n < 8 || !mb::crc_ok(d, n) || d[0] != addr)
      return;
    const uint8_t fn = d[1];
    const uint16_t start = static_cast<uint16_t>((d[2] << 8 | d[3]) + 1);
    const uint16_t c = fn == 6 ? 1 : static_cast<uint16_t>(d[4] << 8 | d[5]);
    std::vector<uint8_t> f;
    uint8_t ex = 0;
    for (uint16_t i = 0; i < c && ex == 0; i++) {
      const uint16_t a = static_cast<uint16_t>(start + i);
      if ((fn == 3 || fn == 4) && rexc.count(a))
        ex = 2;
      if ((fn == 6 || fn == 16) && wexc.count(a))
        ex = wexc[a];
    }
    if (ex != 0) {
      f = {d[0], static_cast<uint8_t>(fn | 0x80), ex};
    } else if (fn == 6 || fn == 16) {
      writes++;
      if (fn == 6) {
        hold[start] = static_cast<uint16_t>(d[4] << 8 | d[5]);
      } else {
        for (uint16_t i = 0; i < c; i++)
          hold[start + i] = static_cast<uint16_t>(d[7 + 2 * i] << 8 | d[8 + 2 * i]);
      }
      f.assign(d, d + 6);
    } else {
      f = {d[0], fn, static_cast<uint8_t>(2 * c)};
      for (uint16_t i = 0; i < c; i++) {
        uint16_t v = fn == 3 ? hold[start + i] : fn == 4 ? input[start + i] : 0;
        f.push_back(static_cast<uint8_t>(v >> 8));
        f.push_back(static_cast<uint8_t>(v));
      }
    }
    f.resize(f.size() + 2);
    mb::append_crc(f.data(), f.size() - 2);
    rx.insert(rx.end(), f.begin(), f.end());
  }
  size_t receive(uint8_t *buf, size_t cap) override {
    size_t n = 0;
    while (n < cap && !rx.empty()) {
      buf[n++] = rx.front();
      rx.pop_front();
    }
    return n;
  }
  void discard_input() override { rx.clear(); }
  size_t count_fn(uint8_t fn) const {
    size_t n = 0;
    for (auto &f : tx)
      n += f[1] == fn;
    return n;
  }
};

struct Rig {
  FakeInv inv;
  dm::SignalStore store;
  mb::ModbusMaster m;
  ControlCore ctl;
  std::vector<std::string> logs;
  std::string last_text;
  std::vector<ExpectStatus> expects;  // every start/stop follow-up change
  explicit Rig(bool online = true, Profile prof = Profile::HYBRID, uint8_t protocol = 0) {
    g_now = 1000;
    store.set_clock([] { return g_now; });
    m.set_transport(&inv);
    m.set_store(&store);
    m.set_clock([] { return g_now; });
    mb::MasterTiming t;
    t.write_rate_limit_ms = 1000;
    m.set_timing(t);
    inv.mute = !online;
    mb::DeviceConfig d;
    d.index = 0;
    d.address = 1;
    d.table = dm::find_table("other_dev18_st6200");  // any fixed table: online after one poll
    m.add_device(d);
    m.start();
    ctl.set_master(&m);
    ctl.set_store(&store);
    ctl.set_clock([] { return g_now; });
    ctl.set_log([this](int, const char *msg) {
      logs.emplace_back(msg);
      if (getenv("CONTROL_TEST_LOG"))
        std::printf("    %s\n", msg);
    });
    ctl.set_result_listener([this](uint8_t, const char *text, bool) { last_text = text; });
    ctl.add_expect_listener([this](uint8_t, const ExpectStatus &st) { expects.push_back(st); });
    ControlCore::DeviceCfg c;
    c.index = 0;
    c.profile = find_profile(prof);
    c.rules = profile_rules(*c.profile, protocol);
    c.protocol = protocol;
    ctl.add_device(c);
    run(300);
  }
  /// Pretend device 0 is of grid-code family `fam` with the descriptor of (inv, protocol) and,
  /// optionally, the devinfo dev_type / dev_code of a model.
  void family(mb::XmlFamily fam, uint16_t inv_type, uint8_t protocol, uint8_t dev_type = 0, uint16_t dev_code = 0) {
    const mb::FamilyDescriptor *desc = mb::family_descriptor(inv_type, protocol);
    ctl.set_family_fn([fam, desc, dev_type, dev_code](uint8_t dev) {
      DeviceIdentity id;
      id.known = dev == 0;
      id.family = fam;
      id.desc = desc;
      id.dev_type = dev_type;
      id.dev_code = dev_code;
      return id;
    });
  }
  /// Running state of device 0 as the poller would store it.
  void state(uint16_t code) { store.set_scalar(0, dm::SIG_DEVICE_STATE, dm::TYPE_U16, 0.0f, code, 2); }
  /// Last follow-up text of device 0 ("" when none).
  std::string expect_text() {
    ExpectStatus st;
    return ctl.expect_status(0, &st) ? st.text : std::string();
  }
  /// Frames sent to the inverter with function `fn` starting at document register `doc`.
  size_t count_frames(uint8_t fn, uint16_t doc) const {
    size_t n = 0;
    for (auto &f : inv.tx)
      n += f[1] == fn && static_cast<uint16_t>((f[2] << 8 | f[3]) + 1) == doc;
    return n;
  }
  bool logged(const char *needle) const {
    for (auto &l : logs)
      if (l.find(needle) != std::string::npos)
        return true;
    return false;
  }
  /// Take the inverter away and bring it back: offline, then online with `hold` as it is then.
  void power_cycle(std::function<void()> while_off) {
    inv.mute = true;
    run(40000);
    CHECK(!m.devices()[0].online());
    while_off();
    inv.mute = false;
    run(20000);  // the next offline probe brings it back
    CHECK(m.devices()[0].online());
  }
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 5) {
      m.tick();
      ctl.tick();
      g_now += 5;
    }
  }
  /// Run until the setup batch is done (or `ms` passed).
  void run_setup(uint32_t ms) {
    for (uint32_t t = 0; t < ms && ctl.setup_busy(); t += 50)
      run(50);
  }
};

static std::vector<Rule> hybrid_rules() { return profile_rules(*find_profile(Profile::HYBRID)); }

// ---------------------------------------------------------------------------------------------
TEST(profiles_generated) {
  const ProfileDef *h = find_profile("hybrid");
  const ProfileDef *s = find_profile(Profile::STRING);
  CHECK(h != nullptr && s != nullptr);
  CHECK(find_profile("nope") == nullptr);
  // Document numbers (1-based table addresses); wire = address - 1.
  bool ems = false, cmd = false, pwr = false, rsv = false, sgsw = false, sgpct = false;
  for (uint8_t i = 0; i < h->nrules; i++) {
    ems |= h->rules[i].address == 13050 && std::strcmp(h->rules[i].key, "ems_mode") == 0;
    cmd |= h->rules[i].address == 13051 && std::strcmp(h->rules[i].key, "forced_cmd") == 0;
    pwr |= h->rules[i].address == 13052 && std::strcmp(h->rules[i].key, "forced_power") == 0;
    rsv |= h->rules[i].address == 13100 && std::strcmp(h->rules[i].key, "reserved_soc_backup") == 0;
  }
  for (uint8_t i = 0; i < s->nrules; i++) {
    sgsw |= s->rules[i].address == 5007 && std::strcmp(s->rules[i].key, "power_limit_switch") == 0;
    sgpct |= s->rules[i].address == 5008 && std::strcmp(s->rules[i].key, "power_limit_pct") == 0;
  }
  CHECK(ems && cmd && pwr && rsv && sgsw && sgpct);
  CHECK_EQ(SIG_EMS_MODE, 0xF100);
  // Read-back rows are fn 3 and cover every rule.
  const dm::RegisterTable *ht = h->readback[0].table;
  CHECK_EQ(ht->count, h->nrules);
  for (uint16_t i = 0; i < ht->count; i++)
    CHECK_EQ(ht->rows[i].fn, 3);
}

TEST(start_stop_address_by_protocol) {
  // Stock descriptor table: power_switch (0,4) -> 30045, (0,5) -> 13000, else 5006.
  auto has = [](const std::vector<Rule> &rs, const char *key, uint16_t addr) {
    for (const Rule &r : rs)
      if (std::strcmp(r.key, key) == 0)
        return r.address == addr;
    return false;
  };
  const ProfileDef *s = find_profile(Profile::STRING);
  const ProfileDef *h = find_profile(Profile::HYBRID);
  CHECK(has(profile_rules(*s, 0), "start_stop", 5006));
  CHECK(has(profile_rules(*s, 3), "start_stop", 5006));
  CHECK(has(profile_rules(*s, 4), "start_stop", 30045));
  CHECK(has(profile_rules(*h, 2), "hybrid_start_stop", 5006));
  CHECK(has(profile_rules(*h, 5), "hybrid_start_stop", 13000));
  // The read-back rows follow the chosen address (same sigid).
  const dm::RegisterTable *t4 = profile_readback(*s, 4).table;
  bool rb = false;
  for (uint16_t i = 0; i < t4->count; i++)
    rb |= t4->rows[i].sigid == SIG_START_STOP && t4->rows[i].regaddr == 30045;
  CHECK(rb);
  CHECK(profile_readback(*s, 3).table == profile_readback(*s, 0).table);
  // Binding a configured device to its protocol re-points rule + read-back.
  Rig r(true, Profile::HYBRID, 0);
  CHECK_EQ(r.ctl.address_for_sigid(0, SIG_HYBRID_START_STOP, 0), 5006);
  r.ctl.bind_protocol(0, 5);
  CHECK_EQ(r.ctl.address_for_sigid(0, SIG_HYBRID_START_STOP, 0), 13000);
  CHECK_EQ(r.ctl.address_for_sigid(0, SIG_EMS_MODE, 0), 13050);
  r.inv.hold[13000] = 0xCF;
  r.run(11000);
  uint64_t b = 0;
  CHECK(r.store.get_bits(0, SIG_HYBRID_START_STOP, &b) && b == 0xCF);
}

// The web UI posts a rule key; the address it resolves to is the one bound to the device's protocol
// type, so a start/stop from the Devices page passes the same gate as MQTT and the native entities.
TEST(web_write_by_key_resolves_protocol_address) {
  uint16_t addr = 0;
  {
    Rig r(true, Profile::STRING, 4);
    CHECK(std::strcmp(r.ctl.profile_name(0), "string") == 0);
    CHECK(r.ctl.address_for_key(0, "start_stop", &addr) && addr == 30045);
    CHECK(r.ctl.address_for_key(0, "power_limit_pct", &addr) && addr == 5008);
    CHECK(!r.ctl.address_for_key(0, "hybrid_start_stop", &addr));  // not in this profile
    CHECK(!r.ctl.address_for_key(0, "", &addr));
    CHECK(!r.ctl.address_for_key(5, "start_stop", &addr));  // no control entry
    CHECK(r.ctl.profile_name(5) == nullptr);
    // The resolved address passes the gate; the default one is refused on this protocol.
    uint16_t v = 0xCF;
    CHECK(r.ctl.write(0, 5006, &v, 1, false, "api:web", nullptr, {}, true) == Verdict::NOT_ALLOWED);
    CHECK(r.ctl.write(0, 30045, &v, 1, false, "api:web", nullptr, {}, true) == Verdict::OK);
    r.run(500);
    CHECK_EQ(r.inv.hold[30045], 0xCF);
  }
  {
    Rig r(true, Profile::HYBRID, 5);
    CHECK(std::strcmp(r.ctl.profile_name(0), "hybrid") == 0);
    CHECK(r.ctl.address_for_key(0, "hybrid_start_stop", &addr) && addr == 13000);
    r.ctl.bind_protocol(0, 2);
    CHECK(r.ctl.address_for_key(0, "hybrid_start_stop", &addr) && addr == 5006);
  }
}

TEST(allow_list_and_ranges) {
  auto r = hybrid_rules();
  uint16_t v;
  v = 2;  // Forced mode
  CHECK(check_rules(r, 13050, &v, 1).verdict == Verdict::OK);
  v = 8;  // MicroGrid: shown, never written
  CHECK(check_rules(r, 13050, &v, 1).verdict == Verdict::BAD_VALUE);
  v = 1;
  Check c = check_rules(r, 13050, &v, 1);
  CHECK(c.verdict == Verdict::BAD_VALUE && c.reg == 13050 && std::strcmp(c.key, "ems_mode") == 0);
  v = 0xAA;
  CHECK(check_rules(r, 13051, &v, 1).verdict == Verdict::OK);
  v = 0xAB;
  CHECK(check_rules(r, 13051, &v, 1).verdict == Verdict::BAD_VALUE);
  v = 10000;
  CHECK(check_rules(r, 13052, &v, 1).verdict == Verdict::OK);
  v = 10001;
  CHECK(check_rules(r, 13052, &v, 1).verdict == Verdict::BAD_VALUE);
  v = 499;  // max SOC 49.9 % < 50 %
  CHECK(check_rules(r, 13058, &v, 1).verdict == Verdict::BAD_VALUE);
  v = 950;
  CHECK(check_rules(r, 13058, &v, 1).verdict == Verdict::OK);
  // Wire-vs-doc trap: 13049 (mkaiser's wire address) is NOT a table address in the allow-list.
  v = 2;
  c = check_rules(r, 13049, &v, 1);
  CHECK(c.verdict == Verdict::NOT_ALLOWED && c.reg == 13049);
  v = 0xCF;  // start/stop 13000: never allowed by default
  CHECK(check_rules(r, 13000, &v, 1).verdict == Verdict::NOT_ALLOWED);
  // A range write: all registers listed -> ok; one hole -> NOT_ALLOWED at the hole.
  uint16_t three[3] = {2, 0xAA, 3000};
  CHECK(check_rules(r, 13050, three, 3).verdict == Verdict::OK);
  uint16_t four[4] = {2, 0xAA, 3000, 0};
  c = check_rules(r, 13050, four, 4);
  CHECK(c.verdict == Verdict::NOT_ALLOWED && c.reg == 13053);
  CHECK(check_rules(r, 13050, three, 0).verdict == Verdict::INVALID);
  CHECK(check_rules(r, 0xFFFF, three, 2).verdict == Verdict::INVALID);
  // YAML extra {address, count, min, max} and an s16 rule.
  Rule extra;
  extra.address = 13200;
  extra.count = 2;
  extra.min = -100;
  extra.max = 100;
  extra.type = dm::TYPE_S16;
  r.push_back(extra);
  uint16_t ex[2] = {static_cast<uint16_t>(-50), 100};
  CHECK(check_rules(r, 13200, ex, 2).verdict == Verdict::OK);
  ex[0] = static_cast<uint16_t>(-101);
  CHECK(check_rules(r, 13200, ex, 2).verdict == Verdict::BAD_VALUE);
  // String profile: switch + setting in one fn 16 (stock row 0x12A5 @5007 nreg 2).
  auto s = profile_rules(*find_profile(Profile::STRING));
  uint16_t sg[2] = {0xAA, 500};
  CHECK(check_rules(s, 5007, sg, 2).verdict == Verdict::OK);
  sg[1] = 1001;
  CHECK(check_rules(s, 5007, sg, 2).verdict == Verdict::BAD_VALUE);
}

TEST(exception_mapping) {
  CHECK_EQ(exception_for(Verdict::DISABLED), 1);
  CHECK_EQ(exception_for(Verdict::NOT_ALLOWED), 2);
  CHECK_EQ(exception_for(Verdict::BAD_VALUE), 3);
  CHECK_EQ(exception_for(Verdict::BUSY), 6);
  CHECK_EQ(exception_for(Verdict::OFFLINE), 0x0B);
  WriteResult r;
  r.status = WriteStatus::OK;
  CHECK_EQ(exception_for(r), 0);
  r.status = WriteStatus::TIMEOUT;
  CHECK_EQ(exception_for(r), 4);
  r.status = WriteStatus::EXCEPTION;
  r.exception = 3;
  CHECK_EQ(exception_for(r), 4);  // stock never forwards the inverter's code
  r.status = WriteStatus::RATE_LIMITED;
  CHECK_EQ(exception_for(r), 6);
}

TEST(select_and_number_mapping) {
  std::vector<Option> ems = {{0, "Self-consumption mode (default)"}, {2, "Forced mode"}, {3, "External EMS"},
                             {4, "VPP"}, {8, "MicroGrid"}};
  uint16_t raw = 99;
  CHECK(option_to_raw(ems, "Forced mode", &raw) && raw == 2);
  CHECK(!option_to_raw(ems, "forced mode", &raw));  // exact strings (battery controller compares them)
  CHECK(std::strcmp(raw_to_option(ems, 0), "Self-consumption mode (default)") == 0);
  CHECK(raw_to_option(ems, 1) == nullptr);
  // max SOC 95 % -> 950 (x0.1), 49 % rejected by the range.
  CHECK(number_to_raw(95.0f, 0.1f, dm::TYPE_U16, 500, 1000, &raw) && raw == 950);
  CHECK(!number_to_raw(49.0f, 0.1f, dm::TYPE_U16, 500, 1000, &raw));
  // battery max charge power 5000 W -> 500 (10 W units); 5004 W rounds to 500.
  CHECK(number_to_raw(5004.0f, 10.0f, dm::TYPE_U16, 1, 1500, &raw) && raw == 500);
  CHECK(number_to_raw(2350.0f, 1.0f, dm::TYPE_U16, 0, 10000, &raw) && raw == 2350);
  CHECK(!number_to_raw(NAN, 1.0f, dm::TYPE_U16, 0, 10000, &raw));
  CHECK(number_to_raw(-5.0f, 1.0f, dm::TYPE_S16, -100, 100, &raw) && raw == 0xFFFB);
  CHECK(std::fabs(raw_to_number(950, dm::TYPE_U16, 0.1f) - 95.0f) < 1e-3);
  CHECK(std::fabs(raw_to_number(0xFFFB, dm::TYPE_S16, 1.0f) + 5.0f) < 1e-6);
}

TEST(gate_disabled_by_default) {
  Rig r;
  CHECK(!r.ctl.enabled());
  uint16_t v = 2;
  int calls = 0;
  CHECK(r.ctl.write(0, 13050, &v, 1, false, "test", [&](const WriteResult &) { calls++; }) == Verdict::DISABLED);
  r.run(500);
  CHECK_EQ(calls, 0);
  CHECK_EQ(r.inv.writes, 0);
  CHECK(r.last_text.find("writes disabled") != std::string::npos);
  CHECK_EQ(r.ctl.writes_rejected(), 1u);
}

TEST(gate_write_verify_and_readback_store) {
  Rig r;
  r.ctl.set_enabled(true);
  uint16_t v = 2;
  WriteResult res;
  CHECK(r.ctl.write(0, 13050, &v, 1, true, "test", [&](const WriteResult &x) { res = x; }) == Verdict::OK);
  r.run(1000);
  CHECK(res.status == WriteStatus::OK);
  CHECK_EQ(r.inv.hold[13050], 2);
  CHECK_EQ(r.inv.count_fn(6), 1u);
  // The read-back signal reflects the inverter (from verify / the poller), not the request.
  CHECK(r.store.is_valid(0, SIG_EMS_MODE));
  uint64_t bits = 0;
  CHECK(r.store.get_bits(0, SIG_EMS_MODE, &bits) && bits == 2);
  CHECK_EQ(r.ctl.writes_ok(), 1u);
  bool logged = false;
  for (auto &l : r.logs)
    logged |= l.find("13050=[2]") != std::string::npos && l.find("ok") != std::string::npos;
  CHECK(logged);
  // Not allowed / bad value never reach the bus.
  v = 1;
  CHECK(r.ctl.write(0, 13050, &v, 1, false, "test", nullptr) == Verdict::BAD_VALUE);
  CHECK(r.ctl.write(0, 13049, &v, 1, false, "test", nullptr) == Verdict::NOT_ALLOWED);
  CHECK(r.ctl.write(5, 13050, &v, 1, false, "test", nullptr) == Verdict::NOT_ALLOWED);  // no control entry
  r.run(500);
  CHECK_EQ(r.inv.writes, 1);
  // Rate limit (1 s in the rig) surfaces as BUSY.
  v = 0;
  CHECK(r.ctl.write(0, 13050, &v, 1, false, "test", nullptr) == Verdict::OK);
  CHECK(r.ctl.write(0, 13050, &v, 1, false, "test", nullptr) == Verdict::BUSY);
}

TEST(gate_offline_rejected) {
  Rig r(false);  // inverter never answers -> never online
  r.ctl.set_enabled(true);
  uint16_t v = 2;
  CHECK(r.ctl.write(0, 13050, &v, 1, false, "test", nullptr) == Verdict::OFFLINE);
  r.run(100);
  CHECK_EQ(r.inv.count_fn(6), 0u);
}

TEST(readback_poller) {
  Rig r;
  r.inv.hold[13050] = 0;
  r.inv.hold[13051] = 0xCC;
  r.inv.hold[13052] = 0;
  r.inv.hold[13058] = 1000;
  r.inv.hold[33047] = 500;
  r.run(10500);  // the first round (at start) read zeros; the next one sees these
  CHECK(r.ctl.readbacks() >= 5u);
  uint64_t b = 0;
  CHECK(r.store.get_bits(0, SIG_FORCED_CMD, &b) && b == 0xCC);
  CHECK(std::fabs(r.store.get_number(0, SIG_MAX_SOC) - 100.0) < 1e-3);
  CHECK(std::fabs(r.store.get_number(0, SIG_BATTERY_MAX_CHARGE_POWER) - 5000.0) < 1e-6);
  // The inverter changes it (e.g. iSolarCloud / app): the entity follows within one period.
  r.inv.hold[13051] = 0xAA;
  r.run(10500);
  CHECK(r.store.get_bits(0, SIG_FORCED_CMD, &b) && b == 0xAA);
  // Offline -> read-back values cleared (entities unknown, the controller sees "dead").
  r.inv.mute = true;
  r.run(40000);
  CHECK(!r.m.devices()[0].online());
  CHECK(!r.store.is_valid(0, SIG_FORCED_CMD));
}

// ---------------------------------------------------------------------------------------------
// :502 end to end: mbtcp_core + gate + master + fake inverter, as sungrow_modbus_tcp glues them.
namespace tcp = esphome::sungrow_modbus_tcp;

struct TcpGate : tcp::StaticBackend {
  ControlCore *ctl;
  TcpGate(const dm::SignalStore *s, ControlCore *c) : tcp::StaticBackend(s), ctl(c) {}
  uint8_t check_write(const tcp::UnitTarget &t, uint8_t, uint16_t start, const uint16_t *v, uint16_t n) override {
    return exception_for(ctl->check(t.device, start, v, n).verdict);
  }
};

/// Send one :502 request; returns the reply bytes once the RS485 side is done.
static std::vector<uint8_t> mbtcp(Rig &r, TcpGate &be, const std::vector<uint8_t> &q) {
  tcp::FrameAssembler fa;
  fa.feed(q.data(), q.size());
  tcp::Frame f;
  CHECK(fa.peek(&f) == tcp::FrameAssembler::Status::FRAME);
  uint8_t out[tcp::MAX_ADU_LEN];
  size_t n = 0;
  tcp::Options opt;
  tcp::Decision d = tcp::handle_frame(f, be, opt, out, &n);
  std::vector<uint8_t> reply(out, out + n);
  if (d.kind != tcp::Decision::WRITE)
    return reply;
  const tcp::ReqHeader h = f.hdr;
  bool done = false;
  mb::WriteOptions wo;
  wo.fn = d.fn;
  const uint16_t echo = d.fn == 6 ? d.values[0] : d.count;
  Verdict v = r.ctl.write(d.device, d.start, d.values, d.count, false, "mbtcp test",
                          [&](const WriteResult &res) {
                            size_t m = tcp::build_write_reply(h, d.start, echo, exception_for(res), out);
                            reply.assign(out, out + m);
                            done = true;
                          },
                          wo);
  if (v != Verdict::OK) {
    size_t m = tcp::build_write_reply(h, d.start, echo, exception_for(v), out);
    return std::vector<uint8_t>(out, out + m);
  }
  for (int i = 0; i < 2000 && !done; i++)
    r.run(5);
  CHECK(done);
  return reply;
}

TEST(mbtcp_write_end_to_end) {
  Rig r;
  TcpGate be(&r.store, &r.ctl);
  tcp::UnitTarget tgt;
  tgt.device = 0;
  tgt.table = dm::tcpslave_table_for_dev_type(35);
  tgt.rtu_address = 1;
  be.add_unit(1, tgt);
  // mkaiser select.ems_mode -> "Forced mode": modbus.write_register(address 13049, value 2).
  const std::vector<uint8_t> ems = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x01, 0x06, 0x32, 0xF9, 0x00, 0x02};
  // 1) writes disabled (default): exception 1, nothing on RS485.
  auto rep = mbtcp(r, be, ems);
  CHECK(rep.size() == 9 && rep[7] == 0x86 && rep[8] == 0x01);
  CHECK_EQ(r.inv.writes, 0);
  // 2) enabled: forwarded as fn 6 to wire 13049, reply = request byte for byte.
  r.ctl.set_enabled(true);
  rep = mbtcp(r, be, ems);
  CHECK(rep == ems);
  CHECK_EQ(r.inv.hold[13050], 2);
  CHECK_EQ(r.inv.count_fn(6), 1u);
  CHECK(r.inv.tx.back()[1] == 6 || r.inv.tx.back()[1] == 3);  // (read-back may follow)
  // 3) mkaiser number.export_power_limit 10000 W -> 13073 (doc 13074).
  const std::vector<uint8_t> epl = {0x00, 0x02, 0x00, 0x00, 0x00, 0x06, 0x01, 0x06, 0x33, 0x11, 0x27, 0x10};
  rep = mbtcp(r, be, epl);
  CHECK(rep == epl);
  CHECK_EQ(r.inv.hold[13074], 10000);
  // 4) not in the allow-list (start/stop 13000 = wire 12999): exception 2.
  rep = mbtcp(r, be, {0x00, 0x03, 0x00, 0x00, 0x00, 0x06, 0x01, 0x06, 0x32, 0xC7, 0x00, 0xCE});
  CHECK(rep.size() == 9 && rep[8] == 0x02);
  // 5) bad value (EMS 7): exception 3.
  rep = mbtcp(r, be, {0x00, 0x04, 0x00, 0x00, 0x00, 0x06, 0x01, 0x06, 0x32, 0xF9, 0x00, 0x07});
  CHECK(rep.size() == 9 && rep[8] == 0x03);
  // 6) same register again within the 1 s rate limit: busy (6).
  rep = mbtcp(r, be, ems);
  CHECK(rep.size() == 9 && rep[8] == 0x06);
  // 7) fn 16 range write of EMS + cmd + power, after the rate-limit window.
  r.run(1100);
  const std::vector<uint8_t> multi = {0x00, 0x05, 0x00, 0x00, 0x00, 0x0D, 0x01, 0x10, 0x32, 0xF9, 0x00, 0x03,
                                      0x06, 0x00, 0x00, 0x00, 0xCC, 0x00, 0x00};
  rep = mbtcp(r, be, multi);
  const std::vector<uint8_t> multi_ok = {0x00, 0x05, 0x00, 0x00, 0x00, 0x06, 0x01, 0x10, 0x32, 0xF9, 0x00, 0x03};
  CHECK(rep == multi_ok);
  CHECK_EQ(r.inv.count_fn(16), 1u);
  CHECK_EQ(r.inv.hold[13051], 0xCC);
  // 8) inverter silent: exception 4 after the RS485 timeout.
  r.run(1100);
  r.inv.mute = true;
  rep = mbtcp(r, be, ems);
  CHECK(rep.size() == 9 && rep[8] == 0x04);
  // 9) offline: 0x0B without touching the bus.
  r.run(40000);
  CHECK(!r.m.devices()[0].online());
  size_t sent = r.inv.tx.size();
  rep = mbtcp(r, be, ems);
  CHECK(rep.size() == 9 && rep[8] == 0x0B);
  CHECK(r.inv.tx.size() - sent <= 1u);  // at most an offline probe
}

// ---- Setup / Installer ------------------------------------------------------------------------

// A plausible str_pv / unit protection block 32318..32329 (0.1 V / 0.01 Hz): recovery OV/UV/OF/UF,
// stage 1 UV/OV/UF/OF, stage 2 UV/OV/UF/OF. Satisfies every stock relation.
static const uint16_t PROT[12] = {2530, 2160, 5015, 4985, 1800, 2650, 4700, 5200, 700, 2750, 4600, 5300};

static void load_protection(Rig &r) {
  uint16_t a[12];
  for (uint16_t i = 0; i < 12; i++) {
    r.inv.hold[32318 + i] = PROT[i];
    a[i] = static_cast<uint16_t>(32318 + i);
  }
  r.ctl.grid_read_request(0, a, 12);
  r.run(1000);
  CHECK(r.ctl.grid_read_complete());
}

TEST(setup_apply_batch_and_allowlist) {
  Rig r;
  r.family(mb::XmlFamily::STR_PV, 0, 1);
  // Self-authorising (confirmed web path): no set_enabled() needed.
  std::vector<ControlCore::SetupReg> bad = {{1234, 5}};
  CHECK(r.ctl.setup_apply(0, bad, "test") == Verdict::NOT_ALLOWED);
  CHECK(r.ctl.setup_detail() == "register 1234");
  CHECK(!r.ctl.setup_busy());
  // Routine value out of range (13050 = 8 MicroGrid is shown, never written).
  std::vector<ControlCore::SetupReg> badval = {{13050, 8}};
  CHECK(r.ctl.setup_apply(0, badval, "test") == Verdict::BAD_VALUE);
  std::vector<ControlCore::SetupReg> dup = {{30500, 6}, {30500, 6}};
  CHECK(r.ctl.setup_apply(0, dup, "test") == Verdict::INVALID);
  // Routine reg + str_pv commit registers (no range / relation on 30502).
  std::vector<ControlCore::SetupReg> batch = {{13050, 2}, {30500, 6}};
  CHECK(r.ctl.setup_apply(0, batch, "test") == Verdict::OK);
  CHECK(r.ctl.setup_busy());
  CHECK(r.ctl.setup_apply(0, batch, "test") == Verdict::BUSY);
  r.run_setup(5000);
  CHECK(!r.ctl.setup_busy());
  CHECK_EQ(r.ctl.setup_total(), 2);
  CHECK_EQ(r.ctl.setup_ok(), 2);
  CHECK_EQ(r.ctl.setup_failed_n(), 0);
  CHECK_EQ(r.inv.hold[13050], 2);
  CHECK_EQ(r.inv.hold[30500], 6);
  CHECK_EQ(r.ctl.writes_ok(), 2u);  // grid frames count too
}

TEST(setup_family_scope) {
  Rig r;
  r.family(mb::XmlFamily::STR_PV, 0, 1);
  load_protection(r);
  // The res-family block that knocked the SG5.0RS offline is not str_pv.
  std::vector<ControlCore::SetupReg> res = {{30646, 1}};
  CHECK(r.ctl.setup_apply(0, res, "test") == Verdict::NOT_ALLOWED);
  std::vector<ControlCore::SetupReg> ok = {{32318, 2540}};
  CHECK(r.ctl.setup_apply(0, ok, "test") == Verdict::OK);
  r.run_setup(5000);
  CHECK_EQ(r.inv.hold[32318], 2540);
  // Family none: nothing.
  r.family(mb::XmlFamily::NONE, 0, 4);
  CHECK(r.ctl.setup_apply(0, ok, "test") == Verdict::NOT_ALLOWED);
  // Default identity from the master: a device without a descriptor (unit_nodesc) allows nothing.
  r.ctl.set_family_fn(nullptr);
  CHECK(r.ctl.identity(0).known);
  CHECK(r.ctl.identity(0).desc == nullptr);
  CHECK(r.ctl.setup_apply(0, ok, "test") == Verdict::NOT_ALLOWED);
  CHECK(!r.ctl.identity(7).known);
}

TEST(setup_range_and_relation) {
  Rig r;
  r.family(mb::XmlFamily::STR_PV, 0, 1);
  // No cached peers yet: 32318 > 32319 cannot be evaluated.
  std::vector<ControlCore::SetupReg> b = {{32318, 2530}};
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::BAD_VALUE);
  CHECK(r.ctl.setup_detail() == "relation:32319");
  // The profile path writes the XML values verbatim (no value checks).
  CHECK(r.ctl.setup_apply(0, b, "test", true) == Verdict::OK);
  r.run_setup(5000);
  load_protection(r);
  // OV1 2650.0 V (a typo for 265.0 V): outside the stock range.
  b = {{32323, 26500}};
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::BAD_VALUE);
  CHECK(r.ctl.setup_detail() == "range:32323");
  // UV2 above UV1.
  b = {{32326, 1900}};
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::BAD_VALUE);
  CHECK(r.ctl.setup_detail().rfind("relation:", 0) == 0);
  // OV1 above OV2.
  b = {{32323, 2800}};
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::BAD_VALUE);
  CHECK(r.ctl.setup_detail() == "relation:32323<=32327");
  // Both moved together in one batch: evaluated over the batch values.
  b = {{32323, 2800}, {32327, 2900}};
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::OK);
  CHECK(r.ctl.setup_detail().empty());
  r.run_setup(5000);
  CHECK_EQ(r.inv.hold[32323], 2800);
  CHECK_EQ(r.inv.hold[32327], 2900);
  // OV1 == OV2 is allowed (<=).
  b = {{32323, 2900}};
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::OK);
}

TEST(setup_commit_first) {
  Rig r;
  r.family(mb::XmlFamily::STR_PV, 0, 1);
  load_protection(r);
  std::vector<ControlCore::SetupReg> b = {{32322, 1900}, {30500, 6}};
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::OK);
  CHECK_EQ(r.ctl.setup_frames().size(), 2u);
  CHECK_EQ(r.ctl.setup_frames()[0].start, 30500);
  CHECK_EQ(r.ctl.setup_frames()[1].start, 32322);
  const size_t before = r.inv.tx.size();
  r.run_setup(5000);
  std::vector<uint16_t> order;
  for (size_t i = before; i < r.inv.tx.size(); i++)
    if (r.inv.tx[i][1] == 16)
      order.push_back(static_cast<uint16_t>((r.inv.tx[i][2] << 8 | r.inv.tx[i][3]) + 1));
  CHECK(order.size() == 2 && order[0] == 30500 && order[1] == 32322);
}

TEST(setup_offline_sync_reject_and_deadline) {
  {
    Rig r(false);
    r.family(mb::XmlFamily::STR_PV, 0, 1);
    std::vector<ControlCore::SetupReg> b = {{30500, 6}};
    CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::OFFLINE);
    CHECK(!r.ctl.setup_busy());
    r.ctl.set_family_fn(nullptr);
    CHECK(r.ctl.setup_apply(9, b, "test") == Verdict::OFFLINE);  // unknown device
  }
  Rig r;
  r.family(mb::XmlFamily::STR_PV, 0, 1);
  // Twelve separate frames (profile path: no relation checks).
  std::vector<ControlCore::SetupReg> b = {{32318, 2530}, {32320, 5015}, {32322, 1800}, {32324, 4700},
                                          {32326, 700},  {32328, 4600}, {30092, 60},   {31200, 85},
                                          {31230, 85},   {31400, 85},   {32362, 150},  {32364, 150}};
  const uint32_t t0 = g_now;
  CHECK(r.ctl.setup_apply(0, b, "test", true) == Verdict::OK);
  CHECK_EQ(r.ctl.setup_frames().size(), 12u);
  CHECK(r.ctl.setup_deadline_left() > 59000);
  r.run(150);
  r.inv.mute = true;  // the device drops mid-batch
  while (g_now - t0 < 61000)
    r.run(100);
  CHECK(!r.ctl.setup_busy());
  CHECK(r.ctl.setup_ok() > 0 && r.ctl.setup_ok() < 12);
  CHECK_EQ(r.ctl.setup_ok() + r.ctl.setup_failed_n(), 12);
  CHECK_EQ(r.ctl.setup_deadline_left(), 0u);
  size_t expired = 0;
  for (const auto &f : r.ctl.setup_frames()) {
    CHECK(f.status != WriteStatus::QUEUED);
    expired += f.expired;
  }
  CHECK(expired > 0);
  // Back online: nothing from the dropped batch is sent.
  const int w = r.inv.writes;
  const size_t fn16 = r.inv.count_fn(16);
  r.inv.mute = false;
  r.run(40000);
  CHECK(r.m.devices()[0].online());
  CHECK_EQ(r.inv.writes, w);
  CHECK_EQ(r.inv.count_fn(16), fn16);
}

TEST(setup_frames_results) {
  Rig r;
  r.family(mb::XmlFamily::STR_PV, 0, 1);
  load_protection(r);
  r.inv.wexc[32322] = 3;
  const uint32_t failed0 = r.ctl.writes_failed();
  std::vector<ControlCore::SetupReg> b = {{32322, 1900}, {30500, 6}};
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::OK);
  CHECK(r.ctl.setup_frames()[1].status == WriteStatus::QUEUED);
  r.run_setup(5000);
  const auto &f = r.ctl.setup_frames();
  CHECK_EQ(f.size(), 2u);
  CHECK(f[0].start == 30500 && f[0].count == 1 && f[0].status == WriteStatus::OK);
  CHECK(f[1].start == 32322 && f[1].status == WriteStatus::EXCEPTION && f[1].exception == 3);
  CHECK(std::strcmp(frame_status_name(f[1].status, f[1].expired), "exception") == 0);
  CHECK_EQ(r.ctl.setup_ok(), 1);
  CHECK_EQ(r.ctl.setup_failed_n(), 1);
  CHECK_EQ(r.ctl.writes_failed(), failed0 + 1);
}

TEST(grid_read_split_and_reason) {
  Rig r;
  r.family(mb::XmlFamily::STR_PV, 0, 1);
  for (uint16_t i = 0; i < 12; i++)
    r.inv.hold[32318 + i] = PROT[i];
  r.inv.rexc.insert(32323);  // one unimplemented register in the middle of the run
  r.inv.hold[30644] = 0;
  std::vector<uint16_t> a;
  for (uint16_t i = 0; i < 12; i++)
    a.push_back(static_cast<uint16_t>(32318 + i));
  a.push_back(1234);   // off-list
  a.push_back(30644);  // diagnostic
  r.ctl.grid_read_request(0, a.data(), static_cast<uint16_t>(a.size()));
  CHECK_EQ(r.ctl.grid_read_device(), 0);
  r.run(3000);
  CHECK(r.ctl.grid_read_complete());
  int valid = 0;
  for (const auto &e : r.ctl.grid_read_results()) {
    if (e.address == 1234) {
      CHECK(!e.valid && e.reason == ReadReason::OFF_LIST);
    } else if (e.address == 32323) {
      CHECK(!e.valid && e.reason == ReadReason::EXCEPTION);
    } else {
      CHECK(e.valid && e.reason == ReadReason::OK);
      if (e.address >= 32318 && e.address <= 32329)
        CHECK_EQ(e.value, PROT[e.address - 32318]);
      valid++;
    }
  }
  CHECK_EQ(valid, 12);
  CHECK_EQ(r.ctl.grid_read_results().size(), 14u);
  CHECK(std::strcmp(read_reason_name(ReadReason::OFF_LIST), "off-list") == 0);
  // A silent device: timeout.
  r.inv.mute = true;
  const uint16_t one = 32318;
  r.ctl.grid_read_request(0, &one, 1);
  r.run(6000);  // a poll in flight may time out first
  CHECK(r.ctl.grid_read_complete());
  CHECK(r.ctl.grid_read_results()[0].reason == ReadReason::TIMEOUT);
}

// The live SG5.0RS: protocol 3 (tag ZS), inverter type 0 -> unit, descriptor 32310 / 32311 / 5006,
// W32B5 + W29B12 set, input 5479 = 1 (PV), country 6, 30036 = 13 (AS/NZS 4777.2:2020 Region A).
static void sg5rs_capabilities(Rig &r) {
  uint8_t bm[64] = {};
  bm[2 * (32 - 1) + 1] |= 1u << 5;         // word 32 bit 5
  bm[2 * (29 - 1)] |= 1u << (12 - 8);      // word 29 bit 12
  r.store.set(0, dm::SIG_CAPABILITY_BITMAP, dm::TYPE_STRING, 0.0f, dm::ValueKind::STRING, bm, sizeof(bm));
}

static std::vector<ResolvedItem> unit13_items() {
  SetupContext ctx;
  ctx.family = mb::XmlFamily::UNIT;
  ctx.desc = mb::family_descriptor(0, 3);
  ctx.w32b5 = true;
  ctx.w29b12 = true;
  ctx.have_5479 = true;
  ctx.reg5479 = 1;
  std::vector<ResolvedItem> items;
  CHECK(resolve_profile(ctx, 13, &items) == ResolveStatus::OK);
  return items;
}

static const ControlCore::Prepared *prepare(Rig &r, uint16_t option) {
  CHECK(r.ctl.setup_prepare(0, option) == Verdict::OK);
  for (int i = 0; i < 100; i++) {
    const ControlCore::Prepared *p = r.ctl.setup_prepared(0, option);
    if (p != nullptr && p->complete())
      return p;
    r.run(100);
  }
  return r.ctl.setup_prepared(0, option);
}

TEST(setup_prepare_needs_capability_bitmap) {
  // A protocol 3 device whose capability bitmap has not been polled yet cannot be resolved: the
  // 30036 item depends on W32B5, which is only known from sig 0x2181.
  Rig r(true, Profile::STRING, 3);
  r.family(mb::XmlFamily::UNIT, 0, 3);
  r.inv.input[5479] = 1;
  CHECK(!r.ctl.feature_w32b5(0));
  const ControlCore::Prepared *p = prepare(r, 13);
  CHECK(p != nullptr && p->complete());
  CHECK(p->status == ResolveStatus::NEED_CAPABILITY);
  CHECK(p->items.empty());
  CHECK(r.ctl.setup_apply_profile(0, 13, "test") == Verdict::PREVIEW_REQUIRED);
  sg5rs_capabilities(r);
  p = prepare(r, 13);
  CHECK(p != nullptr && p->complete() && p->status == ResolveStatus::OK);
  CHECK(p->items.size() > 100 && p->items[1].address == 30036);
}

TEST(setup_prepare_preview_apply_unit13) {
  Rig r(true, Profile::STRING, 3);
  r.family(mb::XmlFamily::UNIT, 0, 3);
  sg5rs_capabilities(r);
  CHECK(r.ctl.feature_w32b5(0) && r.ctl.feature_w29b12(0));
  r.inv.input[5479] = 1;
  const std::vector<ResolvedItem> want = unit13_items();
  CHECK(want.size() > 100);
  CHECK(want[0].address == 32310 && want[0].value == 6 && want[1].address == 30036 && want[1].value == 13);
  for (const ResolvedItem &it : want)
    r.inv.hold[it.address] = it.value;
  // No preview yet.
  CHECK(r.ctl.setup_apply_profile(0, 13, "test") == Verdict::PREVIEW_REQUIRED);
  // Option picker reads.
  r.ctl.setup_info_request(0);
  r.run(1000);
  CHECK(r.ctl.setup_info().complete());
  CHECK(r.ctl.setup_info_fresh(0, 10000));
  const auto *i5479 = r.ctl.setup_info().find(4, 5479);
  const auto *icountry = r.ctl.setup_info().find(3, 32310);
  const auto *icompany = r.ctl.setup_info().find(3, 30036);
  CHECK(i5479 && i5479->valid && i5479->value == 1);
  CHECK(icountry && icountry->valid && icountry->value == 6);
  CHECK(icompany && icompany->valid && icompany->value == 13);
  // Preview of the unchanged inverter: nothing differs.
  const ControlCore::Prepared *p = prepare(r, 13);
  CHECK(p != nullptr && p->complete());
  CHECK(p->status == ResolveStatus::OK);
  CHECK(p->have_5479 && p->reg5479 == 1 && p->variant >= 0);
  CHECK_EQ(p->items.size(), want.size());
  CHECK_EQ(p->n_changed(), 0u);
  // The live inverter's 32362 / 32370 differ from the 2020 Region A PV profile.
  r.inv.hold[32362] = 150;
  r.inv.hold[32370] = 100;
  p = prepare(r, 13);
  CHECK(p != nullptr && p->complete());
  std::vector<uint16_t> changed;
  for (const ResolvedItem &it : p->items) {
    uint16_t cur = 0;
    if (!p->current(it.address, &cur) || cur != it.value)
      changed.push_back(it.address);
  }
  CHECK(changed.size() == 2 && changed[0] == 32362 && changed[1] == 32370);
  CHECK_EQ(p->n_changed(), 2u);
  // A prepare of another option discards this one.
  CHECK(r.ctl.setup_prepare(0, 14) == Verdict::OK);
  CHECK(r.ctl.setup_prepared(0, 13) == nullptr);
  CHECK(r.ctl.setup_apply_profile(0, 13, "test") == Verdict::PREVIEW_REQUIRED);
  // Apply: every item, in resolver order, country first and 30036 second.
  p = prepare(r, 13);
  CHECK(p != nullptr && p->complete());
  std::vector<uint16_t> countries;
  r.ctl.set_country_written_cb([&](uint16_t c) { countries.push_back(c); });
  CHECK(r.ctl.setup_apply_profile(0, 13, "test") == Verdict::OK);
  CHECK(r.ctl.setup_prepared(0, 13) == nullptr);  // single use
  CHECK_EQ(r.ctl.setup_total(), want.size());
  const auto &f = r.ctl.setup_frames();
  CHECK(f.size() >= 3 && f[0].start == 32310 && f[0].count == 1 && f[1].start == 30036);
  r.run_setup(50000);
  CHECK(!r.ctl.setup_busy());
  CHECK_EQ(r.ctl.setup_ok(), want.size());
  for (const ResolvedItem &it : want)
    CHECK_EQ(r.inv.hold[it.address], it.value);
  CHECK(countries.size() == 1 && countries[0] == 6);
  // Again: PREVIEW_REQUIRED (the prepare was used).
  CHECK(r.ctl.setup_apply_profile(0, 13, "test") == Verdict::PREVIEW_REQUIRED);
}

TEST(setup_start_uses_descriptor_register) {
  {
    // (0,5) residential ES (SH hybrid): power_switch 13000 via the hybrid start/stop rule.
    Rig r(true, Profile::HYBRID, 5);
    r.family(mb::XmlFamily::RES_ES, 0, 5);
    CHECK(r.ctl.setup_start(0, "test") == Verdict::OK);
    r.run(1000);
    CHECK_EQ(r.inv.hold[13000], 0xCF);
    CHECK_EQ(r.inv.count_fn(16), 1u);
  }
  {
    // The SG5.0RS (protocol 3, unit): 5006.
    Rig r(true, Profile::STRING, 3);
    r.family(mb::XmlFamily::UNIT, 0, 3);
    CHECK(r.ctl.setup_start(0, "test") == Verdict::OK);
    r.run(1000);
    CHECK_EQ(r.inv.hold[5006], 0xCF);
  }
  {
    // No profile rule for the descriptor register: a dedicated allow of exactly 0xCF.
    Rig r(true, Profile::HYBRID, 0);
    r.family(mb::XmlFamily::RES_ES, 0, 5);
    CHECK(r.ctl.setup_start(0, "test") == Verdict::OK);
    r.run(1000);
    CHECK_EQ(r.inv.hold[13000], 0xCF);
    uint16_t stop = 0xCE;
    CHECK(r.ctl.write(0, 13000, &stop, 1, false, "test", nullptr, {}, true) == Verdict::NOT_ALLOWED);
  }
  {
    // No descriptor: nothing to start.
    Rig r;
    r.family(mb::XmlFamily::UNIT_NODESC, 2, 3);
    CHECK(r.ctl.setup_start(0, "test") == Verdict::NOT_ALLOWED);
  }
}

TEST(web_write_bypasses_switch) {
  Rig r;
  CHECK(!r.ctl.enabled());
  uint16_t v = 2;
  // Web interface (authorised): the switch does not apply; allow-list + range still do.
  CHECK(r.ctl.write(0, 13050, &v, 1, false, "api:web", nullptr, {}, true) == Verdict::OK);
  uint16_t bad = 8;
  CHECK(r.ctl.write(0, 13051, &bad, 1, false, "api:web", nullptr, {}, true) == Verdict::BAD_VALUE);
  CHECK(r.ctl.write(0, 13049, &v, 1, false, "api:web", nullptr, {}, true) == Verdict::NOT_ALLOWED);
  // MQTT / :502 / HA action (unauthorised): DISABLED.
  CHECK(r.ctl.write(0, 13052, &v, 1, false, "mqtt", nullptr) == Verdict::DISABLED);
  r.run(500);
  CHECK_EQ(r.inv.hold[13050], 2);
  CHECK_EQ(r.inv.writes, 1);
}

TEST(clock_write_bypasses_switch) {
  Rig r;
  CHECK(!r.ctl.enabled());
  const uint16_t t[6] = {2026, 10, 24, 20, 15, 7};
  WriteResult res;
  CHECK(r.ctl.write_clock(0, t, "clock sync", [&](const WriteResult &x) { res = x; }) == Verdict::OK);
  r.run(1000);
  CHECK(res.status == WriteStatus::OK);
  CHECK_EQ(r.inv.count_fn(16), 1u);
  for (int i = 0; i < 6; i++)
    CHECK_EQ(r.inv.hold[5000 + i], t[i]);
  // The inverter's own ranges; no profile (battery) -> not allowed.
  const uint16_t bad[6] = {2026, 13, 24, 20, 15, 7};
  CHECK(r.ctl.write_clock(0, bad, "clock sync", nullptr) == Verdict::BAD_VALUE);
  const uint16_t y[6] = {1999, 1, 1, 0, 0, 0};
  CHECK(r.ctl.write_clock(0, y, "clock sync", nullptr) == Verdict::BAD_VALUE);
  CHECK(r.ctl.write_clock(5, t, "clock sync", nullptr) == Verdict::NOT_ALLOWED);
  // The clock stays out of the normal allow-list: HA / :502 / MQTT cannot write it.
  r.ctl.set_enabled(true);
  CHECK(r.ctl.write(0, 5000, t, 6, false, "mqtt", nullptr) == Verdict::NOT_ALLOWED);
}

TEST(diag_write_only_capture_triggers) {
  Rig r;
  CHECK(!r.ctl.enabled());
  const uint16_t iv = 0x01A0;
  WriteResult res;
  CHECK(r.ctl.write_diag(0, 32000, &iv, 1, "capture", [&](const WriteResult &x) { res = x; }) == Verdict::OK);
  r.run(1000);
  CHECK(res.status == WriteStatus::OK);
  CHECK_EQ(r.inv.count_fn(16), 1u);  // fn 16 even for one register
  CHECK_EQ(r.inv.hold[32000], 0x01A0);
  const uint16_t sel = 0x00A0, grid = 0x01B0, fmt = 0;
  CHECK(r.ctl.write_diag(0, 32000, &sel, 1, "capture", nullptr) == Verdict::OK);
  r.run(1100);  // the rig's per-register rate limit
  CHECK(r.ctl.write_diag(0, 32000, &grid, 1, "capture", nullptr) == Verdict::OK);
  CHECK(r.ctl.write_diag(0, 31999, &fmt, 1, "capture", nullptr) == Verdict::OK);
  const uint16_t start[2] = {0xAA, 8};
  CHECK(r.ctl.write_diag(0, 32010, start, 2, "capture", nullptr) == Verdict::OK);
  // Fault-recording export: catalogue re-read, fetch, file select.
  const uint16_t scan = 0xAA, fetch = 0x01A1, fsel = 0x00B0;
  CHECK(r.ctl.write_diag(0, 30034, &scan, 1, "capture", nullptr) == Verdict::OK);
  r.run(1100);
  CHECK(r.ctl.write_diag(0, 32000, &fetch, 1, "capture", nullptr) == Verdict::OK);
  r.run(1100);
  CHECK(r.ctl.write_diag(0, 32000, &fsel, 1, "capture", nullptr) == Verdict::OK);
  // Anything else is refused.
  const uint16_t other = 0x0010, one = 1, bad_fetch = 0x01B1;
  CHECK(r.ctl.write_diag(0, 32000, &other, 1, "capture", nullptr) == Verdict::BAD_VALUE);
  CHECK(r.ctl.write_diag(0, 32000, &bad_fetch, 1, "capture", nullptr) == Verdict::BAD_VALUE);
  for (uint16_t v : {0x55, 0xA0, 0xB1})
    CHECK(r.ctl.write_diag(0, 30034, &v, 1, "capture", nullptr) == Verdict::BAD_VALUE);
  CHECK(r.ctl.write_diag(0, 31999, &one, 1, "capture", nullptr) == Verdict::BAD_VALUE);
  const uint16_t nine[2] = {0xAA, 9}, zero[2] = {0xAA, 0}, unarmed[2] = {0x55, 1};
  CHECK(r.ctl.write_diag(0, 32010, nine, 2, "capture", nullptr) == Verdict::BAD_VALUE);
  CHECK(r.ctl.write_diag(0, 32010, zero, 2, "capture", nullptr) == Verdict::BAD_VALUE);
  CHECK(r.ctl.write_diag(0, 32010, unarmed, 2, "capture", nullptr) == Verdict::BAD_VALUE);
  CHECK(r.ctl.write_diag(0, 13050, &one, 1, "capture", nullptr) == Verdict::NOT_ALLOWED);
  CHECK(r.ctl.write_diag(5, 32000, &iv, 1, "capture", nullptr) == Verdict::NOT_ALLOWED);  // no profile
  // The triggers stay out of the normal allow-list, even with the switch on.
  r.ctl.set_enabled(true);
  CHECK(r.ctl.write(0, 32000, &iv, 1, false, "mqtt", nullptr) == Verdict::NOT_ALLOWED);
  CHECK(r.ctl.write(0, 32000, &iv, 1, false, "api:web", nullptr, {}, true) == Verdict::NOT_ALLOWED);
  CHECK(r.ctl.write(0, 30034, &scan, 1, false, "mqtt", nullptr) == Verdict::NOT_ALLOWED);
}

// ---- start/stop follow-up ------------------------------------------------------------------------

static Verdict start_stop(Rig &r, uint16_t value, bool authorized = false) {
  return r.ctl.write(0, 5006, &value, 1, false, "test", nullptr, {}, authorized);
}

TEST(expect_stop_reaches_0x1300) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  r.state(0x0000);  // running
  CHECK(r.expect_text().empty());
  CHECK(start_stop(r, 0xCE) == Verdict::OK);
  r.run(1000);
  CHECK_EQ(r.inv.hold[5006], 0xCE);
  CHECK(r.expect_text() == "stop pending: output ramping");
  CHECK(r.last_text == "dev 0 stop pending: output ramping");
  ExpectStatus st;
  CHECK(r.ctl.expect_status(0, &st));
  CHECK(st.kind == ExpectKind::STOP && !st.final && std::strcmp(st.key, "start_stop") == 0);
  CHECK(st.state_valid && st.state == 0x0000);
  r.run(5000);
  CHECK_EQ(r.expects.size(), 1u);  // unchanged text: not re-reported
  r.state(0x1300);  // key stop
  r.run(100);
  CHECK(r.expect_text() == "stopped");
  CHECK(r.ctl.expect_status(0, &st) && st.final && st.ok && st.state == 0x1300 && st.age_ms < 1000);
  CHECK_EQ(r.expects.size(), 2u);
  CHECK(r.expects.back().final && r.expects.back().ok);
  // The outcome stays readable; a later state change does not re-open it.
  r.state(0x0000);
  r.run(2000);
  CHECK(r.ctl.expect_status(0, &st) && st.final && st.text == "stopped" && st.age_ms >= 2000);
}

TEST(expect_stop_timeout) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  r.state(0x8200);  // dispatched running
  CHECK(start_stop(r, 0xCE) == Verdict::OK);
  r.run(1000);
  CHECK(r.expect_text() == "stop pending: output ramping");
  r.run(ControlCore::EXPECT_STOP_TIMEOUT_MS - 2000);
  CHECK(r.expect_text() == "stop pending: output ramping");
  r.run(2000);
  ExpectStatus st;
  CHECK(r.ctl.expect_status(0, &st) && st.final && !st.ok);
  CHECK(st.text == "stop pending: time-out (state 0x8200)");
  CHECK(r.last_text == "dev 0 stop pending: time-out (state 0x8200)");
}

TEST(expect_start_reads_30091_and_waits) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  r.state(0x1300);
  r.inv.hold[30091] = 120;  // reconnection delay 120 s
  CHECK(start_stop(r, 0xCF) == Verdict::OK);
  r.run(1000);
  CHECK_EQ(r.count_frames(3, 30091), 1u);  // one ad-hoc read, not a poll
  CHECK(r.expect_text() == "start pending: reconnection delay (120 s)");
  r.state(0x1600);  // starting
  r.run(100);
  CHECK(r.expect_text() == "start pending: starting");
  r.run(120000);  // inside delay + grace
  CHECK(r.expect_text() == "start pending: starting");
  CHECK_EQ(r.count_frames(3, 30091), 1u);
  r.state(0x0000);
  r.run(100);
  ExpectStatus st;
  CHECK(r.ctl.expect_status(0, &st) && st.final && st.ok && st.text == "started");
  // A silent 30091 keeps the 60 s default; the clamp bounds a wild value.
  r.run(1100);
  r.inv.rexc.insert(30091);
  r.state(0x1300);
  CHECK(start_stop(r, 0xCF) == Verdict::OK);
  r.run(1500);
  CHECK(r.expect_text() == "start pending: reconnection delay (60 s)");
  r.state(0xA100);  // standby (low PV)
  r.run(100);
  CHECK(r.expect_text() == "start pending: standby");
  r.run(ControlCore::RECONNECT_DELAY_DEFAULT_S * 1000u + ControlCore::EXPECT_START_GRACE_MS);
  CHECK(r.ctl.expect_status(0, &st) && st.final && st.ok && st.text == "start accepted: standby (low PV)");
  r.inv.rexc.clear();
  r.inv.hold[30091] = 5000;
  r.state(0x1300);
  CHECK(start_stop(r, 0xCF) == Verdict::OK);
  r.run(1500);
  CHECK(r.expect_text() == "start pending: reconnection delay (900 s)");
}

TEST(expect_start_extends_on_grid_trip) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  r.state(0x1300);
  r.inv.hold[30091] = 10;
  CHECK(start_stop(r, 0xCF) == Verdict::OK);
  r.run(1000);
  CHECK(r.expect_text() == "start pending: reconnection delay (10 s)");
  r.state(0x1600);  // the power stage keeps re-running its delay (grid trips)
  r.run(10000 + ControlCore::EXPECT_START_GRACE_MS);
  CHECK(r.expect_text() == "start pending: reconnection delay extended (grid trip 1/3)");
  ExpectStatus st;
  CHECK(r.ctl.expect_status(0, &st) && !st.final);
  for (unsigned k = 2; k <= 3; k++) {
    g_now += ControlCore::EXPECT_START_EXTEND_MS;
    r.run(100);
    char want[80];
    std::snprintf(want, sizeof(want), "start pending: reconnection delay extended (grid trip %u/3)", k);
    CHECK(r.expect_text() == want);
  }
  g_now += ControlCore::EXPECT_START_EXTEND_MS;
  r.run(100);
  CHECK(r.ctl.expect_status(0, &st) && st.final && !st.ok && st.text == "start time-out (state 0x1600)");
}

TEST(expect_sg_uses_5006_and_string_key) {
  {
    // SG5.0RS: protocol 3 string profile -> rule start_stop at 5006; a persisting key stop on a
    // string-family model names its causes.
    Rig r(true, Profile::STRING, 3);
    r.family(mb::XmlFamily::UNIT, 0, 3, /*dev_type=*/21, /*dev_code=*/9734);
    r.state(0x1300);
    r.inv.hold[30091] = 10;
    CHECK(start_stop(r, 0xCF, /*authorized=*/true) == Verdict::OK);  // web: no switch needed
    r.run(1000);
    CHECK_EQ(r.inv.hold[5006], 0xCF);
    ExpectStatus st;
    CHECK(r.ctl.expect_status(0, &st) && std::strcmp(st.key, "start_stop") == 0);
    r.run(10000 + ControlCore::EXPECT_START_GRACE_MS);
    CHECK(r.ctl.expect_status(0, &st) && st.final && !st.ok);
    CHECK(st.text == "start failed: still stopped (key stop or rapid shutdown)");
  }
  {
    // SH hybrid on protocol 5: setup_start writes the descriptor's 13000, the hybrid rule's key.
    Rig r(true, Profile::HYBRID, 5);
    r.family(mb::XmlFamily::RES_ES, 0, 5, /*dev_type=*/35, /*dev_code=*/3355);
    r.state(0x1300);
    r.inv.hold[30091] = 10;
    CHECK(r.ctl.setup_start(0, "api:setup") == Verdict::OK);
    r.run(1000);
    ExpectStatus st;
    CHECK(r.ctl.expect_status(0, &st) && std::strcmp(st.key, "hybrid_start_stop") == 0);
    CHECK(st.kind == ExpectKind::START);
    r.run(10000 + ControlCore::EXPECT_START_GRACE_MS);
    CHECK(r.ctl.expect_status(0, &st) && st.final && !st.ok);
    CHECK(st.text == "start failed: still stopped (state 0x1300)");
  }
  {
    // Any other register, or another value, starts no expectation.
    Rig r(true, Profile::STRING, 3);
    r.ctl.set_enabled(true);
    uint16_t v = 0xAA;
    CHECK(r.ctl.write(0, 5007, &v, 1, false, "test", nullptr) == Verdict::OK);
    r.run(1000);
    CHECK(r.expect_text().empty());
  }
}

TEST(expect_offline_cancels) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  r.state(0x1300);
  r.inv.hold[30091] = 600;
  CHECK(start_stop(r, 0xCF) == Verdict::OK);
  r.run(1000);
  CHECK(r.expect_text() == "start pending: reconnection delay (600 s)");
  r.inv.mute = true;
  r.run(40000);
  CHECK(!r.m.devices()[0].online());
  ExpectStatus st;
  CHECK(r.ctl.expect_status(0, &st) && st.final && !st.ok && st.text == "start cancelled: device offline");
}

// ---- power-limit re-assert --------------------------------------------------------------------

/// Set the SG power limit to `pct` (x0.1 %) with the switch on, as the controls do (one fn 16).
static void set_power_limit(Rig &r, uint16_t pct, bool authorized = false) {
  const uint16_t vals[2] = {0xAA, pct};
  mb::WriteOptions opt;
  opt.fn = mb::FN_WRITE_MULTIPLE;
  CHECK(r.ctl.write(0, 5007, vals, 2, true, authorized ? "api:web" : "test", nullptr, opt, authorized) == Verdict::OK);
  r.run(1500);
  CHECK_EQ(r.inv.hold[5008], pct);
}

/// The inverter restarted while away: the power stage boots with its 110 % default.
static void boot_limit_lost(Rig &r) {
  r.inv.hold[5007] = 0x55;
  r.inv.hold[5008] = 1100;
}

TEST(reassert_power_limit_on_online_edge) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  CHECK(r.ctl.power_limit_memo(0) != nullptr && !r.ctl.power_limit_memo(0)->valid);
  set_power_limit(r, 500);
  const ControlCore::PowerLimitMemo *m = r.ctl.power_limit_memo(0);
  CHECK(m->valid && m->sw == 0xAA && m->pct == 500 && !m->authorized && m->source == "test");
  r.power_cycle([&] { boot_limit_lost(r); });
  CHECK_EQ(r.inv.hold[5008], 500);
  CHECK_EQ(r.inv.hold[5007], 0xAA);
  CHECK_EQ(m->count, 1u);
  CHECK(r.logged("power limit re-asserted: 1100 -> 500"));
  CHECK(r.logged("5007=[0xAA, 0x1F4] (reassert(test)"));
  CHECK(m->source == "test");  // the re-assert itself does not become the reference
  uint64_t b = 0;
  CHECK(r.store.get_bits(0, SIG_POWER_LIMIT_PCT, &b) && b == 500);
}

TEST(reassert_waits_for_fresh_readback) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  set_power_limit(r, 500);
  r.inv.rexc.insert(5008);  // the read-back block is refused for a while
  r.power_cycle([&] { boot_limit_lost(r); });
  r.run(3000);
  CHECK_EQ(r.inv.hold[5008], 1100);  // nothing fresh to compare: no write
  CHECK(r.ctl.power_limit_memo(0)->due);
  r.inv.rexc.clear();
  r.run(11000);  // the block's next read-back
  CHECK_EQ(r.inv.hold[5008], 500);
  CHECK(!r.ctl.power_limit_memo(0)->due);
}

TEST(reassert_skipped_when_value_intact) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  set_power_limit(r, 500);
  const int writes = r.inv.writes;
  r.power_cycle([] {});
  r.run(2000);
  CHECK_EQ(r.inv.writes, writes);
  CHECK(r.logged("power limit intact (500)"));
  CHECK_EQ(r.ctl.power_limit_memo(0)->count, 0u);
}

TEST(reassert_blocked_when_disabled_and_not_web) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  set_power_limit(r, 500);
  r.ctl.set_enabled(false);
  r.power_cycle([&] { boot_limit_lost(r); });
  r.run(2000);
  CHECK_EQ(r.inv.hold[5008], 1100);
  CHECK(r.logged("not re-asserted, inverter writes disabled"));
  // Set from the web interface: the login authorised it, so the re-assert needs no switch.
  r.inv.hold[5008] = 1100;
  set_power_limit(r, 400, /*authorized=*/true);
  CHECK(r.ctl.power_limit_memo(0)->authorized);
  r.power_cycle([&] { boot_limit_lost(r); });
  CHECK_EQ(r.inv.hold[5008], 400);
  CHECK(r.logged("(reassert(api:web)"));
}

TEST(reassert_rate_limited) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  set_power_limit(r, 500);
  r.power_cycle([&] { boot_limit_lost(r); });
  CHECK_EQ(r.inv.hold[5008], 500);
  // A second restart within 10 min: one warning, no write.
  r.power_cycle([&] { boot_limit_lost(r); });
  r.run(2000);
  CHECK_EQ(r.inv.hold[5008], 1100);
  CHECK(r.logged("not again within 10 min"));
  CHECK_EQ(r.ctl.power_limit_memo(0)->count, 1u);
  g_now += ControlCore::REASSERT_MIN_INTERVAL_MS;
  r.power_cycle([&] { boot_limit_lost(r); });
  CHECK_EQ(r.inv.hold[5008], 500);
  CHECK_EQ(r.ctl.power_limit_memo(0)->count, 2u);
}

TEST(reassert_on_0x1200_after_running) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  set_power_limit(r, 500);
  r.state(0x0000);
  r.run(100);
  // Nightly standby keeps the power stage's RAM: no check.
  boot_limit_lost(r);
  r.state(0x1400);
  r.run(12000);  // a routine read-back sees 1100, but nothing is due
  CHECK_EQ(r.inv.hold[5008], 1100);
  CHECK_EQ(r.ctl.power_limit_memo(0)->count, 0u);
  // Running, then the DSP-initial state: the power stage restarted.
  r.state(0x0000);
  r.run(100);
  r.state(ControlCore::STATE_POWER_STAGE_INITIAL);
  r.run(2000);
  CHECK_EQ(r.inv.hold[5008], 500);
  CHECK_EQ(r.ctl.power_limit_memo(0)->count, 1u);
  CHECK(r.logged("power limit check after power-stage restart"));
}

TEST(reassert_opt_out) {
  Rig r(true, Profile::STRING, 3);
  r.ctl.set_enabled(true);
  r.ctl.device(0)->reassert_power_limit = false;
  set_power_limit(r, 500);
  r.power_cycle([&] { boot_limit_lost(r); });
  r.run(2000);
  CHECK_EQ(r.inv.hold[5008], 1100);
  CHECK_EQ(r.ctl.power_limit_memo(0)->count, 0u);
  // The hybrid profile has no power-limit rules: nothing is remembered.
  Rig h;
  h.ctl.set_enabled(true);
  uint16_t v = 2;
  CHECK(h.ctl.write(0, 13050, &v, 1, false, "test", nullptr) == Verdict::OK);
  h.run(1000);
  CHECK(!h.ctl.power_limit_memo(0)->valid);
}

// ---- power-stage acceptance ranges ------------------------------------------------------------

// The unit family lists 32550 with the stock range 1000-1400; the SH10RS power stage accepts
// 1000-1200, so the model table is the deciding check.
TEST(setup_dsp_range_installer_reject) {
  Rig r(true, Profile::STRING, 3);
  r.family(mb::XmlFamily::UNIT, 0, 3, /*dev_type=*/35, /*dev_code=*/3355);
  std::vector<ControlCore::SetupReg> b = {{32550, 1300}};
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::BAD_VALUE);
  CHECK(r.ctl.setup_detail() == "dsp_range:32550:1000-1200");
  CHECK(!r.ctl.setup_busy());
  b = {{32550, 1100}};
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::OK);
  r.run_setup(5000);
  CHECK_EQ(r.inv.hold[32550], 1100);
  // A U32 setting is compared as one number (low word first).
  b = {{30813, 0x2D00}, {30814, 0x0131}};  // 20 000 000 > 14 400 000
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::BAD_VALUE);
  CHECK(r.ctl.setup_detail() == "dsp_range:30813:0-14400000");
}

TEST(setup_dsp_range_profile_skips) {
  Rig r(true, Profile::STRING, 3);
  r.family(mb::XmlFamily::UNIT, 0, 3, /*dev_type=*/35, /*dev_code=*/3355);
  r.inv.hold[32550] = 1100;
  r.inv.hold[30813] = 1;
  r.inv.hold[30814] = 0;
  std::vector<ControlCore::SetupReg> b = {{32549, 880}, {32550, 1300}, {30813, 0x2D00}, {30814, 0x0131}, {30092, 60}};
  const std::string note = "32550=1300 outside the power-stage range 1000-1200; "
                           "30813=20000000 outside the power-stage range 0-14400000";
  CHECK(r.ctl.setup_plan_note(0, b) == note);
  CHECK_EQ(r.ctl.plan_frames(0, b).size(), 2u);  // 32549 alone, 30092
  CHECK(r.ctl.setup_apply(0, b, "test", true) == Verdict::OK);
  CHECK(r.ctl.setup_note() == note);
  CHECK_EQ(r.ctl.setup_total(), 2);
  CHECK(r.logged("skipped 32550=1300"));
  r.run_setup(5000);
  CHECK_EQ(r.ctl.setup_ok(), 2);
  CHECK_EQ(r.inv.hold[32549], 880);
  CHECK_EQ(r.inv.hold[32550], 1100);  // untouched
  CHECK_EQ(r.inv.hold[30813], 1);
  CHECK_EQ(r.inv.hold[30092], 60);
  // Nothing to skip: no note.
  b = {{32549, 880}};
  CHECK(r.ctl.setup_apply(0, b, "test", true) == Verdict::OK);
  CHECK(r.ctl.setup_note().empty());
}

TEST(setup_dsp_range_other_model_not_gated) {
  Rig r(true, Profile::STRING, 3);
  r.family(mb::XmlFamily::UNIT, 0, 3, /*dev_type=*/35, /*dev_code=*/1234);
  std::vector<ControlCore::SetupReg> b = {{32550, 1300}};
  CHECK(r.ctl.setup_apply(0, b, "test") == Verdict::OK);
  CHECK(r.ctl.setup_plan_note(0, b).empty());
  r.run_setup(5000);
  CHECK_EQ(r.inv.hold[32550], 1300);
  // The generated table: known models only.
  CHECK(dsp_model(3355) != nullptr && dsp_model(9734) != nullptr && dsp_model(1234) == nullptr);
  const DspRange *rg = dsp_range(dsp_model(9734), 31205);
  CHECK(rg != nullptr && rg->min == 0 && rg->max == 1500 && rg->count == 1);
  CHECK(dsp_range(dsp_model(3355), 1) == nullptr && dsp_range(nullptr, 32550) == nullptr);
}

int main() { return th::run_all(); }
