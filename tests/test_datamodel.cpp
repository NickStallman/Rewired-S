// Host unit tests for components/sungrow_datamodel core (decoder, request builder,
// signal store, derived values). Build/run: make -C tests
#include <cmath>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "decoder.h"
#include "derived.h"
#include "register_tables.h"
#include "request_builder.h"
#include "signal_store.h"
#include "test_harness.h"

using namespace esphome::sungrow_datamodel;

static const WordOrder HW = WordOrder::HIGH_WORD_FIRST;
static const WordOrder LW = WordOrder::LOW_WORD_FIRST;
static const WordOrder RAWO = WordOrder::RAW;

// ---------------------------------------------------------------------------------------
// Decoder
TEST(decode_u16_s16) {
  const uint8_t w1[] = {0x12, 0x34};
  for (WordOrder wo : {HW, LW}) {
    Decoded d = decode(w1, 1, TYPE_U16, wo);
    CHECK(d.present);
    CHECK_EQ(d.bits, 0x1234u);
    CHECK_EQ(d.nbytes, 2);
  }
  CHECK_EQ(decode(w1, 1, TYPE_U16, RAWO).bits, 0x3412u);  // raw copy into LE host
  const uint8_t neg[] = {0xFF, 0xFE};
  CHECK_EQ(bits_to_int(decode(neg, 1, TYPE_S16, LW).bits, TYPE_S16, 1), -2);
  CHECK(decode(neg, 1, TYPE_S16, LW).present);
  const uint8_t s16s[] = {0x7F, 0xFF}, u16s[] = {0xFF, 0xFF};
  CHECK(!decode(s16s, 1, TYPE_S16, LW).present);
  CHECK(!decode(u16s, 1, TYPE_U16, LW).present);
  CHECK(decode(u16s, 1, TYPE_S16, LW).present);   // 0xFFFF is -1 for s16, not a sentinel
  CHECK(decode(s16s, 1, TYPE_U16, LW).present);
  double v;
  const uint8_t volts[] = {0x09, 0x01};  // 2305 x 0.1
  CHECK(decode_number(volts, 1, TYPE_U16, 0.1f, LW, &v));
  CHECK_NEAR(v, 230.5, 1e-4);
  CHECK(decode_number(volts, 1, TYPE_U16, 0.0f, LW, &v));
  CHECK_NEAR(v, 2305, 0);
  CHECK(!decode_number(u16s, 1, TYPE_U16, 0.1f, LW, &v));
}

