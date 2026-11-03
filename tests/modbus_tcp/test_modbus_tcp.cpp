// Host tests for components/sungrow_modbus_tcp/mbtcp_core.
#include <cmath>
#include <cstring>
#include <vector>

#include "../test_harness.h"
#include "mbtcp_core.h"
#include "register_tables.h"
#include "signal_store.h"

using namespace esphome::sungrow_modbus_tcp;
namespace dm = esphome::sungrow_datamodel;

static const dm::RegisterTable &T121() { return *dm::tcpslave_table_for_dev_type(21); }

static const dm::RegisterRow *row_at(const dm::RegisterTable &t, uint8_t fn, uint16_t addr) {
  for (uint16_t i = 0; i < t.count; i++)
    if (t.rows[i].fn == fn && t.rows[i].regaddr == addr)
      return &t.rows[i];
  return nullptr;
}

static std::vector<uint8_t> req(uint16_t tid, uint8_t unit, uint8_t fc, uint16_t wire, uint16_t count) {
  return {static_cast<uint8_t>(tid >> 8), static_cast<uint8_t>(tid), 0, 0, 0, 6, unit, fc,
          static_cast<uint8_t>(wire >> 8), static_cast<uint8_t>(wire), static_cast<uint8_t>(count >> 8),
          static_cast<uint8_t>(count)};
}

struct Fixture {
  dm::SignalStore store;
  StaticBackend be{&store};
  Options opt;
  Fixture(const dm::RegisterTable *t = &T121(), uint8_t rtu = 1) {
    UnitTarget tgt;
    tgt.device = 0;
    tgt.table = t;
    tgt.rtu_address = rtu;
    be.add_unit(1, tgt);
  }
  void set(uint16_t addr, uint64_t bits) {
    const dm::RegisterRow *r = row_at(T121(), 4, addr);
    CHECK(r != nullptr);
    if (r == nullptr)
      return;
    uint8_t w = dm::scalar_regs(r->type, r->nreg);
    store.set_scalar(0, r->sigid, r->type, r->scale, bits, static_cast<uint8_t>(2 * w));
  }
  // Run one request through framing + handling.
  Decision run(const std::vector<uint8_t> &bytes, std::vector<uint8_t> *reply) {
    FrameAssembler fa;
    fa.feed(bytes.data(), bytes.size());
    Frame f;
    Decision d;
    CHECK(fa.peek(&f) == FrameAssembler::Status::FRAME);
    uint8_t out[MAX_ADU_LEN];
    size_t n = 0;
    d = handle_frame(f, be, opt, out, &n);
    reply->assign(out, out + n);
    return d;
  }
};

static uint16_t reg(const std::vector<uint8_t> &r, int i) { return static_cast<uint16_t>((r[9 + 2 * i] << 8) | r[10 + 2 * i]); }

// ---- framing -------------------------------------------------------------------------------------

TEST(mbap_split_bytewise) {
  FrameAssembler fa;
  auto r = req(0x1234, 1, 4, 5002, 1);
  Frame f;
  for (size_t i = 0; i + 1 < r.size(); i++) {
    fa.feed(&r[i], 1);
    CHECK(fa.peek(&f) == FrameAssembler::Status::NEED_MORE);
  }
  fa.feed(&r.back(), 1);
  CHECK(fa.peek(&f) == FrameAssembler::Status::FRAME);
  CHECK_EQ(f.hdr.tid, 0x1234);
  CHECK_EQ(f.hdr.unit, 1);
  CHECK_EQ(f.hdr.fc, 4);
  CHECK_EQ(f.pdu_len, 5);
  fa.pop(f);
  CHECK_EQ(fa.buffered(), 0u);
}

TEST(mbap_pipelined_and_partial_tail) {
  FrameAssembler fa;
  auto a = req(1, 1, 4, 5002, 1), b = req(2, 1, 4, 5007, 1), c = req(3, 1, 3, 4999, 6);
  std::vector<uint8_t> all;
  all.insert(all.end(), a.begin(), a.end());
  all.insert(all.end(), b.begin(), b.end());
  all.insert(all.end(), c.begin(), c.begin() + 5);  // third frame cut mid-header
  fa.feed(all.data(), all.size());
  Frame f;
  CHECK(fa.peek(&f) == FrameAssembler::Status::FRAME);
  CHECK_EQ(f.hdr.tid, 1);
  fa.pop(f);
  CHECK(fa.peek(&f) == FrameAssembler::Status::FRAME);
  CHECK_EQ(f.hdr.tid, 2);
  CHECK_EQ(f.pdu[2], 5007 & 0xFF);
  fa.pop(f);
  CHECK(fa.peek(&f) == FrameAssembler::Status::NEED_MORE);
  fa.feed(c.data() + 5, c.size() - 5);
  CHECK(fa.peek(&f) == FrameAssembler::Status::FRAME);
  CHECK_EQ(f.hdr.tid, 3);
  CHECK_EQ(f.hdr.fc, 3);
  fa.pop(f);
  CHECK_EQ(fa.buffered(), 0u);
}

