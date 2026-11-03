// Host tests for the sungrow_debug flash-layout core (partition table / otadata / image headers /
// dump regions). The kPt fixture is a synthetic partition table (the historical candidate layout)
// used ONLY to exercise the parser; it is NOT the device's real table — see
// boards/partitions_winet_s.stock.csv for the authoritative live-decoded layout.
#include <cstring>
#include <string>
#include <vector>

#include "../test_harness.h"
#include "flash_layout.h"

using namespace esphome::sungrow_debug;

static const uint8_t kPt[256] = {
    0xaa,0x50,0x01,0x02,0x00,0x90,0x00,0x00,0x00,0x40,0x00,0x00,0x6e,0x76,0x73,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xaa,0x50,0x01,0x00,0x00,0xd0,0x00,0x00,0x00,0x20,0x00,0x00,0x6f,0x74,0x61,0x64,
    0x61,0x74,0x61,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xaa,0x50,0x01,0x01,0x00,0xf0,0x00,0x00,0x00,0x10,0x00,0x00,0x70,0x68,0x79,0x5f,
    0x69,0x6e,0x69,0x74,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xaa,0x50,0x00,0x10,0x00,0x00,0x01,0x00,0x00,0x00,0x40,0x00,0x6f,0x74,0x61,0x5f,
    0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xaa,0x50,0x00,0x11,0x00,0x00,0x41,0x00,0x00,0x00,0x40,0x00,0x6f,0x74,0x61,0x5f,
    0x31,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xaa,0x50,0x01,0x81,0x00,0x00,0x81,0x00,0x00,0x00,0x30,0x00,0x6f,0x65,0x6d,0x64,
    0x61,0x74,0x61,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xaa,0x50,0x01,0x03,0x00,0x00,0xb1,0x00,0x00,0x00,0x01,0x00,0x63,0x6f,0x72,0x65,
    0x64,0x75,0x6d,0x70,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xeb,0xeb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0x2e,0x44,0xf3,0x7a,0xb3,0xf3,0x1a,0x82,0x73,0xfa,0x7b,0x9d,0x12,0x5c,0x92,0xf4};

static std::string hexs(const uint8_t *d, size_t n) {
  static const char *h = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) {
    s += h[d[i] >> 4];
    s += h[d[i] & 15];
  }
  return s;
}

TEST(md5_and_crc_vectors) {
  uint8_t d[16];
  md5(reinterpret_cast<const uint8_t *>(""), 0, d);
  CHECK(hexs(d, 16) == "d41d8cd98f00b204e9800998ecf8427e");
  md5(reinterpret_cast<const uint8_t *>("abc"), 3, d);
  CHECK(hexs(d, 16) == "900150983cd24fb0d6963f7d28e17f72");
  std::string m(1000, 'a');
  md5(reinterpret_cast<const uint8_t *>(m.data()), m.size(), d);
  CHECK(hexs(d, 16) == "cabe45dcc9ae5b66ba86600cca6b8ba8");
  const uint8_t one[4] = {1, 0, 0, 0};
  CHECK_EQ(crc32_le(0xFFFFFFFFu, one, 4), 0x4743989Au);  // otadata seq 1
  CHECK_EQ(crc32_le(0, reinterpret_cast<const uint8_t *>("123456789"), 9), 0xCBF43926u);
}

