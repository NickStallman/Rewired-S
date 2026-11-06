// Host tests for components/sungrow_analytics/analytics_core.h.
#include <string>
#include <vector>

#include "../test_harness.h"
#include "analytics_core.h"

using namespace esphome::sungrow_analytics;
namespace dm = esphome::sungrow_datamodel;

static bool contains(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }
static size_t count_of(const std::string &s, const char *needle) {
  size_t n = 0, pos = 0;
  while ((pos = s.find(needle, pos)) != std::string::npos) {
    n++;
    pos += std::strlen(needle);
  }
  return n;
}

TEST(hex_and_json_escape) {
  const uint8_t d[4] = {0x00, 0xAB, 0x7f, 0xff};
  CHECK(hex_lower(d, 4) == "00ab7fff");
  std::string out;
  json_escape_into(out, "a\"b\\c\nd\x01");
  CHECK(out == "a\\\"b\\\\c\\nd\\u0001");
  CHECK(std::string(reset_reason_code(1)) == "POWERON");
  CHECK(std::string(reset_reason_code(6)) == "TASK_WDT");
  CHECK(std::string(reset_reason_code(99)) == "UNKNOWN");
}

TEST(roles_versions_triggers) {
  CHECK(std::string(role_name(dm::Family::ENERGY_BRIDGE)) == "bridge");
  CHECK(std::string(role_name(dm::Family::INVERTER)) == "inverter");
  const VersionSig *v = nullptr;
  CHECK_EQ(version_sigids(dm::Family::INVERTER, &v), 5u);
  CHECK_EQ(v[0].sigid, 0x2008);
  CHECK_EQ(version_sigids(dm::Family::BATTERY, &v), 2u);
  CHECK_EQ(v[0].sigid, 0x2016);
  CHECK_EQ(version_sigids(dm::Family::METER, &v), 0u);
  CHECK(v == nullptr);
  CHECK(trigger_known("manual"));
  CHECK(trigger_known("device"));
  CHECK(!trigger_known("other"));
  CHECK(!trigger_known(nullptr));
}

static PingProps sample_ping() {
  PingProps p;
  p.version = "0.9.0";
  p.esphome_version = "2026.9.1";
  p.stock_version = "WINET-SV200.001.00.P046";
  p.board = "winet_s";
  p.chip = "esp32";
  p.network = "wifi";
  p.uptime_s = 86412;
  p.reset_reason = "POWERON";
  p.cloud_uplink = true;
  p.mqtt = true;
  p.modbus_tcp = false;
  p.control_writes = false;
  p.psram_kb = 4096;
  p.heap_min_kb = 61;
  return p;
}

