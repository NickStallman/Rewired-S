// Host tests for components/sungrow_modbus (framing, parser, discovery, scheduler, end-to-end
// with a fake inverter feeding the sungrow_datamodel signal store).
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "../test_harness.h"
#include "discovery.h"
#include "master.h"
#include "modbus_rtu.h"

using namespace esphome::sungrow_modbus;
namespace dm = esphome::sungrow_datamodel;

static std::vector<uint8_t> hex(const char *s) {
  std::vector<uint8_t> v;
  while (*s) {
    while (*s == ' ')
      s++;
    if (!*s)
      break;
    unsigned b;
    std::sscanf(s, "%2x", &b);
    v.push_back(static_cast<uint8_t>(b));
    s += 2;
  }
  return v;
}

// ---------------------------------------------------------------------------------------------
TEST(crc_vectors) {
  // Published Modbus examples and the CRC-16/MODBUS check value.
  auto a = hex("01 03 00 00 00 0A");
  CHECK_EQ(crc16(a.data(), a.size()), 0xCDC5);  // on the wire: C5 CD
  auto b = hex("01 04 00 00 00 01");
  CHECK_EQ(crc16(b.data(), b.size()), 0xCA31);  // 31 CA
  const char *chk = "123456789";
  CHECK_EQ(crc16(reinterpret_cast<const uint8_t *>(chk), 9), 0x4B37);
  auto f = hex("01 03 00 00 00 0A C5 CD");
  CHECK(crc_ok(f.data(), f.size()));
  f[7] ^= 1;
  CHECK(!crc_ok(f.data(), f.size()));
}

TEST(read_request_frame) {
  uint8_t out[8];
  // fn 4, table address 5000 (1-based) -> wire 4999 = 0x1387, count 10.
  CHECK_EQ(build_read_request(1, 4, 5000 - 1, 10, out), 8u);
  auto want = hex("01 04 13 87 00 0A C4 A0");
  CHECK(std::memcmp(out, want.data(), 8) == 0);
  CHECK_EQ(build_read_request(1, 4, 0, 0, out), 0u);
  CHECK_EQ(build_read_request(1, 4, 0, 126, out), 0u);
  CHECK_EQ(build_read_request(1, 6, 0, 1, out), 0u);
}

TEST(write_request_frame) {
  uint8_t out[32];
  uint16_t v[2] = {0x000A, 0x0102};
  size_t n = build_write_request(1, 1, v, 2, out, sizeof(out));
  auto want = hex("01 10 00 01 00 02 04 00 0A 01 02");
  CHECK_EQ(n, want.size() + 2);
  CHECK(std::memcmp(out, want.data(), want.size()) == 0);
  CHECK(crc_ok(out, n));
}

static std::vector<uint8_t> read_reply(uint8_t addr, uint8_t fn, const std::vector<uint16_t> &regs) {
  std::vector<uint8_t> f{addr, fn, static_cast<uint8_t>(regs.size() * 2)};
  for (uint16_t r : regs) {
    f.push_back(r >> 8);
    f.push_back(r & 0xFF);
  }
  f.resize(f.size() + 2);
  append_crc(f.data(), f.size() - 2);
  return f;
}

TEST(parse_ok_and_split) {
  ResponseParser p;
  p.begin({1, 4, 4999, 3});
  auto f = read_reply(1, 4, {0x1234, 0xFFFF, 0x0001});
  // Byte-by-byte delivery.
  for (size_t i = 0; i + 1 < f.size(); i++)
    CHECK(p.feed(&f[i], 1) == ParseStatus::INCOMPLETE);
  CHECK(p.feed(&f.back(), 1) == ParseStatus::OK);
  CHECK_EQ(p.nregs(), 3);
  CHECK_EQ(p.data()[0], 0x12);
  CHECK_EQ(p.data()[5], 0x01);
  CHECK_EQ(p.crc_errors(), 0);
}

TEST(parse_stray_bytes_and_wrong_slave) {
  ResponseParser p;
  p.begin({1, 4, 0, 2});
  std::vector<uint8_t> in = {0x00, 0xFF, 0x55};          // line noise
  auto other = read_reply(2, 4, {1, 2});                 // reply from another slave
  auto good = read_reply(1, 4, {0xABCD, 0x0102});
  in.insert(in.end(), other.begin(), other.end());
  in.insert(in.end(), good.begin(), good.end());
  CHECK(p.feed(in.data(), in.size()) == ParseStatus::OK);
  CHECK_EQ(p.data()[0], 0xAB);
  CHECK(p.dropped() >= 3 + other.size());
}

TEST(parse_exception) {
  ResponseParser p;
  p.begin({1, 3, 0, 10});
  auto e = hex("01 83 02");
  e.resize(5);
  append_crc(e.data(), 3);
  // Split in two chunks.
  CHECK(p.feed(e.data(), 2) == ParseStatus::INCOMPLETE);
  CHECK(p.feed(e.data() + 2, 3) == ParseStatus::EXCEPTION);
  CHECK_EQ(p.exception_code(), 2);
}

TEST(parse_bad_crc_then_resync) {
  ResponseParser p;
  p.begin({1, 4, 0, 1});
  auto bad = read_reply(1, 4, {0x0102});
  bad[3] ^= 0x40;
  CHECK(p.feed(bad.data(), bad.size()) == ParseStatus::INCOMPLETE);
  CHECK_EQ(p.crc_errors(), 1);
  auto good = read_reply(1, 4, {0x0304});
  CHECK(p.feed(good.data(), good.size()) == ParseStatus::OK);
  CHECK_EQ(p.data()[1], 0x04);
}

TEST(parse_wrong_bytecount_and_write_echo) {
  ResponseParser p;
  p.begin({1, 4, 0, 2});
  auto shortreply = read_reply(1, 4, {1});  // 1 register instead of 2
  CHECK(p.feed(shortreply.data(), shortreply.size()) == ParseStatus::INCOMPLETE);
  ResponseParser w;
  w.begin({1, 0x10, 0x0001, 2});
  auto echo = hex("01 10 00 01 00 02");
  echo.resize(8);
  append_crc(echo.data(), 6);
  CHECK(w.feed(echo.data(), echo.size()) == ParseStatus::OK);
  CHECK_EQ(w.nregs(), 2);
}

// ---------------------------------------------------------------------------------------------
TEST(discovery_classify) {
  // Modern protocol code wins regardless of the dev_code range.
  CHECK_EQ(classify_protocol(0x0D0F, "AA10", "\0\0"), 2);
  CHECK_EQ(classify_protocol(0x0D0F, "AA12", "\0\0"), 2);
  CHECK_EQ(classify_protocol(0x0D0F, "AA09", "\0\0"), 1);  // < "AA10"
  CHECK_EQ(classify_protocol(0x0D0F, "\0\0\0\0", "\0\0"), 1);
  CHECK_EQ(classify_protocol(0x0D03, "\0\0\0\0", "\0\0"), 5);  // 0x259 bit 0
  CHECK_EQ(classify_protocol(0x0D04, "\0\0\0\0", "\0\0"), 1);
  CHECK_EQ(classify_protocol(0x0D0C, "\0\0\0\0", "\0\0"), 5);  // bit 9
  CHECK_EQ(classify_protocol(0x0E01, "\0\0\0\0", "\0\0"), 4);
  CHECK_EQ(classify_protocol(0x0E04, "\0\0\0\0", "\0\0"), 1);
  CHECK_EQ(classify_protocol(0x1061, "\0\0\0\0", "\0\0"), 4);
  CHECK_EQ(classify_protocol(0x2FC5, "\0\0\0\0", "\0\0"), 4);
  CHECK_EQ(classify_protocol(0x0027, "\0\0\0\0", "\0\0"), 4);
  CHECK_EQ(classify_protocol(0x010F, "\0\0\0\0", "\0\0"), 4);  // 0x1081 bit 0
  CHECK_EQ(classify_protocol(0x0110, "\0\0\0\0", "\0\0"), 1);
  CHECK_EQ(classify_protocol(0x0131, "\0\0\0\0", "\0\0"), 4);  // 0x303010FB bit 0
  CHECK_EQ(classify_protocol(0x0133, "\0\0\0\0", "\0\0"), 1);  // bit 2 clear
  CHECK_EQ(classify_protocol(0x0E01, "\0\0\0\0", "ZS"), 3);
  // dev_type classification
  CHECK_EQ(classify_dev_type(0x0D0F, nullptr), 35);
  CHECK_EQ(classify_dev_type(0x0E03, nullptr), 35);
  CHECK_EQ(classify_dev_type(0x2401, nullptr), 21);
  CHECK_EQ(classify_dev_type(0x0110, nullptr), 21);
  CHECK_EQ(classify_dev_type(0x0120, nullptr), 21);
  CHECK_EQ(classify_dev_type(0x0133, nullptr), 21);  // 0x719A01 bit 0
  CHECK_EQ(classify_dev_type(0x0134, nullptr), 1);
  const uint8_t r[] = "SG5R";
  const uint8_t b[] = "SH5B";
  CHECK_EQ(classify_dev_type(0x0134, r), 21);
  CHECK_EQ(classify_dev_type(0x0134, b), 35);
  // parse_devinfo passes the raw bytes of regs 2534..2535 as pData: pdata[3] = 2535 low byte.
  uint8_t blk[2 * DEVINFO_COUNT] = {};
  blk[0] = 0x01;
  blk[1] = 0x34;  // dev_code 0x0134 (outside the HOME ranges)
  uint8_t *p = blk + 2 * (2534 - DEVINFO_START);
  p[0] = '1';
  p[1] = '0';
  p[2] = 'A';
  p[3] = 'R';
  DevInfo di;
  CHECK(parse_devinfo(blk, DEVINFO_COUNT, &di));
  CHECK_EQ(di.dev_type, 21);
  p[3] = 'B';
  CHECK(parse_devinfo(blk, DEVINFO_COUNT, &di));
  CHECK_EQ(di.dev_type, 35);
  // The model string does not matter ("...R" in devModel, not in pData).
  p[3] = 'x';
  blk[2 * 1 + 3] = 'R';
  CHECK(parse_devinfo(blk, DEVINFO_COUNT, &di));
  CHECK_EQ(di.dev_type, 1);
}

// Stock XML family tree and descriptor table for every pair.
TEST(xml_family_table) {
  const uint16_t invs[] = {0, 1, 2, 0xFFFF};
  for (uint16_t inv : invs) {
    for (uint8_t p = 0; p <= 5; p++) {
      const FamilyDescriptor *d = family_descriptor(inv, p);
      XmlFamily want;
      if (inv == 0 && p == 1)
        want = XmlFamily::STR_PV;
      else if (inv == 0 && p == 5)
        want = XmlFamily::RES_ES;
      else if (inv == 0 && p == 4)
        want = XmlFamily::NONE;
      else if (inv == 1 && p == 1)
        want = XmlFamily::RES_NEW_PV;
      else if ((inv == 0 || inv == 1) && (p == 2 || p == 3))
        want = XmlFamily::UNIT;
      else
        want = XmlFamily::UNIT_NODESC;
      CHECK(xml_family(inv, p) == want);
      // Descriptor present exactly for the 8 stock pairs.
      const bool has = (inv == 0 && p >= 1) || (inv == 1 && p >= 1 && p <= 3);
      CHECK_EQ(d != nullptr, has);
      if (d == nullptr)
        continue;
      CHECK_EQ(d->inv_type, inv);
      CHECK_EQ(d->protocol, p);
      if (inv == 0 && p == 1) {
        CHECK_EQ(d->country_reg, 30500);
        CHECK_EQ(d->grid_reg, 30502);
        CHECK_EQ(d->power_switch_reg, 5006);
      } else if (inv == 0 && p == 5) {
        CHECK_EQ(d->country_reg, 30643);
        CHECK_EQ(d->grid_reg, 0);
        CHECK_EQ(d->power_switch_reg, 13000);
      } else if (inv == 1 && p == 1) {
        CHECK_EQ(d->country_reg, 30643);
        CHECK_EQ(d->grid_reg, 0);
        CHECK_EQ(d->power_switch_reg, 5006);
      } else if (inv == 0 && p == 4) {
        CHECK_EQ(d->country_reg, 30643);
        CHECK_EQ(d->grid_reg, 30644);
        CHECK_EQ(d->power_switch_reg, 30045);
      } else {  // (0|1, 2|3)
        CHECK_EQ(d->country_reg, 32310);
        CHECK_EQ(d->grid_reg, 32311);
        CHECK_EQ(d->power_switch_reg, 5006);
      }
    }
  }
  CHECK(std::string(xml_family_name(XmlFamily::NONE)) == "none");
  CHECK(std::string(xml_family_name(XmlFamily::STR_PV)) == "str_pv");
  CHECK(std::string(xml_family_name(XmlFamily::RES_ES)) == "res_es");
  CHECK(std::string(xml_family_name(XmlFamily::RES_NEW_PV)) == "res_new_pv");
  CHECK(std::string(xml_family_name(XmlFamily::UNIT)) == "unit");
  CHECK(std::string(xml_family_name(XmlFamily::UNIT_NODESC)) == "unit_nodesc");
  // Commit registers: country, grid code (when present), 30036, 30037.
  const FamilyDescriptor *u = family_descriptor(0, 3);
  CHECK(is_commit_register(u, 32310));
  CHECK(is_commit_register(u, 32311));
  CHECK(is_commit_register(u, 30036));
  CHECK(is_commit_register(u, 30037));
  CHECK(!is_commit_register(u, 30500));
  CHECK(!is_commit_register(u, 0));
  const FamilyDescriptor *rn = family_descriptor(1, 1);
  CHECK(is_commit_register(rn, 30643));
  CHECK(!is_commit_register(rn, 30644));
  CHECK(!is_commit_register(nullptr, 30036));
}