TEST(partition_table_fixture) {
  uint8_t buf[0xC00];
  std::memset(buf, 0xFF, sizeof(buf));
  std::memcpy(buf, kPt, sizeof(kPt));
  PartTable t = parse_partition_table(buf, sizeof(buf));
  CHECK_EQ(t.entries.size(), 7u);
  CHECK(t.md5_present && t.md5_ok && t.terminated);
  CHECK(t.error.empty());
  CHECK(std::string(t.entries[0].label) == "nvs");
  CHECK_EQ(t.entries[3].offset, 0x10000u);
  CHECK_EQ(t.entries[5].subtype, 0x81);
  auto csv = partition_csv(t);
  CHECK(csv[2] == "nvs,data,nvs,0x9000,16K,");
  CHECK(csv[5] == "ota_0,app,ota_0,0x10000,4M,");
  CHECK(csv[7] == "oemdata,data,fat,0x810000,3M,");
  CHECK(csv[9] == "# MD5 entry: present, OK");
  CHECK(check_partition_table(t, 16u << 20).empty());
  CHECK(!check_partition_table(t, 8u << 20).empty());  // oemdata/coredump beyond 8 MB
  buf[20] ^= 1;  // corrupt a label byte -> MD5 mismatch
  CHECK(!parse_partition_table(buf, sizeof(buf)).md5_ok);
}

TEST(partition_table_bad) {
  uint8_t ff[64];
  std::memset(ff, 0xFF, sizeof(ff));
  PartTable e = parse_partition_table(ff, sizeof(ff));
  CHECK(e.entries.empty() && !e.error.empty());
  uint8_t junk[64] = {0x12, 0x34};
  CHECK_EQ(parse_partition_table(junk, sizeof(junk)).bad_entry, 0);
  CHECK(format_part_size(0x1234) == "0x1234");
  char tmp[8];
  CHECK(std::string(part_subtype_name(0, 0x11, tmp)) == "ota_1");
  CHECK(std::string(part_subtype_name(1, 0x99, tmp)) == "0x99");
}

static void put_entry(uint8_t *p, uint32_t seq, uint32_t state) {
  std::memset(p, 0xFF, 32);
  std::memcpy(p, &seq, 4);
  std::memcpy(p + 24, &state, 4);
  uint32_t c = crc32_le(0xFFFFFFFFu, p, 4);
  std::memcpy(p + 28, &c, 4);
}

TEST(otadata_parse) {
  std::vector<uint8_t> d(0x2000, 0xFF);
  OtaData o = parse_otadata(d.data(), d.size(), 2);
  CHECK_EQ(o.active, -1);
  CHECK(o.e[0].erased && o.e[1].erased);
  put_entry(d.data(), 1, OTA_STATE_VALID);
  o = parse_otadata(d.data(), d.size(), 2);
  CHECK_EQ(o.active, 0);
  CHECK_EQ(o.active_slot, 0);
  put_entry(d.data() + 0x1000, 2, OTA_STATE_PENDING);  // after an OTA into ota_1
  o = parse_otadata(d.data(), d.size(), 2);
  CHECK_EQ(o.active, 1);
  CHECK_EQ(o.active_slot, 1);
  CHECK(describe_otadata(o).find("PENDING_VERIFY") != std::string::npos);
  put_entry(d.data() + 0x1000, 2, OTA_STATE_INVALID);  // rolled back
  CHECK_EQ(parse_otadata(d.data(), d.size(), 2).active_slot, 0);
  d[0x1000 + 28] ^= 1;  // bad CRC
  put_entry(d.data() + 0x1000, 3, OTA_STATE_VALID);
  d[0x1000 + 28] ^= 1;
  OtaData b = parse_otadata(d.data(), d.size(), 2);
  CHECK(!b.e[1].crc_ok);
  CHECK_EQ(b.active, 0);
}

