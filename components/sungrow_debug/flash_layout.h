#pragma once
// sungrow_debug flash-layout core: pure parsers for the raw structures the debug build reads
// from internal flash (READ-ONLY): the partition table @0x8000 (with its MD5 entry), the
// otadata entries, ESP image headers (bootloader @0x1000, app slots), the bootloader / app
// descriptors, and the dump-region planner behind the /dump/<region> web endpoints.
// No ESPHome / IDF headers: host-tested in tests/debug/.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace sungrow_debug {

// ---- hashes ------------------------------------------------------------------------------------
/// MD5 (RFC 1321), for the partition table's MD5 entry.
void md5(const uint8_t *data, size_t len, uint8_t out[16]);
/// esp_rom_crc32_le(crc, data, len): zlib-compatible CRC-32 continuing from `crc`
/// (bootloader_common_ota_select_crc() passes UINT32_MAX).
uint32_t crc32_le(uint32_t crc, const uint8_t *data, size_t len);

// ---- partition table ---------------------------------------------------------------------------
static constexpr uint32_t PTABLE_OFFSET = 0x8000;
static constexpr uint32_t PTABLE_MAX_LEN = 0xC00;
static constexpr uint32_t BOOTLOADER_OFFSET_ESP32 = 0x1000;  ///< ESP32 classic; S3 = 0x0

struct PartEntry {
  uint8_t type{0};
  uint8_t subtype{0};
  uint32_t offset{0};
  uint32_t size{0};
  char label[17]{};
  uint32_t flags{0};
};

struct PartTable {
  std::vector<PartEntry> entries;
  bool md5_present{false};
  bool md5_ok{false};
  bool terminated{false};  ///< ended on an erased (0xFFFF) entry or after the MD5 entry
  int bad_entry{-1};       ///< index of the first entry with an unknown magic, -1 = none
  std::string error;
};
/// Parse raw table bytes (normally 0xC00 read from 0x8000).
PartTable parse_partition_table(const uint8_t *buf, size_t len);
const char *part_type_name(uint8_t type);
/// gen_esp32part names ("nvs", "ota_0", "fat", ...), or "0x%02x" into `tmp` if unknown.
const char *part_subtype_name(uint8_t type, uint8_t subtype, char tmp[8]);
/// gen_esp32part-style size: "16K", "3M", else hex.
std::string format_part_size(uint32_t size);
/// One CSV line in gen_esp32part form: "nvs,data,nvs,0x9000,16K,".
std::string partition_csv_line(const PartEntry &e);
/// Header + every entry; sanity notes (overlaps, alignment) appended as "# ..." lines.
std::vector<std::string> partition_csv(const PartTable &t);
/// Overlap / alignment / out-of-flash problems as human-readable notes (empty = none).
std::vector<std::string> check_partition_table(const PartTable &t, uint32_t flash_size);

// ---- otadata -----------------------------------------------------------------------------------
static constexpr uint32_t OTA_STATE_NEW = 0x0, OTA_STATE_PENDING = 0x1, OTA_STATE_VALID = 0x2,
                          OTA_STATE_INVALID = 0x3, OTA_STATE_ABORTED = 0x4, OTA_STATE_UNDEFINED = 0xFFFFFFFFu;
const char *ota_state_str(uint32_t state);

struct OtaEntry {
  uint32_t seq{0xFFFFFFFFu};
  uint32_t state{OTA_STATE_UNDEFINED};
  uint32_t crc{0};
  bool crc_ok{false};
  bool valid{false};  ///< bootloader_common_ota_select_valid()
  bool erased{true};
};
struct OtaData {
  OtaEntry e[2];
  int active{-1};      ///< bootloader_common_get_active_otadata(); -1 = none (boot ota_0 / factory)
  int active_slot{-1}; ///< (seq - 1) % app_count of the active entry; -1 = none
};
/// buf = the otadata partition (two 4 KB sectors; entry i at i * 0x1000). app_count = OTA slots.
OtaData parse_otadata(const uint8_t *buf, size_t len, unsigned app_count);
std::string describe_otadata(const OtaData &o);

// ---- image headers -----------------------------------------------------------------------------
struct ImageHeader {
  bool ok{false};      ///< magic 0xE9
  uint8_t segments{0};
  uint8_t spi_mode{0};
  uint8_t spi_speed{0};  ///< low nibble of byte 3
  uint8_t spi_size{0};   ///< high nibble of byte 3
  uint32_t entry{0};
  uint16_t chip_id{0};
  uint8_t min_chip_rev{0};
  uint16_t min_rev_full{0}, max_rev_full{0};
  uint8_t hash_appended{0};
};
static constexpr size_t IMAGE_HEADER_LEN = 24;
static constexpr size_t SEGMENT_HEADER_LEN = 8;
ImageHeader parse_image_header(const uint8_t *buf, size_t len);
const char *flash_mode_str(uint8_t mode);
const char *flash_freq_str(uint8_t speed);
const char *flash_size_str(uint8_t size);
/// image_info-like one-liner.
std::string describe_image_header(const ImageHeader &h);

/// esp_bootloader_desc_t (IDF >= 5.2 bootloaders; magic 0x50 at the start of segment 0's data).
struct BootloaderDesc {
  bool ok{false};
  uint8_t secure_version{0};
  uint32_t version{0};
  std::string idf_ver, date_time;
};
BootloaderDesc parse_bootloader_desc(const uint8_t *buf, size_t len);
/// esp_app_desc_t (magic 0xABCD5432 at the start of an app's segment 0 data).
struct AppDesc {
  bool ok{false};
  uint32_t secure_version{0};
  std::string version, project_name, time, date, idf_ver;
};
AppDesc parse_app_desc(const uint8_t *buf, size_t len);

// ---- dump regions ------------------------------------------------------------------------------
/// A partition as the running app reports it (esp_partition API).
struct PartInfo {
  std::string label;
  uint8_t type{0};
  uint8_t subtype{0};
  uint32_t offset{0};
  uint32_t size{0};
  bool external{false};  ///< not on the main flash chip (sungrow_extflash's usrdataout)
  bool running{false};
};
struct Region {
  std::string name;   ///< URL name: /dump/<name>
  std::string label;  ///< partition label ("" for fixed regions)
  uint32_t offset{0};
  uint32_t size{0};
  bool external{false};
  std::string note;
};
/// Region list for /dump/index: bootloader, ptable, one per partition (alias names nvs/otadata/
/// phy/coredump/oemdata/inactive_app/running_app/extflash where they apply, else the label),
/// and "flash" (the whole internal chip). Names are unique.
std::vector<Region> plan_regions(const std::vector<PartInfo> &parts, uint32_t flash_size,
                                 uint32_t bootloader_offset = BOOTLOADER_OFFSET_ESP32);
const Region *find_region(const std::vector<Region> &r, const std::string &name);
/// raw?offset=&len=: false if outside [0, flash_size) or len is 0 / above max_len.
bool validate_raw_range(uint64_t offset, uint64_t len, uint32_t flash_size, uint32_t max_len = 0x1000000);
/// Parse "0x1000" / "4096"; false on garbage.
bool parse_u32(const std::string &s, uint32_t *out);

}  // namespace sungrow_debug
}  // namespace esphome
