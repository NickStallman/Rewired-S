// Host unit tests for the isolarcloud core (no ESPHome). Build/run: make -C tests/isolarcloud
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "isolarcloud/cloud_time.h"
#include "isolarcloud/cloud_types.h"
#include "isolarcloud/commands.h"
#include "isolarcloud/envelope.h"
#include "isolarcloud/inbound.h"
#include "isolarcloud/json_compat.h"
#include "isolarcloud/mqtt_point_table.h"
#include "isolarcloud/publish_queue.h"
#include "isolarcloud/rundata_format.h"
#include "isolarcloud/scheduler.h"
#include "isolarcloud/server_switch.h"
#include "isolarcloud/topics.h"
#include "isolarcloud/zcodec.h"

using namespace esphome::isolarcloud;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (c) {                                                             \
      g_pass++;                                                          \
    } else {                                                             \
      g_fail++;                                                          \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    }                                                                    \
  } while (0)
#define CHECK_EQ(a, b)                                                                              \
  do {                                                                                              \
    auto _a = (a);                                                                                  \
    auto _b = (b);                                                                                  \
    if (_a == _b) {                                                                                 \
      g_pass++;                                                                                     \
    } else {                                                                                        \
      g_fail++;                                                                                     \
      fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s\n  got:  %s\n  want: %s\n", __FILE__, __LINE__, #a, #b, \
              to_s(_a).c_str(), to_s(_b).c_str());                                                  \
    }                                                                                               \
  } while (0)

static std::string to_s(const std::string &s) { return s; }
static std::string to_s(const char *s) { return s; }
template<typename T> static std::string to_s(T v) { return std::to_string(v); }

static const std::string SN = "B2101234567";
static const std::string T0 = "20261018123456";
static PublicData sample_pd(const std::string &time = T0) {
  PublicData pd;
  pd.product_type = 0x720;
  pd.product_name = "WiNet-S2";
  pd.sn = SN;
  pd.send_time = time;
  return pd;
}
static std::vector<uint8_t> bytes(const std::string &s) { return {s.begin(), s.end()}; }

// ---- fake telemetry ------------------------------------------------------------------------
static TelemetrySignal sig_int(uint16_t id, uint64_t bits, uint8_t len, uint8_t type, bool valid = true) {
  TelemetrySignal s;
  s.sigid = id;
  s.valid = valid;
  s.bits = bits;
  s.len = len;
  s.type = type;
  return s;
}
static TelemetrySignal sig_str(uint16_t id, const std::string &raw) {
  TelemetrySignal s;
  s.sigid = id;
  s.valid = true;
  s.is_string = true;
  s.type = 12;
  s.len = static_cast<uint8_t>(raw.size());
  s.bytes = raw;
  s.str = raw.substr(0, raw.find('\0'));
  return s;
}
static TelemetrySignal sig_float(uint16_t id, float v) {
  TelemetrySignal s;
  s.sigid = id;
  s.valid = true;
  s.is_float = true;
  s.type = 0x80;
  s.len = 4;
  s.value = v;
  return s;
}

class FakeTelemetry : public TelemetrySource {
 public:
  std::vector<TelemetryDevice> devs;
  std::map<uint8_t, std::map<uint16_t, TelemetrySignal>> sigs;
  void add(uint8_t idx, const TelemetrySignal &s) { sigs[idx][s.sigid] = s; }
  void list_devices(std::vector<TelemetryDevice> &out) const override { out = devs; }
  void for_each_signal(uint8_t idx, const std::function<void(const TelemetrySignal &)> &fn) const override {
    auto it = sigs.find(idx);
    if (it != sigs.end())
      for (auto &kv : it->second)
        fn(kv.second);
  }
  bool get_signal(uint8_t idx, uint16_t sigid, TelemetrySignal &out) const override {
    auto it = sigs.find(idx);
    if (it == sigs.end())
      return false;
    auto jt = it->second.find(sigid);
    if (jt == it->second.end())
      return false;
    out = jt->second;
    return true;
  }
};

static TelemetryDevice dev(uint8_t idx, uint16_t code, uint16_t table, bool online = true) {
  TelemetryDevice d;
  d.index = idx;
  d.dev_code = code;
  d.table_type = table;
  d.dev_type = cloud_dev_type_for_table(table);
  d.online = online;
  return d;
}

// ---------------------------------------------------------------------------------------------
static void test_topics() {
  TopicBuilder tb(SN, {0, 0, 1});
  CHECK_EQ(TopicBuilder::enum_suffix({0, 0, 1}), "enum_0_0_1");
  CHECK_EQ(tb.pub("beatdata"), "isolarcloud/devicedata/pub/beatdata/B2101234567/enum_0_0_1");
  CHECK_EQ(tb.pub("rundata/5min"), "isolarcloud/devicedata/pub/rundata/5min/B2101234567/enum_0_0_1");
  CHECK_EQ(tb.rsp("setparam"), "isolarcloud/devicedata/rsp/setparam/B2101234567/enum_0_0_1");
  CHECK_EQ(tb.rsp("upgrade", {2, 3, 0}), "isolarcloud/devicedata/rsp/upgrade/B2101234567/enum_2_3_0");
  CHECK_EQ(tb.cmd("getparam"), "isolarcloud/devicedata/cmd/B2101234567/getparam/enum_0_0_1");
  CHECK_EQ(tb.will(), "isolarcloud/devicedata/pub/willdata/B2101234567/enum_0_0_1");
  CHECK_EQ(TopicBuilder::group_cmd(8), "isolarcloud/devicedata/cmd/8/enum_0_0_0");

  // Remote-control mask 0x4FF: bit1 master + groups 1..7 -> every family; settimezone always.
  auto subs = tb.subscriptions(DEFAULT_REMOTE_CTL_MASK);
  CHECK_EQ(subs.size(), (size_t) 14);
  CHECK_EQ(subs[0], "isolarcloud/devicedata/cmd/B2101234567/settimezone/enum_0_0_1");
  CHECK(std::find(subs.begin(), subs.end(), tb.cmd("getparam")) != subs.end());
  // Master bit off: only settimezone.
  CHECK_EQ(tb.subscriptions(0x4FD).size(), (size_t) 1);
  // Only groups 1 (apidata, fcas, propertydata, generalcmd) and 2 (set/get param): 1 + 4 + 4.
  subs = tb.subscriptions(0x06);
  CHECK_EQ(subs.size(), (size_t) 9);
  CHECK(std::find(subs.begin(), subs.end(), tb.cmd("upgrade")) == subs.end());

  CHECK(remote_ctl_allows(0x4FF, "10325"));
  CHECK(!remote_ctl_allows(0x4FB, "10325"));  // bit 2 off
  CHECK(remote_ctl_allows(0x4FB, "10310"));   // group 1
  CHECK(remote_ctl_allows(0, "10324"));       // settimezone: in no group
  CHECK(remote_ctl_allows(0, "10383"));

  ParsedTopic p;
  CHECK(parse_topic(tb.cmd("generalcmd"), p));
  CHECK(p.kind == ParsedTopic::Kind::CMD);
  CHECK_EQ(p.sn, SN);
  CHECK_EQ(p.family, "generalcmd");
  CHECK(p.has_enum);
  CHECK_EQ(p.topic_enum.compress, 1);

  CHECK(parse_topic("isolarcloud/devicedata/cmd/8/enum_0_0_0", p));
  CHECK(p.kind == ParsedTopic::Kind::GROUP_CMD);
  CHECK_EQ(p.sn, "8");
  CHECK(p.has_enum && p.topic_enum.compress == 0);

  CHECK(parse_topic(tb.pub("faultdata/history"), p));
  CHECK(p.kind == ParsedTopic::Kind::PUB);
  CHECK_EQ(p.family, "faultdata/history");

  CHECK(parse_topic("isolarcloud/devicedata/cmd/X1/setparam/enum_1_x_1", p));
  CHECK(!p.has_enum);
  CHECK(!parse_topic("other/devicedata/cmd/X1/setparam/enum_0_0_1", p));
}

// ---------------------------------------------------------------------------------------------
static void test_zlib() {
  std::mt19937 rng(42);
  for (size_t len : {0ul, 1ul, 100ul, 2561ul, 40000ul, 200000ul}) {
    std::vector<uint8_t> in(len);
    for (size_t i = 0; i < len; i++)
      in[i] = (i & 1) ? static_cast<uint8_t>(rng()) : static_cast<uint8_t>('a' + i % 7);
    std::vector<uint8_t> z, back;
    CHECK(zlib_compress(in.data(), in.size(), z));
    CHECK(looks_like_zlib(z.data(), z.size()));
    CHECK_EQ(z[0], 0x78);  // RFC1950, 32K window, deflate
    CHECK(zlib_inflate(z.data(), z.size(), back, 1 << 20));
    CHECK(back == in);
  }
  // Corrupt / truncated / oversize
  std::string text(5000, 'x');
  std::vector<uint8_t> z, back;
  CHECK(zlib_compress(reinterpret_cast<const uint8_t *>(text.data()), text.size(), z));
  std::vector<uint8_t> bad = z;
  bad[bad.size() - 1] ^= 0xFF;  // Adler-32
  CHECK(!zlib_inflate(bad.data(), bad.size(), back));
  CHECK(!zlib_inflate(z.data(), z.size() / 2, back));
  CHECK(!zlib_inflate(z.data(), z.size(), back, 100));
  CHECK(zlib_inflate(z.data(), z.size(), back, 5000));
  // Stored-block fallback (no compressor state): valid zlib for any inflater, incl. >64K blocks.
  for (size_t len : {0ul, 5ul, 65535ul, 65536ul, 140000ul}) {
    std::vector<uint8_t> in(len);
    for (size_t i = 0; i < len; i++)
      in[i] = static_cast<uint8_t>(rng());
    std::vector<uint8_t> st;
    zlib_store(in.data(), in.size(), st);
    CHECK(looks_like_zlib(st.data(), st.size()));
    CHECK(zlib_inflate(st.data(), st.size(), back, 1 << 20));
    CHECK(back == in);
  }
  const uint8_t junk[] = {'{', '"', 'a', '"'};
  CHECK(!looks_like_zlib(junk, sizeof(junk)));

  // encode_payload: compressed form includes the trailing NUL (stock strlen+1), plain doesn't.
  std::string json = "{\"a\":1}";
  std::vector<uint8_t> enc;
  CHECK(encode_payload(json, 0, enc));
  CHECK(enc == bytes(json));
  CHECK(encode_payload(json, 1, enc));
  CHECK(zlib_inflate(enc.data(), enc.size(), back));
  CHECK_EQ(back.size(), json.size() + 1);
  CHECK_EQ(back.back(), 0);
  std::string dec;
  CHECK(decode_payload(enc.data(), enc.size(), 1, dec));
  CHECK_EQ(dec, json);
  CHECK(decode_payload(enc.data(), enc.size(), 0, dec));  // compress=0: passthrough
  CHECK(!decode_payload(reinterpret_cast<const uint8_t *>(json.data()), json.size(), 1, dec));
}

// ---------------------------------------------------------------------------------------------
static void test_time() {
  int32_t off = 0;
  CHECK(parse_cloud_timezone("UTC+8:00", off) && off == 8 * 3600);
  CHECK(parse_cloud_timezone("UTC+10:00", off) && off == 10 * 3600);
  CHECK(parse_cloud_timezone("UTC-03:30", off) && off == -(3 * 3600 + 1800));
  CHECK(parse_cloud_timezone("UTC", off) && off == 0);
  CHECK(parse_cloud_timezone("UTC+5", off) && off == 5 * 3600);
  CHECK(!parse_cloud_timezone("UTC+15:00", off));
  CHECK(!parse_cloud_timezone("CET", off));
  CHECK(!parse_cloud_timezone("UTC+8:0", off));
  CHECK(settimezone_string_ok("UTC+10:00"));
  CHECK(!settimezone_string_ok("UTC+8:00"));  // 8 chars: stock wants exactly 9
  CHECK(!settimezone_string_ok("GMT+10:00"));

  // 2026-10-18 04:30:00 UTC = 1792297800.
  CivilTime u = civil_from_epoch(1792297800);
  CHECK(u.year == 2026 && u.month == 10 && u.day == 18 && u.hour == 4 && u.minute == 30 && u.second == 0);
  CHECK_EQ(epoch_from_civil(u), (int64_t) 1792297800);
  // Local: UTC + offset (cloud "UTC+10:00" = 10 h ahead), crossing midnight / month / year.
  CHECK_EQ(format_cloud_time(local_from_utc(1792297800, 10 * 3600)), "20261018143000");
  CHECK_EQ(format_cloud_time(local_from_utc(1792297800, -5 * 3600)), "20261017233000");
  CivilTime nye{2026, 12, 31, 23, 59, 59};
  CHECK_EQ(format_cloud_time(local_from_utc(epoch_from_civil(nye), 3600)), "20270101005959");
  CivilTime leap{2028, 2, 28, 23, 0, 0};
  CHECK_EQ(format_cloud_time(local_from_utc(epoch_from_civil(leap), 3600)), "20280229000000");
  CHECK_EQ(format_send_time(2026, 1, 2, 3, 4, 5), "20260102030405");

  CivilTime t;
  CHECK(parse_cloud_time("20261018123000", t) && t.hour == 12 && t.minute == 30 && t.day == 18);
  CHECK(!parse_cloud_time("2026-10-18 12:30", t));
  CHECK(!parse_cloud_time("20260230120000", t));  // Feb 30
  CHECK(clock_plausible(CivilTime{2026, 1, 1, 0, 0, 0}));
  CHECK(!clock_plausible(CivilTime{1970, 1, 1, 0, 0, 0}));
}

// ---------------------------------------------------------------------------------------------
static void test_envelope_and_will() {
  CHECK_EQ(make_message_id(SN, T0, 7), "B2101234567_20261018123456_00007");
  MessageCounter mc;
  CHECK_EQ(mc.next(), 1);
  CHECK_EQ(mc.next(), 2);
  for (int i = 0; i < 65533; i++)
    mc.next();
  CHECK_EQ(mc.next(), 1);  // 65535 -> skips 0

  // cloud_identity.md will payload.
  const std::string want =
      "{\"messageId\":\"B2101234567_20261018123456_00001\",\"packgeCount\":\"1\",\"packgeId\":\"1\","
      "\"publicData\":{\"productType\":\"1824\",\"productName\":\"WiNet-S2\",\"productSN\":\"B2101234567\","
      "\"sendTime\":\"20261018123456\"},"
      "\"businessData\":[{\"devCode\":247,\"devType\":22,\"beat\":0}]}";
  std::string will = build_will_json(sample_pd(), make_message_id(SN, T0, 1));
  CHECK_EQ(will, want);
  PublicData pd_s = sample_pd();
  pd_s.product_type = 0x715;
  CHECK(build_will_json(pd_s, "m").find("\"productType\":\"1813\"") != std::string::npos);
  pd_s.product_type = 7;
  CHECK(build_will_json(pd_s, "m").find("\"productType\":\"0007\"") != std::string::npos);
  // Stock reads the will length as one byte (C40): the default zlib'd will fits even with a
  // 20-char SN; the plain one fits for our 11-char SN (a 20-char plain will would not: compress 0
  // is not a stock default).
  PublicData pd20 = sample_pd();
  pd20.sn = "ABCDEFGHIJKLMNOPQRST";
  std::string w20 = build_will_json(pd20, make_message_id(pd20.sn, T0, 65535));
  std::vector<uint8_t> enc;
  CHECK(encode_payload(w20, 1, enc));
  CHECK(enc.size() <= MAX_WILL_PAYLOAD);
  CHECK(encode_payload(will, 0, enc));
  CHECK(enc.size() <= MAX_WILL_PAYLOAD);

  auto one = build_envelopes(sample_pd(), "mid", "[{\"x\":1}]");
  CHECK_EQ(one.size(), (size_t) 1);
  JsonDocument d;
  CHECK(deserializeJson(d, one[0]) == DeserializationError::Ok);
  CHECK(d["businessData"].is<JsonArray>());

  std::string exact(CHUNK_SIZE, 'a');
  CHECK_EQ(build_envelopes(sample_pd(), "m", exact).size(), (size_t) 1);
  CHECK_EQ(build_envelopes(sample_pd(), "m", exact + "b").size(), (size_t) 2);

  std::string big = "[";
  for (int i = 0; i < 400; i++)
    big += std::string(i ? "," : "") + "{\"devCode\":" + std::to_string(i) + ",\"data\":{\"5003\":\"12.5\"}}";
  big += "]";
  auto parts = build_envelopes(sample_pd(), "B2101234567_t_00042", big);
  size_t want_n = (big.size() + CHUNK_SIZE - 1) / CHUNK_SIZE;
  CHECK_EQ(parts.size(), want_n);
  std::string joined;
  for (size_t i = 0; i < parts.size(); i++) {
    JsonDocument p;
    CHECK(deserializeJson(p, parts[i]) == DeserializationError::Ok);
    CHECK_EQ(std::string(p["packgeId"].as<const char *>()), std::to_string(i + 1));
    CHECK(p["businessData"].is<const char *>());
    joined += p["businessData"].as<const char *>();
  }
  CHECK_EQ(joined, big);
  CHECK(parts[0].find("{\"messageId\":") == 0);
  CHECK(parts[0].find("\"publicData\"") < parts[0].find("\"businessData\""));
}

// ---------------------------------------------------------------------------------------------
static void test_silence_devtype_backoff() {
  CHECK(!in_silence_window(false, 22, 6, 23));
  CHECK(in_silence_window(true, 22, 6, 23));
  CHECK(in_silence_window(true, 22, 6, 0));
  CHECK(in_silence_window(true, 22, 6, 5));
  CHECK(!in_silence_window(true, 22, 6, 6));
  CHECK(!in_silence_window(true, 22, 6, 21));
  CHECK(in_silence_window(true, 1, 5, 1));
  CHECK(!in_silence_window(true, 1, 5, 5));
  CHECK(!in_silence_window(true, 1, 5, 0));
  // start == end with enable on: silenced all day.
  for (uint8_t h = 0; h < 24; h++)
    CHECK(in_silence_window(true, 3, 3, h));
  CHECK(!in_silence_window(false, 3, 3, 3));

  // table type -> cloud devType (cloud_outbound.md C7.1).
  CHECK_EQ(cloud_dev_type_for_table(35), 14);
  CHECK_EQ(cloud_dev_type_for_table(21), 1);
  CHECK_EQ(cloud_dev_type_for_table(44), 43);
  CHECK_EQ(cloud_dev_type_for_table(18), 7);
  CHECK_EQ(cloud_dev_type_for_table(34), 46);
  CHECK_EQ(cloud_dev_type_for_table(6), 7);
  CHECK_EQ(cloud_dev_type_for_table(0x2e), 51);
  CHECK_EQ(cloud_dev_type_for_table(1), 1);
  CHECK_EQ(cloud_dev_type_for_table(2), 22);
  CHECK_EQ(cloud_dev_type_for_table(0x5c), 92);
  CHECK_EQ(cloud_dev_type_for_table(0x77), 0);
  // The generated tables agree with the map.
  for (size_t i = 0; i < NUM_MQTT_TABLES; i++)
    CHECK_EQ(kMqttTables[i].cloud_dev_type, cloud_dev_type_for_table(kMqttTables[i].table_type));
  CHECK(find_mqtt_table(0x15) != nullptr && find_mqtt_table(0x15)->points == find_mqtt_table(0x01)->points);

  // ssl flag: 0 TCP, 2 TLS, else invalid; port 19999 forces TCP.
  ServerEndpoint e;
  CHECK(e.transport() == Transport::TLS);
  e.ssl_flag = 1;
  CHECK(e.transport() == Transport::INVALID);
  e.ssl_flag = 0;
  CHECK(e.transport() == Transport::TCP);
  e.ssl_flag = 2;
  e.port = 19999;
  CHECK(e.transport() == Transport::TCP);

  Backoff b(5000, 300000);
  CHECK_EQ(b.next(), 5000u);
  CHECK_EQ(b.next(), 10000u);
  for (int i = 0; i < 10; i++)
    b.next();
  CHECK_EQ(b.next(), 300000u);
}

// ---------------------------------------------------------------------------------------------
static void test_point_format() {
  const MqttTable *t = find_mqtt_table(0x15);
  CHECK(t != nullptr);
  auto point = [&](uint16_t sig) -> const MqttPoint * {
    for (uint16_t i = 0; i < t->count; i++)
      if (t->points[i].sigid == sig && (t->points[i].mask & MQTT_MASK_RUNDATA))
        return &t->points[i];
    return nullptr;
  };
  std::string v;
  // 0x2020 -> "1": u16 daily yield, fmt 9 x100 (raw 123 = 12.3 kWh in 0.1 kWh -> 12300).
  CHECK(point(0x2020) && point(0x2020)->point_id == 1);
  CHECK(format_point_value(*point(0x2020), sig_int(0x2020, 123, 2, 4), v) && v == "12300");
  // 0x2021 -> "2": stock u64, fmt 10 x100, "%.19g"; our u32 raw widened.
  CHECK(point(0x2021) && point(0x2021)->point_id == 2);
  CHECK(format_point_value(*point(0x2021), sig_int(0x2021, 4000000000u, 4, 6), v));
  CHECK_EQ(v, "400000000000");
  // 0x2031 -> "4": s16 x0.1.
  CHECK(format_point_value(*point(0x2031), sig_int(0x2031, 0xFFFB, 2, 3), v) && v == "-0.5");
  // 0x2080 -> point 24 (total active power): s32 raw, no scale.
  CHECK(format_point_value(*point(0x2080), sig_int(0x2080, 0xFFFFFB2E, 4, 5), v) && v == "-1234");
  // fmt 9 really goes through float32: 2^24 + 1 loses its last bit (len 4 -> "%.13g").
  CHECK(format_point_value(*point(0x2083), sig_int(0x2083, 16777217, 4, 6), v) && v == "167772.16");
  // Sentinels by the stock sig type: u16 0xffff, s32 0x7fffffff; our s16 into a stock s32 widens.
  CHECK(!format_point_value(*point(0x2040), sig_int(0x2040, 0xFFFF, 2, 4), v));
  CHECK(!format_point_value(*point(0x2080), sig_int(0x2080, 0x7FFFFFFF, 4, 5), v));
  CHECK(format_point_value(*point(0x2080), sig_int(0x2080, 0xFFFF, 2, 3), v) && v == "-1");
  CHECK(!format_point_value(*point(0x2020), sig_int(0x2020, 1, 2, 4, false), v));  // invalid
  // String (fmt 12, sig 0x0002 -> "206"): raw bytes up to the first NUL; all-NUL = sentinel.
  CHECK(point(0x0002) && point(0x0002)->point_id == 206);
  CHECK(format_point_value(*point(0x0002), sig_str(0x0002, std::string("A2201234\0\0  ", 12)), v) && v == "A2201234");
  CHECK(!format_point_value(*point(0x0002), sig_str(0x0002, std::string(20, '\0')), v));
  // Hex (fmt 101) on an attribute: 0x231b (40 B) -> lowercase hex of every DM byte (zero padded).
  const MqttPoint *hexp = nullptr;
  for (uint16_t i = 0; i < t->count; i++)
    if (t->points[i].sigid == 0x231b)
      hexp = &t->points[i];
  CHECK(hexp && hexp->fmt == 101 && hexp->size == 40);
  CHECK(format_point_value(*hexp, sig_str(0x231b, std::string("\x01\xab", 2)), v));
  CHECK_EQ(v, "01ab" + std::string(76, '0'));
  // Meter (table 0x12, stock "f32?" type 9): our wire float32 is used as the value.
  const MqttTable *m = find_mqtt_table(0x12);
  const MqttPoint *mp = nullptr;
  for (uint16_t i = 0; i < m->count; i++)
    if (m->points[i].sigid == 0x2023)
      mp = &m->points[i];
  CHECK(mp && mp->fmt == 9 && mp->sig_type == 9);
  CHECK(format_point_value(*mp, sig_float(0x2023, 1.5f), v) && v == "1500");
}

static void test_formatter() {
  FakeTelemetry t;
  t.devs = {dev(0, 1, 0x15), dev(1, 2, 0x15, false), dev(2, 3, 0x23)};
  t.add(0, sig_int(0x2020, 123, 2, 4));
  t.add(0, sig_int(0x2021, 4000000000u, 4, 6));
  t.add(0, sig_int(0x2031, 0xFFFB, 2, 3));
  t.add(0, sig_int(0x2040, 0xFFFF, 2, 4));  // sentinel: dropped
  t.add(0, sig_str(0x0002, std::string("A2201234\0\0", 10)));
  t.add(0, sig_int(0x9999, 5, 2, 4));  // not in the table: never sent
  BusinessFormatter f;
  std::string j = f.rundata(t.devs[0], &t, "20261018120000");
  CHECK_EQ(j,
           "[{\"devCode\":1,\"devType\":1,\"dataTime\":\"20261018120000\",\"data\":{\"206\":\"A2201234\",\"1\":\"12300\","
           "\"2\":\"400000000000\",\"4\":\"-0.5\"}}]");
  // All signals invalid / none: the element is still sent with "data":{}.
  CHECK_EQ(f.rundata(t.devs[2], &t, "20261018120000"),
           "[{\"devCode\":3,\"devType\":14,\"dataTime\":\"20261018120000\",\"data\":{}}]");
  CHECK_EQ(f.rundata(dev(0, 9, 0x77), &t, "x"), "");  // unknown table type
  // Logger element: devCode 247 / devType 22, its 10 rundata points (none known -> {}).
  LoggerSignals ls;
  CHECK_EQ(f.rundata(logger_device(), &ls, "20261018120000"),
           "[{\"devCode\":247,\"devType\":22,\"dataTime\":\"20261018120000\",\"data\":{}}]");
  ls.set_u16(0x1130, 3);
  ls.set_s16(0x1134, -61);
  CHECK(f.rundata(logger_device(), &ls, "t").find("\"23001\":\"3\",\"23014\":\"-61\"") != std::string::npos);

  // SBR battery under the hybrid: upDevType/upDevCode from sigs 0x2022/0x2028, both keys always
  // present (0 while the sigs are unreadable).
  TelemetryDevice bat = dev(3, 200, 0x2c);
  CHECK(f.rundata(bat, &t, "t").find("{\"devCode\":200,\"devType\":43,\"upDevType\":0,\"upDevCode\":0,\"dataTime\"") == 1);
  t.add(3, sig_int(0x2022, 14, 2, 4));
  t.add(3, sig_int(0x2028, 1, 2, 4));
  CHECK(f.rundata(bat, &t, "t").find("{\"devCode\":200,\"devType\":43,\"upDevType\":14,\"upDevCode\":1,\"dataTime\"") == 1);

  // beatdata: one element per device, logger first (beat 1), offline devices beat 0.
  CHECK_EQ(f.beat(t.devs), "[{\"devCode\":247,\"devType\":22,\"beat\":1},{\"devCode\":1,\"devType\":1,\"beat\":1},"
                           "{\"devCode\":2,\"devType\":1,\"beat\":0},{\"devCode\":3,\"devType\":14,\"beat\":1}]");
  CHECK_EQ(f.beat({}), "[{\"devCode\":247,\"devType\":22,\"beat\":1}]");

  // devinfo (logger only, C7.4).
  DevinfoContext ctx;
  ctx.data_time = "20261018120000";
  ctx.old_server.host = "old.example";
  ctx.old_server.port = 1883;
  CHECK_EQ(f.devinfo(ctx),
           "[{\"batchId\":\"999999999999999\",\"devCode\":247,\"devType\":22,\"dataTime\":\"20261018120000\","
           "\"data\":{\"10461\":{\"10479\":2,\"10480\":1,\"10481\":1},"
           "\"38338\":\"iot.isolarcloud.com;16668;Sun#0*2Pow925Mdk\",\"38337\":\"old.example;1883;Sun#0*2Pow925Mdk\","
           "\"38781\":\"0000000000000000\",\"5767\":\"\",\"5768\":\"11111110000000000000000000000000\"}}]");

  // propertydata (C7.5): continuityPoint, attrs, functionEnum, cmdType 10310, identity strings.
  t.add(2, sig_str(0x2181, std::string("\x01", 1)));  // capability bitmap -> hybrid point 13400
  t.add(2, sig_str(0x0001, "SH10RT"));
  t.add(2, sig_str(0x0002, "A2201234"));
  t.add(2, sig_int(0x0004, 0xE03, 2, 4));
  t.add(2, sig_str(0x5423, "SAPPHIRE-H_01011.95.07"));
  t.add(2, sig_int(0x2191, 0x0102030405060708ull, 8, 8));
  t.add(2, sig_int(0x2001, 7, 2, 4));
  std::string pdj = f.propertydata(t.devs[2], &t, "20261018120000", UNSOLICITED_BATCH_ID);
  JsonDocument d;
  CHECK(deserializeJson(d, pdj) == DeserializationError::Ok);
  JsonObject o = d[0];
  CHECK_EQ(std::string(o["batchId"].as<const char *>()), "999999999999999");
  CHECK_EQ(o["devType"].as<int>(), 14);
  CHECK_EQ(std::string(o["data"]["continuityPoint"][0].as<const char *>()), "13400");
  CHECK_EQ(o["data"]["continuityPoint"].size(), (size_t) 1);
  CHECK_EQ(std::string(o["data"]["13264"].as<const char *>()), "7");  // 0x2001 on the hybrid table
  CHECK_EQ(std::string(o["functionEnum"].as<const char *>()), "08070605040302010000000000000000");
  CHECK_EQ(std::string(o["cmdType"].as<const char *>()), "10310");
  CHECK_EQ(std::string(o["devName"].as<const char *>()), "SH10RT");
  CHECK_EQ(std::string(o["sn"].as<const char *>()), "A2201234");
  CHECK_EQ(o["devModel"].as<int>(), 0xE03);
  CHECK_EQ(std::string(o["devVersion"].as<const char *>()), "SAPPHIRE-H_01011.95.07");
  CHECK_EQ(o["result"].as<int>(), 1);
  CHECK(pdj.find("\"batchId\"") < pdj.find("\"devCode\"") && pdj.find("\"data\"") < pdj.find("\"functionEnum\""));
  // Logger attrs: the stock "-1":"1824" quirk (0x0004 has point id -1).
  ls.set_u16(0x0004, 1824);
  ls.set_bytes(0x0104, std::string("\x07\x10", 2));
  std::string lj = f.propertydata(logger_device(), &ls, "t", UNSOLICITED_BATCH_ID);
  CHECK(lj.find("\"-1\":\"1824\"") != std::string::npos);
  CHECK(lj.find("\"functionEnum\":\"07100000000000000000000000000000\"") != std::string::npos);
  CHECK(lj.find("continuityPoint") == std::string::npos);  // no continuity sig on the logger
  // The full stock bitmaps (cloud_types.h), embedded NULs kept.
  ls.set_bytes(0x0104, function_enum_bytes(false));
  lj = f.propertydata(logger_device(), &ls, "t", UNSOLICITED_BATCH_ID);
  CHECK(lj.find("\"functionEnum\":\"07100000d7f1c427b065200000100600\"") != std::string::npos);
  CHECK(function_enum_bytes(true) == std::string("\x07\x00\x00\x00\xd7\xf1\x44\x21\x00\x41\x00\x00\x00\x00\x06\x00", 16));

  // Devices: missing 0x2191 / 0x219d fall back to the stock default per half.
  BusinessFormatter fd;
  fd.set_device_function_enum(function_enum_bytes(false));
  JsonDocument d2;
  CHECK(deserializeJson(d2, fd.propertydata(t.devs[2], &t, "t", UNSOLICITED_BATCH_ID)) == DeserializationError::Ok);
  CHECK_EQ(std::string(d2[0]["functionEnum"].as<const char *>()), "0807060504030201b065200000100600");
  CHECK(deserializeJson(d2, fd.propertydata(t.devs[0], &t, "t", UNSOLICITED_BATCH_ID)) == DeserializationError::Ok);
  CHECK_EQ(std::string(d2[0]["functionEnum"].as<const char *>()), "07100000d7f1c427b065200000100600");
  // SBR (devType 43): bit 108 (byte 13, 0x10) follows bit 4 of the first big-endian word of the
  // 8-byte 0x202c block (4 input registers), cleared when unreadable.
  CHECK(deserializeJson(d2, fd.propertydata(bat, &t, "t", UNSOLICITED_BATCH_ID)) == DeserializationError::Ok);
  CHECK_EQ(std::string(d2[0]["functionEnum"].as<const char *>()), "07100000d7f1c427b065200000000600");
  t.add(bat.index, sig_str(0x202c, std::string("\x00\x10\x00\x00\x00\x00\x00\x00", 8)));
  CHECK(deserializeJson(d2, fd.propertydata(bat, &t, "t", UNSOLICITED_BATCH_ID)) == DeserializationError::Ok);
  CHECK_EQ(std::string(d2[0]["functionEnum"].as<const char *>()), "07100000d7f1c427b065200000100600");
  t.add(bat.index, sig_str(0x202c, std::string("\x10\x00\x00\x10\x00\x00\x00\x00", 8)));  // bit 4 clear
  CHECK(deserializeJson(d2, fd.propertydata(bat, &t, "t", UNSOLICITED_BATCH_ID)) == DeserializationError::Ok);
  CHECK_EQ(std::string(d2[0]["functionEnum"].as<const char *>()), "07100000d7f1c427b065200000000600");
  // A meter's table has no functionEnum: all zeros on the WiNet-S2, no element on the WiNet-S.
  TelemetryDevice meter = dev(4, 4, 0x12);
  t.add(4, sig_str(0x0001, "DTSU666"));
  CHECK(!table_has_function_enum(0x12) && table_has_function_enum(0x23) && table_has_function_enum(0x15));
  CHECK(deserializeJson(d2, fd.propertydata(meter, &t, "t", UNSOLICITED_BATCH_ID)) == DeserializationError::Ok);
  CHECK_EQ(std::string(d2[0]["functionEnum"].as<const char *>()), "00000000000000000000000000000000");
  CHECK_EQ(d2[0]["devType"].as<int>(), 7);
  // A subordinate logger (cloud devType 46) reports the logger's bitmap.
  CHECK(deserializeJson(d2, fd.propertydata(dev(5, 5, 0x22), &t, "t", UNSOLICITED_BATCH_ID)) == DeserializationError::Ok);
  CHECK_EQ(d2[0]["devType"].as<int>(), 46);
  CHECK_EQ(std::string(d2[0]["functionEnum"].as<const char *>()), "07100000d7f1c427b065200000100600");
  BusinessFormatter fs;
  fs.set_device_function_enum(function_enum_bytes(true));
  fs.set_winet_s(true);
  CHECK_EQ(fs.propertydata(meter, &t, "t", UNSOLICITED_BATCH_ID), "");
  CHECK(deserializeJson(d2, fs.propertydata(t.devs[0], &t, "t", UNSOLICITED_BATCH_ID)) == DeserializationError::Ok);
  CHECK_EQ(std::string(d2[0]["functionEnum"].as<const char *>()), "07000000d7f144210041000000000600");
}

// ---------------------------------------------------------------------------------------------
struct SchedRun {
  CloudScheduler s;
  int64_t base;
  std::vector<std::pair<uint32_t, SchedulerActions>> log;
  explicit SchedRun(uint32_t delay, int64_t base_epoch, SchedulerTiming timing = {})
      : s(delay, timing), base(base_epoch) {}
  void run(uint32_t from, uint32_t to, bool link, bool silenced = false, uint16_t period = 5, bool clock = true) {
    for (uint32_t t = from; t < to; t++) {
      SchedulerInput in;
      in.mono_s = t;
      in.clock_valid = clock;
      in.local = civil_from_epoch(base + t);
      in.link_up = link;
      in.silenced = silenced;
      in.period_min = period;
      SchedulerActions a = s.tick(in);
      if (a.any())
        log.emplace_back(t, a);
    }
  }
};

static void test_scheduler() {
  // Boot at local 2026-10-18 11:58:30; link up at t=5.
  const int64_t base = epoch_from_civil(CivilTime{2026, 10, 18, 11, 58, 30});
  SchedRun r(100, base);
  r.run(0, 5, false);
  CHECK(r.log.empty());  // no link: the 12:00 boundary is still ahead
  r.run(5, 2000, true);
  // first connect: 30 s after the link came up (t=5..34 counting), dataTime = now.
  CHECK(r.log.size() >= 4);
  CHECK_EQ(r.log[0].first, 35u);
  CHECK(r.log[0].second.rundata && r.log[0].second.first_connect);
  CHECK_EQ(format_cloud_time(r.log[0].second.data_time), "20261018115905");
  CHECK(!r.log[0].second.beat);  // no beat on connect
  // devinfo on the next tick.
  CHECK_EQ(r.log[1].first, 36u);
  CHECK(r.log[1].second.devinfo && !r.log[1].second.rundata);
  // 12:00:00 boundary at t=90, + 100 s countdown -> t=190, dataTime with seconds 0.
  CHECK_EQ(r.log[2].first, 190u);
  CHECK(r.log[2].second.rundata && !r.log[2].second.first_connect);
  CHECK_EQ(format_cloud_time(r.log[2].second.data_time), "20261018120000");
  // propertydata of every device 180 ticks after the first connect.
  CHECK_EQ(r.log[3].first, 215u);
  CHECK(r.log[3].second.propertydata_all);
  // Next boundaries every 5 min: 12:05 -> 490.
  CHECK_EQ(r.log[4].first, 490u);
  CHECK_EQ(format_cloud_time(r.log[4].second.data_time), "20261018120500");
  // Beat: 1800 s after boot (the first tick), not before.
  bool beat_seen = false;
  for (auto &e : r.log) {
    if (e.second.beat) {
      CHECK_EQ(e.first, 1800u);
      beat_seen = true;
    }
  }
  CHECK(beat_seen);
  // Exactly one first-connect burst per boot even after a reconnect.
  r.log.clear();
  r.run(2000, 2100, false);
  r.run(2100, 2200, true);
  for (auto &e : r.log)
    CHECK(!e.second.first_connect && !e.second.devinfo);

  // Period: forced to 5 for the first 1800 s, 0 -> 5.
  SchedulerInput in;
  in.period_min = 1;
  in.mono_s = 100;
  CHECK_EQ(r.s.effective_period(in), 5);
  in.mono_s = 1800;
  CHECK_EQ(r.s.effective_period(in), 1);
  in.period_min = 0;
  CHECK_EQ(r.s.effective_period(in), 5);
  in.period_min = 15;
  CHECK_EQ(r.s.effective_period(in), 15);

  // Silence suppresses everything (beats included), counters do not advance.
  SchedRun q(60, base);
  q.run(0, 4000, true, true);
  CHECK(q.log.empty());
  q.run(4000, 4031, true, false);  // leaving the window: the 30 s hold runs now
  CHECK_EQ(q.log.size(), (size_t) 1);
  CHECK(q.log.size() == 1 && q.log[0].second.first_connect && q.log[0].first == 4030u);
  CHECK(q.log.size() == 1 && q.log[0].second.beat);  // the overdue beat fires once the window ends

  // No valid clock: nothing.
  SchedRun c(60, base);
  c.run(0, 3000, true, false, 5, false);
  CHECK(c.log.empty());

  // Logger attrs every 86400 s.
  SchedRun l(60, base);
  l.run(0, 86401, true);
  int n_attr = 0;
  for (auto &e : l.log)
    n_attr += e.second.logger_attr ? 1 : 0;
  CHECK_EQ(n_attr, 1);

  CHECK_EQ(stock_random_delay(0), 60u);
  CHECK_EQ(stock_random_delay(119), 179u);
  CHECK_EQ(stock_random_delay(120), 60u);
}

// ---------------------------------------------------------------------------------------------
class FakePub : public MqttPublisher {
 public:
  struct Pub {
    std::string topic;
    std::vector<uint8_t> payload;
    int qos;
    bool retain;
    int id;
  };
  std::vector<Pub> sent;
  int next_id{100};
  bool fail{false};
  int publish(const std::string &topic, const std::vector<uint8_t> &payload, int qos, bool retain) override {
    if (this->fail)
      return -1;
    int id = qos == 0 ? 0 : this->next_id++;
    this->sent.push_back({topic, payload, qos, retain, id});
    return id;
  }
};

static OutMessage msg(const std::string &topic, int chunks, int qos = 1) {
  OutMessage m;
  m.topic = topic;
  m.label = topic;
  m.qos = qos;
  for (int i = 0; i < chunks; i++)
    m.chunks.push_back(bytes(topic + "#" + std::to_string(i + 1)));
  return m;
}
static std::string payload(const FakePub::Pub &p) { return {p.payload.begin(), p.payload.end()}; }

static void test_publish_queue() {
  FakePub pub;
  PublishQueue q(8, 10000, 2);
  int done_ok = 0, done_fail = 0;
  q.set_done_callback([&](const OutMessage &, bool ok) { (ok ? done_ok : done_fail)++; });
  CHECK(q.push(msg("A", 3)));
  CHECK(q.push(msg("B", 1)));
  q.tick(pub, 0);
  CHECK_EQ(pub.sent.size(), (size_t) 1);  // only chunk 1 goes out
  CHECK_EQ(payload(pub.sent[0]), "A#1");
  q.tick(pub, 100);
  CHECK_EQ(pub.sent.size(), (size_t) 1);  // still waiting for the PUBACK
  q.on_puback(999, 150);                  // someone else's ack
  q.tick(pub, 200);
  CHECK_EQ(pub.sent.size(), (size_t) 1);
  q.on_puback(pub.sent[0].id, 300);
  q.tick(pub, 300);
  CHECK_EQ(pub.sent.size(), (size_t) 2);
  CHECK_EQ(payload(pub.sent[1]), "A#2");
  q.on_puback(pub.sent[1].id, 400);
  q.tick(pub, 400);
  CHECK_EQ(payload(pub.sent[2]), "A#3");
  CHECK_EQ(pub.sent.size(), (size_t) 3);
  // The next message (= next device) only after the last chunk's ack.
  q.on_puback(pub.sent[2].id, 500);
  q.tick(pub, 500);
  CHECK_EQ(pub.sent.size(), (size_t) 4);
  CHECK_EQ(payload(pub.sent[3]), "B#1");
  CHECK_EQ(done_ok, 1);
  q.on_puback(pub.sent[3].id, 600);
  q.tick(pub, 600);
  CHECK(q.idle());
  CHECK_EQ(done_ok, 2);
  for (auto &p : pub.sent)
    CHECK(p.qos == 1 && !p.retain);

  // No ack: re-send the same chunk after the timeout, max_retries times, then drop the message.
  pub.sent.clear();
  q.push(msg("C", 2));
  q.push(msg("D", 1));
  q.tick(pub, 1000);
  q.tick(pub, 10999);
  CHECK_EQ(pub.sent.size(), (size_t) 1);
  q.tick(pub, 11000);
  CHECK_EQ(pub.sent.size(), (size_t) 2);
  CHECK_EQ(payload(pub.sent[1]), "C#1");
  q.tick(pub, 21000);
  CHECK_EQ(pub.sent.size(), (size_t) 3);
  CHECK_EQ(q.retries(), 2u);
  q.tick(pub, 31000);  // gave up on C: D goes out
  CHECK_EQ(done_fail, 1);
  CHECK_EQ(payload(pub.sent.back()), "D#1");
  // A late ack for an earlier attempt of the current chunk does not count; the current id does.
  q.on_puback(pub.sent.back().id, 31100);
  q.tick(pub, 31100);
  CHECK(q.idle());

  // Client refusal (-1) is retried like a missing ack.
  pub.sent.clear();
  pub.fail = true;
  q.push(msg("E", 1));
  q.tick(pub, 40000);
  pub.fail = false;
  q.tick(pub, 50000);
  CHECK_EQ(pub.sent.size(), (size_t) 1);
  q.on_puback(pub.sent[0].id, 50001);
  q.tick(pub, 50001);
  CHECK(q.idle());

  // Retained clear (empty, QoS1, retain) and QoS 0 complete; clear() drops everything.
  OutMessage rc;
  rc.topic = "cmd";
  rc.chunks.emplace_back();
  rc.retain = true;
  q.push(rc);
  q.tick(pub, 60000);
  CHECK(pub.sent.back().retain && pub.sent.back().payload.empty());
  q.clear();
  CHECK(q.idle() && !q.awaiting_ack());
  q.push(msg("F", 2, 0));
  q.tick(pub, 70000);
  CHECK(q.idle());
  CHECK_EQ(payload(pub.sent.back()), "F#2");

  // Bounded.
  PublishQueue small(2);
  CHECK(small.push(msg("a", 1)) && small.push(msg("b", 1)));
  CHECK(!small.push(msg("c", 1)));
  CHECK(!small.push(OutMessage{}));

  // Byte cap: periodic rundata is evicted first, oldest first; control traffic is kept; the
  // message being sent is never evicted; telemetry is refused while control traffic fills the cap.
  auto sized = [](const std::string &label, size_t n, bool droppable) {
    OutMessage m;
    m.topic = label;
    m.label = label;
    m.droppable = droppable;
    m.chunks.emplace_back(n, 'x');
    return m;
  };
  FakePub bp;
  PublishQueue capq(32, 10000, 2, 1000);
  CHECK(capq.push(sized("r1", 300, true)));
  capq.tick(bp, 0);  // r1 in flight
  CHECK(capq.push(sized("r2", 300, true)));
  CHECK(capq.push(sized("dev", 200, false)));
  CHECK_EQ(capq.bytes(), (size_t) 800);
  CHECK(capq.push(sized("r3", 300, true)));  // evicts r2 (oldest droppable, not in flight)
  CHECK_EQ(capq.last_push_evicted(), (size_t) 1);
  CHECK_EQ(capq.size(), (size_t) 3);
  CHECK_EQ(capq.bytes(), (size_t) 800);
  CHECK(capq.push(sized("beat", 400, false)));  // evicts r3; r1 is busy, dev is kept
  CHECK_EQ(capq.size(), (size_t) 3);           // r1, dev, beat
  CHECK_EQ(capq.bytes(), (size_t) 900);
  CHECK(!capq.push(sized("r4", 300, true)));   // only control traffic left: telemetry waits
  CHECK_EQ(capq.evicted(), 2u);
  // Drain: bytes go back to zero; one message bigger than the cap still fits an empty queue.
  for (int i = 0; i < 3; i++) {
    capq.on_puback(bp.sent.back().id, 100 + i);
    capq.tick(bp, 100 + i);
  }
  CHECK(capq.idle());
  CHECK_EQ(capq.bytes(), (size_t) 0);
  CHECK(capq.push(sized("huge", 5000, true)));
  capq.clear();
  CHECK_EQ(capq.bytes(), (size_t) 0);
}

// ---------------------------------------------------------------------------------------------
// devinfo timing per variant: the stock S2 sends devinfo after
// the first-connect rundata and on server-switch confirm, stock WiNet-S only on confirm. 38781 is
// zeros in every devinfo on both variants (our choice; stock WiNet-S sends its link-failure mask).
static void test_devinfo_timing() {
  const int64_t base = epoch_from_civil(CivilTime{2026, 10, 18, 11, 58, 30});
  BusinessFormatter f;
  for (bool on_connect : {true, false}) {  // true = winet_s2 (auto), false = winet_s (auto)
    SchedulerTiming timing;
    CHECK(timing.devinfo_on_connect);  // default (also `auto` without a sungrow_board)
    timing.devinfo_on_connect = on_connect;
    SchedRun r(100, base, timing);
    r.run(0, 5, false);
    r.run(5, 400, true);
    // The first-connect burst itself is the same on both variants.
    CHECK(!r.log.empty() && r.log[0].first == 35u && r.log[0].second.rundata && r.log[0].second.first_connect);
    CHECK(!r.log[0].second.devinfo);
    int n_devinfo = 0;
    for (auto &e : r.log) {
      if (e.second.devinfo) {
        n_devinfo++;
        CHECK_EQ(e.first, 36u);  // the tick after the first-connect rundata
      }
    }
    CHECK_EQ(n_devinfo, on_connect ? 1 : 0);
    // Rest of the schedule unchanged: 12:00 rundata at 190, propertydata of all at 215.
    bool rd190 = false, pd215 = false;
    for (auto &e : r.log) {
      rd190 |= e.first == 190u && e.second.rundata && !e.second.first_connect;
      pd215 |= e.first == 215u && e.second.propertydata_all;
    }
    CHECK(rd190 && pd215);
    // A reconnect later in the boot sends no devinfo either way.
    r.log.clear();
    r.run(400, 500, false);
    r.run(500, 600, true);
    for (auto &e : r.log)
      CHECK(!e.second.devinfo && !e.second.first_connect);

    // Server-switch confirm: independent of devinfo_on_connect (the component publishes devinfo
    // on every CONFIRMED, isolarcloud.cpp). After the commit both 38338 and 38337 name the new
    // broker; 38781 is zeros.
    CloudIdentity id;
    id.sn = SN;
    ServerEndpoint nb;
    nb.host = "auiot.isolarcloud.com";
    nb.port = 19999;
    nb.ssl_flag = 2;
    ServerSwitch sw(5000, 600000, 10000);
    CHECK(sw.request(id, nb, 0));
    CHECK(sw.update(id, true, 10000) == ServerSwitch::Action::APPLY);
    CHECK(sw.update(id, true, 15001) == ServerSwitch::Action::CONFIRMED);
    DevinfoContext ctx;
    ctx.data_time = "20261018120000";
    ctx.server = id.server;
    ctx.old_server = id.old_server;
    JsonDocument d;
    CHECK(deserializeJson(d, f.devinfo(ctx)) == DeserializationError::Ok);
    JsonObject data = d[0]["data"];
    CHECK_EQ(std::string(data["38781"].as<const char *>()), "0000000000000000");
    CHECK_EQ(std::string(data["38338"].as<const char *>()), "auiot.isolarcloud.com;19999;Sun#0*2Pow925Mdk");
    CHECK_EQ(std::string(data["38337"].as<const char *>()), "auiot.isolarcloud.com;19999;Sun#0*2Pow925Mdk");
  }
}

// ---------------------------------------------------------------------------------------------
static void test_server_switch() {
  CloudIdentity id;
  id.sn = SN;
  ServerEndpoint a = id.server, nb;
  nb.host = "auiot.isolarcloud.com";
  nb.port = 19999;
  nb.ssl_flag = 2;

  ServerSwitch sw(5000, 600000, 10000);
  CHECK(sw.update(id, true, 0) == ServerSwitch::Action::NONE);
  // Same host/port/key (ssl-only change) is ignored like stock.
  ServerEndpoint same = a;
  same.ssl_flag = 0;
  CHECK(!sw.request(id, same, 0));
  CHECK(sw.request(id, nb, 1000));
  CHECK(sw.pending() && !sw.confirming());
  CHECK(sw.update(id, true, 10999) == ServerSwitch::Action::NONE);
  CHECK(id.server == a);  // not applied yet
  CHECK(sw.update(id, true, 11000) == ServerSwitch::Action::APPLY);
  CHECK(id.server == nb && id.old_server == a);
  CHECK(sw.confirming());
  CHECK(sw.update(id, true, 16000) == ServerSwitch::Action::NONE);  // not > 5 s yet
  CHECK(sw.update(id, true, 16001) == ServerSwitch::Action::CONFIRMED);
  CHECK(id.old_server == nb);
  CHECK(!sw.pending());

  CloudIdentity id2;
  ServerSwitch sw2(5000, 600000, 10000);
  sw2.request(id2, nb, 0);
  CHECK(sw2.update(id2, false, 10000) == ServerSwitch::Action::APPLY);
  CHECK(sw2.update(id2, false, 609999) == ServerSwitch::Action::NONE);
  CHECK(sw2.update(id2, false, 610000) == ServerSwitch::Action::ROLLBACK);
  CHECK(id2.server == a);

  CloudIdentity id3;
  id3.remote_ctl_mask = 0x4FD;
  ServerSwitch sw3;
  sw3.request(id3, nb, 0);
  sw3.update(id3, false, 10000);
  CHECK(sw3.update(id3, false, 700000) == ServerSwitch::Action::STAY);
  CHECK(id3.server == nb);
  // The "mask at default" test uses the variant's default: 0x4EF on the WiNet-S.
  CHECK_EQ(default_remote_ctl_mask(0x720), 0x4FFu);
  CHECK_EQ(default_remote_ctl_mask(0x715), 0x4EFu);
  CloudIdentity id4;
  id4.product_type = 0x715;
  id4.remote_ctl_mask = 0x4EF;
  ServerSwitch sw4;
  sw4.request(id4, nb, 0);
  sw4.update(id4, false, 10000);
  CHECK(sw4.update(id4, false, 700000) == ServerSwitch::Action::ROLLBACK);
  CHECK(id4.server == a);
  CloudIdentity id5;
  id5.product_type = 0x715;
  id5.remote_ctl_mask = 0x4FF;
  ServerSwitch sw5;
  sw5.request(id5, nb, 0);
  sw5.update(id5, false, 10000);
  CHECK(sw5.update(id5, false, 700000) == ServerSwitch::Action::STAY);
  // A WiNet-S with the default mask does not subscribe the Layout group (structurefileset).
  TopicBuilder tbs("S", {0, 0, 1});
  bool layout = false;
  for (const auto &tp : tbs.subscriptions(0x4EF))
    layout |= tp.find("structurefileset") != std::string::npos;
  CHECK(!layout);

  CloudIdentity dst, src;
  src.sn = "S";
  src.server.port = 1234;
  src.timezone = "UTC+10:00";
  apply_identity_fields(dst, src, FIELD_SN | FIELD_SERVER_PORT);
  CHECK_EQ(dst.sn, "S");
  CHECK_EQ(dst.server.port, 1234);
  CHECK_EQ(dst.timezone, std::string(DEFAULT_TIMEZONE));
  apply_identity_fields(dst, src, FIELD_TIMEZONE);
  CHECK_EQ(dst.timezone, "UTC+10:00");
}

// ---------------------------------------------------------------------------------------------
static std::vector<uint8_t> make_cmd_payload(const std::string &business, bool compress) {
  std::string json = std::string("{\"messageId\":\"cloud_1\",\"businessData\":") + business + "}";
  std::vector<uint8_t> out;
  encode_payload(json, compress ? 1 : 0, out);
  return out;
}

static void test_inbound() {
  TopicBuilder tb(SN, {0, 0, 1});
  InboundCommand c;

  auto p = make_cmd_payload("[{\"cmdType\":\"10325\",\"batchId\":\"b-1\"}]", true);
  CHECK(decode_inbound(tb.cmd("getparam"), p.data(), p.size(), SN, c) == InboundStatus::OK);
  CHECK_EQ(c.cmd_type, "10325");
  CHECK_EQ(c.batch_id, "b-1");
  CHECK(c.has_batch_id);
  CHECK_EQ(c.topic.family, "getparam");
  CHECK_EQ(c.raw_topic, tb.cmd("getparam"));
  CHECK_EQ(c.business_all, "[{\"cmdType\":\"10325\",\"batchId\":\"b-1\"}]");

  // De-dup: depth 1, whole businessData, only with a batchId.
  CommandDeduper dd;
  CHECK(!dd.check_and_add(c));
  CHECK(dd.check_and_add(c));
  InboundCommand c2 = c;
  c2.business_all = "[{\"cmdType\":\"10325\",\"batchId\":\"b-2\"}]";
  CHECK(!dd.check_and_add(c2));
  CHECK(!dd.check_and_add(c));  // only the last one is remembered
  InboundCommand nob = c;
  nob.has_batch_id = false;
  CHECK(!dd.check_and_add(nob));
  CHECK(!dd.check_and_add(nob));

  // Tolerated: object businessData, numeric cmdType, string businessData.
  TopicBuilder tb0(SN, {0, 0, 0});
  p = make_cmd_payload("{\"cmdType\":10421,\"batchId\":99}", false);
  CHECK(decode_inbound(tb0.cmd("second"), p.data(), p.size(), SN, c) == InboundStatus::OK);
  CHECK_EQ(c.cmd_type, "10421");
  p = make_cmd_payload("\"[{\\\"cmdType\\\":\\\"10303\\\"}]\"", false);
  CHECK(decode_inbound(tb0.cmd("setparam"), p.data(), p.size(), SN, c) == InboundStatus::OK);
  CHECK_EQ(c.cmd_type, "10303");
  CHECK(!c.has_batch_id);

  // Group topic (enum_0_0_0 -> uncompressed), no reply topic.
  p = make_cmd_payload("[{\"cmdType\":\"10307\",\"dataTime\":\"20261018123000\"}]", false);
  CHECK(decode_inbound(TopicBuilder::group_cmd(8), p.data(), p.size(), SN, c) == InboundStatus::OK);
  CHECK(c.topic.kind == ParsedTopic::Kind::GROUP_CMD);
  CHECK_EQ(reply_topic(tb, c), "");

  // Failures.
  CHECK(decode_inbound(tb.cmd("getparam"), p.data(), 0, SN, c) == InboundStatus::EMPTY);
  p = make_cmd_payload("[{\"cmdType\":\"1\"}]", false);
  CHECK(decode_inbound(tb.cmd("getparam"), p.data(), p.size(), SN, c) == InboundStatus::INFLATE_FAILED);
  CHECK(decode_inbound(tb0.cmd("getparam"), p.data(), p.size(), "OTHER", c) == InboundStatus::WRONG_SN);
  std::vector<uint8_t> huge(4097, ' ');
  CHECK(decode_inbound(tb0.cmd("getparam"), huge.data(), huge.size(), SN, c) == InboundStatus::TOO_LARGE);
  // Inflated > 4096 B is dropped too.
  std::string big_json = "{\"businessData\":[{\"cmdType\":\"10325\",\"pad\":\"" + std::string(5000, 'x') + "\"}]}";
  std::vector<uint8_t> z;
  encode_payload(big_json, 1, z);
  CHECK(z.size() < 4096);
  CHECK(decode_inbound(tb.cmd("getparam"), z.data(), z.size(), SN, c) == InboundStatus::INFLATE_FAILED);
  const uint8_t tlv[] = {1, 0, 1, 0x12, 0x34, 0};
  CHECK(decode_inbound("isolarcloud/devicedata/pub/x/" + SN, tlv, sizeof(tlv), SN, c) == InboundStatus::BRANCH_A);
  std::string notjson = "{oops";
  CHECK(decode_inbound(tb0.cmd("getparam"), reinterpret_cast<const uint8_t *>(notjson.data()), notjson.size(), SN,
                       c) == InboundStatus::BAD_JSON);
  p = make_cmd_payload("[]", false);
  CHECK(decode_inbound(tb0.cmd("getparam"), p.data(), p.size(), SN, c) == InboundStatus::NO_BUSINESS);
  p = make_cmd_payload("[{\"batchId\":\"x\"}]", false);
  CHECK(decode_inbound(tb0.cmd("getparam"), p.data(), p.size(), SN, c) == InboundStatus::NO_BUSINESS);

  // Router: cmdType only, first strstr match in registration order; family ignored.
  CommandRouter r;
  std::string hit;
  r.on_cmd_type("10325", [&](const InboundCommand &) { hit = "getparam"; });
  r.on_cmd_type("1032", [&](const InboundCommand &) { hit = "prefix"; });
  p = make_cmd_payload("[{\"cmdType\":\"10325\",\"batchId\":\"x\"}]", true);
  decode_inbound(tb.cmd("generalcmd"), p.data(), p.size(), SN, c);
  CHECK(r.dispatch(c));
  CHECK_EQ(hit, "getparam");
  c.cmd_type = "10324";
  CHECK(r.dispatch(c));
  CHECK_EQ(hit, "prefix");
  c.cmd_type = "10308";
  CHECK(!r.dispatch(c));  // unknown: no handler, no reply

  // Reply topic: rsp/<incoming family>/<SN>/<OUR enum>; reply body (C8).
  p = make_cmd_payload("[{\"cmdType\":\"10325\",\"batchId\":\"B7\"}]", false);
  CHECK(decode_inbound(tb0.cmd("getparam"), p.data(), p.size(), SN, c) == InboundStatus::OK);
  CHECK_EQ(reply_topic(tb, c), "isolarcloud/devicedata/rsp/getparam/B2101234567/enum_0_0_1");
  CHECK_EQ(reply_compress(tb, c), 1);
  CHECK_EQ(build_reply_business(c, T0, 1),
           "[{\"batchId\":\"B7\",\"cmdType\":\"10325\",\"dataTime\":\"20261018123456\",\"result\":1}]");
  CHECK_EQ(build_reply_business(c, T0, 0, "{\"devSwitch\":1}"),
           "[{\"batchId\":\"B7\",\"cmdType\":\"10325\",\"dataTime\":\"20261018123456\",\"result\":0,\"devSwitch\":1}]");
}

// ---------------------------------------------------------------------------------------------
static void test_commands() {
  InboundCommand cmd;
  cmd.batch_id = "B1";
  cmd.cmd_type = "10325";
  GetParamRequest g;
  std::string err;
  CHECK(parse_getparam(R"({"cmdType":"10325","devCode":1,"mode":0,"readOnly":1,"data":{"a":{"modbusAddress":"5000","dataType":"6"},"b":{"modbusAddress":"13049","dataType":"3"}}})",
                       g, err));
  CHECK_EQ(g.dev_code, 1);
  CHECK_EQ(g.fn(), 4);
  CHECK_EQ(g.points.size(), (size_t) 2);
  CHECK_EQ(g.points[0].reg, 5000);  // as given, no -1
  CHECK_EQ(g.points[0].count, 2);
  CHECK_EQ(g.points[1].count, 1);
  g.read_only = 0;
  CHECK_EQ(g.fn(), 3);
  g.mode = 1;
  CHECK_EQ(g.fn(), 0x77);
  CHECK(!parse_getparam(R"({"data":{"a":{"modbusAddress":"1","dataType":"4"}}})", g, err));  // readOnly required
  CHECK(!parse_getparam(R"({"readOnly":1,"data":{"a":{"modbusAddress":"x","dataType":"4"}}})", g, err));
  CHECK(parse_getparam(R"({"readOnly":0,"data":{}})", g, err));
  CHECK_EQ(g.dev_code, 247);
  CHECK_EQ(getparam_count(7), 4);
  CHECK_EQ(getparam_count(12), 1);
  CHECK_EQ(getparam_count(0), 1);

  const uint8_t r2[] = {0xFF, 0xFE, 0x00, 0x10};
  CHECK_EQ(getparam_read_value(3, r2, 1), "-2");
  CHECK_EQ(getparam_read_value(4, r2, 1), "65534");
  // Two registers: the first is the low word (stock byte order 2).
  CHECK_EQ(getparam_read_value(6, r2, 2), "1114110");  // 0x0010FFFE
  const uint8_t r3[] = {0x00, 0x10, 0xFF, 0xFE};
  CHECK_EQ(getparam_read_value(5, r3, 2), "-131056");  // 0xFFFE0010
  CHECK_EQ(getparam_read_value(6, r3, 2), "4294836240");

  // setparam encoding: low word first, decimal echo; types stock cannot write are refused.
  std::vector<uint16_t> regs;
  std::string echo;
  CHECK(setparam_encode(4, "170", regs, echo) && regs == std::vector<uint16_t>{170} && echo == "170");
  CHECK(setparam_encode(3, "-5", regs, echo) && regs == std::vector<uint16_t>{0xFFFB});
  CHECK(setparam_encode(6, "0x12345678", regs, echo) && regs == (std::vector<uint16_t>{0x5678, 0x1234}) &&
        echo == "305419896");
  CHECK(setparam_encode(5, "-2", regs, echo) && regs == (std::vector<uint16_t>{0xFFFE, 0xFFFF}));
  CHECK(setparam_encode(8, "4294967296", regs, echo) && regs == (std::vector<uint16_t>{0, 0, 1, 0}));
  CHECK(!setparam_encode(4, "70000", regs, echo));
  CHECK(!setparam_encode(4, "-1", regs, echo));
  CHECK(!setparam_encode(12, "abc", regs, echo));
  CHECK(!setparam_encode(9, "1.5", regs, echo));
  CHECK(!setparam_encode(4, "", regs, echo));
  SetParamRequest sp;
  CHECK(parse_setparam(R"({"cmdType":"10303","devCode":1,"mode":0,"data":{"ems":{"modbusAddress":"13050",)"
                       R"("dataType":"4","dataValue":"2"},"cmd":{"modbusAddress":"13051","dataType":"4","dataValue":"170"}}})",
                       sp, err));
  CHECK_EQ(sp.points.size(), (size_t) 2);
  CHECK_EQ(sp.points[0].reg, 13050);
  CHECK(sp.points[1].encodable && sp.points[1].values == std::vector<uint16_t>{170});
  CHECK(!parse_setparam(R"({"devCode":1,"data":{"a":{"modbusAddress":"0","dataType":"4","dataValue":"1"}}})", sp, err));
  CHECK(!parse_setparam(R"({"devCode":1})", sp, err));
  InboundCommand sc;
  sc.batch_id = "S1";
  sc.cmd_type = "10303";
  CHECK(parse_setparam(R"({"devCode":1,"data":{"ems":{"modbusAddress":"13050","dataType":"4","dataValue":"2"}}})", sp,
                       err));
  GetParamResult res;
  res.key = "ems";
  res.modbus_address = "13050";
  res.read_value = "2";
  res.read_result = "1";
  CHECK_EQ(build_setparam_reply(sc, "20261024150000", sp, {res}),
           "[{\"batchId\":\"S1\",\"cmdType\":\"10303\",\"dataTime\":\"20261024150000\",\"mode\":0,\"readOnly\":0,"
           "\"result\":1,\"data\":{\"ems\":{\"modbusAddress\":\"13050\",\"readValue\":\"2\",\"readResult\":\"1\"}}}]");
  CHECK_EQ(getparam_read_value(1, r2, 1), "-2");
  CHECK_EQ(getparam_read_value(2, r2, 1), "254");
  const uint8_t s[] = {'S', 'H', 0, 0};
  CHECK_EQ(getparam_read_value(12, s, 2), "SH");

  GetParamRequest req;
  req.mode = 0;
  req.read_only = 1;
  std::vector<GetParamResult> vals(1);
  vals[0].key = "a";
  vals[0].modbus_address = "5000";
  vals[0].read_value = "123";
  vals[0].read_result = "1";
  CHECK_EQ(build_getparam_reply(cmd, T0, req, 1, vals),
           "[{\"batchId\":\"B1\",\"cmdType\":\"10325\",\"dataTime\":\"20261018123456\",\"mode\":0,\"readOnly\":1,"
           "\"result\":1,\"data\":{\"a\":{\"modbusAddress\":\"5000\",\"readValue\":\"123\",\"readResult\":\"1\"}}}]");
  CHECK_EQ(build_getparam_reply(cmd, T0, req, 0, vals),
           "[{\"batchId\":\"B1\",\"cmdType\":\"10325\",\"dataTime\":\"20261018123456\",\"mode\":0,\"readOnly\":1,"
           "\"result\":0}]");

  // 10384 logger parameter set.
  LoggerParamSet lp;
  CHECK(parse_logger_param_set(R"({"devCode":247,"data":{"10457":{"dataType":"12","dataValue":"eu.example;19999;ABCDEFGHIJKLMNOP","sslFlag":"1"},"4":{}}})",
                               lp, err));
  CHECK_EQ(lp.point, "10457");
  ServerEndpoint ep;
  CHECK(parse_server_switch(lp, ep));
  CHECK_EQ(ep.host, "eu.example");
  CHECK_EQ(ep.port, 19999);
  CHECK(ep.key == key_from_string("ABCDEFGHIJKLMNOP"));
  CHECK_EQ(ep.ssl_flag, 2);  // "1" -> 2
  lp.has_ssl_flag = false;
  CHECK(parse_server_switch(lp, ep) && ep.ssl_flag == 0);  // absent -> 0 (plain TCP)
  lp.data_value = "nohost";
  CHECK(!parse_server_switch(lp, ep));
  CHECK(!parse_logger_param_set(R"({"devCode":1,"data":{"4":{"dataValue":"5"}}})", lp, err));
  CHECK(parse_logger_param_set(R"({"devCode":247,"data":{"4":{"dataType":"4","dataValue":"10"}}})", lp, err));
  cmd.cmd_type = "10384";
  // sslFlag is always present as a number: the request's value, 0 when it had none.
  CHECK_EQ(build_logger_param_reply(cmd, T0, lp, "10", 1),
           "[{\"batchId\":\"B1\",\"cmdType\":\"10384\",\"dataTime\":\"20261018123456\",\"result\":1,"
           "\"data\":{\"4\":{\"readValue\":\"10\",\"readResult\":1,\"sslFlag\":0}}}]");
  lp.has_ssl_flag = true;
  lp.ssl_flag = "1";
  CHECK(build_logger_param_reply(cmd, T0, lp, "10", 5).find("\"readResult\":5,\"sslFlag\":1}") != std::string::npos);
  // 10303 with devCode 247 is a logger-point set (reply with cmdType 10303); other devCodes are not.
  CHECK(parse_logger_param_set(R"({"cmdType":"10303","devCode":247,"data":{"4":{"dataType":"4","dataValue":"5"}}})",
                               lp, err));
  CHECK_EQ(lp.point, "4");
  CHECK_EQ(lp.data_value, "5");
  cmd.cmd_type = "10303";
  CHECK(build_logger_param_reply(cmd, T0, lp, lp.data_value, 1).find("\"cmdType\":\"10303\"") != std::string::npos);
  CHECK(!parse_logger_param_set(R"({"cmdType":"10303","devCode":1,"data":{"ems":{"modbusAddress":"13050","dataType":"4","dataValue":"2"}}})",
                                lp, err));

  SetTimezone tz;
  CHECK(parse_settimezone(R"({"cmdType":"10324","timezoneId":10,"timezone":"UTC+10:00"})", tz));
  CHECK_EQ(tz.timezone_id, 10);
  CHECK_EQ(tz.timezone, "UTC+10:00");
  CHECK(!parse_settimezone(R"({"timezoneId":10,"timezone":"UTC+8:00"})", tz));
  CHECK(!parse_settimezone(R"({"timezone":"UTC+10:00"})", tz));
  CivilTime t;
  CHECK(parse_settime(R"({"cmdType":"10307","dataTime":"20261018123000"})", t) && t.minute == 30);
  CHECK(!parse_settime(R"({"cmdType":"10307"})", t));
  std::vector<uint16_t> arr;
  CHECK(parse_dev_code_arr(R"({"devCodeArr":[1,"247"]})", arr));
  CHECK(arr.size() == 2 && arr[0] == 1 && arr[1] == 247);
  CHECK(!parse_dev_code_arr(R"({"batchId":"x"})", arr));  // no devCodeArr: no reply
}

// ---------------------------------------------------------------------------------------------
// --dump <dir>: write sample messages (topic + wire payload) for the local broker test.
static void write_file(const std::string &path, const std::vector<uint8_t> &data) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}
static int dump(const std::string &dir) {
  mkdir(dir.c_str(), 0755);
  TopicBuilder tb(SN, {0, 0, 1});
  std::ofstream idx(dir + "/index.tsv");
  int n = 0;
  auto emit = [&](const std::string &topic, const std::string &json, uint16_t compress) {
    std::vector<uint8_t> wire;
    encode_payload(json, compress, wire);
    std::string name = "msg" + std::to_string(n++) + ".bin";
    write_file(dir + "/" + name, wire);
    idx << topic << "\t" << name << "\t" << compress << "\t" << json.size() << "\n";
  };
  MessageCounter mc;
  PublicData pd = sample_pd();
  BusinessFormatter f;
  FakeTelemetry t;
  t.devs = {dev(0, 1, 0x23)};
  t.add(0, sig_int(0x2031, 0xFFFB, 2, 3));  // hybrid table: 0x2031 -> point 13019, s16 x0.1
  emit(tb.will(), build_will_json(pd, make_message_id(SN, pd.send_time, mc.next())), 1);
  emit(tb.pub("beatdata"), build_envelopes(pd, make_message_id(SN, pd.send_time, mc.next()), f.beat(t.devs))[0], 1);
  emit(tb.pub("rundata/5min"),
       build_envelopes(pd, make_message_id(SN, pd.send_time, mc.next()), f.rundata(t.devs[0], &t, "20261018123000"))[0],
       1);
  std::string big = "[";
  for (int i = 0; i < 300; i++)
    big += std::string(i ? "," : "") + "{\"devCode\":1,\"devType\":14,\"data\":{\"" + std::to_string(5000 + i) +
           "\":\"1.5\"}}";
  big += "]";
  for (auto &e : build_envelopes(pd, make_message_id(SN, pd.send_time, mc.next()), big))
    emit(tb.pub("rundata/5min"), e, 1);
  printf("wrote %d samples to %s\n", n, dir.c_str());
  return 0;
}

// --decode <topic> <file>: run a received payload through the inbound path and print the result
// (status, cmdType, batchId, reply topic, a result-1 reply body) for the local broker test.
static int decode(const std::string &topic, const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  InboundCommand c;
  InboundStatus st = decode_inbound(topic, data.data(), data.size(), SN, c);
  printf("status=%s\n", inbound_status_str(st));
  if (st != InboundStatus::OK)
    return 2;
  TopicBuilder tb(SN, {0, 0, 1});
  printf("family=%s\ncmdType=%s\nbatchId=%s\nreply_topic=%s\nreply_compress=%u\nreply=%s\n", c.topic.family.c_str(),
         c.cmd_type.c_str(), c.batch_id.c_str(), reply_topic(tb, c).c_str(), reply_compress(tb, c),
         build_reply_business(c, T0, 1).c_str());
  return 0;
}

// ---------------------------------------------------------------------------------------------
static void test_second() {
  InboundCommand cmd;
  cmd.batch_id = "4579756187000000";
  cmd.cmd_type = "10421";
  std::string err;
  SecondRequest r;
  CHECK(parse_second_request(
      R"({"batchId":"4579756187000000","cmdType":"10421","devCode":247,"devCodeArr":[3,1],"devSwitch":1,)"
      R"("data":{"1":[13003,99999],"3":[13264,13003,13003]}})",
      r, err));
  CHECK_EQ(r.dev_codes.size(), (size_t) 2);
  CHECK_EQ(r.dev_switch, (int64_t) 1);
  CHECK_EQ(r.data.size(), (size_t) 2);
  CHECK(!parse_second_request(R"({"devSwitch":1})", r, err));        // devCodeArr required
  CHECK(!parse_second_request(R"({"devCodeArr":[1]})", r, err));     // devSwitch required

  // On: keys looked up directly (order differs from devCodeArr), duplicates kept.
  SecondData sd;
  CHECK(parse_second_request(R"({"devCodeArr":[3,1],"devSwitch":1,"data":{"1":[13003],"3":[13264,13003,13003]}})", r,
                             err));
  SecondData::Outcome o = sd.apply(r);
  CHECK_EQ(o.result, 1);
  CHECK_EQ(o.dev_switch, 1);
  CHECK(sd.active());
  CHECK_EQ(sd.node(3)->point_ids.size(), (size_t) 3);
  CHECK(sd.node(1)->enabled);
  CHECK_EQ(build_second_reply(cmd, "20261024141000", r, o),
           "[{\"batchId\":\"4579756187000000\",\"cmdType\":\"10421\",\"dataTime\":\"20261024141000\",\"result\":1,"
           "\"devSwitch\":1,\"devCodeArr\":[\"3\",\"1\"]}]");
  // Persistence round trip.
  const std::string ser = sd.serialize();
  SecondData sd2;
  CHECK(sd2.deserialize(ser));
  CHECK(sd2.active());
  CHECK_EQ(sd2.serialize(), ser);
  // Off for one device: still on for the other.
  CHECK(parse_second_request(R"({"devCodeArr":[3],"devSwitch":0})", r, err));
  o = sd.apply(r);
  CHECK_EQ(o.result, 1);
  CHECK_EQ(o.dev_switch, 0);
  CHECK(sd.node(3) == nullptr);
  CHECK(sd.active());
  // Any other devSwitch: everything off, replied as devSwitch 0.
  CHECK(parse_second_request(R"({"devCodeArr":[],"devSwitch":2})", r, err));
  o = sd.apply(r);
  CHECK_EQ(o.dev_switch, 0);
  CHECK(!sd.active());
  CHECK_EQ(sd.serialize(), "");
  // data missing -> result 0, flag not set; > 400 points -> result 3.
  CHECK(parse_second_request(R"({"devCodeArr":[1],"devSwitch":1})", r, err));
  CHECK_EQ(sd.apply(r).result, 0);
  CHECK(!sd.enable_flag());
  std::string big = R"({"devCodeArr":[1],"devSwitch":1,"data":{"1":[)";
  for (int i = 0; i < 401; i++)
    big += (i ? "," : "") + std::to_string(i);
  big += "]}}";
  CHECK(parse_second_request(big, r, err));
  CHECK_EQ(sd.apply(r).result, 3);
  // 15 nodes max -> result 4.
  SecondData full;
  for (int c = 1; c <= 16; c++) {
    CHECK(parse_second_request(R"({"devCodeArr":[)" + std::to_string(c) + R"(],"devSwitch":1,"data":{")" +
                                   std::to_string(c) + R"(":[1]}})",
                               r, err));
    o = full.apply(r);
  }
  CHECK_EQ(o.result, 4);

  // rundata/second element: request order, unknown ids dropped, keyed by the sig's first point.
  FakeTelemetry t;
  t.devs = {dev(0, 1, 0x23)};
  t.add(0, sig_int(0x2001, 7, 2, 4));
  t.add(0, sig_int(0x2320, 5230, 4, 6));
  BusinessFormatter f;
  const std::string js = f.rundata_second(t.devs[0], &t, "20261024141010", {13003, 99999, 13264});
  CHECK_EQ(js, "[{\"devCode\":1,\"devType\":14,\"dataTime\":\"20261024141010\",\"data\":{\"13003\":\"5230\",\"13264\":\"7\"}}]");
  CHECK_EQ(f.rundata_second(t.devs[0], &t, "x", {99999}), "");
}