TEST(mbap_bad_length) {
  FrameAssembler fa;
  uint8_t bad[] = {0, 1, 0, 0, 0, 1, 1};  // length 1: no function code
  fa.feed(bad, sizeof(bad));
  Frame f;
  CHECK(fa.peek(&f) == FrameAssembler::Status::BAD);
  fa.clear();
  uint8_t big[] = {0, 1, 0, 0, 0x01, 0x00, 1, 4};  // length 256 > 254
  fa.feed(big, sizeof(big));
  CHECK(fa.peek(&f) == FrameAssembler::Status::BAD);
}

TEST(mbap_capacity_limits_feed) {
  FrameAssembler fa;
  std::vector<uint8_t> junk(FrameAssembler::CAPACITY + 50, 0);
  CHECK_EQ(fa.feed(junk.data(), junk.size()), FrameAssembler::CAPACITY);
  CHECK_EQ(fa.space(), 0u);
}

// ---- public addresses (mkaiser HA config: 0-based wire address = documented - 1) ---------------

TEST(read_daily_yield_5003) {
  Fixture fx;
  fx.set(5003, 1234);  // 123.4 kWh
  std::vector<uint8_t> r;
  Decision d = fx.run(req(7, 1, 4, 5002, 1), &r);
  CHECK(d.kind == Decision::REPLY);
  CHECK_EQ(d.exception, 0);
  const uint8_t expect[] = {0, 7, 0, 0, 0, 5, 1, 4, 2, 0x04, 0xD2};
  CHECK_EQ(r.size(), sizeof(expect));
  CHECK(std::memcmp(r.data(), expect, sizeof(expect)) == 0);
}

TEST(read_internal_temp_5008_signed) {
  Fixture fx;
  fx.set(5008, 0xFFC9);  // -5.5 C
  std::vector<uint8_t> r;
  fx.run(req(1, 1, 4, 5007, 1), &r);
  CHECK_EQ(reg(r, 0), 0xFFC9);
}

TEST(read_total_dc_power_5017_u32_low_word_first) {
  Fixture fx;
  fx.set(5017, 0x00012345);
  std::vector<uint8_t> r;
  fx.run(req(1, 1, 4, 5016, 2), &r);
  CHECK_EQ(r.size(), 9u + 4u);
  CHECK_EQ(reg(r, 0), 0x2345);  // low word first (mkaiser: swap: word)
  CHECK_EQ(reg(r, 1), 0x0001);
}

TEST(read_total_active_power_5031_s32_negative) {
  Fixture fx;
  fx.set(5031, static_cast<uint32_t>(-1500));
  std::vector<uint8_t> r;
  fx.run(req(1, 1, 4, 5030, 2), &r);
  CHECK_EQ(reg(r, 0), 0xFA24);
  CHECK_EQ(reg(r, 1), 0xFFFF);
}

TEST(high_word_first_option) {
  Fixture fx;
  fx.opt.word_order = dm::WordOrder::HIGH_WORD_FIRST;
  fx.set(5017, 0x00012345);
  std::vector<uint8_t> r;
  fx.run(req(1, 1, 4, 5016, 2), &r);
  CHECK_EQ(reg(r, 0), 0x0001);
  CHECK_EQ(reg(r, 1), 0x2345);
}

TEST(sentinels_for_invalid_and_absent) {
  Fixture fx;
  fx.set(5003, 1);
  fx.set(5008, 1);
  fx.set(5017, 1);
  const dm::RegisterRow *r3 = row_at(T121(), 4, 5003), *r8 = row_at(T121(), 4, 5008),
                        *r17 = row_at(T121(), 4, 5017);
  fx.store.clear(0, r3->sigid);
  fx.store.clear(0, r8->sigid);
  fx.store.clear(0, r17->sigid);
  std::vector<uint8_t> r;
  fx.run(req(1, 1, 4, 5002, 1), &r);
  CHECK_EQ(reg(r, 0), 0xFFFF);  // u16
  fx.run(req(1, 1, 4, 5007, 1), &r);
  CHECK_EQ(reg(r, 0), 0x7FFF);  // s16
  fx.run(req(1, 1, 4, 5016, 2), &r);
  CHECK_EQ(reg(r, 0), 0xFFFF);  // u32
  CHECK_EQ(reg(r, 1), 0xFFFF);
  // never stored s32 -> 0x7FFFFFFF, low word first
  fx.run(req(1, 1, 4, 5030, 2), &r);
  CHECK_EQ(reg(r, 0), 0xFFFF);
  CHECK_EQ(reg(r, 1), 0x7FFF);
  for (uint8_t t : {dm::TYPE_U16, dm::TYPE_S16, dm::TYPE_U32, dm::TYPE_S32, dm::TYPE_U64, dm::TYPE_S64}) {
    uint8_t w = dm::type_width(t);
    CHECK(dm::is_sentinel(sentinel_bits(t, w), t, w));
  }
}

