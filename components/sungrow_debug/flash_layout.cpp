#include "flash_layout.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace esphome {
namespace sungrow_debug {

static inline uint32_t le32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
static inline uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

static std::string fixed_str(const uint8_t *p, size_t n) {
  std::string s;
  for (size_t i = 0; i < n && p[i] != 0; i++)
    s += (p[i] >= 0x20 && p[i] < 0x7F) ? static_cast<char>(p[i]) : '.';
  return s;
}

// ---- MD5 (RFC 1321) ----------------------------------------------------------------------------
namespace {
struct Md5 {
  uint32_t a{0x67452301}, b{0xefcdab89}, c{0x98badcfe}, d{0x10325476};
  uint64_t len{0};
  uint8_t buf[64]{};
  size_t n{0};
  static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
  void block(const uint8_t *p) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,
                              14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
      M[i] = le32(p + 4 * i);
    uint32_t A = a, B = b, C = c, D = d;
    for (int i = 0; i < 64; i++) {
      uint32_t F;
      int g;
      if (i < 16) {
        F = (B & C) | (~B & D);
        g = i;
      } else if (i < 32) {
        F = (D & B) | (~D & C);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        F = B ^ C ^ D;
        g = (3 * i + 5) % 16;
      } else {
        F = C ^ (B | ~D);
        g = (7 * i) % 16;
      }
      F = F + A + K[i] + M[g];
      A = D;
      D = C;
      C = B;
      B = B + rol(F, S[i]);
    }
    a += A;
    b += B;
    c += C;
    d += D;
  }
  void update(const uint8_t *p, size_t l) {
    len += l;
    while (l > 0) {
      size_t k = 64 - n < l ? 64 - n : l;
      std::memcpy(buf + n, p, k);
      n += k;
      p += k;
      l -= k;
      if (n == 64) {
        block(buf);
        n = 0;
      }
    }
  }
  void final(uint8_t out[16]) {
    const uint64_t bits = len * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t z = 0;
    while (n != 56)
      update(&z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++)
      lb[i] = static_cast<uint8_t>(bits >> (8 * i));
    update(lb, 8);
    const uint32_t v[4] = {a, b, c, d};
    for (int i = 0; i < 4; i++)
      for (int k = 0; k < 4; k++)
        out[4 * i + k] = static_cast<uint8_t>(v[i] >> (8 * k));
  }
};
}  // namespace

void md5(const uint8_t *data, size_t len, uint8_t out[16]) {
  Md5 m;
  m.update(data, len);
  m.final(out);
}

uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t len) {
  uint32_t c = ~crc;
  for (size_t i = 0; i < len; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++)
      c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
  }
  return ~c;
}

// ---- partition table ---------------------------------------------------------------------------
PartTable parse_partition_table(const uint8_t *buf, size_t len) {
  PartTable t;
  if (buf == nullptr || len < 32) {
    t.error = "too short";
    return t;
  }
  for (size_t off = 0; off + 32 <= len; off += 32) {
    const uint8_t *p = buf + off;
    const uint16_t magic = le16(p);
    if (magic == 0x50AA) {
      PartEntry e;
      e.type = p[2];
      e.subtype = p[3];
      e.offset = le32(p + 4);
      e.size = le32(p + 8);
      std::memcpy(e.label, p + 12, 16);
      e.label[16] = 0;
      e.flags = le32(p + 28);
      t.entries.push_back(e);
      continue;
    }
    if (magic == 0xEBEB) {
      t.md5_present = true;
      uint8_t d[16];
      md5(buf, off, d);
      t.md5_ok = std::memcmp(d, p + 16, 16) == 0;
      t.terminated = true;
      break;
    }
    if (magic == 0xFFFF) {
      t.terminated = true;
      break;
    }
    t.bad_entry = static_cast<int>(t.entries.size());
    char b[48];
    std::snprintf(b, sizeof(b), "unknown magic 0x%04X at +0x%03X", magic, static_cast<unsigned>(off));
    t.error = b;
    break;
  }
  if (t.entries.empty() && t.error.empty())
    t.error = "no entries (erased or not a partition table)";
  return t;
}

const char *part_type_name(uint8_t type) {
  return type == 0x00 ? "app" : type == 0x01 ? "data" : type == 0x02 ? "bootloader" : type == 0x03 ? "partition_table"
                                                                                                    : "custom";
}