/// devinfo registers of the SG5.0RS test site (live reads): dev_code 0x2606, "AA10", tag "ZS".
static void sg5rs_devinfo(std::map<uint16_t, uint16_t> &r) {
  r[2515] = 0x2606;
  const char *model = "SG5.0RS";
  for (int i = 0; i < 8; i++) {
    uint8_t hi = i * 2 < 7 ? model[i * 2] : 0, lo = i * 2 + 1 < 7 ? model[i * 2 + 1] : 0;
    r[2516 + i] = static_cast<uint16_t>(hi << 8 | lo);
  }
  r[2534] = 0x3130;  // "10" (applyProtocolCode low word)
  r[2535] = 0x4141;  // "AA"
  r[2578] = ('Z' << 8) | 'S';
}

TEST(xml_family_hw_sg5rs) {
  std::map<uint16_t, uint16_t> regs;
  sg5rs_devinfo(regs);
  uint8_t blk[2 * DEVINFO_COUNT] = {};
  for (auto &kv : regs) {
    blk[2 * (kv.first - DEVINFO_START)] = kv.second >> 8;
    blk[2 * (kv.first - DEVINFO_START) + 1] = kv.second & 0xFF;
  }
  DevInfo di;
  CHECK(parse_devinfo(blk, DEVINFO_COUNT, &di));
  CHECK_EQ(di.dev_code, 0x2606);
  CHECK(std::string(di.model) == "SG5.0RS");
  CHECK(std::string(di.proto_code) == "AA10");
  CHECK(std::string(di.tag) == "ZS");
  CHECK_EQ(di.protocol, 3);
  CHECK_EQ(di.dev_type, 21);
  CHECK(xml_family(0, di.protocol) == XmlFamily::UNIT);
  const FamilyDescriptor *d = family_descriptor(0, di.protocol);
  CHECK(d != nullptr);
  CHECK_EQ(d->country_reg, 32310);
  CHECK_EQ(d->grid_reg, 32311);
  CHECK_EQ(d->power_switch_reg, 5006);
}

// ---------------------------------------------------------------------------------------------
// Fake clock + transport + slaves.
static uint32_t g_now = 0;

struct FakeSlave {
  uint8_t addr{1};
  std::map<uint32_t, uint16_t> regs;  // key = fn << 16 | 1-based addr
  std::vector<std::pair<uint16_t, uint16_t>> exception_ranges;  // 1-based [start, end)
  std::vector<std::pair<uint16_t, uint16_t>> silent_ranges;     // 1-based [start, end): no reply
  bool mute{false};
  int corrupt_next{0};
  bool ignore_writes{false};  // echo writes but keep the old values (verify mismatch)
  uint8_t write_exception{0};  // != 0: answer every write with this exception
  bool answers_search{false};  // replies to the broadcast read of 30035 with its own address
  int writes_seen{0};
  void set(uint8_t fn, uint16_t a, uint16_t v) { regs[(uint32_t) fn << 16 | a] = v; }
  void set32(uint8_t fn, uint16_t a, uint32_t v) {  // low word first (mode 2)
    set(fn, a, v & 0xFFFF);
    set(fn, a + 1, v >> 16);
  }
  uint16_t get(uint8_t fn, uint16_t a) const {
    auto it = regs.find((uint32_t) fn << 16 | a);
    return it == regs.end() ? 0 : it->second;
  }
};

struct FakeBus : Transport {
  std::vector<FakeSlave *> slaves;
  std::vector<std::vector<uint8_t>> writes;
  std::vector<uint32_t> write_ms;  // g_now at each send
  std::deque<uint8_t> rx;
  size_t chunk{7};  // deliver replies in small chunks (split delivery)
  void send_frame(const uint8_t *d, size_t n) override {
    writes.emplace_back(d, d + n);
    write_ms.push_back(g_now);
    if (n >= 8 && crc_ok(d, n) && (d[1] == 0x06 || d[1] == 0x10)) {
      write_frame_(d, n);
      return;
    }
    if (n != 8 || !crc_ok(d, n))
      return;
    uint8_t fn = d[1];
    uint16_t start = (d[2] << 8 | d[3]) + 1, count = d[4] << 8 | d[5];
    if (d[0] == 0) {
      // Broadcast inverter search: the inverter answers from slave id 0 with its address.
      for (FakeSlave *s : slaves) {
        if (s->mute || !s->answers_search || fn != 3 || start != 30035 || count != 1)
          continue;
        auto f = read_reply(0, 3, {s->addr});
        rx.insert(rx.end(), f.begin(), f.end());
      }
      return;
    }
    for (FakeSlave *s : slaves) {
      if (s->addr != d[0] || s->mute)
        continue;
      std::vector<uint8_t> f;
      bool exc = false, silent = false;
      for (auto &r : s->exception_ranges)
        if (start < r.second && start + count > r.first)
          exc = true;
      for (auto &r : s->silent_ranges)
        if (start < r.second && start + count > r.first)
          silent = true;
      if (silent)
        continue;
      if (exc) {
        f = {d[0], static_cast<uint8_t>(fn | 0x80), 2, 0, 0};
        append_crc(f.data(), 3);
      } else {
        std::vector<uint16_t> regs;
        for (uint16_t i = 0; i < count; i++)
          regs.push_back(s->get(fn, start + i));
        f = read_reply(d[0], fn, regs);
      }
      if (s->corrupt_next > 0) {
        s->corrupt_next--;
        f[3] ^= 0x10;
      }
      rx.insert(rx.end(), f.begin(), f.end());
    }
  }
  size_t receive(uint8_t *buf, size_t cap) override {
    size_t n = 0;
    while (n < cap && n < chunk && !rx.empty()) {
      buf[n++] = rx.front();
      rx.pop_front();
    }
    return n;
  }
  void discard_input() override { rx.clear(); }
  void write_frame_(const uint8_t *d, size_t n) {
    const uint8_t fn = d[1];
    const uint16_t start = (d[2] << 8 | d[3]) + 1;
    for (FakeSlave *s : slaves) {
      if (s->addr != d[0] || s->mute)
        continue;
      s->writes_seen++;
      std::vector<uint8_t> f;
      if (s->write_exception != 0) {
        f = {d[0], static_cast<uint8_t>(fn | 0x80), s->write_exception, 0, 0};
        append_crc(f.data(), 3);
      } else {
        if (fn == 0x06) {
          if (!s->ignore_writes)
            s->set(3, start, d[4] << 8 | d[5]);
        } else {
          uint16_t count = d[4] << 8 | d[5];
          for (uint16_t i = 0; i < count && 7u + 2 * i + 1 < n; i++)
            if (!s->ignore_writes)
              s->set(3, start + i, d[7 + 2 * i] << 8 | d[8 + 2 * i]);
        }
        f.assign(d, d + 6);
        f.resize(8);
        append_crc(f.data(), 6);
      }
      rx.insert(rx.end(), f.begin(), f.end());
    }
  }
};

struct Rig {
  FakeBus bus;
  dm::SignalStore store;
  ModbusMaster m;
  std::vector<std::string> logs;
  Rig() {
    g_now = 1000;
    store.set_clock([] { return g_now; });
    m.set_transport(&bus);
    m.set_store(&store);
    m.set_clock([] { return g_now; });
    m.set_log([this](int lvl, const char *msg) {
      logs.emplace_back(msg);
      if (getenv("MODBUS_TEST_LOG"))
        std::printf("    [%d] %s\n", lvl, msg);
    });
  }
  /// Advance time in 5 ms steps, ticking the master.
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 5) {
      m.tick();
      g_now += 5;
    }
  }
};

static size_t count_valid(dm::SignalStore &st, uint8_t dev) {
  size_t n = 0;
  st.for_each([&](const dm::Signal &s) { n += s.device == dev && s.valid; });
  return n;
}

TEST(scheduler_round_robin_and_adhoc_priority) {
  Rig r;
  FakeSlave a, b;
  a.addr = 1;
  b.addr = 2;
  r.bus.slaves = {&a, &b};
  DeviceConfig da;
  da.index = 0;
  da.address = 1;
  da.table = dm::find_table("other_dev18_st6200");
  DeviceConfig db = da;
  db.index = 1;
  db.address = 2;
  r.m.add_device(da);
  r.m.add_device(db);
  r.m.start();
  CHECK(r.m.devices()[0].jobs().size() >= 1);
  r.run(2000);
  // Probe A, probe B, then rounds of A and B interleave (meters poll every node every round).
  CHECK(r.bus.writes.size() >= 6);
  int alternations = 0;
  for (size_t i = 1; i < 6; i++)
    alternations += r.bus.writes[i][0] != r.bus.writes[i - 1][0];
  CHECK_EQ(alternations, 5);
  CHECK(r.m.devices()[0].online());
  CHECK(r.m.devices()[1].online());
  CHECK(r.m.devices()[0].first_round_done());
  CHECK_EQ(r.m.devices()[0].stats().timeouts, 0u);
  // An ad-hoc read is served before the next cyclic poll.
  bool got = false;
  uint16_t got_regs = 0;
  a.set(4, 10757, 0x1234);
  CHECK(r.m.read_registers(1, 4, 10757, 24, [&](bool ok, const uint8_t *d, uint16_t n) {
    got = ok && d[0] == 0x12 && d[1] == 0x34;
    got_regs = n;
  }));
  size_t before = r.bus.writes.size();
  r.run(400);
  CHECK(got);
  CHECK_EQ(got_regs, 24);
  bool adhoc_seen = false;
  for (size_t i = before; i < before + 3 && i < r.bus.writes.size(); i++)
    adhoc_seen |= (r.bus.writes[i][2] << 8 | r.bus.writes[i][3]) == 10756 && r.bus.writes[i][1] == 4;
  CHECK(adhoc_seen);
}

TEST(timing_defaults) {
  MasterTiming t;
  CHECK_EQ(t.frame_gap_ms, 18u);
  CHECK_EQ(t.frame_gap_fast_ms, 1u);
  CHECK_EQ(t.response_timeout_ms, 1000u);
  CHECK_EQ(t.poll_timeout_margin_ms, 200u);
  CHECK_EQ(t.tunnel_timeout_ms, 2000u);
  CHECK_EQ(t.write_timeout_ms, 2000u);
  CHECK_EQ(t.retries, 0);
  CHECK_EQ(t.offline_after, 10);
  CHECK_EQ(t.offline_silence_ms, 10000u);
  ModbusMaster m;
  DeviceConfig inv;
  inv.table = dm::find_table("inv_proto2");
  DeviceConfig bat;
  bat.address = 2;
  bat.table = dm::find_table("battery");
  m.add_device(inv);
  m.add_device(bat);  // (invalidates references returned by add_device)
  const Device &di = m.devices()[0];
  const Device &db = m.devices()[1];
  // tier periods: 1 s / 5 s / 30 s / 5 min, overridable
  CHECK_EQ(m.tier_period(PollTier::FAST), 1000u);
  CHECK_EQ(m.tier_period(PollTier::LIVE), 5000u);
  CHECK_EQ(m.tier_period(PollTier::SLOW), 30000u);
  CHECK_EQ(m.tier_period(PollTier::STATIC), 300000u);
  m.set_tier_period(PollTier::LIVE, 3000);
  CHECK_EQ(m.tier_period(PollTier::LIVE), 3000u);
  // tunnelled ranges: inverter -> DSP, battery -> BCU
  CHECK(ModbusMaster::tunnel_range(dm::Family::BATTERY, 19939, 2));
  CHECK(!ModbusMaster::tunnel_range(dm::Family::BATTERY, 10740, 6));
  CHECK(ModbusMaster::tunnel_range(dm::Family::INVERTER, 16100, 10));
  CHECK(!ModbusMaster::tunnel_range(dm::Family::INVERTER, 5214, 39));
  // gap: 18 ms inverter; battery 1 ms raised to 3.5 chars at 9600 8N1 (3.65 -> 4 ms)
  CHECK_EQ(m.frame_gap(&di), 18u);
  CHECK_EQ(m.frame_gap(&db), 4u);
  CHECK_EQ(m.frame_gap(nullptr), 18u);
}