TEST(block_read_with_holes_is_zero_filled) {
  Fixture fx;
  fx.set(5003, 10);
  fx.set(5017, 0x10002);
  fx.set(5031, 77);
  std::vector<uint8_t> r;
  Decision d = fx.run(req(9, 1, 4, 4999, 40), &r);  // table 5000..5039
  CHECK(d.kind == Decision::REPLY);
  CHECK_EQ(d.exception, 0);
  CHECK_EQ(r.size(), 9u + 80u);
  CHECK_EQ(reg(r, 3), 10);         // 5003
  CHECK_EQ(reg(r, 17), 0x0002);    // 5017 low
  CHECK_EQ(reg(r, 18), 0x0001);    // 5018 = high word of 5017
  CHECK_EQ(reg(r, 31), 77);        // 5031
  CHECK_EQ(reg(r, 32), 0);         // 5032 high word of 77
  // 5025..5030: no row in dev1_21 -> hole = 0
  CHECK(row_at(T121(), 4, 5025) == nullptr);
  CHECK_EQ(reg(r, 25), 0);
}

TEST(row_clipped_at_request_end) {
  Fixture fx;
  fx.set(5017, 0xABCD1234);
  std::vector<uint8_t> r;
  fx.run(req(1, 1, 4, 5016, 1), &r);
  CHECK_EQ(r.size(), 11u);
  CHECK_EQ(reg(r, 0), 0x1234);
}

TEST(mid_row_start_fn4_is_exception_2) {
  Fixture fx;
  fx.set(5017, 5);
  std::vector<uint8_t> r;
  Decision d = fx.run(req(5, 1, 4, 5017, 1), &r);  // table 5018 = high word of 5017
  CHECK_EQ(d.exception, EX_ILLEGAL_ADDRESS);
  const uint8_t expect[] = {0, 5, 0, 0, 0, 3, 1, 0x84, 0x02};
  CHECK_EQ(r.size(), sizeof(expect));
  CHECK(std::memcmp(r.data(), expect, sizeof(expect)) == 0);
}

TEST(count_limits) {
  Fixture fx;
  std::vector<uint8_t> r;
  CHECK_EQ(fx.run(req(1, 1, 4, 4999, 126), &r).exception, EX_ILLEGAL_ADDRESS);
  CHECK_EQ(fx.run(req(1, 1, 4, 4999, 0), &r).exception, EX_ILLEGAL_ADDRESS);
  CHECK_EQ(fx.run(req(1, 1, 4, 0xFFF0, 20), &r).exception, EX_ILLEGAL_ADDRESS);
  Decision d = fx.run(req(1, 1, 4, 4999, 125), &r);
  CHECK_EQ(d.exception, 0);
  CHECK_EQ(r.size(), 9u + 250u);
}

TEST(writes_and_unknown_functions_are_illegal_function) {
  Fixture fx;
  std::vector<uint8_t> r;
  CHECK_EQ(fx.run(req(1, 1, 6, 13049, 2), &r).exception, EX_ILLEGAL_FUNCTION);
  CHECK_EQ(r[7], 0x86);
  std::vector<uint8_t> w = {0, 2, 0, 0, 0, 9, 1, 0x10, 0x32, 0xF8, 0, 1, 2, 0, 1};
  CHECK_EQ(fx.run(w, &r).exception, EX_ILLEGAL_FUNCTION);
  CHECK_EQ(r[7], 0x90);
  CHECK_EQ(fx.run(req(1, 1, 0x2B, 0x0E01, 0), &r).exception, EX_ILLEGAL_FUNCTION);
  CHECK_EQ(fx.run(req(1, 1, 1, 0, 8), &r).exception, EX_ILLEGAL_FUNCTION);
}

TEST(unknown_unit) {
  Fixture fx;
  std::vector<uint8_t> r;
  Decision d = fx.run(req(1, 2, 4, 5002, 1), &r);
  CHECK_EQ(d.exception, EX_GATEWAY_TARGET);
  CHECK_EQ(r[6], 2);
  fx.opt.unknown_unit_exception = false;
  d = fx.run(req(1, 2, 4, 5002, 1), &r);
  CHECK(d.kind == Decision::DROP);
  CHECK_EQ(r.size(), 0u);
}

TEST(not_ready_unit) {
  dm::SignalStore store;
  StaticBackend be(&store);
  be.add_unit(1, UnitTarget{});  // table unknown yet
  Options opt;
  auto q = req(1, 1, 4, 5002, 1);
  FrameAssembler fa;
  fa.feed(q.data(), q.size());
  Frame f;
  fa.peek(&f);
  uint8_t out[MAX_ADU_LEN];
  size_t n;
  CHECK_EQ(handle_frame(f, be, opt, out, &n).exception, EX_GATEWAY_TARGET);
}

// ---- fn 3 -----------------------------------------------------------------------------------------

static const dm::RegisterRow *first_fn3_u16() {
  const auto &t = T121();
  for (uint16_t i = 0; i < t.count; i++)
    if (t.rows[i].fn == 3 && t.rows[i].nreg == 1 && t.rows[i].sigid != 0 &&
        (t.rows[i].type == dm::TYPE_U16 || t.rows[i].type == dm::TYPE_S16))
      return &t.rows[i];
  return nullptr;
}