const char *part_subtype_name(uint8_t type, uint8_t st, char tmp[8]) {
  if (type == 0x00) {
    if (st == 0x00)
      return "factory";
    if (st >= 0x10 && st < 0x20) {
      std::snprintf(tmp, 8, "ota_%u", st - 0x10);
      return tmp;
    }
    if (st == 0x20)
      return "test";
  } else if (type == 0x01) {
    switch (st) {
      case 0x00:
        return "ota";
      case 0x01:
        return "phy";
      case 0x02:
        return "nvs";
      case 0x03:
        return "coredump";
      case 0x04:
        return "nvs_keys";
      case 0x05:
        return "efuse";
      case 0x06:
        return "undefined";
      case 0x80:
        return "esphttpd";
      case 0x81:
        return "fat";
      case 0x82:
        return "spiffs";
      case 0x83:
        return "littlefs";
      default:
        break;
    }
  }
  std::snprintf(tmp, 8, "0x%02x", st);
  return tmp;
}

std::string format_part_size(uint32_t size) {
  char b[24];
  if (size != 0 && size % 0x100000 == 0)
    std::snprintf(b, sizeof(b), "%" PRIu32 "M", size / 0x100000);
  else if (size != 0 && size % 0x400 == 0)
    std::snprintf(b, sizeof(b), "%" PRIu32 "K", size / 0x400);
  else
    std::snprintf(b, sizeof(b), "0x%" PRIx32, size);
  return b;
}

std::string partition_csv_line(const PartEntry &e) {
  char tmp[8];
  char b[128];
  std::string flags;
  if (e.flags & 1u)
    flags = "encrypted";
  if (e.flags & 2u)
    flags += flags.empty() ? "readonly" : ":readonly";
  std::snprintf(b, sizeof(b), "%s,%s,%s,0x%" PRIx32 ",%s,%s", e.label, part_type_name(e.type),
                part_subtype_name(e.type, e.subtype, tmp), e.offset, format_part_size(e.size).c_str(), flags.c_str());
  return b;
}

std::vector<std::string> partition_csv(const PartTable &t) {
  std::vector<std::string> out;
  out.emplace_back("# ESP-IDF Partition Table (read from flash 0x8000)");
  out.emplace_back("# Name, Type, SubType, Offset, Size, Flags");
  for (const auto &e : t.entries)
    out.push_back(partition_csv_line(e));
  out.push_back(t.md5_present ? (t.md5_ok ? "# MD5 entry: present, OK" : "# MD5 entry: present, MISMATCH")
                              : "# MD5 entry: absent");
  if (!t.error.empty())
    out.push_back("# parse note: " + t.error);
  return out;
}

std::vector<std::string> check_partition_table(const PartTable &t, uint32_t flash_size) {
  std::vector<std::string> notes;
  char b[128];
  for (size_t i = 0; i < t.entries.size(); i++) {
    const PartEntry &a = t.entries[i];
    if (a.type == 0x00 && (a.offset & 0xFFFF) != 0) {
      std::snprintf(b, sizeof(b), "%s: app offset 0x%" PRIx32 " not 64 KB aligned", a.label, a.offset);
      notes.emplace_back(b);
    }
    if ((a.offset & 0xFFF) != 0) {
      std::snprintf(b, sizeof(b), "%s: offset 0x%" PRIx32 " not 4 KB aligned", a.label, a.offset);
      notes.emplace_back(b);
    }
    if (flash_size != 0 && static_cast<uint64_t>(a.offset) + a.size > flash_size) {
      std::snprintf(b, sizeof(b), "%s: ends at 0x%" PRIx64 " beyond flash size 0x%" PRIx32, a.label,
                    static_cast<uint64_t>(a.offset) + a.size, flash_size);
      notes.emplace_back(b);
    }
    for (size_t k = i + 1; k < t.entries.size(); k++) {
      const PartEntry &c = t.entries[k];
      if (a.offset < static_cast<uint64_t>(c.offset) + c.size && c.offset < static_cast<uint64_t>(a.offset) + a.size) {
        std::snprintf(b, sizeof(b), "%s overlaps %s", a.label, c.label);
        notes.emplace_back(b);
      }
    }
  }
  return notes;
}