TEST(scheduler_gap_and_timeout_from_send) {
  Rig r;
  FakeSlave a;
  r.bus.slaves = {&a};
  DeviceConfig d;
  d.table = dm::find_table("inv_proto5");
  r.m.add_device(d);
  r.run(3000);
  CHECK(r.bus.writes.size() > 10);
  for (size_t i = 1; i < r.bus.writes.size(); i++)
    CHECK(r.bus.write_ms[i] - r.bus.write_ms[i - 1] >= 18u);
  // silent slave: one attempt per request; a read times out after its wire time + 200 ms
  a.mute = true;
  size_t w0 = r.bus.writes.size();
  r.run(9000);
  CHECK(r.bus.writes.size() - w0 >= 5u);
  for (size_t i = w0 + 1; i < r.bus.writes.size(); i++) {
    uint32_t dt = r.bus.write_ms[i] - r.bus.write_ms[i - 1];
    const auto &w = r.bus.writes[i - 1];
    const uint32_t wire = ((8u + 5u + 2u * (w[4] << 8 | w[5])) * 1042u + 999u) / 1000u;
    CHECK(dt >= wire + 200u + 18u);
  }
  // battery: 1 ms setting -> 4 ms (t3.5) between frames
  Rig rb;
  FakeSlave b;
  rb.bus.slaves = {&b};
  DeviceConfig db;
  db.table = dm::find_table("battery");
  rb.m.add_device(db);
  rb.run(500);
  uint32_t min_dt = 1000;
  for (size_t i = 1; i < rb.bus.writes.size(); i++)
    min_dt = std::min(min_dt, rb.bus.write_ms[i] - rb.bus.write_ms[i - 1]);
  CHECK(min_dt >= 4u && min_dt < 18u);
}

/// Send times of the frames matching a read request (fn, start, count).
static std::vector<uint32_t> send_times(const FakeBus &bus, const dm::ReadRequest &q, size_t from = 0) {
  std::vector<uint32_t> out;
  for (size_t i = from; i < bus.writes.size(); i++) {
    const auto &w = bus.writes[i];
    if (w.size() == 8 && w[1] == q.fn && (w[2] << 8 | w[3]) == q.wire_start() && (w[4] << 8 | w[5]) == q.count)
      out.push_back(bus.write_ms[i]);
  }
  return out;
}

TEST(poll_plan_tiers) {
  // Curated ranges never overlap within one (table, fn).
  size_t nr = 0;
  const TierRange *rg = tier_ranges(&nr);
  for (size_t i = 0; i < nr; i++) {
    CHECK(rg[i].first <= rg[i].last);
    for (size_t k = i + 1; k < nr; k++)
      CHECK(!(rg[i].table == rg[k].table && rg[i].fn == rg[k].fn && rg[i].first <= rg[k].last &&
              rg[k].first <= rg[i].last));
  }
  const char *names[] = {"inv_proto1_4", "inv_proto2", "inv_proto3", "inv_proto5", "battery", "charger",
                         "energy_bridge", "other_dev18_st6200"};
  for (const char *n : names) {
    const dm::RegisterTable &t = *dm::find_table(n);
    auto plan = build_poll_plan(t);
    // Every pollable row is covered by exactly one read of its own tier; reads stay <= 120.
    for (uint16_t i = 0; i < t.count; i++) {
      const dm::RegisterRow &r = t.rows[i];
      if (r.sigid == 0 || (r.fn != 3 && r.fn != 4) || r.nreg == 0 || r.nreg > 120)
        continue;
      int own = 0;
      for (const auto &p : plan)
        own += p.req.fn == r.fn && r.regaddr >= p.req.start && r.regaddr + r.nreg <= p.req.start + p.req.count &&
               p.tier == poll_tier(t, r);
      CHECK(own >= 1);
    }
    for (const auto &p : plan)
      CHECK(p.req.count >= 1 && p.req.count <= 120);
  }
  // The SH10RS / SG5.0RS fast set: exactly the two power-flow reads (AC block, meter power).
  auto p3 = build_poll_plan(*dm::find_table("inv_proto3"));
  std::vector<std::pair<uint16_t, uint16_t>> fast, live, slow;
  for (const auto &p : p3) {
    if (p.tier == PollTier::FAST)
      fast.push_back({p.req.start, p.req.count});
    if (p.tier == PollTier::LIVE)
      live.push_back({p.req.start, p.req.count});
    if (p.tier == PollTier::SLOW)
      slow.push_back({p.req.start, p.req.count});
  }
  using RS = std::pair<uint16_t, uint16_t>;
  const std::vector<RS> want_fast = {RS(5214, 39), RS(5601, 8)};
  CHECK(fast == want_fast);
  // MPPT 1..4 is its own 5 s read; the battery block 5630..5634 is inside a 5 s read; the
  // string / MPPT 5..20 blocks around it are 30 s reads.
  auto covers = [](const std::vector<RS> &v, uint16_t a, uint16_t n) {
    for (const auto &r : v)
      if (a >= r.first && a + n <= r.first + r.second)
        return true;
    return false;
  };
  CHECK(std::find(live.begin(), live.end(), RS(5333, 8)) != live.end());
  CHECK(covers(live, 5630, 5));
  CHECK(covers(slow, 5253, 1));
  CHECK(covers(slow, 5341, 1));
  CHECK(!covers(live, 5253, 1) && !covers(live, 5341, 1));
  // SBR stack: V/I/T/SOC/SOH + operation status fast.
  auto pb = build_poll_plan(*dm::find_table("battery"));
  CHECK(pb.size() >= 1 && pb[0].tier == PollTier::FAST && pb[0].req.start == 10740 && pb[0].req.count == 6);
}

/// Fake SH10RS-style inverter on the protocol-3 table: answers everything (unset = 0).
static void add_p3(Rig &r, FakeSlave &inv) {
  inv.set(4, 5520, 0xFFFF);  // capability bitmap: everything supported
  for (int i = 1; i < 60; i++)
    inv.set(4, 5520 + i, 0xFFFF);
  r.bus.slaves.push_back(&inv);
  DeviceConfig d;
  d.table = dm::find_table("inv_proto3");
  r.m.add_device(d);
  r.m.start();
}

TEST(scheduler_tiers_periods_and_smear) {
  Rig r;
  FakeSlave inv;
  add_p3(r, inv);
  r.run(20000);  // probe + initial pass
  const Device &dev = r.m.devices()[0];
  CHECK(dev.first_round_done());
  // Every read was sent in the initial pass, capability bitmap first.
  CHECK(r.bus.writes.size() > dev.jobs().size());
  CHECK_EQ((r.bus.writes[1][2] << 8 | r.bus.writes[1][3]), 5519);
  for (const auto &j : dev.jobs())
    CHECK(!send_times(r.bus, j.req).empty());
  const size_t w0 = r.bus.writes.size();
  r.run(60000);
  size_t live_sends = 0;
  std::vector<uint32_t> live_times, slow_times;
  for (const auto &j : dev.jobs()) {
    auto t = send_times(r.bus, j.req, w0);
    switch (j.effective_tier()) {
      case PollTier::FAST:
        CHECK(t.size() >= 59u && t.size() <= 61u);
        for (size_t i = 1; i < t.size(); i++)
          CHECK(t[i] - t[i - 1] >= 950u && t[i] - t[i - 1] <= 1050u);
        break;
      case PollTier::LIVE:
        CHECK(t.size() >= 11u && t.size() <= 13u);
        live_sends += t.size();
        live_times.push_back(t.back());
        break;
      case PollTier::SLOW:
        CHECK(t.size() >= 1u && t.size() <= 3u);
        slow_times.push_back(t.back());
        break;
      case PollTier::STATIC:
        CHECK(t.size() <= 1u);
        break;
    }
  }
  CHECK(live_sends > 0);
  // Smear: the LIVE reads are spread over the 5 s period, the SLOW ones over 30 s (no bursts).
  // Each read repeats at its period, so its latest send folds onto one phase inside the period;
  // the phases of one tier must be at least 300 ms apart (including across the wrap).
  auto phases_apart = [](std::vector<uint32_t> times, uint32_t period) {
    for (auto &t : times)
      t %= period;
    std::sort(times.begin(), times.end());
    for (size_t i = 1; i < times.size(); i++)
      CHECK(times[i] - times[i - 1] >= 300u);
    if (times.size() >= 2)
      CHECK(times.front() + period - times.back() >= 300u);
  };
  phases_apart(live_times, 5000);
  phases_apart(slow_times, 30000);
  // Freshness follows the FAST reads.
  CHECK(g_now - dev.fresh_ms() <= 1100u);
  CHECK(dev.fresh_count() > 150u);
}

TEST(scheduler_fast_slots_stay_on_time) {
  // A FAST read never waits behind a long read: non-FAST reads start only when they end before
  // the next FAST slot. Slow fake bus (1 char = 1.04 ms of delivery) to make reads take time.
  Rig r;
  FakeSlave inv;
  r.bus.chunk = 1;  // one byte per receive() call: long replies span many ticks
  add_p3(r, inv);
  r.run(30000);
  const Device &dev = r.m.devices()[0];
  CHECK(dev.first_round_done());
  const size_t w0 = r.bus.writes.size();
  r.run(60000);
  for (const auto &j : dev.jobs()) {
    if (j.tier != PollTier::FAST)
      continue;
    auto t = send_times(r.bus, j.req, w0);
    CHECK(t.size() >= 58u);
    for (size_t i = 1; i < t.size(); i++)
      CHECK(t[i] - t[i - 1] <= 1100u);
  }
}

TEST(scheduler_unserved_reads_drop_to_static) {
  Rig r;
  FakeSlave inv;
  // Strings + MPPT 5..20 (5253..5372) are not fitted: the inverter serves 0xFFFF there.
  for (uint16_t a = 5253; a <= 5372; a++)
    inv.set(4, a, 0xFFFF);
  add_p3(r, inv);
  r.run(20000);
  const Device &dev = r.m.devices()[0];
  int idx = -1;
  for (size_t i = 0; i < dev.jobs().size(); i++)
    if (dev.jobs()[i].req.start == 5253)
      idx = static_cast<int>(i);
  CHECK(idx >= 0);
  CHECK(dev.jobs()[idx].tier == PollTier::SLOW);
  CHECK(dev.jobs()[idx].unserved);
  CHECK(dev.jobs()[idx].effective_tier() == PollTier::STATIC);
  const size_t w0 = r.bus.writes.size();
  r.run(60000);
  CHECK(send_times(r.bus, dev.jobs()[idx].req, w0).size() <= 1u);
  // MPPT 5 gets fitted: the read is back at its SLOW period after its next (STATIC) slot.
  inv.set(4, 5341, 3000);
  r.run(600000);  // one full STATIC period: the next slot of the read is inside this window
  CHECK(!dev.jobs()[idx].unserved);
  const size_t w1 = r.bus.writes.size();
  r.run(120000);
  CHECK(send_times(r.bus, dev.jobs()[idx].req, w1).size() >= 3u);
}

TEST(scheduler_reconnect_restarts_initial_pass) {
  Rig r;
  FakeSlave inv;
  add_p3(r, inv);
  r.run(20000);
  const Device &dev = r.m.devices()[0];
  CHECK(dev.first_round_done());
  inv.mute = true;
  r.run(15000);
  CHECK(!dev.online());
  inv.mute = false;
  r.run(16000);  // offline probe every 15 s
  CHECK(dev.online());
  r.run(20000);
  CHECK(dev.first_round_done());
}

TEST(scheduler_offline_after_10_lost_frames_keeps_values) {
  Rig r;
  FakeSlave a;
  r.bus.slaves = {&a};
  MasterTiming t;
  t.offline_probe_ms = 5000;
  r.m.set_timing(t);
  DeviceConfig d;
  d.index = 3;
  d.table = dm::find_table("other_dev18_st6200");
  r.m.add_device(d);
  r.run(2000);
  const Device &dev = r.m.devices()[0];
  CHECK(dev.online());
  size_t valid = count_valid(r.store, 3);
  CHECK(valid > 2);
  // Silent slave: no retransmit; offline after 10 lost frames spanning 10 s (the one FAST read
  // of the meter goes out once a second).
  a.mute = true;
  size_t w0 = r.bus.writes.size();
  r.run(9000);
  CHECK(dev.online());  // 9 lost frames
  CHECK(count_valid(r.store, 3) == valid);  // lost frames keep values
  r.run(3000);
  CHECK(!dev.online());
  CHECK(r.bus.writes.size() - w0 >= 10u && r.bus.writes.size() - w0 <= 12u);
  CHECK(dev.stats().timeouts >= 10u);
  // Offline clears only tag-bit1 rows: st6200 has 2 rows without it (kept).
  const auto *tab = dev.table();
  size_t keep = 0;
  for (uint16_t i = 0; i < tab->count; i++)
    keep += tab->rows[i].sigid != 0 && !(tab->rows[i].tag & dm::TAG_CLEAR_OFFLINE) &&
            r.store.is_valid(3, tab->rows[i].sigid);
  CHECK(keep >= 1u);
  CHECK_EQ(count_valid(r.store, 3), keep);
  CHECK(dev.stats().loss_pct() >= 100.0f * 10 / 64 - 0.01f);  // >= 10 lost in the 64-attempt window
  // While offline only a probe every 5 s goes out.
  size_t w1 = r.bus.writes.size();
  r.run(10000);
  size_t probes = r.bus.writes.size() - w1;
  CHECK(probes >= 1 && probes <= 2);
  a.mute = false;
  r.run(8000);
  CHECK(dev.online());
}