static void test_setparam_protected() {
  CHECK(setparam_protected_range(13050, 1) == nullptr);  // EMS mode: app setting
  CHECK(setparam_protected_range(33046, 2) == nullptr);
  CHECK(setparam_protected_range(37004, 1) != nullptr);
  CHECK(setparam_protected_range(36099, 1) != nullptr);
  CHECK(setparam_protected_range(36098, 1) == nullptr);
  CHECK(setparam_protected_range(36098, 2) != nullptr);  // runs into 36099
  CHECK(setparam_protected_range(21004, 2) != nullptr);
  CHECK(setparam_protected_range(30459, 1) != nullptr);
  CHECK(setparam_protected_range(20009, 6) != nullptr);
  CHECK(setparam_protected_range(15999, 1) == nullptr);
  CHECK(setparam_protected_range(15999, 2) != nullptr);
  CHECK(setparam_protected_range(39000, 1) == nullptr);
}

static void test_status_general() {
  FakeTelemetry t;
  t.devs = {dev(0, 1, 0x23), dev(1, 2, 0x2c)};
  t.add(0, sig_str(0x0002, "A2101234567"));
  t.add(0, sig_int(0x2086, 2048, 2, 4));
  t.add(0, sig_int(0x2231, 0, 2, 4));
  t.add(0, sig_int(0x226e, 85, 2, 4));
  BusinessFormatter f;
  // devstatusdata: hybrid carries sn + timestamp before dataTime; only status-mask points.
  CHECK_EQ(f.devstatus(t.devs[0], &t, "20261024141010", "20261024141010123"),
           "[{\"devCode\":1,\"devType\":14,\"sn\":\"A2101234567\",\"timestamp\":\"20261024141010123\","
           "\"dataTime\":\"20261024141010\",\"data\":{\"13146\":\"2048\"}}]");
  // SBR: no sn/timestamp, the parent link keys (0 while unreadable); an element without values is
  // still sent.
  CHECK_EQ(f.devstatus(t.devs[1], &t, "20261024141010", "20261024141010123"),
           "[{\"devCode\":2,\"devType\":43,\"upDevType\":0,\"upDevCode\":0,\"dataTime\":\"20261024141010\",\"data\":{}}]");
  // generaldata: table order, then businessType; nothing when no general point has a value.
  CHECK_EQ(f.generaldata(t.devs[0], &t, "20261024141010"),
           "[{\"devCode\":1,\"devType\":14,\"dataTime\":\"20261024141010\",\"data\":{\"18070\":\"0\","
           "\"38667\":\"85\"},\"businessType\":\"addBattery\"}]");
  CHECK_EQ(f.generaldata(t.devs[1], &t, "20261024141010"), "");
  FakeTelemetry empty;
  empty.devs = {dev(0, 1, 0x23)};
  CHECK_EQ(f.generaldata(empty.devs[0], &empty, "20261024141010"), "");
  // Change signatures.
  CHECK_EQ(f.point_signature(t.devs[0], &t, MQTT_MASK_STATUS), "13146=2048;");
  CHECK_EQ(f.point_signature(t.devs[0], &t, MQTT_MASK_GENERAL), "18070=0;38667=85;");
  t.add(0, sig_int(0x2086, 0, 2, 4));
  CHECK_EQ(f.point_signature(t.devs[0], &t, MQTT_MASK_STATUS), "13146=0;");
}

int main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--dump")
    return dump(argv[2]);
  if (argc == 4 && std::string(argv[1]) == "--decode")
    return decode(argv[2], argv[3]);
  test_topics();
  test_zlib();
  test_time();
  test_envelope_and_will();
  test_silence_devtype_backoff();
  test_point_format();
  test_formatter();
  test_scheduler();
  test_devinfo_timing();
  test_publish_queue();
  test_server_switch();
  test_inbound();
  test_commands();
  test_second();
  test_status_general();
  test_setparam_protected();
  printf("isolarcloud host tests: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