TEST(fn3_resolved_served_from_store) {
  Fixture fx;
  const dm::RegisterRow *row = first_fn3_u16();
  CHECK(row != nullptr);
  if (row == nullptr)
    return;
  fx.store.set_scalar(0, row->sigid, row->type, row->scale, 42, 2);
  std::vector<uint8_t> r;
  Decision d = fx.run(req(1, 1, 3, row->regaddr - 1, 1), &r);
  CHECK(d.kind == Decision::REPLY);
  CHECK_EQ(r[7], 3);
  CHECK_EQ(reg(r, 0), 42);
  // Same address via fn 4 is a different row set: not this row.
  const dm::RegisterRow *r4 = row_at(T121(), 4, row->regaddr);
  if (r4 == nullptr)
    CHECK_EQ(fx.run(req(1, 1, 4, row->regaddr - 1, 1), &r).exception, EX_ILLEGAL_ADDRESS);
}

TEST(fn3_absent_or_unmapped_goes_live) {
  Fixture fx;
  const dm::RegisterRow *row = first_fn3_u16();
  std::vector<uint8_t> r;
  Decision d = fx.run(req(1, 1, 3, row->regaddr - 1, 1), &r);  // mapped but never stored
  CHECK(d.kind == Decision::PASSTHROUGH);
  // EMS mode 13050 (wire 13049, mkaiser holding register): not in the tcpslave map -> live read
  CHECK(row_at(T121(), 3, 13050) == nullptr);
  d = fx.run(req(1, 1, 3, 13049, 2), &r);
  CHECK(d.kind == Decision::PASSTHROUGH);
  CHECK_EQ(d.rtu_address, 1);
  CHECK_EQ(d.fn, 3);
  CHECK_EQ(d.start, 13050);  // 1-based table address for the RS485 master
  CHECK_EQ(d.count, 2);
}

TEST(fn3_without_passthrough) {
  Fixture fx;
  fx.opt.passthrough = false;
  std::vector<uint8_t> r;
  CHECK_EQ(fx.run(req(1, 1, 3, 13049, 2), &r).exception, EX_ILLEGAL_ADDRESS);
  Fixture nort(&T121(), 0);  // no RS485 address: no live read either
  CHECK_EQ(nort.run(req(1, 1, 3, 13049, 2), &r).exception, EX_ILLEGAL_ADDRESS);
}

TEST(passthrough_reply) {
  ReqHeader h{0x0102, 0, 1, 3};
  uint8_t data[] = {0x00, 0x02, 0xAB, 0xCD};
  uint8_t out[MAX_ADU_LEN];
  size_t n = build_passthrough_reply(h, true, data, 2, 2, out);
  const uint8_t expect[] = {1, 2, 0, 0, 0, 7, 1, 3, 4, 0x00, 0x02, 0xAB, 0xCD};
  CHECK_EQ(n, sizeof(expect));
  CHECK(std::memcmp(out, expect, sizeof(expect)) == 0);
  n = build_passthrough_reply(h, false, nullptr, 0, 2, out);
  CHECK_EQ(n, 9u);
  CHECK_EQ(out[7], 0x83);
  CHECK_EQ(out[8], EX_DEVICE_FAILURE);
  n = build_passthrough_reply(h, true, data, 1, 2, out);  // count mismatch
  CHECK_EQ(out[8], EX_DEVICE_FAILURE);
}

// ---- encoding details with synthetic tables -------------------------------------------------------

static const dm::RegisterRow kTcpRows[] = {
    {0x1111, 100, 0, dm::NO_KEY, 1, 4, dm::TYPE_U16, 0, 0, 0, 0, 0, 0.1f},   // tcp sigid not polled
    {0x2222, 101, 0, dm::NO_KEY, 2, 4, dm::TYPE_U32, 0, 0, 0, 0, 0, 0.0f},   // stored as u16
    {0x3333, 103, 0, dm::NO_KEY, 1, 4, dm::TYPE_S16, 0, 0, 0, 0, 0, 0.1f},   // stored with scale 0.01
    {0x4444, 104, 0, dm::NO_KEY, 1, 4, dm::TYPE_U16, 0, 0, 0, 0, 0, 0.0f},   // stored as u32 (too big)
    {0x5555, 105, 0, dm::NO_KEY, 2, 4, dm::TYPE_S32, 0, 0, 0, 0, 0, 0.001f}, // derived float
    {0x6666, 107, 0, dm::NO_KEY, 4, 4, dm::TYPE_STRING, 0, 0, 0, 0, 0, 0.0f},
};
static const dm::RegisterTable kTcp = {0, dm::Family::INVERTER, "test_tcp", kTcpRows, 6};
static const dm::RegisterRow kPollRows[] = {
    {0x7777, 100, 0, dm::NO_KEY, 1, 4, dm::TYPE_U16, 0, 0, 0, 0, 0, 0.1f},
};
static const dm::RegisterTable kPoll = {0, dm::Family::INVERTER, "test_poll", kPollRows, 1};