TEST(scheduler_retries_option) {
  Rig r;
  FakeSlave a;
  r.bus.slaves = {&a};
  MasterTiming t;
  t.retries = 1;
  t.offline_after = 3;
  t.offline_silence_ms = 0;
  t.poll_timeout_margin_ms = 100;
  r.m.set_timing(t);
  DeviceConfig d;
  d.table = dm::find_table("other_dev18_st6200");
  r.m.add_device(d);
  r.run(1000);
  a.mute = true;
  r.run(5000);
  CHECK(!r.m.devices()[0].online());
  CHECK(r.m.devices()[0].stats().timeouts >= 6u);  // 3 transactions x (1 + 1 retry)
}

TEST(scheduler_crc_error_counted) {
  Rig r;
  FakeSlave a;
  a.corrupt_next = 1;
  r.bus.slaves = {&a};
  DeviceConfig d;
  d.table = dm::find_table("other_dev18_st6300");
  r.m.add_device(d);
  r.run(5000);
  const Device &dev = r.m.devices()[0];
  CHECK_EQ(dev.stats().crc_errors, 1u);
  CHECK(dev.online());
  CHECK_EQ(dev.stats().consecutive_failures, 0);
}

TEST(scheduler_exception_keeps_values_and_backs_off) {
  Rig r;
  FakeSlave a;
  r.bus.slaves = {&a};
  a.set(4, 4097, 0x4366);  // phase-A voltage float 230.5 (0x43668000), low word first
  a.set(4, 4097, 0x8000);
  a.set(4, 4098, 0x4366);
  DeviceConfig d;
  d.table = dm::find_table("other_dev18_st6200");
  r.m.add_device(d);
  r.run(1000);
  const Device &dev = r.m.devices()[0];
  CHECK_NEAR(r.store.get_number(0, 0x2000), 230.5, 1e-3);  // float in volts; 0.1 is a decimals hint
  uint16_t s0 = dev.jobs()[0].req.start;
  a.exception_ranges.push_back({s0, static_cast<uint16_t>(s0 + 1)});
  size_t w0 = r.bus.writes.size();
  r.run(3000);
  size_t hits = 0;
  for (size_t i = w0; i < r.bus.writes.size(); i++)
    hits += (r.bus.writes[i][2] << 8 | r.bus.writes[i][3]) == s0 - 1;
  CHECK(hits <= 2u);  // rejected: polled at the STATIC period from now on
  CHECK(dev.jobs()[0].unserved);
  CHECK(dev.online());
  CHECK(dev.stats().exceptions >= 1u);
  CHECK_NEAR(r.store.get_number(0, 0x2000), 230.5, 1e-3);  // old value kept
  // Accepted again: back at its own period after the next STATIC slot.
  a.exception_ranges.clear();
  r.run(301000);
  CHECK(!dev.jobs()[0].unserved);
}

// ---------------------------------------------------------------------------------------------
// End-to-end: fake SH hybrid (dev_code 0x0D0F, protocol code "AA10" -> protocol 2).
static void make_inverter(FakeSlave &inv) {
  inv.set(4, 2515, 0x0D0F);
  const char *model = "SH10RT";
  for (int i = 0; i < 8; i++) {
    uint8_t hi = i * 2 < 6 ? model[i * 2] : 0, lo = i * 2 + 1 < 6 ? model[i * 2 + 1] : 0;
    inv.set(4, 2516 + i, hi << 8 | lo);
  }
  inv.set(4, 2534, ('1' << 8) | '0');  // applyProtocolCode, low word first -> "AA10"
  inv.set(4, 2535, ('A' << 8) | 'A');
  // Capability bitmap (sig 0x2181 @5520, 60 regs): word 1 bit 4 -> today_energy (5223) supported.
  inv.set(4, 5520, 1u << 4);
  inv.set(4, 5223, 123);                 // daily yield 12.3 kWh (u16 x0.1)
  inv.set32(4, 5232, (uint32_t) -2500);  // total active power -2.5 kW (s32 x0.001)
}

TEST(end_to_end_fake_inverter) {
  Rig r;
  FakeSlave inv;
  make_inverter(inv);
  r.bus.slaves = {&inv};
  DeviceConfig d;
  d.index = 0;
  d.address = 1;
  d.table = nullptr;  // auto
  d.fallback_table = dm::find_table("inv_proto2");
  Device &dev0 = r.m.add_device(d);
  (void) dev0;
  r.run(30000);
  const Device &dev = r.m.devices()[0];
  CHECK(dev.discovered());
  CHECK_EQ(dev.devinfo().dev_code, 0x0D0F);
  CHECK_EQ(dev.devinfo().protocol, 2);
  CHECK_EQ(dev.devinfo().dev_type, 35);
  CHECK(std::string(dev.devinfo().model) == "SH10RT");
  CHECK(dev.table() == dm::find_table("inv_proto2"));
  CHECK(dev.gating());
  CHECK(dev.online());
  CHECK_EQ(r.bus.writes[0][1], 4);
  CHECK_EQ((r.bus.writes[0][2] << 8 | r.bus.writes[0][3]), 2514);  // devinfo first
  CHECK_EQ(r.bus.writes[0][5], 66);
  CHECK_NEAR(r.store.get_number(0, 0x2080), -2.5, 1e-5);   // curr_power
  CHECK_NEAR(r.store.get_number(0, 0x2020), 12.3, 1e-5);   // today_energy (gated, bit set)
  CHECK(dev.first_round_done());
  CHECK_EQ((int) r.store.get_number(0, SIG_PROTOCOL_TYPE), 2);
  CHECK_EQ((int) r.store.get_number(0, 0x0004), 0x0D0F);   // devCode stored from the devinfo block
  // Every request of the plan has been sent at least once within 30 s.
  for (auto &j : dev.jobs()) {
    bool sent = false;
    for (auto &w : r.bus.writes)
      sent |= w[1] == j.req.fn && (w[2] << 8 | w[3]) == j.req.start - 1 && w[5] == j.req.count;
    CHECK(sent);
  }
  // Capability bit cleared -> the gated row becomes invalid once the bitmap (STATIC tier, every
  // 5 min) has been read again.
  inv.set(4, 5520, 0);
  r.run(301000);
  CHECK(std::isnan(r.store.get_number(0, 0x2020)));
  CHECK_NEAR(r.store.get_number(0, 0x2080), -2.5, 1e-5);  // ungated row unaffected
}

// As stock: a rejected devinfo read runs the device as protocol 1 (inv_proto1_4), inverter type
// from the inv_proto1_4 poll of 30644.
TEST(discovery_exception_uses_fallback) {
  Rig r;
  FakeSlave inv;
  inv.exception_ranges.push_back({2515, 2516});
  inv.set(3, INV_TYPE_REG, 1);
  r.bus.slaves = {&inv};
  DeviceConfig d;
  d.fallback_table = dm::find_table("inv_proto1_4");  // the YAML default
  r.m.add_device(d);
  r.run(2000);
  const Device &dev = r.m.devices()[0];
  CHECK(!dev.discovered());
  CHECK(dev.identified());
  CHECK(dev.table() == dm::find_table("inv_proto1_4"));
  CHECK(!dev.gating());
  CHECK_EQ(dev.devinfo().protocol, 1);
  CHECK_EQ(dev.devinfo().dev_type, 1);
  CHECK(std::string(dev.dev_type_source()) == "table");
  CHECK_EQ((int) r.store.get_number(0, SIG_PROTOCOL_TYPE), 1);
  // The p14 full round reads 30644 into sig 0x1002: inverter type 1 -> res_new_pv.
  r.run(8000);
  CHECK(dev.inv_type_known());
  CHECK_EQ(dev.inv_type(), 1);
  CHECK(dev.xml_family() == XmlFamily::RES_NEW_PV);

  // Scan slot without scan_fallback_table: the same inv_proto1_4 / protocol 1.
  Rig s;
  FakeSlave exc;
  exc.addr = 1;
  exc.exception_ranges.push_back({DEVINFO_START, static_cast<uint16_t>(DEVINFO_START + 1)});
  s.bus.slaves = {&exc};
  MasterTiming t;
  t.response_timeout_ms = 200;
  s.m.set_timing(t);
  s.m.set_scan(1, 1, 2);
  s.run(3000);
  const Device *d1 = s.m.find_device_by_address(1);
  CHECK(d1 != nullptr);
  CHECK(!d1->discovered());
  CHECK(d1->state() == Device::State::RUN);
  CHECK(d1->table() == dm::find_table("inv_proto1_4"));
  CHECK_EQ(d1->devinfo().protocol, 1);
  CHECK_EQ((int) s.store.get_number(d1->config().index, SIG_PROTOCOL_TYPE), 1);
  CHECK(d1->xml_family() == XmlFamily::STR_PV);  // 30644 reads 0 here
}

struct CountingListener : MasterListener {
  int discovered{0};
  int identity{0};
  void on_discovered(const Device &) override { discovered++; }
  void on_identity_changed(const Device &) override { identity++; }
};

/// HOME P1 inverter: dev_code 0x0110 (dev_type 21), no protocol code, no ZS tag -> protocol 1.
static void make_p1_inverter(FakeSlave &inv) {
  inv.set(4, 2515, 0x0110);
  inv.set(4, 2516, ('S' << 8) | 'G');
}