TEST(batch_shape_and_size) {
  PingProps p = sample_ping();
  std::vector<DeviceEvent> devs;
  DeviceEvent inv;
  inv.model = "SH10RS";
  inv.role = "inverter";
  inv.dev_type = 35;
  inv.protocol = 2;
  inv.dev_code = 0x0D1B;
  inv.inv_type_known = true;
  inv.inv_type = 0;
  inv.versions = {{"arm_version", "SUNSTONE-H_03021.01.16"}, {"dsp_version", "SUNSTONE-H_03021.01.16"}};
  devs.push_back(inv);
  for (int i = 0; i < 2; i++) {
    DeviceEvent b;
    b.model = "SBR192";
    b.role = "battery";
    b.dev_type = 44;
    b.dev_code = 0x20EA;
    b.versions = {{"firmware", "SBRBCU-S_22011.01.08"}};
    devs.push_back(b);
  }
  DeviceEvent m;
  m.model = "DTSU666";
  m.role = "meter";
  devs.push_back(m);
  std::vector<ErrorEvent> errs;
  errs.push_back({"crash", "LoadProhibited", "", 1});
  const std::string id(64, 'a');
  std::string b = build_batch("phc_fw_key", id, "2026-11-02T04:12:09Z", &p, devs, errs);
  CHECK(b.size() < 2048);
  CHECK(b.front() == '{' && b.back() == '}');
  CHECK(contains(b, "\"api_key\":\"phc_fw_key\""));
  CHECK(contains(b, "\"sent_at\":\"2026-11-02T04:12:09Z\""));
  CHECK_EQ(count_of(b, "\"event\":\"ping\""), 1u);
  CHECK_EQ(count_of(b, "\"event\":\"device_registered\""), 4u);
  CHECK_EQ(count_of(b, "\"event\":\"error\""), 1u);
  CHECK_EQ(count_of(b, ("\"distinct_id\":\"" + id + "\"").c_str()), 6u);
  CHECK_EQ(count_of(b, "\"timestamp\":\"2026-11-02T04:12:09Z\""), 6u);
  CHECK(contains(b, "\"winetVersion\":4"));
  CHECK(contains(b, "\"winetCount\":1"));
  CHECK(contains(b, "\"device\":\"SH10RS\",\"role\":\"inverter\",\"dev_type\":35,\"protocol\":2,\"dev_code\":3355,\"inv_type\":0"));
  CHECK(contains(b, "\"arm_version\":\"SUNSTONE-H_03021.01.16\""));
  CHECK(contains(b, "\"type\":\"crash\",\"error\":\"LoadProhibited\",\"count\":1,\"version\":\"0.9.0\""));
  CHECK(contains(b, "\"cloud_uplink\":true,\"mqtt\":true,\"modbus_tcp\":false,\"control_writes\":false"));
  CHECK(contains(b, "\"heap_min_kb\":61"));
  // No clock: no sent_at and no timestamps at all.
  std::string c = build_batch("k", id, "", &p, devs, errs);
  CHECK(!contains(c, "sent_at"));
  CHECK(!contains(c, "timestamp"));
  // Error-only batch carries the version from nowhere (empty) and no ping.
  std::string e = build_batch("k", id, "", nullptr, devs, errs);
  CHECK(!contains(e, "\"event\":\"ping\""));
  CHECK(!contains(e, "device_registered"));
  CHECK_EQ(count_of(e, "\"event\":\"error\""), 1u);
}

TEST(error_throttle) {
  ErrorThrottle t;  // 1 h
  CHECK_EQ(t.report("a", 1000), 1u);
  CHECK_EQ(t.report("a", 2000), 0u);
  CHECK_EQ(t.report("a", 3000), 0u);
  CHECK_EQ(t.report("b", 3000), 1u);
  CHECK_EQ(t.report("a", 1000 + 3600000 - 1), 0u);
  CHECK_EQ(t.report("a", 1000 + 3600000), 4u);  // three suppressed plus this one
  CHECK_EQ(t.report("a", 1000 + 3600000 + 10), 0u);
  CHECK_EQ(t.size(), 2u);
  // Wraps around millis() rollover.
  ErrorThrottle w;
  CHECK_EQ(w.report("x", 0xFFFFFF00u), 1u);
  CHECK_EQ(w.report("x", 100), 0u);
  CHECK_EQ(w.report("x", 3600000u + 100), 2u);
  // Bounded.
  ErrorThrottle m;
  for (int i = 0; i < 40; i++)
    m.report("k" + std::to_string(i), 0);
  CHECK_EQ(m.size(), ErrorThrottle::MAX_KEYS);
}