TEST(encoding_width_scale_float_string) {
  dm::SignalStore store;
  StaticBackend be(&store);
  store.set_scalar(0, 0x2222, dm::TYPE_U16, 0, 0xBEEF, 2);
  store.set_scalar(0, 0x3333, dm::TYPE_S16, 0.01f, static_cast<uint16_t>(-1234), 2);
  store.set_scalar(0, 0x4444, dm::TYPE_U32, 0, 0x10000, 4);
  store.set_float(0, 0x5555, -2.5f);  // kW -> raw -2500 at scale 0.001
  const uint8_t s[] = {'A', 'B', 'C', 0};
  store.set(0, 0x6666, dm::TYPE_STRING, 0, dm::ValueKind::STRING, s, 4);
  uint8_t out[2 * 11];
  AssembleStats st = assemble_registers(kTcp, 4, 100, 11, be, 0, dm::WordOrder::LOW_WORD_FIRST, out);
  auto R = [&](int i) { return static_cast<uint16_t>((out[2 * i] << 8) | out[2 * i + 1]); };
  CHECK_EQ(st.resolved, 6);
  CHECK_EQ(st.absent, 1);
  CHECK_EQ(R(0), 0xFFFF);     // 0x1111 never stored -> u16 sentinel
  CHECK_EQ(R(1), 0xBEEF);     // u16 value zero-extended into u32
  CHECK_EQ(R(2), 0x0000);
  CHECK_EQ(R(3), static_cast<uint16_t>(-123));  // -12.34 -> raw at 0.1 = -123
  CHECK_EQ(R(4), 0xFFFF);     // 65536 does not fit u16 -> sentinel
  CHECK_EQ(R(5), static_cast<uint16_t>(-2500 & 0xFFFF));
  CHECK_EQ(R(6), 0xFFFF);
  CHECK_EQ(R(7), 0x4142);     // string raw bytes
  CHECK_EQ(R(8), 0x4300);
  CHECK_EQ(R(9), 0);
}

TEST(poll_fallback_option) {
  dm::SignalStore store;
  StaticBackend be(&store);
  store.set_scalar(0, 0x7777, dm::TYPE_U16, 0.1f, 555, 2);
  uint8_t out[2];
  assemble_registers(kTcp, 4, 100, 1, be, 0, dm::WordOrder::LOW_WORD_FIRST, out);
  CHECK_EQ((out[0] << 8) | out[1], 0xFFFF);  // stock-faithful: sentinel
  AssembleStats st = assemble_registers(kTcp, 4, 100, 1, be, 0, dm::WordOrder::LOW_WORD_FIRST, out, &kPoll);
  CHECK_EQ((out[0] << 8) | out[1], 555);
  CHECK_EQ(st.fallback, 1);
  CHECK_EQ(st.absent, 0);
}

TEST(rescale_0x2501_style_rows_roundtrip) {
  // Every row of a sigid now carries the same scale: raw bits pass through unchanged.
  const auto &t = T121();
  for (uint16_t sig : {0x2501, 0x2502}) {
    const dm::RegisterRow *tr = nullptr;
    for (uint16_t i = 0; i < t.count; i++)
      if (t.rows[i].sigid == sig && t.rows[i].fn == 4)
        tr = &t.rows[i];
    if (tr == nullptr)
      continue;
    dm::SignalStore store;
    StaticBackend be(&store);
    const auto *poll = dm::inverter_table_for_protocol(2);
    float pscale = tr->scale;
    for (uint16_t i = 0; poll != nullptr && i < poll->count; i++)
      if (poll->rows[i].sigid == sig)
        pscale = poll->rows[i].scale;
    CHECK(pscale == tr->scale);
    store.set_scalar(0, sig, tr->type, pscale, static_cast<uint32_t>(-4321), 4);
    uint8_t out[4];
    assemble_registers(t, 4, tr->regaddr, 2, be, 0, dm::WordOrder::LOW_WORD_FIRST, out);
    CHECK_EQ((out[0] << 8) | out[1], static_cast<uint16_t>(-4321));
    CHECK_EQ((out[2] << 8) | out[3], 0xFFFF);
  }
}

TEST(other_dev_type_tables_exist) {
  CHECK(dm::tcpslave_table_for_dev_type(1) == dm::tcpslave_table_for_dev_type(21));
  CHECK(dm::tcpslave_table_for_dev_type(35) != nullptr);
  CHECK(dm::tcpslave_table_for_dev_type(18) != nullptr);
  CHECK(dm::tcpslave_table_for_dev_type(44) != nullptr);
  CHECK(dm::tcpslave_table_for_dev_type(99) == nullptr);
}

// ---- allow-list -------------------------------------------------------------------------------------