TEST(inv_type_boot_read_and_subscription) {
  {
    // 30644 = 1 on a protocol-1 device -> res_new_pv, read right after the devinfo probe.
    Rig r;
    CountingListener l;
    r.m.set_listener(&l);
    FakeSlave inv;
    make_p1_inverter(inv);
    inv.set(3, INV_TYPE_REG, 1);
    r.bus.slaves = {&inv};
    DeviceConfig d;
    d.fallback_table = dm::find_table("inv_proto1_4");
    r.m.add_device(d);
    r.run(300);
    const Device &dev = r.m.devices()[0];
    CHECK(dev.discovered());
    CHECK_EQ(dev.devinfo().protocol, 1);
    CHECK_EQ(dev.devinfo().dev_type, 21);
    CHECK(std::string(dev.dev_type_source()) == "devinfo");
    CHECK(r.bus.writes.size() >= 2u);
    CHECK_EQ(r.bus.writes[1][1], 3);
    CHECK_EQ((r.bus.writes[1][2] << 8 | r.bus.writes[1][3]), INV_TYPE_REG - 1);
    CHECK_EQ(r.bus.writes[1][5], 1);
    CHECK(dev.inv_type_known());
    CHECK_EQ(dev.inv_type(), 1);
    CHECK(dev.xml_family() == XmlFamily::RES_NEW_PV);
    CHECK(dev.descriptor() != nullptr && dev.descriptor()->country_reg == 30643);
    CHECK_EQ((int) r.store.get_number(0, SIG_INV_TYPE), 1);
    CHECK_EQ(l.discovered, 1);  // after the 30644 answer
    CHECK_EQ(l.identity, 0);
    bool logged = false;
    for (auto &s : r.logs)
      logged |= s.find("inv_type 1, xml family res_new_pv") != std::string::npos;
    CHECK(logged);
    // The p14 poll keeps it current: 30644 -> 0 -> str_pv; -> 0xFFFF (sentinel) -> unknown, inv 0.
    inv.set(3, INV_TYPE_REG, 0);
    r.run(5000);
    CHECK(dev.inv_type_known());
    CHECK_EQ(dev.inv_type(), 0);
    CHECK(dev.xml_family() == XmlFamily::STR_PV);
    CHECK_EQ(l.identity, 1);
    inv.set(3, INV_TYPE_REG, 0xFFFF);
    // The 30644 read (STATIC tier) comes again at its next 5-min slot; after the initial pass
    // that is the first slot more than half a period away (150..450 s).
    r.run(460000);
    CHECK(!dev.inv_type_known());
    CHECK(dev.xml_family() == XmlFamily::STR_PV);
    CHECK_EQ(l.identity, 2);
  }
  {
    // 0xFFFF at boot: inverter type unknown -> inv 0 -> str_pv; signal left invalid.
    Rig r;
    CountingListener l;
    r.m.set_listener(&l);
    FakeSlave inv;
    make_p1_inverter(inv);
    inv.set(3, INV_TYPE_REG, 0xFFFF);
    r.bus.slaves = {&inv};
    DeviceConfig d;
    r.m.add_device(d);
    r.run(3000);  // the initial pass reads 30644 (p14 table) too
    const Device &dev = r.m.devices()[0];
    CHECK(dev.discovered());
    CHECK(!dev.inv_type_known());
    CHECK(dev.xml_family() == XmlFamily::STR_PV);
    CHECK(!r.store.is_valid(0, SIG_INV_TYPE));
    CHECK_EQ(l.discovered, 1);
  }
  {
    // Exception on 30644 (normal for P2/P3/P5): unknown, not a lost frame.
    Rig r;
    FakeSlave inv;
    make_p1_inverter(inv);
    inv.exception_ranges.push_back({INV_TYPE_REG, static_cast<uint16_t>(INV_TYPE_REG + 1)});
    r.bus.slaves = {&inv};
    DeviceConfig d;
    r.m.add_device(d);
    r.run(300);
    const Device &dev = r.m.devices()[0];
    CHECK(dev.discovered());
    CHECK(!dev.inv_type_known());
    CHECK(dev.xml_family() == XmlFamily::STR_PV);
    CHECK_EQ(dev.stats().consecutive_failures, 0);
    CHECK(dev.online());
  }
  {
    // Timeout on 30644: unknown, not counted toward the offline logic.
    Rig r;
    CountingListener l;
    r.m.set_listener(&l);
    FakeSlave inv;
    make_p1_inverter(inv);
    inv.silent_ranges.push_back({INV_TYPE_REG, static_cast<uint16_t>(INV_TYPE_REG + 1)});
    r.bus.slaves = {&inv};
    DeviceConfig d;
    r.m.add_device(d);
    r.run(1100);  // devinfo answered, 30644 timed out after 1000 ms, its poll not reached yet
    const Device &dev = r.m.devices()[0];
    CHECK(dev.discovered());
    CHECK_EQ(l.discovered, 1);
    CHECK(!dev.inv_type_known());
    CHECK_EQ(dev.stats().timeouts, 1u);
    CHECK_NEAR(dev.stats().loss_pct(), 0.0, 1e-9);  // the silent 30644 is no lost frame
    CHECK_EQ(dev.stats().consecutive_failures, 0);
    CHECK(dev.online());
  }
  {
    // SG5.0RS (protocol 3): 30644 = 0xFFFF live -> unit with the 32310 descriptor. A value read on
    // a protocol 2/3/5 device is reported but does not change the family (stock reads it on p14 only).
    Rig r;
    FakeSlave inv;
    std::map<uint16_t, uint16_t> regs;
    sg5rs_devinfo(regs);
    for (auto &kv : regs)
      inv.set(4, kv.first, kv.second);
    inv.set(3, INV_TYPE_REG, 0xFFFF);
    r.bus.slaves = {&inv};
    DeviceConfig d;
    r.m.add_device(d);
    r.run(300);
    const Device &dev = r.m.devices()[0];
    CHECK_EQ(dev.devinfo().protocol, 3);
    CHECK(dev.table() == dm::find_table("inv_proto3"));
    CHECK(dev.inv_type_known());  // stock: the 0xFFFF sentinel is inverter type 0
    CHECK_EQ(dev.inv_type(), 0);
    CHECK(dev.xml_family() == XmlFamily::UNIT);
    CHECK(dev.descriptor() != nullptr && dev.descriptor()->country_reg == 32310);
    Rig r2;
    inv.set(3, INV_TYPE_REG, 2);
    r2.bus.slaves = {&inv};
    r2.m.add_device(d);
    r2.run(300);
    const Device &dev2 = r2.m.devices()[0];
    CHECK(dev2.inv_type_known());
    CHECK_EQ(dev2.inv_type(), 2);
    CHECK(dev2.xml_family() == XmlFamily::UNIT);
    CHECK(dev2.descriptor() != nullptr);
  }
}

// F12: a fixed-table device gets its dev_type from YAML or the table family.
TEST(fixed_table_dev_type_inferred_and_yaml) {
  Rig r;
  struct Case {
    const char *table;
    uint8_t yaml;
    uint8_t want;
    const char *src;
    uint8_t protocol;
  } cases[] = {
      {"inv_proto2", 0, 1, "table", 2},  {"inv_proto2", 35, 35, "yaml", 2},
      {"inv_proto1_4", 0, 1, "table", 1}, {"battery", 0, 44, "table", 0},
      {"other_dev18_st6200", 0, 18, "table", 0}, {"other_dev34_st1819", 0, 34, "table", 0},
      {"charger", 0, 46, "table", 0},  {"energy_bridge", 0, 92, "table", 0},
  };
  uint8_t idx = 0;
  for (const auto &c : cases) {
    DeviceConfig d;
    d.index = idx;
    d.address = static_cast<uint8_t>(idx + 1);
    d.table = dm::find_table(c.table);
    d.dev_type = c.yaml;
    r.m.add_device(d);
    idx++;
  }
  DeviceConfig a;  // auto, never answers: not identified
  a.index = idx;
  a.address = static_cast<uint8_t>(idx + 1);
  r.m.add_device(a);
  r.m.start();
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const Device &d = r.m.devices()[i];
    CHECK(!d.discovered());
    CHECK(d.identified());
    CHECK_EQ(d.devinfo().dev_type, cases[i].want);
    CHECK(std::string(d.dev_type_source()) == cases[i].src);
    CHECK_EQ(d.devinfo().protocol, cases[i].protocol);
    if (cases[i].protocol != 0)
      CHECK_EQ((int) r.store.get_number(d.config().index, SIG_PROTOCOL_TYPE), cases[i].protocol);
    else
      CHECK(r.store.get(d.config().index, SIG_PROTOCOL_TYPE) == nullptr);
  }
  CHECK(r.m.devices()[0].xml_family() == XmlFamily::UNIT);
  CHECK(r.m.devices()[2].xml_family() == XmlFamily::STR_PV);
  // Grid-code families (Setup / Installer) exist for inverters only.
  CHECK(r.m.devices()[3].xml_family() == XmlFamily::NONE);  // battery
  CHECK(r.m.devices()[3].descriptor() == nullptr);
  CHECK(r.m.devices()[6].xml_family() == XmlFamily::NONE);  // charger
  const Device &au = r.m.devices().back();
  CHECK(!au.identified());
  CHECK(std::string(au.dev_type_source()) == "none");
  CHECK_EQ(au.devinfo().dev_type, 0);

  // YAML dev_type also overrides the devinfo block of a discovered device.
  Rig r2;
  FakeSlave inv;
  make_p1_inverter(inv);
  r2.bus.slaves = {&inv};
  DeviceConfig d;
  d.dev_type = 35;
  r2.m.add_device(d);
  r2.run(300);
  CHECK(r2.m.devices()[0].discovered());
  CHECK_EQ(r2.m.devices()[0].devinfo().dev_type, 35);
  CHECK(std::string(r2.m.devices()[0].dev_type_source()) == "yaml");
}

// Boot bus scan: probe a bounded address range; a present device (valid devinfo or a Modbus
// exception) is kept, a silent address is abandoned after scan_attempts misses and never probed
// again (until rescan()).
TEST(bus_scan_autodetect) {
  Rig r;
  FakeSlave inv;   // addr 1: valid devinfo -> discovered, table activated
  make_inverter(inv);
  inv.addr = 1;
  FakeSlave exc;   // addr 3: answers the devinfo read with a Modbus exception -> present
  exc.addr = 3;
  exc.exception_ranges.push_back({DEVINFO_START, static_cast<uint16_t>(DEVINFO_START + 1)});
  r.bus.slaves = {&inv, &exc};  // addr 2 and addr 4: nobody answers -> timeout
  MasterTiming t;
  t.response_timeout_ms = 200;   // keep the four discovery timeouts cheap
  t.offline_probe_ms = 1000;
  r.m.set_timing(t);
  r.m.set_scan(1, 4, 2);
  r.run(10000);

  auto *d1 = r.m.find_device_by_address(1);
  auto *d2 = r.m.find_device_by_address(2);
  auto *d3 = r.m.find_device_by_address(3);
  auto *d4 = r.m.find_device_by_address(4);
  CHECK(d1 != nullptr && d2 != nullptr && d3 != nullptr && d4 != nullptr);
  // The scan created a device per address.
  CHECK_EQ(r.m.devices().size(), 4u);

  // addr 1: real inverter discovered and polling.
  CHECK(d1->discovered());
  CHECK(d1->table() == dm::find_table("inv_proto2"));
  CHECK(d1->online());
  CHECK(!d1->abandoned());

  // addr 3: answered (exception) so it is present and must never be abandoned.
  CHECK(!d3->abandoned());
  CHECK(d3->stats().exceptions >= 1u);

  // addr 2 and addr 4: silent -> abandoned after exactly scan_attempts (2) probes.
  CHECK(d2->abandoned());
  CHECK(d4->abandoned());
  auto sends_to = [&](uint8_t addr) {
    size_t n = 0;
    for (auto &w : r.bus.writes)
      n += !w.empty() && w[0] == addr;
    return n;
  };
  CHECK_EQ(sends_to(2), 2u);
  CHECK_EQ(sends_to(4), 2u);

  // No further frames go to an abandoned slot.
  size_t w2 = sends_to(2), w4 = sends_to(4);
  r.run(10000);
  CHECK_EQ(sends_to(2), w2);
  CHECK_EQ(sends_to(4), w4);

  // rescan() re-arms the abandoned slots (they probe, time out and abandon again).
  r.m.rescan();
  CHECK(!d2->abandoned());
  CHECK(!d4->abandoned());
  r.run(5000);
  CHECK(d2->abandoned());
  CHECK(d4->abandoned());
  CHECK_EQ(sends_to(2), w2 + 2);
  CHECK_EQ(sends_to(4), w4 + 2);
}

// A hybrid inverter reports its battery addresses in 10451.. (as stock); each one is
// probed with the SBR identity read (fn 4 10710 x 11) and added as an SBR battery. A silent battery
// does not block the others and is asked again later; batteries never get the devinfo read.
TEST(children_sbr_from_inverter_list) {
  Rig r;
  FakeSlave inv;
  make_inverter(inv);
  inv.addr = 1;
  inv.set(4, 5524, 1u << 3);  // capability word 5 bit 3: battery address rows 10451..10455
  inv.set(4, 10451, 200);
  inv.set(4, 10452, 201);
  inv.set(4, 10453, 0xFFFF);  // empty slot
  FakeSlave bat;
  bat.addr = 200;
  bat.set(4, 10710, 0x20EA);  // SBR192
  bat.set(4, 10711, ('S' << 8) | '2');
  FakeSlave dead;             // reported but silent
  dead.addr = 201;
  dead.mute = true;
  r.bus.slaves = {&inv, &bat, &dead};
  MasterTiming t;
  t.response_timeout_ms = 200;
  t.offline_probe_ms = 1000;
  r.m.set_timing(t);
  r.m.set_scan(1, 2, 2);
  r.run(20000);

  auto *d200 = r.m.find_device_by_address(200);
  auto *d201 = r.m.find_device_by_address(201);
  CHECK(d200 != nullptr && d201 != nullptr);
  CHECK_EQ(r.m.devices().size(), 4u);  // inverter, scan slot 2, two reported batteries
  CHECK(d200->discovered() && d200->online());
  CHECK(d200->table() == dm::find_table("battery"));
  CHECK_EQ(d200->devinfo().dev_type, 44);
  CHECK_EQ(d200->devinfo().dev_code, 0x20EA);
  CHECK(std::string(d200->devinfo().model) == "SBR192");
  CHECK(d200->config().sbr_cells);
  CHECK(d200->config().child_kind == ChildKind::SBR);
  CHECK_EQ(d200->config().parent, 0);
  CHECK(!d201->discovered() && !d201->abandoned());
  size_t devinfo_to_bat = 0, probes_201 = 0;
  for (auto &w : r.bus.writes) {
    if (w.size() < 6)
      continue;
    const uint16_t start = static_cast<uint16_t>(w[2] << 8 | w[3]) + 1;
    devinfo_to_bat += (w[0] == 200 || w[0] == 201) && start == DEVINFO_START;
    probes_201 += w[0] == 201;
  }
  CHECK_EQ(devinfo_to_bat, 0u);
  CHECK(probes_201 >= 1);
  // The silent one is asked again after CHILD_RETRY_MS, not continuously.
  r.run(30000);
  size_t probes_201_later = 0;
  for (auto &w : r.bus.writes)
    probes_201_later += !w.empty() && w[0] == 201;
  CHECK_EQ(probes_201_later, probes_201);
  r.run(ModbusMaster::CHILD_RETRY_MS);
  probes_201_later = 0;
  for (auto &w : r.bus.writes)
    probes_201_later += !w.empty() && w[0] == 201;
  CHECK(probes_201_later > probes_201);
}