// ---- otadata -----------------------------------------------------------------------------------
const char *ota_state_str(uint32_t s) {
  switch (s) {
    case OTA_STATE_NEW:
      return "NEW";
    case OTA_STATE_PENDING:
      return "PENDING_VERIFY";
    case OTA_STATE_VALID:
      return "VALID";
    case OTA_STATE_INVALID:
      return "INVALID";
    case OTA_STATE_ABORTED:
      return "ABORTED";
    case OTA_STATE_UNDEFINED:
      return "UNDEFINED";
    default:
      return "?";
  }
}

OtaData parse_otadata(const uint8_t *buf, size_t len, unsigned app_count) {
  OtaData o;
  for (int i = 0; i < 2; i++) {
    const size_t off = static_cast<size_t>(i) * 0x1000;
    if (buf == nullptr || off + 32 > len)
      continue;
    const uint8_t *p = buf + off;
    OtaEntry &e = o.e[i];
    e.seq = le32(p);
    e.state = le32(p + 24);
    e.crc = le32(p + 28);
    e.erased = true;
    for (int k = 0; k < 32; k++)
      if (p[k] != 0xFF)
        e.erased = false;
    e.crc_ok = e.crc == crc32_le(0xFFFFFFFFu, p, 4);
    const bool invalid = e.seq == 0xFFFFFFFFu || e.state == OTA_STATE_INVALID || e.state == OTA_STATE_ABORTED;
    e.valid = !invalid && e.crc_ok;
  }
  // bootloader_common_select_otadata(..., max = true)
  if (o.e[0].valid && o.e[1].valid)
    o.active = o.e[0].seq >= o.e[1].seq ? 0 : 1;
  else if (o.e[0].valid)
    o.active = 0;
  else if (o.e[1].valid)
    o.active = 1;
  if (o.active >= 0 && app_count > 0)
    o.active_slot = static_cast<int>((o.e[o.active].seq - 1) % app_count);
  return o;
}

std::string describe_otadata(const OtaData &o) {
  std::string s;
  char b[128];
  for (int i = 0; i < 2; i++) {
    const OtaEntry &e = o.e[i];
    if (e.erased)
      std::snprintf(b, sizeof(b), "entry %d: erased; ", i);
    else
      std::snprintf(b, sizeof(b), "entry %d: seq %" PRIu32 " state %s crc 0x%08" PRIX32 " %s%s; ", i, e.seq,
                    ota_state_str(e.state), e.crc, e.crc_ok ? "ok" : "BAD", e.valid ? "" : " (not valid)");
    s += b;
  }
  if (o.active < 0)
    std::snprintf(b, sizeof(b), "no valid entry -> bootloader boots the first app (factory / ota_0)");
  else
    std::snprintf(b, sizeof(b), "active entry %d -> ota_%d", o.active, o.active_slot);
  s += b;
  return s;
}

// ---- image headers -----------------------------------------------------------------------------
ImageHeader parse_image_header(const uint8_t *p, size_t len) {
  ImageHeader h;
  if (p == nullptr || len < IMAGE_HEADER_LEN || p[0] != 0xE9)
    return h;
  h.ok = true;
  h.segments = p[1];
  h.spi_mode = p[2];
  h.spi_speed = p[3] & 0x0F;
  h.spi_size = p[3] >> 4;
  h.entry = le32(p + 4);
  h.chip_id = le16(p + 12);
  h.min_chip_rev = p[14];
  h.min_rev_full = le16(p + 15);
  h.max_rev_full = le16(p + 17);
  h.hash_appended = p[23];
  return h;
}

const char *flash_mode_str(uint8_t m) {
  static const char *const n[] = {"QIO", "QOUT", "DIO", "DOUT", "FAST_READ", "SLOW_READ"};
  return m < 6 ? n[m] : "?";
}
const char *flash_freq_str(uint8_t s) {
  switch (s) {
    case 0x0:
      return "40MHz";
    case 0x1:
      return "26MHz";
    case 0x2:
      return "20MHz";
    case 0xF:
      return "80MHz";
    default:
      return "?";
  }
}
const char *flash_size_str(uint8_t s) {
  static const char *const n[] = {"1MB", "2MB", "4MB", "8MB", "16MB", "32MB", "64MB", "128MB"};
  return s < 8 ? n[s] : "?";
}