TEST(ip_rules) {
  IpRule r;
  CHECK(parse_ip_rule("192.0.2.10", &r));
  CHECK_EQ(r.addr, 0xC000020Au);
  CHECK_EQ(r.mask, 0xFFFFFFFFu);
  CHECK(parse_ip_rule("10.0.0.0/8", &r));
  CHECK_EQ(r.mask, 0xFF000000u);
  CHECK(!parse_ip_rule("1.2.3", &r));
  CHECK(!parse_ip_rule("1.2.3.256", &r));
  CHECK(!parse_ip_rule("1.2.3.4/33", &r));
  CHECK(!parse_ip_rule("1.2.3.4x", &r));
  std::vector<IpRule> rules;
  CHECK(ip_allowed(rules, 0x01020304));  // empty = allow all
  IpRule a, b;
  parse_ip_rule("192.0.2.10", &a);
  parse_ip_rule("10.0.0.0/8", &b);
  rules = {a, b};
  CHECK(ip_allowed(rules, 0xC000020A));
  CHECK(!ip_allowed(rules, 0xC000020B));
  CHECK(ip_allowed(rules, 0x0A123456));
}

// ---- :502 country rule ----------------------------------------------------------------------------

TEST(tcp_enabled_for_country_stock_set) {
  CHECK(tcp_enabled_for_country(6));
  CHECK(tcp_enabled_for_country(99));
  for (uint16_t c = 114; c <= 117; c++)
    CHECK(tcp_enabled_for_country(c));
  CHECK(!tcp_enabled_for_country(0));
  CHECK(!tcp_enabled_for_country(50));
  CHECK(!tcp_enabled_for_country(113));
  CHECK(!tcp_enabled_for_country(118));
  CHECK(!tcp_enabled_for_country(0xFFFF));
}

// ---- float32 meter rows (tcpslave_dev18, type 9) and 4-register order -------------------------

static const dm::RegisterTable &T18() { return *dm::tcpslave_table_for_dev_type(18); }

static void put_f32(uint8_t *w, float f, bool low_first) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  uint16_t hi = static_cast<uint16_t>(b >> 16), lo = static_cast<uint16_t>(b);
  uint16_t r0 = low_first ? lo : hi, r1 = low_first ? hi : lo;
  w[0] = r0 >> 8; w[1] = r0 & 0xFF; w[2] = r1 >> 8; w[3] = r1 & 0xFF;
}