// Support evidence for the diagnostics report: the raw identification reply is kept, a rejected
// read remembers its exception code, and a device discovery could not classify is reported once
// through MasterListener::on_unsupported.
struct UnsupportedListener : MasterListener {
  std::vector<std::pair<uint8_t, std::string>> seen;
  void on_unsupported(const Device &d, const char *why) override { seen.emplace_back(d.config().index, why); }
};

TEST(support_evidence_ident_raw_and_exceptions) {
  Rig r;
  FakeSlave inv;
  make_inverter(inv);
  r.bus.slaves = {&inv};
  UnsupportedListener ul;
  r.m.set_listener(&ul);
  DeviceConfig d;
  d.fallback_table = dm::find_table("inv_proto2");
  r.m.add_device(d);
  r.run(30000);
  const Device &dev = r.m.devices()[0];
  // The devinfo block as answered: 66 registers from 2515, dev_code first.
  Device::IdentRaw ir = dev.ident_raw();
  CHECK_EQ(ir.nregs, DEVINFO_COUNT);
  CHECK_EQ(ir.start, DEVINFO_START);
  CHECK_EQ(ir.fn, 4);
  CHECK_EQ((ir.data[0] << 8 | ir.data[1]), 0x0D0F);
  CHECK(std::string(reinterpret_cast<const char *>(ir.data + 2), 6) == "SH10RT");
  CHECK(!dev.fallback_used());
  CHECK(std::string(dev.table_name()) == "inv_proto2");
  CHECK(ul.seen.empty());
  for (auto &j : dev.jobs()) {
    CHECK_EQ(j.last_exc, 0);
    CHECK_EQ(j.exc_count, 0u);
  }
  // A block the inverter rejects from now on: its read remembers the exception code.
  uint16_t s0 = 0;
  for (auto &j : dev.jobs())
    if (j.tier == PollTier::FAST) {
      s0 = j.req.start;
      break;
    }
  CHECK(s0 != 0);
  inv.exception_ranges.push_back({s0, static_cast<uint16_t>(s0 + 1)});
  r.run(3000);
  bool found = false;
  for (auto &j : dev.jobs())
    if (j.req.start == s0) {
      found = true;
      CHECK(j.unserved);
      CHECK_EQ(j.last_exc, 2);
      CHECK(j.exc_count >= 1u);
    }
  CHECK(found);
}

TEST(support_evidence_unsupported_once) {
  // Rejected devinfo read: the fallback table runs the device and on_unsupported fires once.
  Rig r;
  FakeSlave inv;
  inv.exception_ranges.push_back({2515, 2516});
  inv.set(3, INV_TYPE_REG, 1);
  r.bus.slaves = {&inv};
  UnsupportedListener ul;
  r.m.set_listener(&ul);
  DeviceConfig d;
  d.fallback_table = dm::find_table("inv_proto1_4");
  r.m.add_device(d);
  r.run(10000);
  const Device &dev = r.m.devices()[0];
  CHECK(dev.fallback_used());
  CHECK(std::string(dev.table_name()) == "inv_proto1_4");
  CHECK_EQ(dev.ident_raw().nregs, 0);
  CHECK_EQ(ul.seen.size(), 1u);
  CHECK(ul.seen[0].second == "devinfo_rejected");
  // Going offline and being rediscovered does not report it again.
  inv.mute = true;
  r.run(60000);
  inv.mute = false;
  r.run(60000);
  CHECK_EQ(ul.seen.size(), 1u);

  // A reported battery whose identity read carries dev_code 0: the reply is kept, the device
  // stays unidentified, and it is reported once with its own reason.
  Rig s;
  FakeSlave inv2;
  make_inverter(inv2);
  inv2.set(4, 5524, 1u << 3);
  inv2.set(4, 10451, 200);
  FakeSlave bat;
  bat.addr = 200;
  bat.set(4, 10710, 0);
  bat.set(4, 10711, ('S' << 8) | '2');
  s.bus.slaves = {&inv2, &bat};
  UnsupportedListener ul2;
  s.m.set_listener(&ul2);
  MasterTiming t;
  t.response_timeout_ms = 200;
  s.m.set_timing(t);
  s.m.set_scan(1, 1, 2);
  s.run(20000);
  auto *d200 = s.m.find_device_by_address(200);
  CHECK(d200 != nullptr);
  CHECK(!d200->discovered());
  CHECK_EQ(d200->ident_raw().nregs, 11);
  CHECK_EQ(d200->ident_raw().start, 10710);
  CHECK_EQ(ul2.seen.size(), 1u);
  CHECK(ul2.seen[0].second == "child_unparsed");
  CHECK_EQ(ul2.seen[0].first, d200->config().index);
}

// 5621 bit 0 / bit 2: an EV charger at 248 (fn 4 21201 x 24) and an Energy Bridge at 246
// (fn 4 4096 x 42), as stock.
TEST(children_charger_and_bridge_from_5621) {
  Rig r;
  FakeSlave inv;
  make_inverter(inv);
  inv.addr = 1;
  inv.set(4, 5524, 1u << 11);  // capability word 5 bit 11: 5621
  inv.set(4, 5621, 0x5);
  FakeSlave chg;
  chg.addr = 248;
  chg.set(4, 21224, 0x1234);
  const char *m = "AC011E";
  for (int i = 0; i < 3; i++)
    chg.set(4, 21216 + i, static_cast<uint16_t>(m[2 * i] << 8 | m[2 * i + 1]));
  FakeSlave eb;
  eb.addr = 246;
  eb.set(4, 4127, 0x4321);
  r.bus.slaves = {&inv, &chg, &eb};
  MasterTiming t;
  t.response_timeout_ms = 200;
  r.m.set_timing(t);
  r.m.set_scan(1, 1, 2);
  r.run(20000);

  auto *c = r.m.find_device_by_address(248);
  auto *e = r.m.find_device_by_address(246);
  CHECK(c != nullptr && c->discovered());
  CHECK(c != nullptr && c->table() == dm::find_table("charger"));
  CHECK(c != nullptr && c->devinfo().dev_type == 46 && c->devinfo().dev_code == 0x1234);
  CHECK(c != nullptr && std::string(c->devinfo().model) == "AC011E");
  CHECK(e != nullptr && e->discovered());
  CHECK(e != nullptr && e->table() == dm::find_table("energy_bridge"));
  CHECK(e != nullptr && e->devinfo().dev_type == 92 && e->devinfo().dev_code == 0x4321);
}

// ---------------------------------------------------------------------------------------------
// Stock inverter search (msg 0x72 HOME search): one broadcast read of 30035, remembered addresses,
// the range probe only as the read-only fallback.
struct RememberListener : MasterListener {
  int changed{0};
  void on_remembered_changed() override { changed++; }
};

static size_t search_frames(const FakeBus &bus) {
  size_t n = 0;
  for (auto &w : bus.writes)
    n += w.size() == 8 && w[0] == 0 && w[1] == 3;
  return n;
}

static size_t frames_to(const FakeBus &bus, uint8_t addr) {
  size_t n = 0;
  for (auto &w : bus.writes)
    n += !w.empty() && w[0] == addr;
  return n;
}

TEST(search_finds_inverter_without_range_probe) {
  Rig r;
  RememberListener l;
  r.m.set_listener(&l);
  FakeSlave inv;
  make_inverter(inv);
  inv.addr = 5;
  inv.answers_search = true;
  r.bus.slaves = {&inv};
  MasterTiming t;
  t.response_timeout_ms = 200;
  r.m.set_timing(t);
  r.m.set_search(true);
  r.m.set_scan(1, 8, 2);
  r.run(10000);

  // The very first frame is the stock broadcast read: 00 03 75 52 00 01 3E 06.
  CHECK(!r.bus.writes.empty());
  auto want = hex("00 03 75 52 00 01 3E 06");
  CHECK(r.bus.writes[0] == want);
  CHECK_EQ(search_frames(r.bus), 1u);
  auto *d = r.m.find_device_by_address(5);
  CHECK(d != nullptr && d->discovered() && d->online());
  CHECK_EQ(r.m.devices().size(), 1u);  // no range slots
  for (uint8_t a = 1; a <= 8; a++)
    if (a != 5)
      CHECK_EQ(frames_to(r.bus, a), 0u);
  CHECK_EQ(r.m.remembered().size(), 1u);
  CHECK_EQ(r.m.remembered()[0], 5);
  CHECK(l.changed >= 1);
}

TEST(search_unanswered_falls_back_to_range_probe) {
  Rig r;
  FakeSlave inv;
  make_inverter(inv);
  inv.addr = 2;  // does not answer the broadcast
  r.bus.slaves = {&inv};
  MasterTiming t;
  t.response_timeout_ms = 200;
  r.m.set_timing(t);
  r.m.set_search(true);
  r.m.set_scan(1, 3, 2);
  r.run(15000);

  CHECK_EQ(search_frames(r.bus), 2u);  // two tries, then the fallback
  auto *d = r.m.find_device_by_address(2);
  CHECK(d != nullptr && d->discovered() && d->online());
  CHECK(r.m.find_device_by_address(1)->abandoned());
  CHECK(r.m.find_device_by_address(3)->abandoned());
  CHECK_EQ(r.m.remembered().size(), 1u);
  CHECK_EQ(r.m.remembered()[0], 2);
  // An inverter is online: no periodic search.
  r.run(2 * ModbusMaster::SEARCH_REPEAT_MS);
  CHECK_EQ(search_frames(r.bus), 2u);
}

TEST(search_remembered_inverter_polled_directly) {
  Rig r;
  FakeSlave inv;
  make_inverter(inv);
  inv.addr = 1;
  inv.answers_search = true;
  r.bus.slaves = {&inv};
  MasterTiming t;
  t.response_timeout_ms = 200;
  r.m.set_timing(t);
  r.m.set_search(true);
  r.m.set_scan(1, 8, 2);
  r.m.set_remembered({1});
  r.run(10000);
  CHECK_EQ(r.m.devices().size(), 1u);
  CHECK(r.m.devices()[0].discovered());
  CHECK_EQ(r.m.devices()[0].config().index, 0);
  CHECK_EQ(search_frames(r.bus), 1u);
  for (uint8_t a = 2; a <= 8; a++)
    CHECK_EQ(frames_to(r.bus, a), 0u);
}

// At night the remembered inverter is dark: it is retried (never abandoned), the search repeats
// every SEARCH_REPEAT_MS, and the range is probed once after the remembered address has been
// asked (not again on the repeated searches; again after rescan()).
TEST(search_remembered_dark_inverter_range_probed_once) {
  Rig r;
  FakeSlave inv;
  make_inverter(inv);
  inv.addr = 1;
  inv.answers_search = true;
  inv.mute = true;
  r.bus.slaves = {&inv};
  MasterTiming t;
  t.response_timeout_ms = 200;
  t.offline_probe_ms = 5000;
  r.m.set_timing(t);
  r.m.set_search(true);
  r.m.set_scan(1, 4, 2);
  r.m.set_remembered({1});
  r.run(4000);
  // The search is unanswered before address 1 has been asked: no range probe yet.
  CHECK_EQ(search_frames(r.bus), 2u);
  CHECK_EQ(frames_to(r.bus, 2), 0u);
  r.run(ModbusMaster::SEARCH_REPEAT_MS + 6000);
  CHECK_EQ(r.m.devices().size(), 4u);  // remembered 1 + range slots 2..4
  CHECK(!r.m.devices()[0].abandoned());
  CHECK_EQ(search_frames(r.bus), 4u);  // 2 at start + 2 after SEARCH_REPEAT_MS
  for (uint8_t a = 2; a <= 4; a++) {
    CHECK(r.m.find_device_by_address(a)->abandoned());
    CHECK_EQ(frames_to(r.bus, a), 2u);  // scan_attempts, once
  }
  const size_t probes = frames_to(r.bus, 1);
  CHECK(probes >= 3);
  // The repeated searches do not probe the range again; rescan() does, once.
  r.run(ModbusMaster::SEARCH_REPEAT_MS);
  CHECK_EQ(frames_to(r.bus, 2), 2u);
  r.m.rescan();
  r.run(10000);
  CHECK_EQ(frames_to(r.bus, 2), 4u);
  CHECK_EQ(r.m.remembered().size(), 1u);
  CHECK_EQ(r.m.remembered()[0], 1);
  // Morning: the inverter answers and is discovered at its remembered address.
  inv.mute = false;
  r.run(20000);
  CHECK(r.m.devices()[0].discovered() && r.m.devices()[0].online());
}