TEST(schedule) {
  Schedule s;
  CHECK(!s.due(1000000, true));  // not armed
  s.arm(12345, 1000);
  const uint32_t first = s.next_due_ms - 1000;
  CHECK(first >= 60000u && first < 120000u);
  Schedule s2;
  s2.arm(12345 + 60000, 1000);
  CHECK_EQ(s2.next_due_ms, s.next_due_ms);  // the spread is modulo 60 s
  CHECK(!s.due(s.next_due_ms - 1, true));
  CHECK(s.due(s.next_due_ms, true));
  // Without a valid clock the ping waits up to 5 min, then goes anyway.
  CHECK(!s.due(s.next_due_ms, false));
  CHECK(!s.due(s.next_due_ms + Schedule::TIME_WAIT_MS - 1, false));
  CHECK(s.due(s.next_due_ms + Schedule::TIME_WAIT_MS, false));
  s.sent(s.next_due_ms + 5);
  CHECK(!s.due(s.next_due_ms - 1, true));
  CHECK_EQ(s.next_due_ms - (5 + first + 1000), Schedule::PERIOD_MS);
  // A due ping across the millis() rollover.
  Schedule r;
  r.arm(0, 0xFFFFFFFFu - 30000);
  CHECK(!r.due(0xFFFFFFFFu - 1, true));
  CHECK(r.due(60000, true));
}