static float reply_f32(const std::vector<uint8_t> &r, int i, bool low_first) {
  uint16_t a = reg(r, i), b = reg(r, i + 1);
  uint32_t bits = low_first ? (uint32_t(b) << 16 | a) : (uint32_t(a) << 16 | b);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

TEST(meter_float32_roundtrip_both_word_orders) {
  for (int lf = 0; lf < 2; lf++) {
    for (int fs = 0; fs < 2; fs++) {
      Fixture fx(&T18());
      fx.store.set_float_scale(fs != 0);
      WordOrder wo = lf ? WordOrder::LOW_WORD_FIRST : WordOrder::HIGH_WORD_FIRST;
      fx.opt.word_order = wo;
      // 4097..4102: three voltages (scale 0.1), as the meter sent them.
      uint8_t wire[12];
      put_f32(wire + 0, 2301.5f, lf);
      put_f32(wire + 4, -12.25f, lf);
      put_f32(wire + 8, 0.0f, lf);
      CHECK_EQ(fx.store.ingest_block(0, T18(), 4, 4097, wire, 6, wo), (size_t) 3);
      const dm::Signal *s = fx.store.get(0, 0x2000);
      CHECK(s != nullptr && s->kind == dm::ValueKind::FLOAT);
      std::vector<uint8_t> r;
      Decision d = fx.run(req(1, 1, 4, 4096, 6), &r);
      CHECK_EQ(d.exception, 0);
      CHECK_EQ(r.size(), (size_t) (9 + 12));
      if (fs == 0) {
        // Default: the float is stored in engineering units and re-packed bit for bit.
        CHECK(std::memcmp(r.data() + 9, wire, 12) == 0);
        CHECK_EQ(reply_f32(r, 0, lf), 2301.5f);
        CHECK_EQ(reply_f32(r, 2, lf), -12.25f);
      } else {
        // float_scale on: the row carries the scaled value the store reports.
        CHECK_EQ(reply_f32(r, 0, lf), static_cast<float>(s->number()));
        CHECK(std::fabs(reply_f32(r, 0, lf) - 230.15f) < 1e-3f);
        CHECK(std::fabs(reply_f32(r, 2, lf) - -1.225f) < 1e-4f);
      }
    }
  }
}

TEST(meter_float32_absent_is_nan_and_int_source_converts) {
  Fixture fx(&T18());
  std::vector<uint8_t> r;
  fx.run(req(1, 1, 4, 4108, 2), &r);  // 4109 (sig 0x2006), never stored
  CHECK_EQ(reg(r, 0), 0x0000);         // low word first: 0x7FC00000 -> 0000 7FC0
  CHECK_EQ(reg(r, 1), 0x7FC0);
  fx.store.set_scalar(0, 0x2006, dm::TYPE_S32, 0.001f, static_cast<uint64_t>(-1500) & 0xFFFFFFFFu, 4);
  fx.run(req(1, 1, 4, 4108, 2), &r);
  CHECK(std::fabs(reply_f32(r, 0, true) - -1.5f) < 1e-6f);  // raw x its scale: engineering units
  fx.store.set_scalar(0, 0x2006, dm::TYPE_S32, 0.01f, 7, 4);
  fx.run(req(1, 1, 4, 4108, 2), &r);
  CHECK(std::fabs(reply_f32(r, 0, true) - 0.07f) < 1e-6f);  // 7 x 0.01
  fx.store.clear(0, 0x2006);
  fx.run(req(1, 1, 4, 4108, 2), &r);
  CHECK_EQ(reg(r, 1), 0x7FC0);
}

TEST(four_register_low_word_first_order) {
  static const dm::RegisterRow rows[] = {
      {0x0700, 101, 0, 0xFFFF, 4, 4, dm::TYPE_U64, 0, 0, 0, 0, 0, 0.0f},
  };
  static const dm::RegisterTable t = {0, dm::Family::INVERTER, "test_u64", rows, 1};
  Fixture fx(&t);
  // Wire r0..r3 = 1111 2222 3333 4444 -> stock (r1<<16|r0)<<32 | (r3<<16|r2).
  const uint8_t wire[8] = {0x11, 0x11, 0x22, 0x22, 0x33, 0x33, 0x44, 0x44};
  CHECK_EQ(fx.store.ingest_block(0, t, 4, 101, wire, 4), (size_t) 1);
  const dm::Signal *s = fx.store.get(0, 0x0700);
  CHECK(s != nullptr);
  CHECK_EQ(s->bits(), 0x2222111144443333ULL);
  std::vector<uint8_t> r;
  fx.run(req(1, 1, 4, 100, 4), &r);
  CHECK(std::memcmp(r.data() + 9, wire, 8) == 0);
  fx.store.set_scalar(0, 0x0700, dm::TYPE_U64, 0.0f, 0x0102030405060708ULL, 8);
  fx.run(req(1, 1, 4, 100, 4), &r);
  // high dword 0x01020304 = r1<<16|r0, low dword 0x05060708 = r3<<16|r2.
  CHECK_EQ(reg(r, 0), 0x0304);
  CHECK_EQ(reg(r, 1), 0x0102);
  CHECK_EQ(reg(r, 2), 0x0708);
  CHECK_EQ(reg(r, 3), 0x0506);
}

// ---- writes (fn 6 / fn 16) ------------------------------------------------------------------------
/// Backend whose write gate answers a fixed code and records what it was asked.
struct GateBackend : StaticBackend {
  using StaticBackend::StaticBackend;
  uint8_t answer{0};
  uint8_t fc{0};
  uint16_t start{0}, count{0};
  std::vector<uint16_t> values;
  uint8_t check_write(const UnitTarget &, uint8_t f, uint16_t s, const uint16_t *v, uint16_t n) override {
    fc = f;
    start = s;
    count = n;
    values.assign(v, v + n);
    return answer;
  }
};

static Decision run_gate(GateBackend &be, const std::vector<uint8_t> &bytes, std::vector<uint8_t> *reply) {
  FrameAssembler fa;
  fa.feed(bytes.data(), bytes.size());
  Frame f;
  CHECK(fa.peek(&f) == FrameAssembler::Status::FRAME);
  uint8_t out[MAX_ADU_LEN];
  size_t n = 0;
  Options opt;
  Decision d = handle_frame(f, be, opt, out, &n);
  reply->assign(out, out + n);
  return d;
}

TEST(write_fn6_mkaiser_bytes_forwarded_and_echo) {
  dm::SignalStore store;
  GateBackend be(&store);
  UnitTarget tgt;
  tgt.device = 0;
  tgt.table = &T121();
  tgt.rtu_address = 1;
  be.add_unit(1, tgt);
  // HA modbus.write_register(address: 13049, slave: 1, value: 2) = mkaiser "Forced mode":
  // MBAP tid 0x0007, pid 0, len 6, unit 1, fc 6, wire 0x32F9, value 0x0002.
  std::vector<uint8_t> q = {0x00, 0x07, 0x00, 0x00, 0x00, 0x06, 0x01, 0x06, 0x32, 0xF9, 0x00, 0x02};
  std::vector<uint8_t> r;
  Decision d = run_gate(be, q, &r);
  CHECK(d.kind == Decision::WRITE);
  CHECK(r.empty());  // reply only after the inverter confirmed
  CHECK_EQ(d.fn, 6);
  CHECK_EQ(d.start, 13050);  // document number = wire + 1
  CHECK_EQ(d.count, 1);
  CHECK_EQ(d.values[0], 2);
  CHECK_EQ(d.rtu_address, 1);
  CHECK_EQ(be.start, 13050);
  // Success reply = the request, byte for byte (fn 6 echo).
  ReqHeader h{0x0007, 0, 1, 6};
  uint8_t out[MAX_ADU_LEN];
  size_t n = build_write_reply(h, d.start, d.values[0], 0, out);
  CHECK(n == q.size() && std::memcmp(out, q.data(), n) == 0);
  // Failure -> exception 4 (stock).
  n = build_write_reply(h, d.start, d.values[0], EX_DEVICE_FAILURE, out);
  std::vector<uint8_t> want = {0x00, 0x07, 0x00, 0x00, 0x00, 0x03, 0x01, 0x86, 0x04};
  CHECK(n == want.size() && std::memcmp(out, want.data(), n) == 0);
}

TEST(write_fn16_parse_and_reply) {
  dm::SignalStore store;
  GateBackend be(&store);
  UnitTarget tgt;
  tgt.device = 3;
  tgt.table = &T121();
  tgt.rtu_address = 2;
  be.add_unit(2, tgt);
  // Write multiple 13049..13051 (wire) = EMS 2, cmd 0xAA, power 3000 W.
  std::vector<uint8_t> q = {0x12, 0x34, 0x00, 0x00, 0x00, 0x0D, 0x02, 0x10, 0x32, 0xF9, 0x00, 0x03,
                            0x06, 0x00, 0x02, 0x00, 0xAA, 0x0B, 0xB8};
  std::vector<uint8_t> r;
  Decision d = run_gate(be, q, &r);
  CHECK(d.kind == Decision::WRITE);
  CHECK_EQ(d.fn, 16);
  CHECK_EQ(d.device, 3);
  CHECK_EQ(d.rtu_address, 2);
  CHECK_EQ(d.count, 3);
  CHECK(d.values[0] == 2 && d.values[1] == 0xAA && d.values[2] == 3000);
  CHECK(be.values.size() == 3 && be.values[2] == 3000);
  ReqHeader h{0x1234, 0, 2, 0x10};
  uint8_t out[MAX_ADU_LEN];
  size_t n = build_write_reply(h, d.start, d.count, 0, out);
  std::vector<uint8_t> want = {0x12, 0x34, 0x00, 0x00, 0x00, 0x06, 0x02, 0x10, 0x32, 0xF9, 0x00, 0x03};
  CHECK(n == want.size() && std::memcmp(out, want.data(), n) == 0);
  // Malformed: byte count != 2 x count, count 0, count 124, short fn 6.
  auto bad = q;
  bad[12] = 0x04;
  CHECK_EQ(run_gate(be, bad, &r).exception, EX_ILLEGAL_VALUE);
  std::vector<uint8_t> z = {0, 1, 0, 0, 0, 7, 2, 0x10, 0x32, 0xF9, 0x00, 0x00, 0x00};
  CHECK_EQ(run_gate(be, z, &r).exception, EX_ILLEGAL_VALUE);
  std::vector<uint8_t> big = {0, 1, 0, 0, 0, 7, 2, 0x10, 0x32, 0xF9, 0x00, 124, 248};
  CHECK_EQ(run_gate(be, big, &r).exception, EX_ILLEGAL_VALUE);
  std::vector<uint8_t> s6 = {0, 1, 0, 0, 0, 5, 2, 0x06, 0x32, 0xF9, 0x00};
  CHECK_EQ(run_gate(be, s6, &r).exception, EX_ILLEGAL_VALUE);
}

TEST(write_gate_codes_and_other_fcs) {
  dm::SignalStore store;
  GateBackend be(&store);
  UnitTarget tgt;
  tgt.table = &T121();
  tgt.rtu_address = 1;
  be.add_unit(1, tgt);
  std::vector<uint8_t> q = {0, 9, 0, 0, 0, 6, 1, 0x06, 0x32, 0xC7, 0x00, 0xCF};  // 13000 start/stop
  std::vector<uint8_t> r;
  for (uint8_t code : {EX_ILLEGAL_FUNCTION, EX_ILLEGAL_ADDRESS, EX_ILLEGAL_VALUE, EX_BUSY, EX_GATEWAY_TARGET}) {
    be.answer = code;
    Decision d = run_gate(be, q, &r);
    CHECK(d.kind == Decision::REPLY);
    CHECK_EQ(d.exception, code);
    CHECK(r.size() == 9 && r[7] == 0x86 && r[8] == code);
  }
  // fn 5 / fn 0x60 are never forwarded (and never reach the gate).
  be.answer = 0;
  be.fc = 0;
  std::vector<uint8_t> c5 = {0, 9, 0, 0, 0, 6, 1, 0x05, 0x00, 0x01, 0xFF, 0x00};
  CHECK_EQ(run_gate(be, c5, &r).exception, EX_ILLEGAL_FUNCTION);
  CHECK_EQ(be.fc, 0);
  // Unknown unit: 0x0B before any gate.
  std::vector<uint8_t> u9 = {0, 9, 0, 0, 0, 6, 9, 0x06, 0x32, 0xF9, 0x00, 0x02};
  CHECK_EQ(run_gate(be, u9, &r).exception, EX_GATEWAY_TARGET);
  // No RS485 hub (rtu_address 0): gateway path unavailable.
  GateBackend nb(&store);
  UnitTarget t0;
  t0.table = &T121();
  nb.add_unit(1, t0);
  std::vector<uint8_t> ok = {0, 9, 0, 0, 0, 6, 1, 0x06, 0x32, 0xF9, 0x00, 0x02};
  CHECK_EQ(run_gate(nb, ok, &r).exception, EX_GATEWAY_PATH);
}

int main() { return th::run_all(); }