TEST(decode_32bit_word_orders) {
  const uint8_t w[] = {0x00, 0x01, 0x00, 0x02};  // reg0 = 1, reg1 = 2
  CHECK_EQ(decode(w, 2, TYPE_U32, LW).bits, 0x00020001u);
  CHECK_EQ(decode(w, 2, TYPE_U32, HW).bits, 0x00010002u);
  CHECK_EQ(decode(w, 2, TYPE_U32, RAWO).bits, 0x02000100u);
  CHECK_EQ(decode(w, 2, TYPE_U32, WordOrder::UNSWAPPED).bits, 0x02000100u);
  // -1000 = 0xFFFFFC18, low word first: reg0 = FC18, reg1 = FFFF
  const uint8_t n[] = {0xFC, 0x18, 0xFF, 0xFF};
  CHECK_EQ(bits_to_int(decode(n, 2, TYPE_S32, LW).bits, TYPE_S32, 2), -1000);
  double v;
  CHECK(decode_number(n, 2, TYPE_S32, 0.001f, LW, &v));
  CHECK_NEAR(v, -1.0, 1e-6);
  // sentinels
  const uint8_t s32_lw[] = {0xFF, 0xFF, 0x7F, 0xFF}, s32_hw[] = {0x7F, 0xFF, 0xFF, 0xFF};
  CHECK(!decode(s32_lw, 2, TYPE_S32, LW).present);
  CHECK(!decode(s32_hw, 2, TYPE_S32, HW).present);
  CHECK(decode(s32_lw, 2, TYPE_S32, HW).present);  // 0xFFFF7FFF in mode 0 is a real value
  const uint8_t ones[] = {0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(!decode(ones, 2, TYPE_U32, LW).present);
  CHECK(decode(ones, 2, TYPE_S32, LW).present);  // -1
}

TEST(decode_64bit) {
  // regs 1,2,3,4. Mode 2 with 4 registers is stock's (r1<<16|r0)<<32 | (r3<<16|r2).
  const uint8_t w[] = {0, 1, 0, 2, 0, 3, 0, 4};
  CHECK(decode(w, 4, TYPE_U64, LW).bits == 0x0002000100040003ULL);
  CHECK(decode(w, 4, TYPE_U64, HW).bits == 0x0001000200030004ULL);
  CHECK_EQ(decode(w, 4, TYPE_U64, LW).nbytes, 8);
  const uint8_t u64s[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(!decode(u64s, 4, TYPE_U64, LW).present);
  // s64 sentinel 0x7FFF_FFFF_FFFF_FFFF in mode 2: r1 = 0x7FFF carries the top word.
  const uint8_t s64s[] = {0xFF, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(!decode(s64s, 4, TYPE_S64, LW).present);
  CHECK(decode(s64s, 4, TYPE_S64, HW).present);
  // mode 3 (UNSWAPPED) = raw copy, like RAW
  CHECK(decode(w, 4, TYPE_U64, WordOrder::UNSWAPPED).bits == decode(w, 4, TYPE_U64, RAWO).bits);
  CHECK_NEAR(bits_to_double(0xFFFFFFFFFFFFFFF0ULL, TYPE_U64, 4), 1.8446744073709552e19, 1e5);
  CHECK_NEAR(bits_to_double(0xFFFFFFFFFFFFFFF0ULL, TYPE_S64, 4), -16, 0);
}

TEST(decode_string_array_type9) {
  const uint8_t s[] = {'A', '1', '2', 'B', 0, 0};
  Decoded d = decode(s, 3, TYPE_STRING, LW);
  CHECK(d.kind == ValueKind::STRING);
  CHECK(d.present);
  CHECK_EQ(d.nbytes, 6);
  const uint8_t e[] = {0, 'x', 0, 0};
  CHECK(!decode(e, 2, TYPE_STRING, LW).present);
  // type 9: IEEE float32 over 2 registers (word order as the device), no sentinel, NaN -> 0
  const uint8_t m[] = {0xFF, 0xFF, 0xFF, 0xFF};  // a NaN pattern
  Decoded d9 = decode(m, 2, TYPE_F32_WIRE, LW);
  CHECK(d9.present);
  CHECK(d9.kind == ValueKind::FLOAT);
  CHECK_EQ(bits_to_float(static_cast<uint32_t>(d9.bits)), 0.0f);
  // 230.5f = 0x43668000; low word first on the wire: 8000 4366
  const uint8_t f_lw[] = {0x80, 0x00, 0x43, 0x66}, f_hw[] = {0x43, 0x66, 0x80, 0x00};
  CHECK_EQ(bits_to_float(static_cast<uint32_t>(decode(f_lw, 2, TYPE_F32_WIRE, LW).bits)), 230.5f);
  CHECK_EQ(bits_to_float(static_cast<uint32_t>(decode(f_hw, 2, TYPE_F32_WIRE, HW).bits)), 230.5f);
  const uint8_t qnan[] = {0x00, 0x00, 0x7F, 0xC0};  // 0x7fc00000 (stock logs it) -> 0
  CHECK_EQ(bits_to_float(static_cast<uint32_t>(decode(qnan, 2, TYPE_F32_WIRE, LW).bits)), 0.0f);
  double fv;
  CHECK(decode_number(f_lw, 2, TYPE_F32_WIRE, 0.1f, LW, &fv));
  CHECK_NEAR(fv, 23.05, 1e-4);
  CHECK(type_is_signed(TYPE_F32_WIRE));
  CHECK_EQ(scalar_regs(TYPE_F32_WIRE, 2), 2);
  // u16 x 3 -> array; absent iff first reg is the sentinel
  const uint8_t a[] = {0x00, 0x05, 0xFF, 0xFF, 0x00, 0x01};
  Decoded da = decode(a, 3, TYPE_U16, LW);
  CHECK(da.kind == ValueKind::ARRAY);
  CHECK(da.present);
  CHECK(!decode(a + 2, 2, TYPE_U16, LW).present);
  // u32 x 1 (odd tcpslave rows) -> 16-bit scalar
  CHECK(value_kind(TYPE_U32, 1) == ValueKind::SCALAR);
  // ... and never a sentinel (stock tests 4 bytes whose upper half is zero)
  const uint8_t ff[] = {0xFF, 0xFF};
  CHECK(decode(ff, 1, TYPE_U32, LW).present);
  CHECK_EQ(decode(ff, 1, TYPE_U32, LW).bits, 0xFFFFu);
  CHECK(!is_sentinel(0x7FFF, TYPE_S32, 1));
}

TEST(disassemble_roundtrip) {
  uint8_t w[8];
  for (WordOrder wo : {HW, LW, RAWO}) {
    for (uint8_t n : {1, 2, 4}) {
      uint64_t v = 0x1122334455667788ULL & (n == 4 ? ~0ULL : ((1ULL << (16 * n)) - 1));
      disassemble(v, n, wo, w);
      CHECK(assemble(w, n, wo) == v);
    }
  }
  disassemble(0x00020001, 2, LW, w);
  const uint8_t exp[] = {0, 1, 0, 2};
  CHECK(std::memcmp(w, exp, 4) == 0);
}

TEST(bitfield_and_capability) {
  std::vector<int> on;
  size_t n = expand_bits(0x8005, 16, [](uint8_t bit, bool v, void *ctx) {
    if (v) static_cast<std::vector<int> *>(ctx)->push_back(bit);
  }, &on);
  CHECK_EQ(n, 16u);
  CHECK_EQ(on.size(), 3u);
  CHECK(on == (std::vector<int>{0, 2, 15}));
  CHECK_EQ(expand_bits(~0ULL, 200, [](uint8_t, bool, void *) {}, nullptr), 64u);
  uint8_t bm[6] = {0x00, 0x01, 0x80, 0x00, 0, 0};  // word1 bit0, word2 bit15
  CHECK(capability_bit_set(bm, 6, 1, 0));
  CHECK(!capability_bit_set(bm, 6, 1, 1));
  CHECK(capability_bit_set(bm, 6, 2, 15));
  CHECK(capability_bit_set(bm, 6, 0, 9));    // no dependency
  CHECK(!capability_bit_set(bm, 6, 4, 0));   // out of range
  CHECK(!capability_bit_set(nullptr, 0, 1, 0));
}

// ---------------------------------------------------------------------------------------
// Request builder
static RegisterRow row(uint16_t sig, uint16_t addr, uint8_t nreg, uint8_t fn = 4, uint16_t group = 1,
                       uint8_t type = TYPE_U16, float scale = 0, uint8_t dep_word = 0, uint8_t dep_bit = 0,
                       uint8_t tag = 0) {
  RegisterRow r{};
  r.sigid = sig;
  r.regaddr = addr;
  r.nreg = nreg;
  r.fn = fn;
  r.group = group;
  r.type = type;
  r.key = NO_KEY;
  r.scale = scale;
  r.dep_word = dep_word;
  r.dep_bit = dep_bit;
  r.tag = tag;
  return r;
}
static RegisterTable tbl(const std::vector<RegisterRow> &rows, Family f = Family::INVERTER) {
  return RegisterTable{99, f, "test", rows.data(), static_cast<uint16_t>(rows.size())};
}

TEST(request_merge_split) {
  // Stock grouping rules: one pass per fn (3 first, then 4); group changes merge.
  std::vector<RegisterRow> rows = {
      row(1, 100, 1), row(2, 101, 2), row(3, 150, 1),  // merged (gap spanning)
      row(4, 219, 1, 4, 20),                            // span 120, other group -> still merged
      row(5, 220, 1),                                   // span 121 -> split
      row(6, 230, 2, 3),                                // fn 3: own pass, does not break fn 4
      row(7, 232, 1, 3, 30),                            // fn 3, group change -> merged with 6
      row(8, 200, 1, 3, 30),                            // address lower: u16 span wraps -> split
      row(9, 300, 4, 16, 30),                           // write row: skipped
      row(10, 205, 1, 3, 30),                           // merges with 8
      row(11, 221, 1, 4, 1, TYPE_U16, 0, 0, 0, TAG_END_REQUEST),  // fn 4 again: joins 220, closes
      row(12, 222, 1),                                  // new node after the tag-bit0 row
      row(13, 14701, 1, 4, 1),                          // 0x396d, same group as node: merged? no:
                                                        //   span too big -> its own node
      row(14, 14702, 1, 4, 600),                        // group change merges
      row(15, 14701, 1, 4, 600),                        // 0x396d with another group: skipped
  };
  RegisterTable t = tbl(rows);
  auto reqs = build_requests(t);
  CHECK_EQ(reqs.size(), 6u);
  // fn 3 first
  CHECK_EQ(reqs[0].fn, 3);
  CHECK_EQ(reqs[0].start, 230);
  CHECK_EQ(reqs[0].count, 3);
  CHECK_EQ(reqs[0].group, 1);  // first row's group
  CHECK_EQ(reqs[1].fn, 3);
  CHECK_EQ(reqs[1].start, 200);
  CHECK_EQ(reqs[1].count, 6);
  CHECK_EQ(reqs[1].group, 30);
  // fn 4
  CHECK_EQ(reqs[2].start, 100);
  CHECK_EQ(reqs[2].count, 120);
  CHECK_EQ(reqs[2].wire_start(), 99);
  CHECK_EQ(reqs[2].group, 1);
  CHECK_EQ(reqs[2].first_row, 0);
  CHECK_EQ(reqs[2].last_row, 3);
  CHECK_EQ(reqs[3].start, 220);
  CHECK_EQ(reqs[3].count, 2);   // closed by the tag-bit0 row 221
  CHECK_EQ(reqs[4].start, 222);
  CHECK_EQ(reqs[4].count, 1);
  CHECK_EQ(reqs[5].start, 14701);
  CHECK_EQ(reqs[5].count, 2);   // 14701 + 14702; the second 14701 (group 600) skipped
  CHECK_EQ(reqs[5].last_row, 13);
  // tag bit0 is ignored when the next table row has another fn
  std::vector<RegisterRow> r2 = {row(1, 100, 1, 4, 1, TYPE_U16, 0, 0, 0, TAG_END_REQUEST), row(2, 50, 1, 3),
                                 row(3, 101, 1)};
  RegisterTable t2 = tbl(r2);
  auto q2 = build_requests(t2);
  CHECK_EQ(q2.size(), 2u);
  CHECK_EQ(q2[1].start, 100);
  CHECK_EQ(q2[1].count, 2);
  // group filter / fn filter
  RequestBuildOptions o;
  o.group = 30;
  CHECK_EQ(build_requests(t, o).size(), 2u);
  o = RequestBuildOptions();
  o.include_holding = false;
  CHECK_EQ(build_requests(t, o).size(), 4u);
  // row filter: rejected rows are skipped like other-fn rows
  o = RequestBuildOptions();
  o.filter = [](const RegisterRow &r) { return r.sigid != 5; };
  auto q3 = build_requests(t, o);
  CHECK_EQ(q3.size(), 6u);
  CHECK_EQ(q3[3].start, 221);
}

TEST(request_stock_node_counts) {
  // Node counts of the stock builder (tests/fixtures/stock_request_nodes.py; tests/test_request_nodes.py
  // compares the nodes one by one). inv_proto3 has one node more than stock: total battery
  // charge reads the full u32 at 13041 instead of the truncated 5673 (inverter.yaml `fixes`).
  struct {
    const char *name;
    size_t nodes;
  } want[] = {{"inv_proto2", 54},         {"inv_proto3", 54},         {"inv_proto1_4", 18},
              {"inv_proto5", 12},         {"battery", 6},             {"charger", 3},
              {"energy_bridge", 2},       {"other_dev18_st6100", 1},  {"other_dev18_st6200", 1},
              {"other_dev18_st6300", 1},  {"other_dev34_st1819", 2},  {"other_dev34_st1820", 2}};
  for (auto &w : want) {
    auto reqs = build_requests(*find_table(w.name));
    if (reqs.size() != w.nodes)
      std::printf("  %s: %zu nodes, stock %zu\n", w.name, reqs.size(), w.nodes);
    CHECK_EQ(reqs.size(), w.nodes);
  }
  // proto2 mixes groups in one node: fn 3 @30051 x65 keeps its first row's 36000
  bool mixed = false;
  for (auto &q : build_requests(get_table(TableId::INV_PROTO2)))
    mixed |= q.fn == 3 && q.start == 30051 && q.count == 65 && q.group == 36000;
  CHECK(mixed);
}

TEST(request_cap) {
  std::vector<RegisterRow> rows;
  for (int i = 0; i < 70; i++)
    rows.push_back(row(i + 1, 1000 + 200 * i, 1));
  RegisterTable t = tbl(rows);
  bool overflow = false;
  auto reqs = build_requests(t, RequestBuildOptions(), &overflow);
  CHECK_EQ(reqs.size(), 59u);
  CHECK(overflow);
  ReadRequest small[10];
  RequestBuildResult r = build_requests(t, RequestBuildOptions(), small, 10);
  CHECK_EQ(r.count, 10u);
  CHECK(r.overflow);
}

TEST(request_real_tables) {
  for (size_t ti = 0; ti < kTableCount; ti++) {
    const RegisterTable &t = kTables[ti];
    bool overflow = true;
    auto reqs = build_requests(t, RequestBuildOptions(), &overflow);
    CHECK(!overflow);
    CHECK(reqs.size() <= 59);
    // every read row is covered by exactly one request it was merged into
    for (uint16_t i = 0; i < t.count; i++) {
      const RegisterRow &r = t.rows[i];
      if (r.fn != 3 && r.fn != 4)
        continue;
      int hits = 0;
      for (auto &q : reqs) {
        CHECK(q.count >= 1 && q.count <= 120);
        if (i >= q.first_row && i <= q.last_row && q.fn == r.fn && r.regaddr >= q.start &&
            r.regaddr + r.nreg <= q.start + q.count)
          hits++;
      }
      if (hits != 1)
        std::printf("  table %s row %u not covered (%d)\n", t.name, i, hits);
      CHECK(hits == 1);
    }
  }
  CHECK(find_table("inv_proto2") == &get_table(TableId::INV_PROTO2));
  CHECK(find_table("nope") == nullptr);
  CHECK(inverter_table_for_protocol(4) == &get_table(TableId::INV_PROTO1_4));
  CHECK(tcpslave_table_for_dev_type(21) == &get_table(TableId::TCPSLAVE_DEV1_21));
  CHECK_EQ(get_table(TableId::INV_PROTO2).count, 623);
}

// ---------------------------------------------------------------------------------------
// SignalStore / ingest
TEST(ingest_synthetic_block) {
  std::vector<RegisterRow> rows = {
      row(0x10, 5000, 1, 4, 1, TYPE_U16, 0.1f),
      row(0x11, 5001, 2, 4, 1, TYPE_S32, 0.001f),
      row(0x12, 5003, 4, 4, 1, TYPE_STRING),
      row(0x13, 5007, 1, 4, 1, TYPE_U16),          // sentinel
      row(0x14, 5008, 2, 4, 1, TYPE_U32),          // only half inside block -> skipped
      row(0x00, 5005, 1, 4, 1, TYPE_U16),          // sigid 0 gap row -> never stored
      row(0x15, 5000, 1, 3, 1, TYPE_U16),          // holding register, other address space
  };
  RegisterTable t = tbl(rows, Family::CHARGER);
  SignalStore st;
  uint32_t clock = 1000;
  st.set_clock([&] { return clock; });
  int all = 0, sub = 0;
  st.add_listener([&](const Signal &) { all++; });
  st.subscribe(0, 0x10, [&](const Signal &s) {
    sub++;
    CHECK(s.sigid == 0x10);
  });
  const uint8_t data[] = {
      0x09, 0x01,              // 5000 = 2305 -> 230.5
      0xFC, 0x18, 0xFF, 0xFF,  // 5001 = -1000 (low word first) -> -1.0
      'S', 'G', '1', '0', 'R', 'T', 0, 0,  // 5003..5006
      0xFF, 0xFF,              // 5007 sentinel
      0x00, 0x01,              // 5008 (first half of 0x14)
  };
  size_t n = st.ingest_block(0, t, 4, 5000, data, 9);
  CHECK_EQ(n, 4u);
  CHECK_NEAR(st.get_number(0, 0x10), 230.5, 1e-4);
  CHECK_NEAR(st.get_number(0, 0x11), -1.0, 1e-6);
  CHECK(st.get_string(0, 0x12) == "SG10RT");
  CHECK(std::isnan(st.get_number(0, 0x12)));
  CHECK(!st.is_valid(0, 0x13));
  CHECK(st.get(0, 0x14) == nullptr);
  CHECK(st.get(0, 0x15) == nullptr);
  CHECK(st.get(0, 0x00) == nullptr);
  CHECK(st.get(1, 0x10) == nullptr);  // other device untouched
  CHECK_EQ(st.get(0, 0x10)->updated_ms, 1000u);
  CHECK_EQ(all, 3);  // 0x13 never valid -> no notification
  CHECK_EQ(sub, 1);
  // same data again: no change callbacks, timestamps refresh
  clock = 2000;
  st.ingest_block(0, t, 4, 5000, data, 9);
  CHECK_EQ(all, 3);
  CHECK_EQ(st.get(0, 0x10)->updated_ms, 2000u);
  // value change
  uint8_t d2[sizeof(data)];
  std::memcpy(d2, data, sizeof(data));
  d2[1] = 0x02;
  st.ingest_block(0, t, 4, 5000, d2, 9);
  CHECK_EQ(sub, 2);
  CHECK_NEAR(st.get_number(0, 0x10), 230.6, 1e-4);
  // high-word-first device
  const uint8_t hw[] = {0x00, 0x00, 0xFF, 0xFF, 0xFC, 0x18};
  st.ingest_block(2, t, 4, 5000, hw, 3, WordOrder::HIGH_WORD_FIRST);
  CHECK_NEAR(st.get_number(2, 0x11), -1.0, 1e-6);
  // timeout -> invalidate
  size_t c = st.invalidate_block(0, t, 4, 5000, 9);
  CHECK_EQ(c, 4u);
  CHECK(std::isnan(st.get_number(0, 0x10)));
  CHECK(st.get_string(0, 0x12).empty());
  CHECK_EQ(sub, 3);
  st.clear_device(2);
  CHECK(!st.is_valid(2, 0x11));
  // row filter: only store 0x10
  SignalStore sf;
  sf.set_row_filter([](const RegisterTable &, const RegisterRow &r) { return r.sigid == 0x10; });
  CHECK_EQ(sf.ingest_block(0, t, 4, 5000, data, 9), 1u);
  CHECK_EQ(sf.size(), 1u);
  CHECK(static_cast<int>(sizeof(RegisterRow)) == 20);
}

TEST(ingest_capability_gating) {
  std::vector<RegisterRow> rows = {
      row(SIG_CAPABILITY_BITMAP, 100, 2, 4, 1, TYPE_STRING),
      row(0x20, 102, 1, 4, 1, TYPE_U16, 0, 1, 0),   // needs word1 bit0
      row(0x21, 103, 1, 4, 1, TYPE_U16, 0, 1, 1),   // needs word1 bit1
      row(0x22, 104, 1, 4, 1, TYPE_U16, 0, 0, 0),   // no dependency
  };
  RegisterTable t = tbl(rows);
  SignalStore st;
  const uint8_t data[] = {0x00, 0x01, 0x00, 0x00, 0, 5, 0, 6, 0, 7};
  st.ingest_block(0, t, 4, 100, data, 5, LW, true);
  CHECK_EQ((int) st.get_number(0, 0x20), 5);
  CHECK(!st.is_valid(0, 0x21));
  CHECK_EQ((int) st.get_number(0, 0x22), 7);
  // without gating every row decodes
  SignalStore st2;
  st2.ingest_block(0, t, 4, 100, data, 5, LW, false);
  CHECK_EQ((int) st2.get_number(0, 0x21), 6);
}

// ---------------------------------------------------------------------------------------
// SBR cells + MPPT total
static void put16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

TEST(sbr_cell_decode_and_guard) {
  const RegisterTable &bt = get_table(TableId::BATTERY);
  uint8_t blk[48] = {};
  put16(blk + 0, 34512);   // max 3.4512 V
  put16(blk + 2, 0x0203);  // module 2 cell 3
  put16(blk + 4, 33001);   // min 3.3001 V
  put16(blk + 6, 0x0105);  // module 1 cell 5
  put16(blk + 8, 253);     // 25.3 C
  put16(blk + 10, 0x0400);
  put16(blk + 12, 0xFFCE); // -5.0 C
  put16(blk + 14, 0x0200);
  put16(blk + 16, 34000);  // module 1 max
  put16(blk + 18, 34512);  // module 2 max
  // modules 3..8 max = 0 (absent)
  put16(blk + 32, 33001);  // module 1 min
  put16(blk + 34, 33100);  // module 2 min
  SignalStore st;
  size_t n = st.ingest_block(1, bt, 4, SBR_CELL_BLOCK_START, blk, 24);
  CHECK_EQ(n, 24u);
  CHECK_NEAR(st.get_number(1, kSbrCellSigids[0]), 3.4512, 1e-6);
  CHECK_NEAR(st.get_number(1, kSbrCellSigids[2]), 3.3001, 1e-6);
  CHECK_NEAR(st.get_number(1, SIG_CELL_VOLTAGE_SPREAD), 0.1511, 1e-5);
  CHECK_EQ((int) st.get_number(1, SIG_CELL_VOLTAGE_MAX_MODULE), 2);
  CHECK_EQ((int) st.get_number(1, SIG_CELL_VOLTAGE_MAX_CELL), 3);
  CHECK_EQ((int) st.get_number(1, SIG_CELL_VOLTAGE_MIN_MODULE), 1);
  CHECK_EQ((int) st.get_number(1, SIG_CELL_VOLTAGE_MIN_CELL), 5);
  CHECK_EQ((int) st.get_number(1, SIG_MODULE_TEMPERATURE_MAX_MODULE), 4);
  CHECK_EQ((int) st.get_number(1, SIG_MODULE_TEMPERATURE_MIN_MODULE), 2);
  CHECK_NEAR(st.get_number(1, kSbrCellSigids[4]), 25.3, 1e-4);
  CHECK_NEAR(st.get_number(1, kSbrCellSigids[6]), -5.0, 1e-4);
  CHECK_NEAR(st.get_number(1, kSbrCellSigids[8]), 3.4, 1e-6);   // module 1 max
  CHECK(!st.is_valid(1, kSbrCellSigids[10]));                    // module 3 max absent
  CHECK_NEAR(st.get_number(1, kSbrCellSigids[17]), 3.31, 1e-6);  // module 2 min
  CHECK(!st.is_valid(1, kSbrCellSigids[23]));
  // the battery table row for 10757 must be named by the generator overlay
  bool named = false;
  for (uint16_t i = 0; i < bt.count; i++)
    if (bt.rows[i].regaddr == SBR_CELL_BLOCK_START)
      named = bt.rows[i].key != NO_KEY && std::string(kRowKeys[bt.rows[i].key]) == "cell_voltage_max" &&
              std::fabs(bt.rows[i].scale - 0.0001f) < 1e-9;
  CHECK(named);
  // position byte 0xFF / 0 -> absent
  put16(blk + 2, 0x00FF);
  st.ingest_block(1, bt, 4, SBR_CELL_BLOCK_START, blk, 24);
  CHECK(!st.is_valid(1, SIG_CELL_VOLTAGE_MAX_MODULE));
  CHECK(!st.is_valid(1, SIG_CELL_VOLTAGE_MAX_CELL));
  // implausible layout (different battery model): 5.0 V max -> whole block cleared
  put16(blk + 0, 50000);
  st.ingest_block(1, bt, 4, SBR_CELL_BLOCK_START, blk, 24);
  CHECK(!st.is_valid(1, kSbrCellSigids[0]));
  CHECK(!st.is_valid(1, kSbrCellSigids[8]));
  CHECK(!st.is_valid(1, SIG_CELL_VOLTAGE_SPREAD));
  {
    SignalStore s2;  // direct status check without ingest (decode is not idempotent)
    s2.set_scalar(1, kSbrCellSigids[0], TYPE_U16, 0.0001f, 50000, 2);
    s2.set_scalar(1, kSbrCellSigids[2], TYPE_U16, 0.0001f, 33001, 2);
    CHECK(decode_sbr_cells(s2, 1) == SbrStatus::IMPLAUSIBLE);
    s2.set_scalar(1, kSbrCellSigids[0], TYPE_U16, 0.0001f, 34000, 2);
    s2.set_scalar(1, kSbrCellSigids[2], TYPE_U16, 0.0001f, 33001, 2);
    CHECK(decode_sbr_cells(s2, 1) == SbrStatus::OK);
  }
  // min > max is implausible too
  CHECK(!sbr_cells_plausible(3.5, 3.4));
  CHECK(!sbr_cells_plausible(1.4, 3.4));
  CHECK(sbr_cells_plausible(1.5, 4.5));
  // absent
  uint8_t zero[48] = {};
  st.ingest_block(1, bt, 4, SBR_CELL_BLOCK_START, zero, 24);
  CHECK(decode_sbr_cells(st, 1) == SbrStatus::ABSENT);
}

TEST(mppt_total_power) {
  const RegisterTable &t = get_table(TableId::INV_PROTO1_4);  // MPPT1..3 at 5011..5016
  uint8_t blk[12];
  put16(blk + 0, 3000);    // MPPT1 300.0 V
  put16(blk + 2, 50);      // 5.0 A
  put16(blk + 4, 2000);    // MPPT2 200.0 V
  put16(blk + 6, 20);      // 2.0 A
  put16(blk + 8, 0xFFFF);  // MPPT3 V absent
  put16(blk + 10, 10);
  SignalStore st;
  st.ingest_block(0, t, 4, 5011, blk, 6);
  CHECK_NEAR(st.get_number(0, SIG_MPPT_TOTAL_POWER), 1900.0, 1e-3);
  st.invalidate_block(0, t, 4, 5011, 6);
  CHECK(!st.is_valid(0, SIG_MPPT_TOTAL_POWER));
  CHECK(is_mppt_sigid(0x2040));
  CHECK(!is_mppt_sigid(0x2080));
  CHECK(kMpptPairCount >= 12);
}

TEST(mppt_power_per_pair) {
  const RegisterTable &t = get_table(TableId::INV_PROTO1_4);
  uint8_t blk[12];
  put16(blk + 0, 3000);
  put16(blk + 2, 50);
  put16(blk + 4, 2000);
  put16(blk + 6, 20);
  put16(blk + 8, 0xFFFF);
  put16(blk + 10, 10);
  SignalStore st;
  st.ingest_block(0, t, 4, 5011, blk, 6);
  CHECK_NEAR(st.get_number(0, SIG_MPPT_POWER_BASE + 1), 1500.0, 1e-3);
  CHECK_NEAR(st.get_number(0, SIG_MPPT_POWER_BASE + 2), 400.0, 1e-3);
  CHECK(!st.is_valid(0, SIG_MPPT_POWER_BASE + 3));
  CHECK(kMpptPairs[0].mppt == 1);
}

TEST(ingest_tag_keep_sentinel_and_float_rows) {
  std::vector<RegisterRow> rows = {
      row(0x30, 100, 1, 4, 1, TYPE_U16, 0, 0, 0, TAG_KEEP_SENTINEL),
      row(0x31, 101, 1, 4, 1, TYPE_U16),
      row(0x32, 102, 2, 4, 1, TYPE_F32_WIRE, 0.1f),
  };
  RegisterTable t = tbl(rows, Family::METER);
  // 0xFFFF, 0xFFFF, 2305.0f (0x45101000) low word first
  const uint8_t data[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x10, 0x00, 0x45, 0x10};
  SignalStore st;
  st.ingest_block(0, t, 4, 100, data, 4);
  CHECK(st.is_valid(0, 0x30));  // tag bit4: stored although it is the u16 sentinel
  CHECK_EQ((int) st.get_number(0, 0x30), 0xFFFF);
  CHECK(!st.is_valid(0, 0x31));
  CHECK(st.get(0, 0x32)->kind == ValueKind::FLOAT);
  // Default: the float is in engineering units; the 0.1 scale is only a decimals hint.
  CHECK(!st.float_scale());
  CHECK_NEAR(st.get_number(0, 0x32), 2305.0, 1e-3);
  CHECK_EQ(st.get(0, 0x32)->scale, 0.0f);
  SignalStore scaled;
  scaled.set_float_scale(true);
  scaled.ingest_block(0, t, 4, 100, data, 4);
  CHECK_NEAR(scaled.get_number(0, 0x32), 230.5, 1e-3);
}

TEST(offline_clears_only_tag_bit1_rows) {
  std::vector<RegisterRow> rows = {
      row(0x40, 100, 1, 4, 1, TYPE_U16, 0, 0, 0, TAG_CLEAR_OFFLINE),
      row(0x41, 101, 1, 4, 1, TYPE_U16, 0, 0, 0, 0),
      row(0x42, 102, 1, 4, 1, TYPE_U16, 0, 0, 0, TAG_CLEAR_OFFLINE | TAG_END_REQUEST),
  };
  RegisterTable t = tbl(rows);
  const uint8_t data[] = {0, 1, 0, 2, 0, 3};
  SignalStore st;
  st.ingest_block(0, t, 4, 100, data, 3);
  CHECK_EQ(st.clear_offline(0, t), 2u);
  CHECK(!st.is_valid(0, 0x40));
  CHECK_EQ((int) st.get_number(0, 0x41), 2);
  CHECK(!st.is_valid(0, 0x42));
}

TEST(capability_missing_bitmap_means_absent) {
  std::vector<RegisterRow> rows = {
      row(0x20, 102, 1, 4, 1, TYPE_U16, 0, 1, 0),  // needs word1 bit0
      row(0x22, 103, 1, 4, 1, TYPE_U16, 0, 0, 0),  // no dependency
  };
  RegisterTable t = tbl(rows);
  const uint8_t data[] = {0, 5, 0, 7};
  SignalStore st;
  st.ingest_block(0, t, 4, 102, data, 2, LW, true);
  CHECK(!st.is_valid(0, 0x20));  // stock: no bitmap = zeroed buffer = bit clear
  CHECK_EQ((int) st.get_number(0, 0x22), 7);
  CHECK(!st.capability_ok(0, 1, 0));
  CHECK(st.capability_ok(0, 0, 0));
}

static void put32lw(uint8_t *p, uint32_t v) {
  put16(p, static_cast<uint16_t>(v));
  put16(p + 2, static_cast<uint16_t>(v >> 16));
}

TEST(total_yield_hooks) {
  // inv_proto1_4: 0x2022 @5004 (kWh), 0x2023 @5144 (0.1 kWh)
  const RegisterTable &p14 = get_table(TableId::INV_PROTO1_4);
  SignalStore st;
  uint8_t b[4];
  put32lw(b, 1234);  // 1234 kWh
  st.ingest_block(0, p14, 4, 5004, b, 2);
  CHECK_NEAR(st.get_number(0, SIG_TOTAL_YIELD), 1234.0, 1e-3);  // 0x2022 x10 in 0.1 kWh
  CHECK_EQ(st.get(0, SIG_TOTAL_YIELD)->len, 8);
  put32lw(b, 12345);  // 1234.5 kWh
  st.ingest_block(0, p14, 4, 5144, b, 2);
  CHECK_NEAR(st.get_number(0, SIG_TOTAL_YIELD), 1234.5, 1e-3);  // 0x2023 wins
  put32lw(b, 0);  // 0x2023 == 0 -> falls back to 0x2022 x10
  st.ingest_block(0, p14, 4, 5144, b, 2);
  CHECK_NEAR(st.get_number(0, SIG_TOTAL_YIELD), 1234.0, 1e-3);
  put32lw(b, 0xFFFFFFFF);  // 0x2022 invalid and 0x2023 == 0 -> all cleared
  st.ingest_block(0, p14, 4, 5004, b, 2);
  CHECK(!st.is_valid(0, SIG_TOTAL_YIELD));
  CHECK(!st.is_valid(0, SIG_YIELD_TOTAL_01KWH));
  // inv_proto2: 0x2024 @5228 (0.1 kWh) -> 0x2021, 0x2023 = v, 0x2022 = v / 10
  const RegisterTable &p2 = get_table(TableId::INV_PROTO2);
  SignalStore s2;
  put32lw(b, 98765);
  s2.ingest_block(0, p2, 4, 5228, b, 2);
  CHECK_NEAR(s2.get_number(0, SIG_TOTAL_YIELD), 9876.5, 1e-3);
  CHECK_NEAR(s2.get_number(0, SIG_YIELD_TOTAL_01KWH), 9876.5, 1e-3);
  CHECK_NEAR(s2.get_number(0, SIG_YIELD_TOTAL_KWH), 9876.0, 1e-3);
  put32lw(b, 0xFFFFFFFF);
  s2.ingest_block(0, p2, 4, 5228, b, 2);
  CHECK(!s2.is_valid(0, SIG_TOTAL_YIELD));
  CHECK(!s2.is_valid(0, SIG_YIELD_TOTAL_KWH));
}

TEST(grid_import_export_power) {
  const RegisterTable &p2 = get_table(TableId::INV_PROTO2);  // export_power 0x2501 s32 @5216, 0.001 kW
  SignalStore st;
  uint8_t b[4];
  put32lw(b, static_cast<uint32_t>(-2500));  // importing 2.5 kW
  st.ingest_block(0, p2, 4, 5216, b, 2);
  CHECK_NEAR(st.get_number(0, SIG_GRID_IMPORT_POWER), 2.5, 1e-6);
  CHECK_NEAR(st.get_number(0, SIG_GRID_EXPORT_POWER), 0.0, 1e-9);
  put32lw(b, 1200);
  st.ingest_block(0, p2, 4, 5216, b, 2);
  CHECK_NEAR(st.get_number(0, SIG_GRID_IMPORT_POWER), 0.0, 1e-9);
  CHECK_NEAR(st.get_number(0, SIG_GRID_EXPORT_POWER), 1.2, 1e-6);
  put32lw(b, 0x7FFFFFFF);  // s32 sentinel
  st.ingest_block(0, p2, 4, 5216, b, 2);
  CHECK(!st.is_valid(0, SIG_GRID_IMPORT_POWER));
}

// Stock dev-35 cloud hooks: raw copies, source chosen by
// functionEnum bit 22 (+ 0x20F1 > 0x010004FF for the plant powers), sign splits, gated on dev_type 35.
TEST(hybrid_cloud_hooks) {
  SignalStore st;
  auto u = [&](uint16_t sig, uint8_t type, uint64_t v, uint8_t len) { st.set_scalar(0, sig, type, 1.0f, v, len); };
  u(0x2032, TYPE_U32, 700, 4);   // normal-path DC power
  u(0x2308, TYPE_U32, 757, 4);   // 7816 block
  u(0x2309, TYPE_S32, 1149, 4);
  u(0x230A, TYPE_S32, 1164, 4);
  u(0x230D, TYPE_S32, 0, 4);
  u(0x230F, TYPE_U16, 420, 2);   // SOC 42.0 %
  u(0x230C, TYPE_S32, 33, 4);
  u(0x2514, TYPE_U16, 469, 2);
  u(0x230E, TYPE_S32, static_cast<uint32_t>(-521), 4);  // charging 521 W
  u(0x252C, TYPE_S32, 600, 4);
  u(0x230B, TYPE_S32, 250, 4);   // feeding in 250 W
  u(0x207A, TYPE_S16, 84, 2);
  u(0x208C, TYPE_U16, 4997, 2);
  u(0x2077, TYPE_U16, 2445, 2);
  u(0x2003, TYPE_U16, 0, 2);     // single phase
  u(0x2510, TYPE_U16, 105, 2);
  uint64_t v = 0;
  // functionEnum bit 22 set, new firmware code: the 7816 block is the source.
  u(0x2173, TYPE_U32, 0x1179C0DD, 4);
  u(0x20F1, TYPE_U32, 0x01000504, 4);
  update_hybrid_cloud(st, 0);
  CHECK(st.get_bits(0, 0x2320, &v) && v == 757);
  CHECK(st.get_bits(0, 0x2324, &v) && v == 420);
  CHECK(st.get_bits(0, 0x2325, &v) && v == 33);
  CHECK(st.get_bits(0, 0x2326, &v) && v == 33);
  CHECK(st.get_bits(0, 0x5402, &v) && v == 521);  // charge
  CHECK(st.get_bits(0, 0x5403, &v) && v == 0);
  CHECK(st.get_bits(0, 0x5401, &v) && v == 250);  // feed-in
  CHECK(st.get_bits(0, 0x5400, &v) && v == 0);
  CHECK(st.get_bits(0, 0x5008, &v) && v == 84);
  CHECK(st.get_bits(0, 0x2083, &v) && v == 4997);
  CHECK(st.get_bits(0, 0x2074, &v) && v == 2445);
  CHECK(!st.is_valid(0, 0x2071) && !st.is_valid(0, 0x2075));
  CHECK(st.get_bits(0, 0x2536, &v) && v == 105);
  // Old firmware code: plant powers from the normal registers, splits still from the block.
  u(0x20F1, TYPE_U32, 0x01000400, 4);
  update_hybrid_cloud(st, 0);
  CHECK(st.get_bits(0, 0x2320, &v) && v == 700);
  CHECK(st.get_bits(0, 0x2324, &v) && v == 469);
  CHECK(!st.is_valid(0, 0x2325));  // 0x2540 has no poll row
  CHECK(st.get_bits(0, 0x5402, &v) && v == 521);
  // Bit 22 clear: 5214 (discharging 600 W) for the battery split.
  u(0x2173, TYPE_U32, 0x1139C0DD, 4);
  update_hybrid_cloud(st, 0);
  CHECK(st.get_bits(0, 0x5403, &v) && v == 600);
  CHECK(st.get_bits(0, 0x5402, &v) && v == 0);
  // Gating: the hook runs from ingest_block only for dev_type 35.
  const RegisterTable &p3 = get_table(TableId::INV_PROTO3);
  SignalStore s21, s35;
  s21.set_dev_type(0, 21);
  s35.set_dev_type(0, 35);
  uint8_t b[4];
  put32lw(b, 900);  // 5214 battery power +900 W
  s21.ingest_block(0, p3, 4, 5214, b, 2);
  s35.ingest_block(0, p3, 4, 5214, b, 2);
  CHECK(!s21.is_valid(0, 0x5403));
  CHECK(s35.get_bits(0, 0x5403, &v) && v == 900);
}

// SBR: a module slot whose cell type (10781+) reads 0 is empty, so its SOC (10675+), its cell
// voltages and the type code itself are cleared; a populated slot keeps all of them.
TEST(sbr_absent_module_cleared) {
  SignalStore st;
  st.set_scalar(0, 0x2073 + 5, TYPE_U16, 0.01f, 5657, 2);      // module 6 SOC 56.57 %
  st.set_scalar(0, 0x2073 + 6, TYPE_U16, 0.01f, 0, 2);         // module 7 SOC (slot empty)
  st.set_scalar(0, 0x2036 + 3 * 5, TYPE_U16, 0.0001f, 33497, 2);  // module 6 cell max
  st.set_scalar(0, 0x2037 + 3 * 5, TYPE_U16, 0.0001f, 33476, 2);  // module 6 cell min
  st.set_scalar(0, 0x2036 + 3 * 6, TYPE_U16, 0.0001f, 0, 2);      // module 7 cell max (empty)
  st.set_scalar(0, 0x2037 + 3 * 6, TYPE_U16, 0.0001f, 0, 2);      // module 7 cell min (empty)
  st.set_scalar(0, 0x2038 + 3 * 5, TYPE_U16, 1.0f, 68, 2);     // module 6 cell type 'D'
  st.set_scalar(0, 0x2038 + 3 * 6, TYPE_U16, 1.0f, 0, 2);      // module 7 cell type 0 = empty
  clear_absent_modules(st, 0);
  CHECK(st.is_valid(0, 0x2073 + 5));
  CHECK(st.is_valid(0, 0x2036 + 3 * 5));
  CHECK(st.is_valid(0, 0x2037 + 3 * 5));
  CHECK(st.is_valid(0, 0x2038 + 3 * 5));
  CHECK(!st.is_valid(0, 0x2073 + 6));
  CHECK(!st.is_valid(0, 0x2036 + 3 * 6));
  CHECK(!st.is_valid(0, 0x2037 + 3 * 6));
  CHECK(!st.is_valid(0, 0x2038 + 3 * 6));
}

TEST(bitfield_children) {
  CHECK_EQ(kBitfieldCount, 15u);
  const RegisterTable &p5 = get_table(TableId::INV_PROTO5);
  const BitfieldDesc *bf = find_bitfield(p5.id, 0x5022);
  CHECK(bf != nullptr);
  CHECK_EQ(bf->child_base, 0x5040);
  CHECK_EQ(bf->nbits, 32);
  CHECK(find_bitfield(p5.id, 0x5030)->child_base == 0x5040 + 14 * 32);
  CHECK(find_bitfield(p5.id, 0x2080) == nullptr);
  uint8_t b[4];
  put32lw(b, 0x80000005u);
  SignalStore off;  // default: children not stored
  off.ingest_block(0, p5, 4, 13050, b, 2);
  CHECK(off.is_valid(0, 0x5022));
  CHECK(off.get(0, 0x5040) == nullptr);
  SignalStore st;
  st.set_expand_bitfields(true);
  st.ingest_block(0, p5, 4, 13050, b, 2);
  CHECK_EQ((int) st.get_number(0, 0x5040), 1);
  CHECK_EQ((int) st.get_number(0, 0x5041), 0);
  CHECK_EQ((int) st.get_number(0, 0x5042), 1);
  CHECK_EQ((int) st.get_number(0, 0x505F), 1);
  // parent sentinel: parent cleared, children untouched (stock expands only stored parents)
  put32lw(b, 0xFFFFFFFF);
  st.ingest_block(0, p5, 4, 13050, b, 2);
  CHECK(!st.is_valid(0, 0x5022));
  CHECK(st.is_valid(0, 0x5040));
  // offline: the parent has tag bit1, so its children are cleared too
  st.clear_offline(0, p5);
  CHECK(!st.is_valid(0, 0x5040));
}

namespace {
uint16_t row_addr(const RegisterTable &t, uint16_t sigid) {
  for (uint16_t i = 0; i < t.count; i++)
    if (t.rows[i].sigid == sigid)
      return t.rows[i].regaddr;
  return 0;
}
}  // namespace

TEST(fault_list_decoded) {
  const RegisterTable &bt = get_table(TableId::BATTERY);
  CHECK_EQ(row_addr(bt, SIG_FAULT_LIST), 5448);
  uint8_t b[2 * FAULT_LIST_NREG] = {};
  SignalStore st;
  // No faults: the first word is 0, so the stock string row is absent, but the list decodes.
  st.ingest_block(0, bt, 4, 5448, b, FAULT_LIST_NREG);
  CHECK(!st.is_valid(0, SIG_FAULT_LIST));
  CHECK_EQ((int) st.get_number(0, SIG_ACTIVE_FAULT_COUNT), 0);
  CHECK(st.get_string(0, SIG_ACTIVE_FAULT_CODES) == "No active faults");
  put16(b + 2 * 1, 0x180);  // flags
  put16(b + 2 * 4, 2);      // count
  put16(b + 2 * 5, 732);
  put16(b + 2 * 6, 25);
  put16(b + 2 * 7, 933);
  put16(b + 2 * 8, 24);
  st.ingest_block(0, bt, 4, 5448, b, FAULT_LIST_NREG);
  CHECK_EQ((int) st.get_number(0, SIG_ACTIVE_FAULT_COUNT), 2);
  CHECK(st.get_string(0, SIG_ACTIVE_FAULT_CODES) == "732 fault, 933 alarm");
  // Codes with a known protection condition carry their name; the class word is then implied.
  put16(b + 2 * 5, 12);
  put16(b + 2 * 7, 997);
  st.ingest_block(0, bt, 4, 5448, b, FAULT_LIST_NREG);
  CHECK(st.get_string(0, SIG_ACTIVE_FAULT_CODES) ==
        "12 Excess leakage current, 997 Frequency derating active");
  put16(b + 2 * 5, 44);
  put16(b + 2 * 7, 39);
  st.ingest_block(0, bt, 4, 5448, b, FAULT_LIST_NREG);
  CHECK(st.get_string(0, SIG_ACTIVE_FAULT_CODES) ==
        "44 Inverter self-check fault, 39 Low insulation resistance");
  put16(b + 2 * 5, 71);
  put16(b + 2 * 7, 605);
  st.ingest_block(0, bt, 4, 5448, b, FAULT_LIST_NREG);
  CHECK(st.get_string(0, SIG_ACTIVE_FAULT_CODES) ==
        "71 AC surge protector alarm, 605 Battery relay check failed");
  // Unserved block (all 0xFFFF): both derived signals absent.
  for (auto &x : b)
    x = 0xFF;
  st.ingest_block(0, bt, 4, 5448, b, FAULT_LIST_NREG);
  CHECK(!st.is_valid(0, SIG_ACTIVE_FAULT_COUNT));
  CHECK(!st.is_valid(0, SIG_ACTIVE_FAULT_CODES));
}

TEST(bms_request_flag_bits) {
  const RegisterTable &bt = get_table(TableId::BATTERY);
  CHECK_EQ(row_addr(bt, SIG_BMS_REQUEST_FLAGS), 10666);
  uint8_t b[2];
  SignalStore st;
  put16(b, 0x0006);  // discharge not allowed + forced charge request
  st.ingest_block(0, bt, 4, 10666, b, 1);
  CHECK_EQ((int) st.get_number(0, SIG_CHARGE_PROHIBITED), 0);
  CHECK_EQ((int) st.get_number(0, SIG_DISCHARGE_PROHIBITED), 1);
  CHECK_EQ((int) st.get_number(0, SIG_FORCED_CHARGE_REQUEST), 1);
  put16(b, 0xFFFF);  // sentinel: parent and bits absent
  st.ingest_block(0, bt, 4, 10666, b, 1);
  CHECK(!st.is_valid(0, SIG_CHARGE_PROHIBITED));
  CHECK(!st.is_valid(0, SIG_FORCED_CHARGE_REQUEST));
}

TEST(single_phase_unserved_currents_absent) {
  const RegisterTable &p3 = get_table(TableId::INV_PROTO3);
  const uint16_t a = row_addr(p3, SIG_PHASE_B_CURRENT);
  CHECK(a != 0);
  CHECK_EQ(row_addr(p3, SIG_PHASE_C_CURRENT), a + 1);
  uint8_t b[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  SignalStore st;
  st.ingest_block(0, p3, 4, a, b, 2);
  CHECK(!st.is_valid(0, SIG_PHASE_B_CURRENT));
  CHECK(!st.is_valid(0, SIG_PHASE_C_CURRENT));
  put16(b, 52);  // 5.2 A on a three-phase inverter
  st.ingest_block(0, p3, 4, a, b, 2);
  CHECK(std::fabs(st.get_number(0, SIG_PHASE_B_CURRENT) - 5.2) < 1e-4);
}

TEST(single_phase_output_type_drops_phase_b_c) {
  // The SG5.0RS serves 0 on phases B/C; output type 0 (5375, polled after the phase rows) marks
  // the inverter single-phase, which clears what the first cycle stored and keeps them absent.
  const RegisterTable &p3 = get_table(TableId::INV_PROTO3);
  const uint16_t va = row_addr(p3, 0x2077), ot = row_addr(p3, SIG_OUTPUT_TYPE);
  CHECK_EQ(va, 5247);
  CHECK_EQ(ot, 5375);
  uint8_t ph[12] = {0};  // 5247..5252: A V, B V, C V, A I, B I, C I
  put16(ph, 2479);
  put16(ph + 6, 62);
  SignalStore st;
  st.ingest_block(0, p3, 4, va, ph, 6);
  CHECK(st.is_valid(0, 0x2078));
  CHECK(st.is_valid(0, SIG_PHASE_C_CURRENT));
  uint8_t t[2] = {0, 0};
  st.ingest_block(0, p3, 4, ot, t, 1);
  CHECK(!st.is_valid(0, 0x2078));
  CHECK(!st.is_valid(0, 0x2079));
  CHECK(!st.is_valid(0, SIG_PHASE_B_CURRENT));
  CHECK(!st.is_valid(0, SIG_PHASE_C_CURRENT));
  st.ingest_block(0, p3, 4, va, ph, 6);
  CHECK(!st.is_valid(0, 0x2078));
  CHECK(std::fabs(st.get_number(0, 0x2077) - 247.9) < 1e-3);
  CHECK(std::fabs(st.get_number(0, 0x207A) - 6.2) < 1e-4);
  put16(t, 1);  // three-phase (3P4L): phases B/C are real
  st.ingest_block(0, p3, 4, ot, t, 1);
  st.ingest_block(0, p3, 4, va, ph, 6);
  CHECK(st.is_valid(0, 0x2078));
  CHECK(st.is_valid(0, SIG_PHASE_C_CURRENT));
}

TEST(meter_rows_absent_without_meter) {
  // Meterless SG5.0RS: 5744..5746 read 0xFFFF (-0.01 A) and 5737 reads 0 Hz; with no valid meter
  // active power (5601) they are absent.
  const RegisterTable &p3 = get_table(TableId::INV_PROTO3);
  const uint16_t ia = row_addr(p3, 0x20B4), f = row_addr(p3, 0x20A5), mp = row_addr(p3, SIG_METER_ACTIVE_POWER);
  CHECK_EQ(ia, 5744);
  uint8_t cur[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t hz[2] = {0, 0};
  SignalStore st;
  st.ingest_block(0, p3, 4, ia, cur, 3);
  st.ingest_block(0, p3, 4, f, hz, 1);
  CHECK(!st.is_valid(0, 0x20B4));
  CHECK(!st.is_valid(0, 0x20B6));
  CHECK(!st.is_valid(0, 0x20A5));
  uint8_t p[4] = {0xF3, 0xCE, 0xFF, 0xFF};  // -3122 W, s32 low word first
  st.ingest_block(0, p3, 4, mp, p, 2);
  CHECK(std::fabs(st.get_number(0, SIG_METER_ACTIVE_POWER) + 3122) < 1e-6);
  put16(cur, 2153);
  put16(hz, 5001);
  st.ingest_block(0, p3, 4, ia, cur, 3);
  st.ingest_block(0, p3, 4, f, hz, 1);
  CHECK(std::fabs(st.get_number(0, 0x20B4) - 21.53) < 1e-4);
  CHECK(std::fabs(st.get_number(0, 0x20A5) - 50.01) < 1e-4);
}

TEST(power_flow_text_from_flags) {
  CHECK(power_flow_text(43) == "PV generating, Battery charging, Load consuming, Importing from grid");
  CHECK(power_flow_text(25) == "PV generating, Load consuming, Exporting to grid");
  CHECK(power_flow_text(0) == "Idle");
  CHECK(power_flow_text(0x40) == "bit 6");
  const RegisterTable &p3 = get_table(TableId::INV_PROTO3);
  const uint16_t a = row_addr(p3, SIG_POWER_FLOW_STATUS);
  CHECK_EQ(a, 6099);
  uint8_t b[2];
  put16(b, 0x0014);  // battery discharging + exporting
  SignalStore st;
  st.ingest_block(0, p3, 4, a, b, 1);
  CHECK(st.get_string(0, SIG_POWER_FLOW) == "Battery discharging, Exporting to grid");
  put16(b, 0xFFFF);  // sentinel: flags and text absent
  st.ingest_block(0, p3, 4, a, b, 1);
  CHECK(!st.is_valid(0, SIG_POWER_FLOW));
}

TEST(total_battery_charge_reads_full_u32) {
  const RegisterTable &p3 = get_table(TableId::INV_PROTO3);
  CHECK_EQ(row_addr(p3, 0x2522), 13041);
}

TEST(module_manufacture_dates) {
  CHECK(module_date_text(23092306) == "2023-09-23");
  CHECK(module_date_text(25111107) == "2025-11-11");
  CHECK(module_date_text(0).empty());
  CHECK(module_date_text(23130100).empty());  // month 13
  CHECK(module_date_text(23090006).empty());  // day 0
  SignalStore st;
  st.set_scalar(0, SIG_MODULE_DATE_CODE_BASE, TYPE_U32, 0.0f, 23092306, 4);
  st.set_scalar(0, SIG_MODULE_DATE_CODE_BASE + 6, TYPE_U32, 0.0f, 0, 4);  // empty slot
  update_module_dates(st, 0);
  CHECK(st.get_string(0, SIG_MODULE_MANUFACTURING_DATE_BASE) == "2023-09-23");
  CHECK(!st.is_valid(0, SIG_MODULE_MANUFACTURING_DATE_BASE + 6));
}

TEST(derived_first_fault_and_transient) {
  const RegisterTable &bt = get_table(TableId::BATTERY);
  uint8_t b[2 * FAULT_LIST_NREG] = {};
  SignalStore st;
  st.ingest_block(0, bt, 4, 5448, b, FAULT_LIST_NREG);
  CHECK(st.get_string(0, SIG_FIRST_FAULT) == "No active faults");
  put16(b + 2 * 4, 2);  // count
  put16(b + 2 * 5, 44);
  put16(b + 2 * 6, 25);
  put16(b + 2 * 7, 39);
  put16(b + 2 * 8, 25);
  st.ingest_block(0, bt, 4, 5448, b, FAULT_LIST_NREG);
  CHECK(st.get_string(0, SIG_FIRST_FAULT) == "44 Inverter self-check fault");
  CHECK(st.get_string(0, SIG_ACTIVE_FAULT_CODES) ==
        "44 Inverter self-check fault, 39 Low insulation resistance");
  put16(b + 2 * 5, 732);  // unnamed code keeps its class word
  st.ingest_block(0, bt, 4, 5448, b, FAULT_LIST_NREG);
  CHECK(st.get_string(0, SIG_FIRST_FAULT) == "732 fault");
  for (auto &x : b)
    x = 0xFF;
  st.ingest_block(0, bt, 4, 5448, b, FAULT_LIST_NREG);
  CHECK(!st.is_valid(0, SIG_FIRST_FAULT));
  CHECK(fault_code_text(39) != nullptr && std::string(fault_code_text(39)) == "Low insulation resistance");
  CHECK(fault_code_text(39, "inverter") == fault_code_text(39));
  CHECK(fault_code_text(39, "battery") == nullptr);
  CHECK(fault_code_text(1) == nullptr);

  // Transient flag from the running state 0x2086 (inv_proto2 @5221), by model family.
  const RegisterTable &p2 = get_table(TableId::INV_PROTO2);
  CHECK_EQ(row_addr(p2, SIG_DEVICE_STATE), 5221);
  SignalStore hyb, str_;
  hyb.set_dev_type(0, 35);
  str_.set_dev_type(0, 21);
  uint8_t w[2];
  put16(w, 0x1900);  // rapid shutdown engaged: transient for every family
  hyb.ingest_block(0, p2, 4, 5221, w, 1);
  CHECK_EQ((int) hyb.get_number(0, SIG_STATE_TRANSIENT), 1);
  put16(w, 0x0000);
  hyb.ingest_block(0, p2, 4, 5221, w, 1);
  CHECK_EQ((int) hyb.get_number(0, SIG_STATE_TRANSIENT), 0);
  put16(w, 0x1300);  // key stop: a string inverter reports rapid shutdown this way
  str_.ingest_block(0, p2, 4, 5221, w, 1);
  CHECK_EQ((int) str_.get_number(0, SIG_STATE_TRANSIENT), 1);
  hyb.ingest_block(0, p2, 4, 5221, w, 1);
  CHECK_EQ((int) hyb.get_number(0, SIG_STATE_TRANSIENT), 0);
  CHECK(state_transient(0x1800, false) && state_transient(0x6600, true));
  CHECK(!state_transient(0x8200, true));
  // State code gone -> flag gone.
  hyb.invalidate_block(0, p2, 4, 5221, 1);
  CHECK(!hyb.is_valid(0, SIG_DEVICE_STATE));
  CHECK(!hyb.is_valid(0, SIG_STATE_TRANSIENT));
  CHECK(inverter_model_family(35) == ModelFamily::HYBRID);
  CHECK(inverter_model_family(1) == ModelFamily::STRING && inverter_model_family(21) == ModelFamily::STRING);
  CHECK(inverter_model_family(0) == ModelFamily::UNKNOWN);
}

TEST(state_class_table) {
  CHECK(state_class(0x8200) == StateClass::RUNNING);
  CHECK(state_class(0x0000) == StateClass::RUNNING);
  CHECK(state_class(0x1300) == StateClass::STOPPED);
  CHECK(state_class(0x8000) == StateClass::STOPPED);
  CHECK(state_class(0x1200) == StateClass::STANDBY);
  CHECK(state_class(0x1600) == StateClass::STARTING);
  CHECK(state_class(0x1900) == StateClass::TRANSIENT);
  CHECK(state_class(0x5500) == StateClass::FAULT);
  CHECK(state_class(0xA100) == StateClass::STANDBY);
  CHECK(state_class(0xFFFE) == StateClass::OFFLINE);
  CHECK(state_class(0x1234) == StateClass::UNKNOWN);
  CHECK(std::string(state_class_name(StateClass::RUNNING)) == "running");
  CHECK(std::string(state_class_name(StateClass::UNKNOWN)) == "unknown");
  CHECK(std::string(state_class_name(static_cast<StateClass>(200))) == "unknown");
}

int main() { return th::run_all(); }