TEST(image_header_and_descs) {
  uint8_t h[24] = {0xE9, 7, 2, 0x4F, 0x18, 0x08, 0x08, 0x40};
  h[12] = 0;
  h[23] = 1;
  ImageHeader ih = parse_image_header(h, sizeof(h));
  CHECK(ih.ok);
  CHECK_EQ(ih.segments, 7);
  CHECK(std::string(flash_mode_str(ih.spi_mode)) == "DIO");
  CHECK(std::string(flash_freq_str(ih.spi_speed)) == "80MHz");
  CHECK(std::string(flash_size_str(ih.spi_size)) == "16MB");
  CHECK(describe_image_header(ih).find("DIO 80MHz 16MB") != std::string::npos);
  h[0] = 0;
  CHECK(!parse_image_header(h, sizeof(h)).ok);

  uint8_t bd[80] = {0x50, 0, 0, 0, 1, 0, 0, 0};
  std::memcpy(bd + 8, "v5.2.1", 6);
  std::memcpy(bd + 40, "Jan  1 2025 00:00:00", 20);
  BootloaderDesc b = parse_bootloader_desc(bd, sizeof(bd));
  CHECK(b.ok && b.idf_ver == "v5.2.1" && b.version == 1);
  uint8_t ad[0x100] = {0x32, 0x54, 0xCD, 0xAB};
  std::memcpy(ad + 16, "2026.9.1", 8);
  std::memcpy(ad + 48, "winet-s", 7);
  std::memcpy(ad + 112, "v5.5.5", 6);
  AppDesc a = parse_app_desc(ad, sizeof(ad));
  CHECK(a.ok && a.version == "2026.9.1" && a.project_name == "winet-s" && a.idf_ver == "v5.5.5");
  ad[0] = 0;
  CHECK(!parse_app_desc(ad, sizeof(ad)).ok);
}

TEST(region_plan) {
  std::vector<PartInfo> parts = {
      {"nvs", 1, 2, 0x9000, 0x4000, false, false},       {"otadata", 1, 0, 0xd000, 0x2000, false, false},
      {"phy_init", 1, 1, 0xf000, 0x1000, false, false},  {"ota_0", 0, 0x10, 0x10000, 0x400000, false, false},
      {"ota_1", 0, 0x11, 0x410000, 0x400000, false, true}, {"oemdata", 1, 0x81, 0x810000, 0x300000, false, false},
      {"coredump", 1, 3, 0xb10000, 0x10000, false, false}, {"usrdataout", 1, 0x81, 0, 0x2000000, true, false},
  };
  auto r = plan_regions(parts, 16u << 20);
  CHECK_EQ(r.size(), 11u);
  const Region *b = find_region(r, "bootloader");
  CHECK(b && b->offset == 0x1000 && b->size == 0x7000);
  CHECK(find_region(r, "ptable") && find_region(r, "ptable")->size == 0xC00);
  CHECK(find_region(r, "phy") && find_region(r, "phy")->label == "phy_init");
  CHECK(find_region(r, "inactive_app") && find_region(r, "inactive_app")->label == "ota_0");
  CHECK(find_region(r, "running_app") && find_region(r, "running_app")->offset == 0x410000);
  CHECK(find_region(r, "extflash") && find_region(r, "extflash")->external);
  CHECK(find_region(r, "oemdata") && find_region(r, "coredump") && find_region(r, "nvs") && find_region(r, "otadata"));
  CHECK(find_region(r, "flash") && find_region(r, "flash")->size == (16u << 20));
  CHECK(find_region(r, "nope") == nullptr);
  // duplicate aliases fall back to the label
  parts.push_back({"nvs2", 1, 2, 0xc00000, 0x4000, false, false});
  auto r2 = plan_regions(parts, 16u << 20);
  CHECK(find_region(r2, "nvs2") != nullptr);
  CHECK(find_region(r2, "nvs")->offset == 0x9000);
}

TEST(raw_range_and_u32) {
  const uint32_t fs = 16u << 20;
  CHECK(validate_raw_range(0, 0x1000, fs));
  CHECK(validate_raw_range(fs - 1, 1, fs));
  CHECK(!validate_raw_range(fs - 1, 2, fs));
  CHECK(!validate_raw_range(0, 0, fs));
  CHECK(!validate_raw_range(fs, 1, fs));
  CHECK(!validate_raw_range(0xFFFFFFFFull, 0x10, fs));
  uint32_t v = 0;
  CHECK(parse_u32("0x8000", &v) && v == 0x8000);
  CHECK(parse_u32("4096", &v) && v == 4096);
  CHECK(!parse_u32("", &v));
  CHECK(!parse_u32("12x", &v));
  CHECK(!parse_u32("-1", &v));
}

int main() { return th::run_all(); }