// The remembered inverter was re-addressed inside the scan range and, like the SH10RS and the
// SG5.0RS, does not answer the broadcast: the read-only range probe finds it, and the new
// address replaces the silent remembered one.
TEST(search_remembered_inverter_moved_found_by_range_probe) {
  Rig r;
  RememberListener l;
  r.m.set_listener(&l);
  FakeSlave inv;
  make_inverter(inv);
  inv.addr = 6;  // answers_search stays false
  r.bus.slaves = {&inv};
  MasterTiming t;
  t.response_timeout_ms = 200;
  r.m.set_timing(t);
  r.m.set_search(true);
  r.m.set_scan(1, 8, 2);
  r.m.set_remembered({1});
  r.run(20000);
  auto *d = r.m.find_device_by_address(6);
  CHECK(d != nullptr && d->discovered() && d->online());
  CHECK(r.m.find_device_by_address(1)->abandoned());
  CHECK_EQ(r.m.remembered().size(), 1u);
  CHECK_EQ(r.m.remembered()[0], 6);
  CHECK(l.changed >= 2);
  // Read-only: no write frame went out, and no further search once the inverter is online.
  for (auto &w : r.bus.writes)
    CHECK(w.size() >= 2 && (w[1] == 3 || w[1] == 4));
  const size_t searches = search_frames(r.bus), to1 = frames_to(r.bus, 1);
  r.run(2 * ModbusMaster::SEARCH_REPEAT_MS);
  CHECK_EQ(search_frames(r.bus), searches);
  CHECK_EQ(frames_to(r.bus, 1), to1);
}

TEST(search_new_address_replaces_remembered) {
  Rig r;
  RememberListener l;
  r.m.set_listener(&l);
  FakeSlave inv;
  make_inverter(inv);
  inv.addr = 3;
  inv.answers_search = true;
  r.bus.slaves = {&inv};
  MasterTiming t;
  t.response_timeout_ms = 200;
  r.m.set_timing(t);
  r.m.set_search(true);
  r.m.set_remembered({1});
  r.run(10000);
  auto *old = r.m.find_device_by_address(1);
  auto *now3 = r.m.find_device_by_address(3);
  CHECK(old != nullptr && old->abandoned());
  CHECK(now3 != nullptr && now3->discovered() && now3->online());
  CHECK_EQ(r.m.remembered().size(), 1u);
  CHECK_EQ(r.m.remembered()[0], 3);
  CHECK(l.changed >= 2);
  // The abandoned address gets no more frames.
  const size_t to1 = frames_to(r.bus, 1);
  r.run(30000);
  CHECK_EQ(frames_to(r.bus, 1), to1);
}

TEST(search_again_when_inverter_goes_offline) {
  Rig r;
  FakeSlave inv;
  make_inverter(inv);
  inv.addr = 1;
  inv.answers_search = true;
  r.bus.slaves = {&inv};
  MasterTiming t;
  t.response_timeout_ms = 200;
  t.offline_probe_ms = 5000;
  r.m.set_timing(t);
  r.m.set_search(true);
  r.run(10000);
  CHECK_EQ(search_frames(r.bus), 1u);
  CHECK(r.m.devices()[0].online());
  inv.mute = true;
  r.run(20000);
  CHECK(!r.m.devices()[0].online());
  CHECK_EQ(search_frames(r.bus), 3u);  // the offline transition searched again (2 unanswered tries)
}

// scan_fallback_table: a scanned device that answers the devinfo read with a Modbus exception
// (no usable devinfo) still gets a poll table instead of staying stuck in DISCOVER.
TEST(bus_scan_fallback_table) {
  Rig r;
  FakeSlave exc;   // addr 1: rejects the devinfo read, otherwise answers (zeros)
  exc.addr = 1;
  exc.exception_ranges.push_back({DEVINFO_START, static_cast<uint16_t>(DEVINFO_START + 1)});
  r.bus.slaves = {&exc};  // addr 2: silent -> abandoned
  MasterTiming t;
  t.response_timeout_ms = 200;
  t.offline_probe_ms = 1000;
  r.m.set_timing(t);
  r.m.set_scan(1, 2, 2);
  r.m.set_scan_fallback(dm::find_table("inv_proto2"));
  r.run(8000);

  auto *d1 = r.m.find_device_by_address(1);
  auto *d2 = r.m.find_device_by_address(2);
  CHECK(d1 != nullptr && d2 != nullptr);
  // addr 1 answered (exception) -> fallback table activated, not stuck, not abandoned.
  CHECK(!d1->discovered());        // devinfo was rejected
  CHECK(!d1->abandoned());
  CHECK(d1->table() == dm::find_table("inv_proto2"));
  CHECK(d1->state() == Device::State::RUN);
  CHECK(d1->online());
  CHECK(d1->stats().exceptions >= 1u);
  // addr 2 still abandoned (fallback does not keep a silent slot alive).
  CHECK(d2->abandoned());
}

TEST(sbr_block_read_whole) {
  Rig r;
  FakeSlave bat;
  bat.addr = 2;
  r.bus.slaves = {&bat};
  // cell max 3.3 V, min 3.2 V
  bat.set(4, 10757, 33000);
  bat.set(4, 10758, 0x0102);
  bat.set(4, 10759, 32000);
  bat.set(4, 10760, 0x0304);
  DeviceConfig d;
  d.index = 1;
  d.address = 2;
  d.table = dm::find_table("battery");
  d.sbr_cells = true;
  r.m.add_device(d);
  r.m.start();
  // The cell block is one read of its own; no other read overlaps it.
  size_t blocks = 0;
  for (auto &j : r.m.devices()[0].jobs()) {
    const bool block = j.req.start == dm::SBR_CELL_BLOCK_START && j.req.count == dm::SBR_CELL_BLOCK_COUNT;
    blocks += block;
    CHECK(block || j.req.start + j.req.count <= dm::SBR_CELL_BLOCK_START ||
          j.req.start >= dm::SBR_CELL_BLOCK_START + dm::SBR_CELL_BLOCK_COUNT);
  }
  CHECK_EQ(blocks, 1u);
  auto sbr_reads = [&]() {
    size_t n = 0;
    for (auto &w : r.bus.writes)
      n += (w[2] << 8 | w[3]) == dm::SBR_CELL_BLOCK_START - 1 && w[5] == dm::SBR_CELL_BLOCK_COUNT;
    return n;
  };
  r.run(20000);
  CHECK_EQ(sbr_reads(), 1u);  // initial pass, then every 30 s
  CHECK_NEAR(r.store.get_number(1, dm::SIG_CELL_VOLTAGE_SPREAD), 0.1, 1e-6);
  r.run(40000);
  CHECK(sbr_reads() >= 2u && sbr_reads() <= 3u);
}

TEST(plan_sizes) {
  // Request plans for the stock tables stay within the 59-node cap.
  const char *names[] = {"inv_proto1_4", "inv_proto2", "inv_proto3", "inv_proto5", "battery"};
  for (const char *n : names) {
    bool ovf = true;
    auto reqs = dm::build_requests(*dm::find_table(n), dm::RequestBuildOptions(), &ovf);
    std::printf("    %s: %zu requests%s\n", n, reqs.size(), ovf ? " (OVERFLOW)" : "");
    CHECK(!ovf);
  }
}

// ---------------------------------------------------------------------------------------------
// Write core (submit_write): framing, priority, verify, timeout, offline, rate limit, settle.
TEST(write_single_frame_and_echo) {
  uint8_t out[8];
  // EMS mode doc 13050 -> wire 13049 = 0x32F9, value 2 (Forced mode): mkaiser's fn 6 frame.
  CHECK_EQ(build_write_single_request(1, 13049, 2, out), 8u);
  auto want = hex("01 06 32 F9 00 02");
  CHECK(std::memcmp(out, want.data(), 6) == 0);
  CHECK(crc_ok(out, 8));
  ResponseParser p;
  ExpectedReply e;
  e.addr = 1;
  e.fn = FN_WRITE_SINGLE;
  e.wire_start = 13049;
  e.count = 1;
  e.value = 2;
  p.begin(e);
  // A stray echo with another value is skipped; the right echo is accepted.
  uint8_t bad[8];
  build_write_single_request(1, 13049, 3, bad);
  CHECK(p.feed(bad, 8) == ParseStatus::INCOMPLETE);
  CHECK(p.feed(out, 8) == ParseStatus::OK);
  CHECK_EQ(expected_response_len(FN_WRITE_SINGLE, 1), 8u);
}

/// Rig with one fixed-table device at address 1, online after the first poll.
struct WriteRig : Rig {
  FakeSlave inv;
  WriteRig(uint32_t settle = 100, uint32_t rate = 0) {
    inv.addr = 1;
    bus.slaves = {&inv};
    MasterTiming t;
    t.write_settle_ms = settle;
    t.write_rate_limit_ms = rate;
    m.set_timing(t);
    DeviceConfig d;
    d.index = 0;
    d.address = 1;
    d.table = dm::find_table("other_dev18_st6200");
    m.add_device(d);
    m.start();
    run(300);
  }
  size_t find_write(size_t from, uint8_t fn) const {
    for (size_t i = from; i < bus.writes.size(); i++)
      if (bus.writes[i][1] == fn)
        return i;
    return SIZE_MAX;
  }
};

TEST(write_offline_and_invalid_rejected) {
  Rig r;
  FakeSlave inv;
  inv.mute = true;
  r.bus.slaves = {&inv};
  DeviceConfig d;
  d.index = 0;
  d.address = 1;  // table auto: never discovered while mute
  r.m.add_device(d);
  r.m.start();
  r.run(500);
  uint16_t v = 2;
  int calls = 0;
  auto cb = [&](const WriteResult &) { calls++; };
  CHECK(r.m.submit_write(1, 13050, &v, 1, {}, cb) == WriteStatus::OFFLINE);   // undiscovered
  CHECK(r.m.submit_write(9, 13050, &v, 1, {}, cb) == WriteStatus::OFFLINE);   // unknown slave
  CHECK(r.m.submit_write(1, 0, &v, 1, {}, cb) == WriteStatus::INVALID);
  WriteOptions o;
  o.fn = FN_WRITE_SINGLE;
  uint16_t two[2] = {1, 2};
  CHECK(r.m.submit_write(1, 13050, two, 2, o, cb) == WriteStatus::INVALID);  // fn 6 with 2 regs
  CHECK_EQ(calls, 0);
  CHECK_EQ(r.m.write_stats().rejected, 4u);
  for (auto &f : r.bus.writes)
    CHECK(f[1] != 0x06 && f[1] != 0x10);  // nothing written
}

TEST(write_fn6_fn16_and_priority) {
  WriteRig r;
  CHECK(r.m.can_write(1));
  // Queue an ad-hoc read first, then a write: the write still goes out first.
  bool read_done = false;
  CHECK(r.m.read_registers(1, 3, 13050, 3, [&](bool, const uint8_t *, uint16_t) { read_done = true; }));
  WriteResult res1, res2;
  uint16_t v = 2;
  CHECK(r.m.submit_write(1, 13050, &v, 1, {}, [&](const WriteResult &x) { res1 = x; }) == WriteStatus::QUEUED);
  uint16_t vals[3] = {2, 0xAA, 3000};
  CHECK(r.m.submit_write(1, 13050, vals, 3, {}, [&](const WriteResult &x) { res2 = x; }) == WriteStatus::QUEUED);
  size_t before = r.bus.writes.size();
  r.run(1000);
  CHECK(read_done);
  CHECK(res1.status == WriteStatus::OK);
  CHECK_EQ(res1.fn, 6);
  CHECK(res2.status == WriteStatus::OK);
  CHECK_EQ(res2.fn, 16);
  // Frame order after the one already in flight: fn 6, fn 16, then the ad-hoc read.
  size_t i6 = r.find_write(before, 0x06), i16 = r.find_write(before, 0x10);
  CHECK(i6 != SIZE_MAX && i16 != SIZE_MAX && i6 < i16);
  CHECK(i6 <= before + 1);
  size_t iread = SIZE_MAX;
  for (size_t i = before; i < r.bus.writes.size(); i++)
    if (r.bus.writes[i][1] == 3) {
      iread = i;
      break;
    }
  CHECK(iread != SIZE_MAX && iread > i16);
  auto w16 = hex("01 10 32 F9 00 03 06 00 02 00 AA 0B B8");
  CHECK(r.bus.writes[i16].size() == w16.size() + 2 &&
        std::memcmp(r.bus.writes[i16].data(), w16.data(), w16.size()) == 0);
  CHECK_EQ(r.inv.get(3, 13051), 0xAA);
  CHECK_EQ(r.m.write_stats().ok, 2u);
  // Forced fn 16 for a single register.
  WriteOptions o;
  o.fn = FN_WRITE_MULTIPLE;
  WriteResult res3;
  CHECK(r.m.submit_write(1, 13100, &v, 1, o, [&](const WriteResult &x) { res3 = x; }) == WriteStatus::QUEUED);
  r.run(500);
  CHECK(res3.status == WriteStatus::OK && res3.fn == 16);
}

