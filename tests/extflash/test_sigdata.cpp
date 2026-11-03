// Host tests for components/sungrow_extflash/sigdata.{h,cpp}. Build + run: `make -C tests/extflash`.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sigdata.h"

using namespace esphome::sungrow_extflash;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    g_checks++;                                                        \
    if (!(cond)) {                                                     \
      g_fail++;                                                        \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                  \
  } while (0)

struct Builder {
  std::vector<uint8_t> b;
  Builder &rec(uint16_t id, const std::vector<uint8_t> &data) { return this->rec_len(id, data.size(), data); }
  Builder &rec_len(uint16_t id, size_t len, const std::vector<uint8_t> &data) {
    b.push_back(id & 0xFF);
    b.push_back(id >> 8);
    b.push_back(len & 0xFF);
    b.push_back((len >> 8) & 0xFF);
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
};

static void test_empty() {
  SigdataFile f;
  CHECK(f.parse(nullptr, 0));
  CHECK(f.loaded());
  CHECK(f.records().empty());
  CHECK(!f.truncated());
  CHECK(!f.get(sig::SN).has_value());
  CHECK(f.get_string(sig::SN).empty());
  Identity id = decode_identity(f, 0x720);
  CHECK(!id.valid());
  CHECK(id.broker_host == "iot.isolarcloud.com");
  CHECK(id.broker_port == 16668);
  CHECK(id.compress_mode == 1);
  CHECK(id.ssl_flag == 2);
  CHECK(id.old_ssl_flag == 2);
  CHECK(id.timezone_id == 8);
  CHECK(id.rundata_period == 5);
  CHECK(id.remote_ctl_mask == 0x4FF);
  CHECK(id.product_type == 0x720);
  CHECK(id.mqtt_key_str() == "Sun#0*2Pow925Mdk");
  CHECK(id.old_mqtt_key_str() == "Sun#0*2Pow925Mdk");
  CHECK(id.from_file.empty());
}

static void test_normal() {
  Builder b;
  b.str(sig::SN, "B2101234567", 20)
      .u16(sig::PRODUCT_TYPE, 1824)
      .u16(sig::ENCRYPT_MODE, 1)
      .u16(sig::KEY_NUMBER, 3)
      .u16(sig::COMPRESS_MODE, 0)
      .u16(sig::SSL_FLAG, 1)
      .u16(sig::OLD_SSL_FLAG, 0)
      .str(sig::BROKER_HOST, "gateway.isolarcloud.eu", 64)
      .u16(sig::BROKER_PORT, 19999)
      .str(sig::MQTT_KEY, "ABCDEFGHIJKLMNOP", 16)
      .str(sig::OLD_BROKER_HOST, "old.example", 64)
      .u16(sig::OLD_BROKER_PORT, 1883)
      .str(sig::OLD_MQTT_KEY, "0123456789abcdef", 16)
      .u16(sig::TIMEZONE_ID, 12)
      .u16(sig::RUNDATA_PERIOD, 10)
      .u16(sig::SILENCE_ENABLE, 1)
      .u16(sig::SILENCE_START, 22)
      .u16(sig::SILENCE_END, 6)
      .u32(sig::REMOTE_CTL_MASK, 0x12345678)
      .str(0x1053, "MyWifi", 32);  // unknown-length sig: accepted with any len
  SigdataFile f;
  CHECK(f.parse(b.b.data(), b.b.size()));
  CHECK(f.records().size() == 20);
  CHECK(!f.truncated());
  CHECK(f.ignored_records() == 0);
  CHECK(f.parsed_bytes() == b.b.size());
  CHECK(f.get_string(sig::SN) == "B2101234567");
  CHECK(f.get(sig::SN)->size() == 20);
  CHECK(f.get_u16(sig::BROKER_PORT).value_or(0) == 19999);
  CHECK(f.get_u32(sig::REMOTE_CTL_MASK).value_or(0) == 0x12345678);
  CHECK(!f.get_u16(sig::SN).has_value());  // wrong width
  CHECK(f.get_string(0x1053) == "MyWifi");
  Identity id = decode_identity(f, 0);
  CHECK(id.valid());
  CHECK(id.sn == "B2101234567");
  CHECK(id.product_type == 1824);
  CHECK(id.encrypt_mode == 1);
  CHECK(id.key_number == 3);
  CHECK(id.compress_mode == 0);
  CHECK(id.ssl_flag == 1);
  CHECK(id.old_ssl_flag == 0);
  CHECK(id.broker_host == "gateway.isolarcloud.eu");
  CHECK(id.broker_port == 19999);
  CHECK(id.mqtt_key_str() == "ABCDEFGHIJKLMNOP");
  CHECK(id.old_broker_host == "old.example");
  CHECK(id.old_broker_port == 1883);
  CHECK(id.old_mqtt_key_str() == "0123456789abcdef");
  CHECK(id.timezone_id == 12);
  CHECK(id.rundata_period == 10);
  CHECK(id.silence_enable == 1);
  CHECK(id.silence_start == 22);
  CHECK(id.silence_end == 6);
  CHECK(id.remote_ctl_mask == 0x12345678);
  CHECK(id.is_from_file(sig::SN));
  CHECK(id.is_from_file(sig::REMOTE_CTL_MASK));
  CHECK(id.from_file.size() == 19);
}

static void test_duplicates_first_wins() {
  Builder b;
  b.str(sig::SN, "FIRST", 20).u16(sig::BROKER_PORT, 1111).str(sig::SN, "SECOND", 20).u16(sig::BROKER_PORT, 2222);
  SigdataFile f;
  f.parse(b.b.data(), b.b.size());
  CHECK(f.records().size() == 4);
  CHECK(f.get_string(sig::SN) == "FIRST");
  CHECK(f.get_u16(sig::BROKER_PORT).value_or(0) == 1111);
}

static void test_wrong_len_ignored() {
  Builder b;
  // SN with 16 B (declared 20) is ignored; the later 20 B one is used.
  b.str(sig::SN, "BADLEN", 16)
      .rec(sig::BROKER_PORT, {0x01, 0x02, 0x03})  // 3 B port: ignored -> default
      .rec(sig::REMOTE_CTL_MASK, {0x01, 0x00})    // 2 B u32: ignored -> default
      .str(sig::SN, "GOODSN123", 20)
      .str(sig::BROKER_HOST, "short", 32);  // 32 B host (declared 64): ignored -> default
  SigdataFile f;
  f.parse(b.b.data(), b.b.size());
  CHECK(f.records().size() == 5);
  CHECK(f.ignored_records() == 4);
  CHECK(!f.truncated());
  CHECK(f.get_string(sig::SN) == "GOODSN123");
  CHECK(!f.get_u16(sig::BROKER_PORT).has_value());
  Identity id = decode_identity(f, 0x720);
  CHECK(id.sn == "GOODSN123");
  CHECK(id.broker_port == 16668);
  CHECK(id.broker_host == "iot.isolarcloud.com");
  CHECK(id.remote_ctl_mask == 0x4FF);
  CHECK(!id.is_from_file(sig::BROKER_PORT));
}

static void test_truncated() {
  Builder b;
  b.str(sig::SN, "SN0001", 20).u16(sig::BROKER_PORT, 1234).str(sig::BROKER_HOST, "cut.example", 64);
  // Cut in the middle of the last record's data.
  std::vector<uint8_t> cut(b.b.begin(), b.b.end() - 10);
  SigdataFile f;
  f.parse(cut.data(), cut.size());
  CHECK(f.truncated());
  CHECK(f.records().size() == 2);
  CHECK(f.get_string(sig::SN) == "SN0001");
  CHECK(f.get_u16(sig::BROKER_PORT).value_or(0) == 1234);
  CHECK(!f.has(sig::BROKER_HOST));
  // Cut inside a header.
  Builder b2;
  b2.u16(sig::BROKER_PORT, 42);
  b2.b.push_back(0x02);
  b2.b.push_back(0x00);
  b2.b.push_back(0x14);
  SigdataFile f2;
  f2.parse(b2.b.data(), b2.b.size());
  CHECK(f2.truncated());
  CHECK(f2.records().size() == 1);
  CHECK(f2.parsed_bytes() == 6);
}

static void test_len_limit_and_erased() {
  // Erased-looking tail (id 0xFFFF len 0xFFFF): every 4 B header is over-cap and skipped
  // stock-style (len := 0, continue 4 B on); nothing is lost before it.
  Builder b;
  b.u16(sig::BROKER_PORT, 7);
  b.b.insert(b.b.end(), 64, 0xFF);
  SigdataFile f;
  f.parse(b.b.data(), b.b.size());
  CHECK(!f.truncated());
  CHECK(f.resyncs() == 16);
  CHECK(f.records().size() == 1);
  CHECK(f.parsed_bytes() == b.b.size());
  CHECK(f.get_u16(sig::BROKER_PORT).value_or(0) == 7);
  // 0xFF tail not a multiple of 4: the last < 4 B are an incomplete header.
  Builder b1 = b;
  b1.b.push_back(0xFF);
  b1.b.push_back(0xFF);
  SigdataFile f1;
  f1.parse(b1.b.data(), b1.b.size());
  CHECK(f1.truncated());
  CHECK(f1.resyncs() == 16);

  // S2 cap (default): len exactly 0x420 is allowed.
  Builder b3;
  b3.rec(0x1234, std::vector<uint8_t>(0x420, 0xAB)).u16(sig::BROKER_PORT, 9);
  SigdataFile f3;
  CHECK(f3.max_record_len() == SIGDATA_MAX_RECORD_LEN_S2);
  f3.parse(b3.b.data(), b3.b.size());
  CHECK(!f3.truncated());
  CHECK(f3.resyncs() == 0);
  CHECK(f3.records().size() == 2);
  CHECK(f3.get(0x1234)->size() == 0x420);
  CHECK(f3.get_u16(sig::BROKER_PORT).value_or(0) == 9);

  // len 0x421 on S2: header skipped, the scan resyncs inside the data. Data chosen so the resync
  // lands on a valid header right after the bad one: a real record "hidden" in the bad data.
  Builder inner;
  inner.u16(sig::BROKER_PORT, 4242);
  std::vector<uint8_t> data(inner.b);
  Builder b4;
  b4.rec_len(0x1234, 0x421, data);  // header claims 0x421, only 6 B follow
  SigdataFile f4;
  f4.parse(b4.b.data(), b4.b.size());
  CHECK(f4.resyncs() == 1);
  CHECK(!f4.truncated());
  CHECK(f4.records().size() == 1);
  CHECK(f4.get_u16(sig::BROKER_PORT).value_or(0) == 4242);

  // WiNet-S cap 0x100: 0x100 accepted, 0x101 resynced; the same 0x101 record is fine on S2.
  Builder b5;
  b5.rec(0x177b, std::vector<uint8_t>(0x100, 'a')).u16(sig::BROKER_PORT, 11);
  SigdataFile f5(SIGDATA_MAX_RECORD_LEN_S);
  f5.parse(b5.b.data(), b5.b.size());
  CHECK(f5.resyncs() == 0);
  CHECK(f5.records().size() == 2);
  CHECK(f5.get(0x177b)->size() == 0x100);

  Builder b6;
  b6.rec(0x1234, std::vector<uint8_t>(0x101, 0x00)).str(sig::SN, "AFTERBIG", 20);
  SigdataFile f6s(SIGDATA_MAX_RECORD_LEN_S);
  f6s.parse(b6.b.data(), b6.b.size());
  // 0x101 zero bytes = 64 headers {id 0, len 0} + 1 byte; then misaligned from the real SN header.
  CHECK(f6s.resyncs() >= 1);  // more follow: the scan is misaligned after the zero run
  CHECK(!f6s.has(0x1234));
  CHECK(f6s.parsed_bytes() <= b6.b.size());
  SigdataFile f6(SIGDATA_MAX_RECORD_LEN_S2);
  f6.parse(b6.b.data(), b6.b.size());
  CHECK(f6.resyncs() == 0);
  CHECK(f6.records().size() == 2);
  CHECK(f6.get_string(sig::SN) == "AFTERBIG");
  // set_max_record_len() survives clear()/parse().
  SigdataFile f7;
  f7.set_max_record_len(SIGDATA_MAX_RECORD_LEN_S);
  f7.parse(b6.b.data(), b6.b.size());
  CHECK(f7.resyncs() == f6s.resyncs());
  CHECK(f7.max_record_len() == SIGDATA_MAX_RECORD_LEN_S);
}

static void test_winet_s_no_6492() {
  // WiNet-S file: no 0x6492 in its sig table -> remote-control mask = 0x4FF default.
  Builder b;
  b.str(sig::SN, "A2291234567", 20).u16(sig::PRODUCT_TYPE, 1813);
  SigdataFile f(SIGDATA_MAX_RECORD_LEN_S);
  f.parse(b.b.data(), b.b.size());
  Identity id = decode_identity(f, 0x715);
  CHECK(id.valid());
  CHECK(id.remote_ctl_mask == 0x4FF);
  CHECK(!id.is_from_file(sig::REMOTE_CTL_MASK));
  CHECK(id.product_type == 1813);
  // Empty file on S: default productType comes from the board.
  SigdataFile e(SIGDATA_MAX_RECORD_LEN_S);
  e.parse(nullptr, 0);
  CHECK(decode_identity(e, 0x715).product_type == 0x715);
  CHECK(decode_identity(e, 0x715).remote_ctl_mask == 0x4FF);
}

static void test_strings() {
  Builder b;
  // Unpadded 20 B SN (no NUL), and an all-NUL broker host (keeps default).
  b.str(sig::SN, "ABCDEFGHIJKLMNOPQRST", 20).str(sig::BROKER_HOST, "", 64);
  SigdataFile f;
  f.parse(b.b.data(), b.b.size());
  CHECK(f.get_string(sig::SN) == "ABCDEFGHIJKLMNOPQRST");
  Identity id = decode_identity(f, 0x720);
  CHECK(id.valid());
  CHECK(id.broker_host == "iot.isolarcloud.com");
  CHECK(!id.is_from_file(sig::BROKER_HOST));
  // Non-printable SN is not valid.
  Builder b2;
  b2.rec(sig::SN, std::vector<uint8_t>(20, 0xFF));
  SigdataFile f2;
  f2.parse(b2.b.data(), b2.b.size());
  CHECK(!decode_identity(f2).valid());
  CHECK(!sn_plausible(""));
  CHECK(!sn_plausible("A B"));
  CHECK(sn_plausible("B2101234567"));
}

static void test_choose_identity_file() {
  using C = IdentityCandidate;
  // Normal case: the live file has an SN -> it wins, same as stock.
  IdentityChoice c = choose_identity_file({C{true, 500, true}, C{true, 500, true}});
  CHECK(c.index == 0 && c.rule == IdentityRule::FIRST_WITH_SN && c.stock_index == 0);
  // First file >= 10 B has no SN, a later one does -> ours takes the later one; stock the first.
  c = choose_identity_file({C{true, 300, false}, C{false, 0, false}, C{true, 400, true}});
  CHECK(c.index == 2 && c.rule == IdentityRule::FIRST_WITH_SN && c.stock_index == 0);
  // Small / unreadable files are skipped by the stock rule but cannot hide an SN file.
  c = choose_identity_file({C{false, 0, false}, C{true, 4, false}, C{true, 24, true}});
  CHECK(c.index == 2 && c.rule == IdentityRule::FIRST_WITH_SN && c.stock_index == 2);
  // No SN anywhere -> stock's rule: first readable file of >= 10 B (exactly 10 counts).
  c = choose_identity_file({C{true, 9, false}, C{true, 10, false}, C{true, 900, false}});
  CHECK(c.index == 1 && c.rule == IdentityRule::STOCK_FIRST_MIN_SIZE && c.stock_index == 1);
  // Nothing usable -> none, defaults only.
  c = choose_identity_file({C{false, 0, false}, C{true, 0, false}, C{true, 9, false}});
  CHECK(c.index == -1 && c.rule == IdentityRule::NONE && c.stock_index == -1);
  c = choose_identity_file({});
  CHECK(c.index == -1 && c.rule == IdentityRule::NONE);
  CHECK(std::strcmp(identity_rule_str(IdentityRule::FIRST_WITH_SN), "first file with a plausible SN") == 0);
  CHECK(std::strstr(identity_rule_str(IdentityRule::STOCK_FIRST_MIN_SIZE), ">= 10 B") != nullptr);
}

static void test_load_file(const char *tmpdir) {
  Builder b;
  b.str(sig::SN, "FILESN", 20).u16(sig::BROKER_PORT, 555);
  std::string path = std::string(tmpdir) + "/dev_file_247_1824_2";
  FILE *fp = std::fopen(path.c_str(), "wb");
  CHECK(fp != nullptr);
  if (fp == nullptr)
    return;
  std::fwrite(b.b.data(), 1, b.b.size(), fp);
  std::fclose(fp);
  SigdataFile f;
  CHECK(f.load(path));
  CHECK(f.file_size() == b.b.size());
  CHECK(f.get_string(sig::SN) == "FILESN");
  CHECK(!f.load(std::string(tmpdir) + "/does_not_exist"));
  CHECK(!f.loaded());
  // Empty file loads fine, with no records.
  std::string empty = std::string(tmpdir) + "/empty";
  fp = std::fopen(empty.c_str(), "wb");
  std::fclose(fp);
  CHECK(f.load(empty));
  CHECK(f.records().empty());
  CHECK(!f.truncated());
}

// RAM bounds: at most SIGDATA_MAX_RECORDS records and SIGDATA_MAX_FILE_SIZE bytes are kept; the
// rest is flagged (truncated / oversize), never an error.
static void test_ram_caps(const char *tmpdir) {
  std::vector<uint8_t> img;
  for (size_t i = 0; i < SIGDATA_MAX_RECORDS + 100; i++) {
    const uint16_t id = static_cast<uint16_t>(0x7000 + i);
    img.insert(img.end(), {static_cast<uint8_t>(id & 0xFF), static_cast<uint8_t>(id >> 8), 2, 0, 0x34, 0x12});
  }
  SigdataFile f;
  CHECK(f.parse(img.data(), img.size()));
  CHECK(f.records().size() == SIGDATA_MAX_RECORDS);
  CHECK(f.truncated());
  CHECK(!f.oversize());

  std::vector<uint8_t> big(SIGDATA_MAX_FILE_SIZE + 4096, 0xFF);
  const uint8_t sn[] = {0x02, 0x00, 20, 0x00};
  std::copy(sn, sn + 4, big.begin());
  const char *serial = "B2101234567";
  std::memset(big.data() + 4, 0, 20);
  std::memcpy(big.data() + 4, serial, std::strlen(serial));
  const std::string path = std::string(tmpdir) + "/big_sigdata";
  FILE *out = std::fopen(path.c_str(), "wb");
  CHECK(out != nullptr);
  if (out != nullptr) {
    std::fwrite(big.data(), 1, big.size(), out);
    std::fclose(out);
  }
  SigdataFile g;
  CHECK(g.load(path));
  CHECK(g.oversize());
  CHECK(g.file_size() == SIGDATA_MAX_FILE_SIZE);
  CHECK(g.get_string(sig::SN) == serial);
  std::remove(path.c_str());
}

int main(int argc, char **argv) {
  const char *tmpdir = argc > 1 ? argv[1] : "/tmp";
  test_empty();
  test_normal();
  test_duplicates_first_wins();
  test_wrong_len_ignored();
  test_truncated();
  test_len_limit_and_erased();
  test_winet_s_no_6492();
  test_strings();
  test_choose_identity_file();
  test_load_file(tmpdir);
  test_ram_caps(tmpdir);
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