TEST(upload_cap) {
  UploadCap c;
  // No clock: three uploads, then nothing until a clock appears and a day passes.
  CHECK(c.allow(0));
  c.record(0);
  c.record(0);
  CHECK(c.allow(0));
  c.record(0);
  CHECK(!c.allow(0));
  CHECK_EQ(c.window_start_epoch, 0u);
  // Clock appears: the window starts now; still capped.
  CHECK(!c.allow(1'700'000'000u));
  CHECK_EQ(c.window_start_epoch, 1'700'000'000u);
  CHECK(!c.allow(1'700'000'000u + UploadCap::WINDOW_S - 1));
  CHECK(c.allow(1'700'000'000u + UploadCap::WINDOW_S));
  CHECK_EQ(c.uploads, 0);
  c.record(1'700'000'000u + UploadCap::WINDOW_S + 5);
  CHECK_EQ(c.uploads, 1);
  CHECK_EQ(c.window_start_epoch, 1'700'000'000u + UploadCap::WINDOW_S);
  // A clock that goes away again keeps the count.
  CHECK(c.allow(0));
  CHECK_EQ(c.uploads, 1);
}

TEST(scrubber_text) {
  Scrubber s;
  s.add("A2101234567", "<sn>");
  s.add("B2101234567", "<serial>");
  s.add("abc", "<short>");  // ignored
  s.add("my-net 5G", "<ssid>");
  s.add("house-inverter", "<host>");
  s.add("hunter2!", "<psk>");
  s.add("", "<mqtt-password>");  // no password stored: ignored
  CHECK_EQ(s.size(), 5u);
  std::string in =
      "Serial number: A2101234567 and B2101234567\n"
      "WiFi SSID 'my-net 5G' IP Address: 192.0.2.4 gateway 10.0.0.1\n"
      "Rewired-S 0.9.0 on house-inverter, stock WINET-SV200.001.00.P046, idf 5.4.2 at 12:30:45\n"
      "ipv4 1.2.3.4. and 255.255.255.0:1883 but not 1.2.3.4.5 nor 1234.5.6.7\n"
      "Password='hunter2!' and hunter2!hunter2!\n";
  std::string out = s.apply(in);
  CHECK(contains(out, "Password='<psk>' and <psk><psk>"));
  CHECK(contains(out, "Serial number: <sn> and <serial>"));
  CHECK(contains(out, "SSID '<ssid>' IP Address: <ip> gateway <ip>"));
  CHECK(contains(out, "Rewired-S 0.9.0 on <host>, stock WINET-SV200.001.00.P046, idf 5.4.2 at 12:30:45"));
  CHECK(contains(out, "ipv4 <ip>. and <ip>:1883 but not 1.2.3.4.5 nor 1234.5.6.7"));
  CHECK(!contains(out, "A2101234567"));
  CHECK(Scrubber::mask_ipv4("0.0.0.0") == "<ip>");
  CHECK(Scrubber::mask_ipv4("v0.0.0.0") == "v<ip>");
  CHECK(Scrubber::mask_ipv4("a.1.2.3.4") == "a.1.2.3.4");
  CHECK(Scrubber::mask_ipv4("") == "");
}

TEST(scrubber_bytes) {
  Scrubber s;
  s.add("A2101234567", "<sn>");
  // A devinfo-style block: model then the serial as big-endian register bytes.
  std::vector<uint8_t> raw;
  auto put = [&](const char *s, size_t pad_to) {
    for (; *s; s++)
      raw.push_back(static_cast<uint8_t>(*s));
    while (raw.size() < pad_to)
      raw.push_back(0);
  };
  put("SH10RS", 10);
  std::string dump;
  hex_dump_into(dump, raw.data(), raw.size(), 2515);
  CHECK(dump == "   2515: 5348 3130 5253 0000 0000  |SH10RS....|\n");
  put("A2101234567", 26);
  raw.push_back('A');  // a partial match at the end is left alone
  s.apply_bytes(raw.data(), raw.size());
  std::string after(raw.begin(), raw.end());
  CHECK(contains(after, "SH10RS"));
  CHECK(!contains(after, "A2101234567"));
  CHECK(contains(after, "***********"));
  CHECK(after.back() == 'A');
  dump.clear();
  hex_dump_into(dump, raw.data(), raw.size(), 2515);
  CHECK(contains(dump, "   2515: 5348 3130 5253 0000 0000 2a2a 2a2a 2a2a  |SH10RS....******|\n"));
  CHECK(contains(dump, "   2523: 2a2a 2a2a 2a00 0000 0000 41  |*****.....A|\n"));
}

TEST(report_sections) {
  BundleBuild b;
  b.project_version = "0.9.0";
  b.esphome_version = "2026.9.1";
  b.idf_version = "v5.4.2";
  b.build_id = "0x12345678 2026-11-01 10:00:00";
  b.board = "winet_s2";
  b.chip = "esp32s3";
  b.network = "ethernet";
  b.stock_version = "WINET-SV300.001.03.P032";
  b.flash_kb = 16384;
  b.psram_kb = 8192;
  b.boot_count = 12;
  b.uptime_s = 90;
  b.reset_reason = "SW";
  b.last_reset = "Software restart after 100 s";
  std::string out;
  write_headline(out, b);
  CHECK(out == "Rewired-S 0.9.0 on winet_s2 (esp32s3, ethernet), uptime 90 s, reset SW\n");
  write_build(out, b);
  CHECK(contains(out, "== Build ==\nfirmware: Rewired-S 0.9.0\nesphome: 2026.9.1, idf: v5.4.2\nbuild: 0x12345678"));
  CHECK(contains(out, "boot: #12, uptime 90 s, reset reason SW\nlast reset: Software restart after 100 s\n"));

  BundleFeatures f;
  f.analytics_enabled = true;
  f.cloud_present = true;
  f.cloud_enabled = true;
  f.cloud_error = true;
  f.cloud_failures = 3;
  f.mqtt_present = true;
  f.mqtt_connected = true;
  f.control_present = true;
  f.writes_ok = 7;
  f.capture_present = true;
  f.capture_running = true;
  f.capture_kind = "iv";
  out.clear();
  write_features(out, f);
  CHECK(contains(out, "cloud uplink: enabled, not connected, error, connect failures 3\n"));
  CHECK(contains(out, "mqtt: connected\nmodbus tcp: not built\n"));
  CHECK(contains(out, "inverter writes: disabled, ok 7, failed 0, rejected 0, verify mismatch 0\n"));
  CHECK(contains(out, "captures: running iv\n"));

  BundleMemory m;
  m.heap_free = 100000;
  m.heap_min = 61440;
  m.psram_total = 8 * 1024 * 1024;
  m.psram_free = 7 * 1024 * 1024;
  out.clear();
  write_memory(out, m);
  CHECK(contains(out, "internal heap: 100000 free, 61440 minimum\npsram: 8192 KB total, 7168 KB free\n"));

  BundleDevice d;
  d.index = 0;
  d.address = 1;
  d.state = "RUN";
  d.dev_code = 0x0D1B;
  d.model = "SH10RS";
  d.proto_code = "AA10";
  d.tag = "";
  d.protocol = 2;
  d.dev_type = 35;
  d.dev_type_source = "devinfo";
  d.inv_type_known = true;
  d.inv_type = 0;
  d.table = "inv_proto2";
  d.family = "inverter";
  d.xml_family = "unit";
  d.has_descriptor = true;
  d.country_reg = 32310;
  d.grid_reg = 32311;
  d.power_switch_reg = 13000;
  d.identified = d.discovered = d.online = d.first_round_done = true;
  d.ident_raw = {0x0D, 0x1B, 'S', 'H', '1', '0'};
  d.ident_start = 2515;
  d.ident_fn = 4;
  d.sent = 1000;
  d.received = 998;
  d.timeouts = 2;
  d.loss_pct = 0.2f;
  BundleJob j;
  j.fn = 4;
  j.start = 5000;
  j.count = 39;
  j.tier = "fast";
  j.effective_tier = "fast";
  j.last_ok_age_s = 1;
  d.jobs.push_back(j);
  j.start = 5601;
  j.count = 8;
  j.tier = "live";
  j.effective_tier = "static";
  j.unserved = true;
  j.last_exc = 2;
  j.exc_count = 5;
  j.last_ok_age_s = -1;
  d.jobs.push_back(j);
  d.static_values = {{"rated_power", "10000"}, {"serial_number", "A2101234567"}};
  d.versions = {{"arm_version", "SUNSTONE-H_03021.01.16"}};
  out.clear();
  write_device_support(out, d);
  CHECK(contains(out, "-- device 0: SH10RS (inverter, addr 1) --\n"));
  CHECK(contains(out, "state: RUN, online, discovered, first poll round done\n"));
  CHECK(contains(out, "identity: dev_code 0x0D1B, model \"SH10RS\", protocol code \"AA10\", tag \"\", protocol type 2, dev_type 35 (devinfo)\n"));
  CHECK(contains(out, "inverter type (30644): 0 (0x0000)\n"));
  CHECK(contains(out, "table: inv_proto2, xml family: unit, descriptor: yes\n"));
  CHECK(contains(out, "descriptor registers: country 32310, grid code 32311, power switch 13000\n"));
  CHECK(contains(out, "version arm_version: SUNSTONE-H_03021.01.16\n"));
  CHECK(contains(out, "link: sent 1000, received 998, timeouts 2, crc errors 0, exceptions 0, loss 0.2 %\n"));
  CHECK(contains(out, "identification reply: fn 4, 3 registers from 2515\n   2515: 0d1b 5348 3130  |..SH10|\n"));
  CHECK(contains(out, "register blocks (2):\n  fn 4  5000+39  fast   served exc 0 (last 0), data 1 s ago\n"));
  CHECK(contains(out, "  fn 4  5601+8   live   unserved (polled static) exc 5 (last 2), data never\n"));
  CHECK(contains(out, "static values (2):\n  rated_power = 10000\n  serial_number = A2101234567\n"));
  // The scrubber runs over the whole report afterwards.
  Scrubber s;
  s.add("A2101234567", "<serial>");
  CHECK(contains(s.apply(out), "serial_number = <serial>\n"));
  // A device without a table or identification reply.
  BundleDevice u;
  u.index = 3;
  u.address = 200;
  u.state = "DISCOVER";
  u.child_kind = "SBR";
  u.parent = 0;
  u.dev_type_source = "none";
  u.xml_family = "none";
  out.clear();
  write_device_support(out, u);
  CHECK(contains(out, "-- device 3: (no model) (no table, addr 200) --\nstate: DISCOVER, offline, unidentified\n"));
  CHECK(contains(out, "reported by: device 0 as SBR\ninverter type (30644): unknown\ntable: (none), xml family: none, descriptor: none\n"));
  CHECK(contains(out, "identification reply: none kept\nregister blocks (0):\nstatic values (0):\n"));
}

int main() { return th::run_all(); }