std::string describe_image_header(const ImageHeader &h) {
  if (!h.ok)
    return "no image (magic != 0xE9)";
  char b[200];
  std::snprintf(b, sizeof(b),
                "magic 0xE9, %u segments, flash %s %s %s, entry 0x%08" PRIX32 ", chip_id %u, min rev %u (full %u, max "
                "%u), hash appended %u",
                h.segments, flash_mode_str(h.spi_mode), flash_freq_str(h.spi_speed), flash_size_str(h.spi_size),
                h.entry, h.chip_id, h.min_chip_rev, h.min_rev_full, h.max_rev_full, h.hash_appended);
  return b;
}

BootloaderDesc parse_bootloader_desc(const uint8_t *p, size_t len) {
  BootloaderDesc d;
  if (p == nullptr || len < 80 || p[0] != 0x50)
    return d;
  d.ok = true;
  d.secure_version = p[3];
  d.version = le32(p + 4);
  d.idf_ver = fixed_str(p + 8, 32);
  d.date_time = fixed_str(p + 40, 24);
  return d;
}

AppDesc parse_app_desc(const uint8_t *p, size_t len) {
  AppDesc d;
  if (p == nullptr || len < 0x90 || le32(p) != 0xABCD5432u)
    return d;
  d.ok = true;
  d.secure_version = le32(p + 4);
  d.version = fixed_str(p + 16, 32);
  d.project_name = fixed_str(p + 48, 32);
  d.time = fixed_str(p + 80, 16);
  d.date = fixed_str(p + 96, 16);
  d.idf_ver = fixed_str(p + 112, 32);
  return d;
}

// ---- dump regions ------------------------------------------------------------------------------
std::vector<Region> plan_regions(const std::vector<PartInfo> &parts, uint32_t flash_size, uint32_t boot_off) {
  std::vector<Region> out;
  auto taken = [&out](const std::string &n) {
    for (const auto &r : out)
      if (r.name == n)
        return true;
    return false;
  };
  auto add = [&](std::string name, const std::string &label, uint32_t off, uint32_t size, bool ext,
                 const std::string &note) {
    if (name.empty() || taken(name))
      name = label;
    if (name.empty() || taken(name)) {
      char b[24];
      std::snprintf(b, sizeof(b), "part_%" PRIx32, off);
      name = b;
    }
    out.push_back(Region{name, label, off, size, ext, note});
  };
  add("bootloader", "", boot_off, PTABLE_OFFSET - boot_off, false, "2nd-stage bootloader");
  add("ptable", "", PTABLE_OFFSET, PTABLE_MAX_LEN, false, "partition table (+MD5 entry)");
  bool have_inactive = false;
  for (const auto &p : parts) {
    std::string alias;
    std::string note;
    if (p.external) {
      alias = "extflash";
      note = "EXTERNAL SPI NOR (identity / usrdataout), read via the read-only partition";
    } else if (p.type == 0x00) {
      if (p.running) {
        alias = "running_app";
        note = "the app running now (this debug build)";
      } else if (!have_inactive && p.subtype >= 0x10 && p.subtype < 0x20) {
        alias = "inactive_app";
        have_inactive = true;
        note = "inactive OTA slot (stock app after a stock-OTA install: keep this for restore)";
      }
    } else if (p.type == 0x01) {
      if (p.subtype == 0x00)
        alias = "otadata";
      else if (p.subtype == 0x01)
        alias = "phy";
      else if (p.subtype == 0x02)
        alias = "nvs";
      else if (p.subtype == 0x03)
        alias = "coredump";
      else if (p.label == "oemdata")
        alias = "oemdata";
    }
    add(alias, p.label, p.offset, p.size, p.external, note);
  }
  if (flash_size != 0)
    add("flash", "", 0, flash_size, false, "whole internal flash");
  return out;
}

const Region *find_region(const std::vector<Region> &r, const std::string &name) {
  for (const auto &x : r)
    if (x.name == name)
      return &x;
  return nullptr;
}

bool validate_raw_range(uint64_t offset, uint64_t len, uint32_t flash_size, uint32_t max_len) {
  if (len == 0 || len > max_len || flash_size == 0)
    return false;
  return offset < flash_size && offset + len <= flash_size;
}

bool parse_u32(const std::string &s, uint32_t *out) {
  if (s.empty())
    return false;
  char *end = nullptr;
  unsigned long long v = std::strtoull(s.c_str(), &end, 0);
  if (end == s.c_str() || *end != 0 || v > 0xFFFFFFFFull || s[0] == '-')
    return false;
  *out = static_cast<uint32_t>(v);
  return true;
}

}  // namespace sungrow_debug
}  // namespace esphome
