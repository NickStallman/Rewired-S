// Host tests for the sungrow_debug trace core (hex / frame summaries / row decode / filters / scan).
#include <cstring>
#include <string>

#include "../test_harness.h"
#include "trace_core.h"
#include "../../components/sungrow_modbus/modbus_rtu.h"

using namespace esphome::sungrow_debug;
using esphome::sungrow_datamodel::RegisterRow;
using esphome::sungrow_datamodel::RegisterTable;
using esphome::sungrow_datamodel::WordOrder;
namespace dm = esphome::sungrow_datamodel;
namespace mb = esphome::sungrow_modbus;

static bool contains(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

TEST(hex_basic) {
  const uint8_t b[] = {0x01, 0x04, 0xAB, 0x0F};
  CHECK(hex(b, 4) == "01 04 AB 0F");
  CHECK(hex(b, 4, 2) == "01 04 ..(+2)");
  CHECK(hex(b, 0).empty());
  CHECK(hex_words(b, 2) == "0104 AB0F");
  CHECK(hex_words(b, 2, 1) == "0104 ..(+1)");
}

TEST(hex_lines_split) {
  uint8_t b[70];
  for (int i = 0; i < 70; i++)
    b[i] = static_cast<uint8_t>(i);
  auto l = hex_lines(b, 70, 32);
  CHECK_EQ(l.size(), 3u);
  CHECK(l[0].rfind("+000 00 01", 0) == 0);
  CHECK(l[2].rfind("+064 40 41 42 43 44 45", 0) == 0);
  CHECK_EQ(hex_lines(b, 0).size(), 1u);
}

TEST(rtu_request_and_reply) {
  uint8_t f[8];
  CHECK_EQ(mb::build_read_request(1, 4, 4999, 1, f), 8u);
  RtuInfo r = parse_rtu_request(f, 8);
  CHECK(r.parsed && r.crc_ok);
  CHECK_EQ(r.addr, 1);
  CHECK_EQ(r.fn, 4);
  CHECK_EQ(r.start, 5000);
  CHECK_EQ(r.count, 1);
  CHECK(contains(describe_request(r), "start 5000 count 1"));
  f[7] ^= 0xFF;
  CHECK(!parse_rtu_request(f, 8).crc_ok);
  CHECK(contains(describe_request(parse_rtu_request(f, 8)), "CRC-BAD"));

  uint8_t rep[7] = {1, 4, 2, 0x0E, 0x0F, 0, 0};
  mb::append_crc(rep, 5);
  RtuInfo a = parse_rtu_response(rep, 7);
  CHECK(a.parsed && a.crc_ok && !a.exception);
  CHECK_EQ(a.count, 1);
  CHECK(contains(describe_response(a), "CRC ok"));

  uint8_t ex[5] = {3, 0x84, 0x02, 0, 0};
  mb::append_crc(ex, 3);
  RtuInfo e = parse_rtu_response(ex, 5);
  CHECK(e.exception && e.crc_ok);
  CHECK_EQ(e.exc_code, 2);
  CHECK(contains(describe_response(e), "illegal data address"));

  uint8_t w[8];
  mb::build_write_single_request(1, 13049, 2, w);
  RtuInfo ws = parse_rtu_request(w, 8);
  CHECK_EQ(ws.fn, 6);
  CHECK_EQ(ws.start, 13050);
  CHECK_EQ(ws.value, 2);
  CHECK(!is_read_fn(ws.fn));
  CHECK(is_read_fn(3) && is_read_fn(4) && !is_read_fn(0x10));
}

TEST(mbap_parse) {
  const uint8_t req[12] = {0x00, 0x07, 0, 0, 0, 6, 1, 4, 0x13, 0x87, 0, 2};
  MbapInfo m = parse_mbap(req, 12, false);
  CHECK(m.parsed);
  CHECK_EQ(m.tid, 7);
  CHECK_EQ(m.unit, 1);
  CHECK_EQ(m.fc, 4);
  CHECK_EQ(m.start, 5000);
  CHECK_EQ(m.count, 2);
  const uint8_t rep[13] = {0x00, 0x07, 0, 0, 0, 7, 1, 4, 4, 0, 1, 0, 2};
  CHECK_EQ(parse_mbap(rep, 13, true).count, 2);
  const uint8_t ex[9] = {0, 7, 0, 0, 0, 3, 1, 0x84, 0x02};
  MbapInfo e = parse_mbap(ex, 9, true);
  CHECK(e.exception && e.exc_code == 2);
  CHECK(contains(describe_mbap(e, true), "EXCEPTION 2"));
  CHECK(!parse_mbap(req, 7, false).parsed);
}

// sigid regaddr group key nreg fn type tag nbits dep_word dep_bit unit scale
static const RegisterRow kRows[] = {
    {0x2004, 5003, 1, 0, 1, 4, dm::TYPE_U16, 0, 0, 0, 0, 0, 0.1f},
    {0x2010, 5004, 1, 0, 2, 4, dm::TYPE_U32, 0, 0, 0, 0, 0, 1.0f},
    {0x2011, 5006, 1, 0, 1, 4, dm::TYPE_S16, 0, 0, 3, 4, 0, 0.0f},
    {0x0000, 5007, 1, 0, 1, 4, dm::TYPE_U16, 0, 0, 0, 0, 0, 0.0f},  // gap row
    {0x2012, 5008, 1, 0, 4, 4, dm::TYPE_STRING, 0, 0, 0, 0, 0, 0.0f},
    {0x2013, 5008, 1, 0, 1, 3, dm::TYPE_U16, 0, 0, 0, 0, 0, 0.0f},  // other fn
    {0x2014, 5012, 1, 0, 1, 4, dm::TYPE_U16, dm::TAG_KEEP_SENTINEL, 16, 0, 0, 0, 0.0f},
    {0x2015, 5013, 1, 0, 2, 4, dm::TYPE_F32_WIRE, 0, 0, 0, 0, 0, 0.001f},
};
static const RegisterTable kTab = {0, dm::Family::INVERTER, "test", kRows, sizeof(kRows) / sizeof(kRows[0])};

TEST(rows_in_block_rule) {
  auto v = rows_in_block(kTab, 4, 5003, 10);  // 5003..5012
  // 0x2004, 0x2010, 0x2011, string 5008..5011, 0x2014 @5012; gap + fn3 row skipped
  CHECK_EQ(v.size(), 5u);
  CHECK_EQ(v[0], 0);
  CHECK_EQ(v[3], 4);
  CHECK_EQ(v[4], 6);
  // u32 row straddling the block end is excluded
  CHECK_EQ(rows_in_block(kTab, 4, 5003, 2).size(), 1u);
  CHECK_EQ(rows_in_block(kTab, 3, 5008, 1).size(), 1u);
}

TEST(format_row_scaled_and_sentinel) {
  const uint8_t w1[2] = {0x00, 0x12};
  std::string s = format_row(kRows[0], w1, WordOrder::LOW_WORD_FIRST, Gate::NONE, "daily_yield", "kWh");
  CHECK(contains(s, "5003+1 fn4 0x2004 daily_yield u16 [0012] raw=18"));
  CHECK(contains(s, "-> 1.8 kWh"));
  const uint8_t ff[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(contains(format_row(kRows[1], ff, WordOrder::LOW_WORD_FIRST, Gate::NONE, nullptr, nullptr),
                 "SENTINEL (absent)"));
  // low word first: regs 0x0001 0x0002 -> 0x00020001
  const uint8_t w2[4] = {0x00, 0x01, 0x00, 0x02};
  RowDecode d = decode_row(kRows[1], w2, WordOrder::LOW_WORD_FIRST);
  CHECK(d.present);
  CHECK_NEAR(d.raw, 131073.0, 0.1);
  CHECK_NEAR(decode_row(kRows[1], w2, WordOrder::HIGH_WORD_FIRST).raw, 65538.0, 0.1);
  // signed
  const uint8_t neg[2] = {0xFF, 0xFE};
  CHECK_NEAR(decode_row(kRows[2], neg, WordOrder::LOW_WORD_FIRST).raw, -2.0, 0.01);
}

TEST(format_row_gate_string_bits_float) {
  const uint8_t neg[2] = {0xFF, 0xFE};
  CHECK(contains(format_row(kRows[2], neg, WordOrder::LOW_WORD_FIRST, Gate::GATED, "x", nullptr), "GATED cap w3 b4"));
  CHECK(contains(format_row(kRows[2], neg, WordOrder::LOW_WORD_FIRST, Gate::OK, "x", nullptr), "cap w3 b4 ok"));
  const uint8_t str[8] = {'S', 'H', '1', '0', 'R', 'T', ' ', 0};
  CHECK(contains(format_row(kRows[4], str, WordOrder::LOW_WORD_FIRST, Gate::NONE, "model", nullptr), "\"SH10RT\""));
  const uint8_t empty[8] = {0};
  CHECK(contains(format_row(kRows[4], empty, WordOrder::LOW_WORD_FIRST, Gate::NONE, "model", nullptr), "EMPTY"));
  // tag bit4: sentinel kept, bitfield listing
  const uint8_t s16[2] = {0xFF, 0xFF};
  std::string k = format_row(kRows[6], s16, WordOrder::LOW_WORD_FIRST, Gate::NONE, "bits", nullptr);
  CHECK(contains(k, "sentinel kept"));
  CHECK(contains(k, "bits=0xFFFF"));
  const uint8_t b5[2] = {0x00, 0x05};
  CHECK(contains(format_row(kRows[6], b5, WordOrder::LOW_WORD_FIRST, Gate::NONE, "bits", nullptr), "set: 0 2"));
  // float32 1.5f = 0x3FC00000, low word first -> regs 0000 3FC0
  const uint8_t f[4] = {0x00, 0x00, 0x3F, 0xC0};
  // Default (float_scale off): the float is shown unscaled; with float_scale on, x scale.
  CHECK(contains(format_row(kRows[7], f, WordOrder::LOW_WORD_FIRST, Gate::NONE, "p", "kW"), "f=1.5"));
  CHECK(!contains(format_row(kRows[7], f, WordOrder::LOW_WORD_FIRST, Gate::NONE, "p", "kW"), "->"));
  std::string fs = format_row(kRows[7], f, WordOrder::LOW_WORD_FIRST, Gate::NONE, "p", "kW", true);
  CHECK(contains(fs, "f=1.5"));
  CHECK(contains(fs, "-> 0.0015 kW"));
}

TEST(ranges_and_filter) {
  std::vector<Range> r;
  CHECK(parse_ranges("1,3-5, 10", 1, 247, &r));
  CHECK_EQ(r.size(), 3u);
  CHECK(in_ranges(r, 4) && !in_ranges(r, 6) && in_ranges(r, 10));
  CHECK(format_ranges(r) == "1,3-5,10");
  CHECK(parse_ranges("5000+10", 1, 65535, &r));
  CHECK_EQ(r[0].hi, 5009u);
  CHECK(parse_ranges("", 1, 247, &r) && r.empty());
  CHECK(parse_ranges("all", 1, 247, &r) && r.empty());
  std::vector<Range> keep = {{1, 1}};
  CHECK(!parse_ranges("0", 1, 247, &keep));
  CHECK(!parse_ranges("5-3", 1, 247, &keep));
  CHECK(!parse_ranges("1,x", 1, 247, &keep));
  CHECK(!parse_ranges("300", 1, 247, &keep));
  CHECK_EQ(keep.size(), 1u);  // untouched on error
  TraceFilter f;
  CHECK(f.address_ok(9) && f.block_ok(1, 1));
  parse_ranges("5000-5010", 1, 65535, &f.registers);
  CHECK(f.block_ok(4990, 11));   // 4990..5000 overlaps
  CHECK(!f.block_ok(4990, 10));  // ..4999
  CHECK(f.row_ok(5010, 2));
  CHECK(!f.row_ok(5011, 1));
  CHECK(contains(f.describe(), "registers 5000-5010"));
}

TEST(change_tracker_and_rate) {
  ChangeTracker c;
  const uint8_t a[2] = {1, 2}, b[2] = {1, 3};
  CHECK(c.changed(1, a, 2));
  CHECK(!c.changed(1, a, 2));
  CHECK(c.changed(1, b, 2));
  CHECK(c.changed(2, b, 2));
  CHECK_EQ(c.size(), 2u);
  RateLimiter rl;
  CHECK(rl.allow(7, 1000, 500));
  CHECK(!rl.allow(7, 1400, 500));
  CHECK(rl.allow(7, 1500, 500));
  CHECK(rl.allow(8, 1500, 500));
  CHECK(rl.allow(7, 1501, 0));
}

TEST(scan_plan) {
  ScanPlan p;
  CHECK(!p.begin(10, 5, 100, 0));
  CHECK(p.begin(0, 3, 100, 0));  // clamped to 1..3
  CHECK_EQ(p.total(), 3u);
  uint8_t a = 0;
  CHECK(p.next(0, &a));
  CHECK_EQ(a, 1);
  CHECK(!p.next(0, &a));  // one at a time
  p.done(1, ProbeResult::TIMEOUT, 0, 0, 10);
  CHECK(!p.next(50, &a));  // interval
  CHECK(p.next(110, &a));
  CHECK_EQ(a, 2);
  p.done(2, ProbeResult::NOT_QUEUED, 0, 0, 120);  // retried
  CHECK(p.next(220, &a));
  CHECK_EQ(a, 2);
  p.done(2, ProbeResult::OK, 0, 0x0E0F, 230);
  CHECK(p.next(330, &a));
  CHECK_EQ(a, 3);
  p.done(3, ProbeResult::EXCEPTION, 2, 0, 340);
  CHECK(!p.active());
  CHECK_EQ(p.found().size(), 2u);
  std::string s = p.summary();
  CHECK(contains(s, "3/3 probed"));
  CHECK(contains(s, "2 (ok 0x0E0F)"));
  CHECK(contains(s, "3 (exception 2)"));
  ScanPlan q;
  q.begin(240, 255, 0, 0);
  CHECK_EQ(q.total(), 8u);  // 240..247
}

int main() { return th::run_all(); }
