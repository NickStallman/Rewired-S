#pragma once
// Parser for the stock "sigdata" device files on /usrdataout (docs/cloud_identity.md). Pure C++17, no ESP-IDF / ESPHome dependencies, so
// it is compiled both into the firmware and into the host tests (tests/extflash/).
//
// File format: concatenated records `u16 id | u16 len | u8 data[len]`, little-endian. No header,
// magic or CRC. len <= the per-board cap (S2 0x420, S 0x100); stock's file limit is 0xFFFFE bytes
// (we hold at most SIGDATA_MAX_FILE_SIZE of it in RAM). Lookup is a
// linear first match on (id, expected len); a record whose len differs from the sig's declared size
// is ignored, exactly as stock does. A missing record means "use the sig table default".
//
// Over-cap length: stock (WiNet-S and WiNet-S2) treats len as 0 and continues
// 4 bytes on, i.e. it re-syncs on whatever follows the bad header. We do the same (resyncs()).

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace esphome::sungrow_extflash {

// Record length caps of the stock readers.
static constexpr size_t SIGDATA_MAX_RECORD_LEN_S2 = 0x420;  // WiNet-S2
static constexpr size_t SIGDATA_MAX_RECORD_LEN_S = 0x100;   // WiNet-S
static constexpr size_t SIGDATA_MAX_RECORD_LEN = SIGDATA_MAX_RECORD_LEN_S2;  // default (largest)
// Bytes of a sigdata file held in RAM. An S2 identity file is about 1.2 KB / 52 records; stock
// parses up to 0xFFFFE in RAM and streams larger files. We parse the first
// SIGDATA_MAX_FILE_SIZE bytes and flag oversize(), never fail.
static constexpr size_t SIGDATA_MAX_FILE_SIZE = 16 * 1024;
// Records kept per file; parsing stops (truncated()) after this many.
static constexpr size_t SIGDATA_MAX_RECORDS = 512;

// Logger (device 0xF7) sig ids used by the cloud path (docs/cloud_identity.md).
namespace sig {
static constexpr uint16_t SN = 0x0002;              // 20 B string, no default
static constexpr uint16_t PRODUCT_TYPE = 0x0004;    // u16
static constexpr uint16_t ENCRYPT_MODE = 0x0020;    // u16
static constexpr uint16_t KEY_NUMBER = 0x0021;      // u16
static constexpr uint16_t COMPRESS_MODE = 0x0022;   // u16
static constexpr uint16_t SSL_FLAG = 0x0023;        // u16
static constexpr uint16_t OLD_SSL_FLAG = 0x0024;    // u16
static constexpr uint16_t MQTT_KEY = 0x1013;        // 16 B
static constexpr uint16_t RUNDATA_PERIOD = 0x1015;  // u16
static constexpr uint16_t SILENCE_ENABLE = 0x1017;  // u16
static constexpr uint16_t SILENCE_START = 0x1018;   // u16
static constexpr uint16_t SILENCE_END = 0x1019;     // u16
static constexpr uint16_t BROKER_HOST = 0x1071;     // 64 B string
static constexpr uint16_t BROKER_PORT = 0x1072;     // u16
static constexpr uint16_t TIMEZONE_ID = 0x107a;     // u16, timezoneId
static constexpr uint16_t TIMEZONE = 0x1089;        // 16 B string, cloud timezone "UTC+8:00"
static constexpr uint16_t OLD_BROKER_HOST = 0x1957; // 64 B string
static constexpr uint16_t OLD_BROKER_PORT = 0x1958; // u16
static constexpr uint16_t OLD_MQTT_KEY = 0x1959;    // 16 B
static constexpr uint16_t REMOTE_CTL_MASK = 0x6492; // u32
}  // namespace sig

// Declared size of a sig in the 0xF7 table, or 0 if we don't know it (then any len is accepted).
size_t sigdata_expected_len(uint16_t id);

class SigdataFile {
 public:
  SigdataFile() = default;
  explicit SigdataFile(size_t max_record_len) : max_record_len_(max_record_len) {}

  struct Record {
    uint16_t id;
    uint16_t len;
    uint32_t offset;  // of data[] within the file
  };

  // Read and parse a file. Returns false if the file can't be opened/read. A truncated or corrupt
  // tail is not an error: every complete record before it is kept (see truncated()).
  bool load(const std::string &path);
  // Parse an in-memory image (copied). Same semantics as load().
  bool parse(const uint8_t *data, size_t size);
  void clear();

  // Per-board record length cap (S 0x100, S2 0x420). Applies to the next load()/parse().
  void set_max_record_len(size_t len) { this->max_record_len_ = len; }
  size_t max_record_len() const { return this->max_record_len_; }

  // First record with this id whose len matches the sig's declared size (any len if unknown).
  std::optional<std::vector<uint8_t>> get(uint16_t id) const;
  // NUL-trimmed string (empty if absent).
  std::string get_string(uint16_t id) const;
  std::optional<uint16_t> get_u16(uint16_t id) const;
  std::optional<uint32_t> get_u32(uint16_t id) const;
  bool has(uint16_t id) const { return this->find_(id) != nullptr; }

  bool loaded() const { return this->loaded_; }
  size_t file_size() const { return this->data_.size(); }
  const std::vector<uint8_t> &raw() const { return this->data_; }
  const std::vector<Record> &records() const { return this->records_; }
  // Records skipped because len != declared size (stock ignores those too).
  size_t ignored_records() const { return this->ignored_; }
  // The tail was incomplete (header or data cut short), or SIGDATA_MAX_RECORDS was reached.
  bool truncated() const { return this->truncated_; }
  // Headers with len > max_record_len() that were skipped stock-style (len := 0, continue 4 B on).
  size_t resyncs() const { return this->resyncs_; }
  // Bytes consumed by the scan (records + skipped headers), <= file_size().
  size_t parsed_bytes() const { return this->parsed_bytes_; }
  // File was larger than SIGDATA_MAX_FILE_SIZE: only its first bytes were parsed.
  bool oversize() const { return this->oversize_; }

 protected:
  const Record *find_(uint16_t id) const;
  // Scan data_ (already loaded, at most SIGDATA_MAX_FILE_SIZE bytes) into records_.
  void scan_();

  std::vector<uint8_t> data_;
  std::vector<Record> records_;
  size_t max_record_len_{SIGDATA_MAX_RECORD_LEN};
  size_t ignored_{0};
  size_t resyncs_{0};
  size_t parsed_bytes_{0};
  bool truncated_{false};
  bool oversize_{false};
  bool loaded_{false};
};

// Cloud identity of the dongle (logger device 0xF7). Missing records get the stock defaults from
// docs/cloud_identity.md, so a valid SN alone is enough to talk to iSolarCloud.
struct Identity {
  std::string sn;                 // 0x0002 (no default)
  uint16_t product_type{0x720};   // 0x0004
  uint16_t encrypt_mode{0};       // 0x0020
  uint16_t key_number{0};         // 0x0021
  uint16_t compress_mode{1};      // 0x0022 (1 = zlib)
  uint16_t ssl_flag{2};           // 0x0023 (forced 0 by stock when port == 19999)
  uint16_t old_ssl_flag{2};       // 0x0024
  std::string broker_host{"iot.isolarcloud.com"};  // 0x1071
  uint16_t broker_port{16668};                     // 0x1072
  std::array<uint8_t, 16> mqtt_key{};              // 0x1013 (default "Sun#0*2Pow925Mdk")
  std::string old_broker_host{"iot.isolarcloud.com"};  // 0x1957
  uint16_t old_broker_port{16668};                     // 0x1958
  std::array<uint8_t, 16> old_mqtt_key{};              // 0x1959
  uint16_t timezone_id{8};        // 0x107a timezoneId, `%d` in isolarcloud/devicedata/cmd/%d/enum_0_0_0
  std::string timezone{"UTC+8:00"};  // 0x1089 (as the cloud writes it; stock flips +/- for POSIX TZ)
  uint16_t rundata_period{5};     // 0x1015 (minutes?)
  uint16_t silence_enable{0};     // 0x1017
  uint16_t silence_start{0};      // 0x1018 (hour)
  uint16_t silence_end{0};        // 0x1019 (hour)
  // 0x6492 (bit1 = master). The WiNet-S sig table has no 0x6492 at all, so on the S this is
  // always the 0x4FF default (= the S2 table default).
  uint32_t remote_ctl_mask{0x4FF};

  // Sig ids that came from the file (the rest are defaults).
  std::vector<uint16_t> from_file;

  Identity();
  bool valid() const;  // SN present and plausible
  bool is_from_file(uint16_t id) const;
  std::string mqtt_key_str() const { return std::string(this->mqtt_key.begin(), this->mqtt_key.end()); }
  std::string old_mqtt_key_str() const {
    return std::string(this->old_mqtt_key.begin(), this->old_mqtt_key.end());
  }
};

// Default 16 B mqtt_pubkey (0x1013 / 0x1959 default, ASCII, no NUL).
extern const char *const DEFAULT_MQTT_KEY;

// Build an Identity from a parsed file, filling defaults for missing records.
Identity decode_identity(const SigdataFile &file, uint16_t default_product_type = 0x720);

// SN plausibility: non-empty, <= 20 chars, printable ASCII without spaces.
bool sn_plausible(const std::string &sn);

// ---- identity file selection ----
// Stock (WiNet-S and WiNet-S2) walks live file -> logbak/ backup -> factory image ->
// logbak/ factory image and takes the FIRST file of at least this many bytes, whatever it holds.
static constexpr size_t IDENTITY_MIN_FILE_SIZE = 10;

struct IdentityCandidate {
  bool readable{false};
  size_t file_size{0};
  bool sn_plausible{false};
};

enum class IdentityRule : uint8_t {
  NONE,                 // no readable candidate of >= IDENTITY_MIN_FILE_SIZE bytes: defaults only
  FIRST_WITH_SN,        // ours: the first candidate whose 0x0002 SN is plausible
  STOCK_FIRST_MIN_SIZE  // fallback when no candidate has a plausible SN: stock's first >= 10 B file
};

struct IdentityChoice {
  int index{-1};        // into the candidate list, -1 = none
  IdentityRule rule{IdentityRule::NONE};
  int stock_index{-1};  // what stock would pick (first readable >= 10 B), for logging
};

// Our rule: the first candidate with a plausible SN; if none has one, stock's rule (first readable
// candidate of >= IDENTITY_MIN_FILE_SIZE bytes) supplies the other values. Pure, host-tested.
IdentityChoice choose_identity_file(const std::vector<IdentityCandidate> &candidates);
const char *identity_rule_str(IdentityRule rule);

}  // namespace esphome::sungrow_extflash