TEST(write_verify_ok_and_mismatch) {
  WriteRig r;
  WriteOptions o;
  o.verify = true;
  WriteResult res;
  uint16_t v = 0xBB;
  CHECK(r.m.submit_write(1, 13051, &v, 1, o, [&](const WriteResult &x) { res = x; }) == WriteStatus::QUEUED);
  size_t before = r.bus.writes.size();
  r.run(1000);
  CHECK(res.status == WriteStatus::OK);
  CHECK_EQ(res.readback.size(), 1u);
  CHECK_EQ(res.readback[0], 0xBB);
  // The verify read (fn 3 of the same range) directly follows the write.
  size_t iw = r.find_write(before, 0x06);
  CHECK(iw != SIZE_MAX && iw + 1 < r.bus.writes.size());
  CHECK_EQ(r.bus.writes[iw + 1][1], 3);
  CHECK_EQ(r.bus.writes[iw + 1][2] << 8 | r.bus.writes[iw + 1][3], 13050);
  // The inverter echoes but keeps the old value.
  r.inv.ignore_writes = true;
  v = 0xAA;
  CHECK(r.m.submit_write(1, 13051, &v, 1, o, [&](const WriteResult &x) { res = x; }) == WriteStatus::QUEUED);
  r.run(1000);
  CHECK(res.status == WriteStatus::VERIFY_MISMATCH);
  CHECK_EQ(res.readback[0], 0xBB);
  CHECK_EQ(r.m.write_stats().verify_mismatch, 1u);
  CHECK_EQ(r.m.write_stats().failed, 1u);
}

TEST(write_exception_and_timeout_no_retry) {
  WriteRig r;
  MasterTiming t = r.m.timing();
  t.retries = 2;  // reads would be retried; writes never are
  r.m.set_timing(t);
  r.inv.write_exception = 2;
  WriteResult res;
  uint16_t v = 9;
  CHECK(r.m.submit_write(1, 13050, &v, 1, {}, [&](const WriteResult &x) { res = x; }) == WriteStatus::QUEUED);
  r.run(500);
  CHECK(res.status == WriteStatus::EXCEPTION);
  CHECK_EQ(res.exception, 2);
  r.inv.write_exception = 0;
  r.inv.mute = true;
  int seen = r.inv.writes_seen;
  size_t before = r.bus.writes.size();
  uint32_t t0 = g_now;
  bool done = false;
  uint32_t t_done = 0;
  CHECK(r.m.submit_write(1, 13050, &v, 1, {}, [&](const WriteResult &x) {
    res = x;
    done = true;
    t_done = g_now;
  }) == WriteStatus::QUEUED);
  while (!done && g_now - t0 < 20000)
    r.run(5);
  CHECK(res.status == WriteStatus::TIMEOUT);
  size_t n6 = 0, i6 = SIZE_MAX;
  for (size_t i = before; i < r.bus.writes.size(); i++)
    if (r.bus.writes[i][1] == 0x06) {
      n6++;
      i6 = i;
    }
  CHECK(i6 != SIZE_MAX);
  if (i6 != SIZE_MAX) {
    CHECK(t_done - r.bus.write_ms[i6] >= 2000);  // stock timeout, counted from the send
    CHECK(t_done - r.bus.write_ms[i6] < 2100);
  }
  CHECK_EQ(n6, 1u);
  CHECK_EQ(r.inv.writes_seen, seen);  // mute: nothing applied
}

TEST(write_rejected_once_offline_while_queued) {
  WriteRig r;
  r.inv.mute = true;
  // 10 lost frames -> offline.
  r.run(30000);
  CHECK(!r.m.devices()[0].online());
  uint16_t v = 1;
  CHECK(r.m.submit_write(1, 13050, &v, 1, {}, nullptr) == WriteStatus::OFFLINE);
}

TEST(write_rate_limit) {
  WriteRig r(100, 1000);
  uint16_t v = 1;
  CHECK(r.m.submit_write(1, 13050, &v, 1, {}, nullptr) == WriteStatus::QUEUED);
  // Same register (inside a range write) within 1 s: rejected; another register: fine.
  uint16_t vv[2] = {0xCC, 0};
  CHECK(r.m.submit_write(1, 13049, vv, 2, {}, nullptr) == WriteStatus::RATE_LIMITED);
  CHECK(r.m.submit_write(1, 13051, vv, 1, {}, nullptr) == WriteStatus::QUEUED);
  r.run(1100);
  CHECK(r.m.submit_write(1, 13050, &v, 1, {}, nullptr) == WriteStatus::QUEUED);
  CHECK_EQ(r.m.write_stats().rejected, 1u);
}

TEST(write_settle_gap) {
  WriteRig r(250);
  uint16_t v = 1;
  CHECK(r.m.submit_write(1, 13050, &v, 1, {}, nullptr) == WriteStatus::QUEUED);
  size_t before = r.bus.writes.size();
  r.run(1500);
  size_t iw = r.find_write(before, 0x06);
  CHECK(iw != SIZE_MAX && iw + 1 < r.bus.writes.size());
  // Reply is in within ~20 ms of the send; the next frame waits >= 250 ms after it.
  CHECK(r.bus.write_ms[iw + 1] - r.bus.write_ms[iw] >= 250);
  // Without settle: just the normal frame gap before the next (queued) write.
  WriteRig q(0);
  before = q.bus.writes.size();
  CHECK(q.m.submit_write(1, 13050, &v, 1, {}, nullptr) == WriteStatus::QUEUED);
  CHECK(q.m.submit_write(1, 13051, &v, 1, {}, nullptr) == WriteStatus::QUEUED);
  q.run(500);
  iw = q.find_write(before, 0x06);
  CHECK(iw != SIZE_MAX && iw + 1 < q.bus.writes.size());
  CHECK(q.bus.write_ms[iw + 1] - q.bus.write_ms[iw] < 100);
}

TEST(write_queue_full) {
  WriteRig r;
  uint16_t v = 1;
  int ok = 0;
  for (int i = 0; i < 12; i++)
    ok += r.m.submit_write(1, static_cast<uint16_t>(13000 + i), &v, 1, {}, nullptr) == WriteStatus::QUEUED;
  CHECK_EQ(ok, (int) ModbusMaster::WRITE_QUEUE_MAX);
  CHECK_EQ(r.m.writes_pending(), ModbusMaster::WRITE_QUEUE_MAX);
}

// Exclusive bus: no cyclic traffic, the owner's writes and ad-hoc reads still go out, raw mode
// silences the master, nothing goes offline, polling resumes on release.
TEST(exclusive_hold_and_raw_mode) {
  WriteRig r;
  CHECK(r.m.devices()[0].online());
  r.run(5000);
  CHECK(r.m.acquire_exclusive(60000));
  CHECK(!r.m.acquire_exclusive(60000));  // one owner
  r.run(200);  // a frame staged before the hold may still go out
  size_t before = r.bus.writes.size();
  r.run(20000);
  CHECK_EQ(r.bus.writes.size(), before);  // no polls while held
  // The owner's write and ad-hoc read are served.
  const uint16_t v = 7;
  bool wrote = false;
  CHECK(r.m.submit_write(1, 40000, &v, 1, WriteOptions{}, [&](const WriteResult &w) { wrote = w.status == WriteStatus::OK; }) ==
        WriteStatus::QUEUED);
  bool read = false;
  CHECK(r.m.read_registers(1, 3, 40000, 1, [&](bool ok, const uint8_t *, uint16_t) { read = ok; }));
  r.run(1000);
  CHECK(wrote);
  CHECK(read);
  CHECK_EQ(r.bus.writes.size(), before + 2);
  // Raw mode: the master sends nothing even with queued work, and does not read the transport.
  CHECK(r.m.idle());
  CHECK(r.m.set_raw(true));
  CHECK(r.m.read_registers(1, 3, 40000, 1, nullptr));
  r.bus.rx.push_back(0x43);
  r.run(2000);
  CHECK_EQ(r.bus.writes.size(), before + 2);
  CHECK_EQ(r.bus.rx.size(), 1u);  // left for the owner
  CHECK(r.m.set_raw(false));
  CHECK(r.bus.rx.empty());  // leaving raw mode discards leftovers
  r.m.release_exclusive();
  r.run(5000);
  CHECK(r.bus.writes.size() > before + 3);  // the queued read and the polls again
  CHECK(r.m.devices()[0].online());
}

TEST(exclusive_raw_refused_unless_held_and_idle) {
  WriteRig r;
  CHECK(!r.m.set_raw(true));  // not held
  CHECK(r.m.acquire_exclusive(60000));
  const uint16_t v = 1;
  CHECK(r.m.submit_write(1, 40000, &v, 1, WriteOptions{}, nullptr) == WriteStatus::QUEUED);
  r.m.tick();  // picked / in flight
  CHECK(!r.m.set_raw(true));
  r.run(500);
  CHECK(r.m.set_raw(true));
  r.m.release_exclusive();
  CHECK(!r.m.raw());
}

TEST(exclusive_auto_release_after_limit) {
  WriteRig r;
  CHECK(r.m.acquire_exclusive(3000));
  r.run(2000);
  r.m.extend_exclusive(3000);
  r.run(2500);
  CHECK(r.m.exclusive());
  CHECK(r.m.set_raw(true));
  r.run(1000);
  CHECK(!r.m.exclusive());
  CHECK(!r.m.raw());
  size_t before = r.bus.writes.size();
  r.run(5000);
  CHECK(r.bus.writes.size() > before);
}

TEST(master_wants_fast_loop) {
  Rig r;
  FakeSlave a;
  r.bus.slaves = {&a};
  DeviceConfig d;
  d.table = dm::find_table("inv_proto5");
  r.m.add_device(d);
  r.m.start();
  CHECK(!r.m.wants_fast_loop());  // idle: nothing sent yet
  r.m.tick();
  CHECK(r.bus.writes.size() == 1u);
  CHECK(r.m.wants_fast_loop());  // a frame is out, the reply is awaited
  // The reply lands within a few ticks; from then until the next frame (gap) nothing is pending.
  int ticks = 0;
  while (r.m.wants_fast_loop() && ticks < 20) {
    g_now += 5;
    r.m.tick();
    ticks++;
  }
  CHECK(!r.m.wants_fast_loop());
  CHECK(r.bus.writes.size() == 1u);
  g_now += 5;
  r.m.tick();  // still inside the 18 ms frame gap
  CHECK(!r.m.wants_fast_loop());
  CHECK(r.bus.writes.size() == 1u);
  // A silent slave: the timeout wait counts, and so does the retry waiting for its gap.
  Rig rr;
  FakeSlave mute;
  mute.mute = true;
  rr.bus.slaves = {&mute};
  MasterTiming t;
  t.retries = 1;
  rr.m.set_timing(t);
  rr.m.add_device(d);
  rr.m.start();
  rr.m.tick();
  CHECK(rr.bus.writes.size() == 1u);
  // From the send through the timeout and the retry's frame gap to the retry: fast throughout.
  bool all_fast = true;
  int ticks2 = 0;
  while (rr.bus.writes.size() < 2 && ticks2 < 400) {
    all_fast = all_fast && rr.m.wants_fast_loop();
    g_now += 5;
    rr.m.tick();
    ticks2++;
  }
  CHECK(rr.bus.writes.size() == 2u);
  CHECK(all_fast);
  // A queued write asks for the fast loop at once; once it is done and the settle gap has passed
  // the request drops until the next cyclic poll.
  WriteRig w;
  while (w.m.wants_fast_loop())
    w.run(5);
  uint16_t v = 1;
  CHECK(w.m.submit_write(1, 40000, &v, 1, WriteOptions{}, nullptr) == WriteStatus::QUEUED);
  CHECK(w.m.wants_fast_loop());
  size_t n0 = w.bus.writes.size();
  w.run(5);  // the write goes out first
  CHECK(w.find_write(n0, 0x06) != SIZE_MAX);
  // Echo, then the 100 ms settle gap: the request stays up.
  int settle_fast = 0;
  for (int i = 0; i < 15; i++) {
    w.run(5);
    settle_fast += w.m.wants_fast_loop();
  }
  CHECK(settle_fast == 15);
  int saw_idle = 0;
  for (int i = 0; i < 100 && !saw_idle; i++) {
    w.run(5);
    saw_idle += !w.m.wants_fast_loop();
  }
  CHECK(saw_idle == 1);
  // A pending ad-hoc read counts too.
  while (w.m.wants_fast_loop())
    w.run(5);
  CHECK(w.m.read_registers(1, 3, 40000, 1, nullptr));
  CHECK(w.m.wants_fast_loop());
}

int main() { return th::run_all(); }
