#include "sungrow_extflash.h"

#include <cinttypes>
#include <cstring>

#include <dirent.h>
#include <sys/stat.h>

#include <driver/spi_common.h>
#include <esp_flash.h>
#include <esp_flash_spi_init.h>
#include <esp_littlefs.h>
#include <esp_partition.h>
#include <esp_timer.h>

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::sungrow_extflash {

static const char *const TAG = "sungrow_extflash";

SungrowExtFlash *global_extflash = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Hex-dump lines emitted per loop() iteration while a dump is running.
static constexpr int DUMP_LINES_PER_LOOP = 8;
static constexpr size_t DUMP_BYTES_PER_LINE = 32;

static const char *io_mode_str(IoMode m) {
  switch (m) {
    case IoMode::SLOWRD:
      return "SLOWRD";
    case IoMode::FASTRD:
      return "FASTRD";
    case IoMode::DOUT:
      return "DOUT";
    case IoMode::DIO:
      return "DIO";
  }
  return "?";
}

SungrowExtFlash::SungrowExtFlash() { global_extflash = this; }

// ---- setup ----

void SungrowExtFlash::setup() {
  const int64_t t0 = esp_timer_get_time();
  if (!this->init_bus_() || !this->register_partition_()) {
    // Non-fatal for the rest of the firmware: no identity, cloud falls back to YAML overrides.
    this->status_set_error(LOG_STR("external flash not available"));
  } else {
    this->log_superblock_();
    if (!this->mount_())
      this->status_set_error(LOG_STR("usrdataout mount failed (NOT formatting)"));
  }
  this->load_identity_();
  if (this->mounted_ && !this->identity_.valid())
    this->status_set_warning(LOG_STR("no valid SN in identity file"));
  this->publish_states_();
  ESP_LOGD(TAG, "Setup took %" PRId64 " ms", (esp_timer_get_time() - t0) / 1000);
  this->disable_loop();
}

bool SungrowExtFlash::init_bus_() {
  spi_bus_config_t bus{};
  bus.mosi_io_num = this->mosi_;
  bus.miso_io_num = this->miso_;
  bus.sclk_io_num = this->clk_;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 0;  // default (4092 with DMA)
  const auto host = static_cast<spi_host_device_t>(this->spi_host_);
  const spi_dma_chan_t dma = this->dma_channel_ == 0 ? SPI_DMA_CH_AUTO : static_cast<spi_dma_chan_t>(this->dma_channel_);
  esp_err_t err = spi_bus_initialize(host, &bus, dma);
  if (err == ESP_ERR_INVALID_STATE) {
    // Someone else owns this host. Their pins/mode may not match ours: refuse rather than guess.
    ESP_LOGE(TAG, "SPI host %d already initialised by another component", this->spi_host_);
    return false;
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
    return false;
  }
  this->bus_initialized_ = true;

  esp_flash_spi_device_config_t dev{};
  dev.host_id = host;
  dev.cs_io_num = this->cs_;
  dev.cs_id = 0;
  dev.input_delay_ns = 0;
  dev.freq_mhz = this->freq_mhz_;
  // Non-quad modes only (validated in Python): esp_flash_init() then never writes the status
  // register (spi_flash_common_set_io_mode only touches QE for QIO/QOUT).
  switch (this->io_mode_) {
    case IoMode::SLOWRD:
      dev.io_mode = SPI_FLASH_SLOWRD;
      break;
    case IoMode::FASTRD:
      dev.io_mode = SPI_FLASH_FASTRD;
      break;
    case IoMode::DOUT:
      dev.io_mode = SPI_FLASH_DOUT;
      break;
    case IoMode::DIO:
    default:
      dev.io_mode = SPI_FLASH_DIO;
      break;
  }
  esp_flash_t *chip = nullptr;
  err = spi_bus_add_flash_device(&chip, &dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "spi_bus_add_flash_device failed: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_flash_init(chip);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_flash_init failed: %s (chip absent / wrong pins?)", esp_err_to_name(err));
    spi_bus_remove_flash_device(chip);
    return false;
  }
  uint32_t id = 0;
  if (esp_flash_read_id(chip, &id) == ESP_OK)
    this->chip_id_ = id;
  uint32_t size = 0;
  err = esp_flash_get_size(chip, &size);
  if (err != ESP_OK || size == 0) {
    ESP_LOGE(TAG, "esp_flash_get_size failed: %s", esp_err_to_name(err));
    spi_bus_remove_flash_device(chip);
    return false;
  }
  this->chip_size_ = size;
  this->chip_ = chip;
  ESP_LOGI(TAG, "External flash: ID 0x%06" PRIX32 ", %" PRIu32 " KB, %s @ %d MHz", this->chip_id_, size / 1024,
           io_mode_str(this->io_mode_), this->freq_mhz_);
  if (this->chip_id_ == 0 || this->chip_id_ == 0xFFFFFF)
    ESP_LOGW(TAG, "Suspicious JEDEC id - check pins");
  return true;
}

bool SungrowExtFlash::register_partition_() {
  uint32_t size = this->partition_size_ != 0 ? this->partition_size_ : this->chip_size_;
  if (size > this->chip_size_) {
    ESP_LOGW(TAG, "partition_size %" PRIu32 " > chip size %" PRIu32 ", clamping", size, this->chip_size_);
    size = this->chip_size_;
  }
  const esp_partition_t *part = nullptr;
  esp_err_t err = esp_partition_register_external(this->chip_, 0, size, this->partition_label_.c_str(),
                                                  ESP_PARTITION_TYPE_DATA,
                                                  static_cast<esp_partition_subtype_t>(this->partition_subtype_), &part);
  if (err != ESP_OK || part == nullptr) {
    ESP_LOGE(TAG, "esp_partition_register_external(%s) failed: %s", this->partition_label_.c_str(),
             esp_err_to_name(err));
    return false;
  }
  // IMPORTANT: do NOT set part->readonly = true. IDF's esp_partition_is_flash_region_writable()
  // (the main-flash erase/write guard, via main_flash_region_protected) matches readonly
  // partitions by raw ADDRESS across ALL chips, ignoring flash_chip. This external partition is
  // registered at offset 0 spanning the whole chip, so a readonly flag blankets the entire
  // 0..chip_size address range and makes IDF return ESP_ERR_NOT_ALLOWED (0x10D) for every erase/
  // write to the MAIN flash in that range - which breaks native OTA (ota_0 @0x110000) and NVS.
  // The chip is already protected against writes by the read-only LittleFS mount below
  // (conf.read_only = 1, format_if_mount_failed = 0), so the flag is redundant as well as harmful.
  this->partition_ = part;
  ESP_LOGD(TAG, "Registered partition '%s' 0x0..0x%08" PRIX32 " (read-only mount)", part->label, size);
  return true;
}

// Parse the first LittleFS superblock commit for diagnostics (geometry/version).
// Layout of block 0/1: u32 revision, u32 tag, "littlefs", u32 tag, struct lfs_superblock (LE).
void SungrowExtFlash::log_superblock_() {
  uint8_t buf[2][44];
  uint32_t rev[2] = {0, 0};
  bool ok[2] = {false, false};
  for (int b = 0; b < 2; b++) {
    if (esp_partition_read(this->partition_, b * 4096, buf[b], sizeof(buf[b])) != ESP_OK)
      continue;
    ok[b] = std::memcmp(&buf[b][8], "littlefs", 8) == 0;
    std::memcpy(&rev[b], &buf[b][0], 4);
  }
  if (!ok[0] && !ok[1]) {
    this->superblock_info_ = "no littlefs magic in block 0/1";
    ESP_LOGW(TAG, "No LittleFS superblock magic found in blocks 0/1 (will still try to mount)");
    return;
  }
  // Revision counts compare with sequence arithmetic; the newer block holds the current pair.
  const int b = (ok[0] && ok[1]) ? ((int32_t) (rev[1] - rev[0]) > 0 ? 1 : 0) : (ok[0] ? 0 : 1);
  auto rd32 = [&](int off) {
    uint32_t v;
    std::memcpy(&v, &buf[b][off], 4);
    return v;
  };
  const uint32_t version = rd32(20), block_size = rd32(24), block_count = rd32(28), name_max = rd32(32);
  char tmp[128];
  snprintf(tmp, sizeof(tmp), "disk v%" PRIu32 ".%" PRIu32 ", block %" PRIu32 " x %" PRIu32 ", name_max %" PRIu32,
           version >> 16, version & 0xFFFF, block_size, block_count, name_max);
  this->superblock_info_ = tmp;
  ESP_LOGI(TAG, "LittleFS superblock (block %d, rev %" PRIu32 "): %s", b, rev[b], tmp);
  if (block_size != 4096)
    ESP_LOGW(TAG, "Superblock block_size %" PRIu32 " != 4096: mount will fail", block_size);
  if (static_cast<uint64_t>(block_size) * block_count > this->partition_->size)
    ESP_LOGW(TAG, "Filesystem (%" PRIu32 " blocks) is larger than the partition (%" PRIu32 " B)", block_count,
             this->partition_->size);
}

bool SungrowExtFlash::mount_() {
  esp_vfs_littlefs_conf_t conf{};
  conf.base_path = this->mount_path_.c_str();
  conf.partition_label = nullptr;
  conf.partition = this->partition_;
  conf.format_if_mount_failed = 0;  // NEVER: this flash holds the only copy of the identity
  conf.read_only = 1;
  conf.dont_mount = 0;
  conf.grow_on_mount = 0;
  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    this->mount_err_ = err;
    ESP_LOGE(TAG, "LittleFS mount of '%s' at %s failed: %s. Not formatting; running without identity.",
             this->partition_label_.c_str(), this->mount_path_.c_str(), esp_err_to_name(err));
    return false;
  }
  this->mounted_ = true;
  const int64_t t0 = esp_timer_get_time();
  if (esp_littlefs_partition_info(this->partition_, &this->fs_total_, &this->fs_used_) == ESP_OK) {
    ESP_LOGI(TAG, "Mounted %s read-only: littlefs total %u B, used %u B (%" PRId64 " ms)", this->mount_path_.c_str(),
             (unsigned) this->fs_total_, (unsigned) this->fs_used_, (esp_timer_get_time() - t0) / 1000);
  } else {
    ESP_LOGI(TAG, "Mounted %s read-only", this->mount_path_.c_str());
  }
  return true;
}

void SungrowExtFlash::load_identity_() {
  this->sigdata_.set_max_record_len(this->max_record_len_);
  this->identity_rule_ = IdentityRule::NONE;
  if (this->mounted_) {
    // Stock (WiNet-S and WiNet-S2) walks live file -> logbak/ backup -> factory image
    // -> logbak/ factory image and uses the first file of at least 10 bytes, even one without an
    // SN. We use the first candidate with a plausible SN instead, and fall back to stock's choice
    // (for its non-SN values) only when no candidate has one
    // (choose_identity_file() in sigdata.cpp). Scanning stops at the first file with an SN.
    std::vector<IdentityCandidate> cands;
    SigdataFile stock_file(this->max_record_len_);  // first >= 10 B file, kept for the fallback
    SigdataFile sn_file(this->max_record_len_);
    for (const auto &path : this->identity_files_) {
      SigdataFile f(this->max_record_len_);
      IdentityCandidate c;
      c.readable = f.load(path);
      if (!c.readable) {
        ESP_LOGD(TAG, "%s: not readable", path.c_str());
        cands.push_back(c);
        continue;
      }
      c.file_size = f.file_size();
      c.sn_plausible = sn_plausible(f.get_string(sig::SN));
      ESP_LOGD(TAG, "%s: %u B, %u records (%u ignored, %u resyncs)%s%s, SN %s", path.c_str(), (unsigned) f.file_size(),
               (unsigned) f.records().size(), (unsigned) f.ignored_records(), (unsigned) f.resyncs(),
               f.truncated() ? ", truncated tail" : "", f.oversize() ? ", oversize" : "",
               c.sn_plausible ? "plausible" : "missing/invalid");
      if (c.file_size < IDENTITY_MIN_FILE_SIZE) {
        ESP_LOGD(TAG, "%s: < %u B (stock skips it)", path.c_str(), (unsigned) IDENTITY_MIN_FILE_SIZE);
      }
      cands.push_back(c);
      const bool first_stock = c.file_size >= IDENTITY_MIN_FILE_SIZE && !stock_file.loaded();
      if (c.sn_plausible) {
        sn_file = std::move(f);
        break;
      }
      if (first_stock)
        stock_file = std::move(f);
    }
    const IdentityChoice choice = choose_identity_file(cands);
    this->identity_rule_ = choice.rule;
    if (choice.index >= 0) {
      this->identity_source_ = this->identity_files_[choice.index];
      this->sigdata_ = std::move(choice.rule == IdentityRule::FIRST_WITH_SN ? sn_file : stock_file);
      ESP_LOGI(TAG, "Identity file %s chosen by %s", this->identity_source_.c_str(), identity_rule_str(choice.rule));
      if (choice.rule == IdentityRule::FIRST_WITH_SN && choice.stock_index >= 0 && choice.stock_index != choice.index)
        ESP_LOGW(TAG, "Stock would use %s (no valid SN) instead of %s",
                 this->identity_files_[choice.stock_index].c_str(), this->identity_source_.c_str());
    } else {
      ESP_LOGW(TAG, "No identity file of >= %u B: using defaults", (unsigned) IDENTITY_MIN_FILE_SIZE);
    }
  }
  this->identity_ = decode_identity(this->sigdata_, this->default_product_type_);
  this->identity_loaded_ = true;
  if (this->identity_.valid()) {
    ESP_LOGI(TAG, "Identity from %s: SN %s, broker %s:%u, %u/%u sigs from file", this->identity_source_.c_str(),
             this->identity_.sn.c_str(), this->identity_.broker_host.c_str(), this->identity_.broker_port,
             (unsigned) this->identity_.from_file.size(), 19u);
  } else if (this->mounted_) {
    ESP_LOGW(TAG, "No valid SN in any identity file%s%s", this->identity_source_.empty() ? "" : " (using ",
             this->identity_source_.empty() ? "" : (this->identity_source_ + " for other values)").c_str());
  }
  this->log_identity_(ESPHOME_LOG_LEVEL_VERBOSE);
  this->identity_callbacks_.call(this->identity_);
}

void SungrowExtFlash::add_on_identity_callback(std::function<void(const Identity &)> &&callback) {
  if (this->identity_loaded_) {
    callback(this->identity_);
    return;
  }
  this->identity_callbacks_.add(std::move(callback));
}

void SungrowExtFlash::publish_states_() {
#ifdef USE_BINARY_SENSOR
  if (this->mounted_bs_ != nullptr)
    this->mounted_bs_->publish_state(this->mounted_);
  if (this->identity_valid_bs_ != nullptr)
    this->identity_valid_bs_->publish_state(this->identity_valid());
#endif
#ifdef USE_TEXT_SENSOR
  if (this->sn_text_ != nullptr)
    this->sn_text_->publish_state(this->identity_valid() ? this->identity_.sn : std::string());
  if (this->broker_text_ != nullptr) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s:%u", this->identity_.broker_host.c_str(), this->identity_.broker_port);
    this->broker_text_->publish_state(buf);
  }
  if (this->source_text_ != nullptr)
    this->source_text_->publish_state(this->identity_source_.empty() ? std::string("none") : this->identity_source_);
  if (this->chip_text_ != nullptr) {
    char buf[64];
    if (this->chip_ != nullptr) {
      snprintf(buf, sizeof(buf), "0x%06" PRIX32 " %" PRIu32 " MB", this->chip_id_, this->chip_size_ >> 20);
    } else {
      snprintf(buf, sizeof(buf), "not detected");
    }
    this->chip_text_->publish_state(buf);
  }
#endif
}

// ---- logging helpers ----

#define EXTFLASH_LOG(level, fmt, ...) ::esphome::esp_log_printf_(level, TAG, __LINE__, fmt, ##__VA_ARGS__)

void SungrowExtFlash::log_identity_(int level) const {
  if (ESPHOME_LOG_LEVEL < level)
    return;
  const Identity &id = this->identity_;
  auto src = [&](uint16_t s) { return id.is_from_file(s) ? "file" : "default"; };
  EXTFLASH_LOG(level, "Identity (%s; %s):", this->identity_source_.empty() ? "defaults" : this->identity_source_.c_str(),
               identity_rule_str(this->identity_rule_));
  EXTFLASH_LOG(level, "  0x0002 SN:              '%s' (%s)", id.sn.c_str(), id.valid() ? "valid" : "INVALID");
  EXTFLASH_LOG(level, "  0x0004 productType:     %u (%s)", id.product_type, src(sig::PRODUCT_TYPE));
  EXTFLASH_LOG(level, "  0x0020-22 enc/key/comp: %u/%u/%u", id.encrypt_mode, id.key_number, id.compress_mode);
  EXTFLASH_LOG(level, "  0x0023/24 ssl/old ssl:  %u/%u", id.ssl_flag, id.old_ssl_flag);
  EXTFLASH_LOG(level, "  0x1071/72 broker:       %s:%u (%s)", id.broker_host.c_str(), id.broker_port,
               src(sig::BROKER_HOST));
  EXTFLASH_LOG(level, "  0x1013 mqtt key:        %s (%s)", format_hex(id.mqtt_key.data(), 16).c_str(),
               src(sig::MQTT_KEY));
  EXTFLASH_LOG(level, "  0x1957/58 old broker:   %s:%u", id.old_broker_host.c_str(), id.old_broker_port);
  EXTFLASH_LOG(level, "  0x1959 old mqtt key:    %s", format_hex(id.old_mqtt_key.data(), 16).c_str());
  EXTFLASH_LOG(level, "  0x107a timezoneId:      %u", id.timezone_id);
  EXTFLASH_LOG(level, "  0x1015 rundata period:  %u", id.rundata_period);
  EXTFLASH_LOG(level, "  0x1017-19 silence:      en %u, %u..%u", id.silence_enable, id.silence_start,
               id.silence_end);
  EXTFLASH_LOG(level, "  0x6492 remote ctl mask: 0x%08" PRIX32, id.remote_ctl_mask);
}

bool SungrowExtFlash::read_file(const std::string &path, std::vector<uint8_t> &out, size_t max_size) {
  out.clear();
  if (!this->mounted_)
    return false;
  FILE *f = fopen(path.c_str(), "rb");
  if (f == nullptr)
    return false;
  // Size first: a file over max_size is rejected without reading it; otherwise one exact read.
  long total = -1;
  if (fseek(f, 0, SEEK_END) == 0)
    total = ftell(f);
  bool ok = total >= 0 && static_cast<size_t>(total) <= max_size && fseek(f, 0, SEEK_SET) == 0;
  if (ok) {
    out.resize(static_cast<size_t>(total));
    ok = out.empty() || (fread(out.data(), 1, out.size(), f) == out.size() && !ferror(f));
  } else if (total >= 0 && static_cast<size_t>(total) > max_size) {
    ESP_LOGW(TAG, "%s: %ld B is over the %u B limit; not read", path.c_str(), total, (unsigned) max_size);
  }
  fclose(f);
  if (!ok)
    out.clear();
  return ok;
}

bool SungrowExtFlash::start_dump_(const std::string &path, bool info_level) {
  if (!this->mounted_) {
    ESP_LOGW(TAG, "dump %s: filesystem not mounted", path.c_str());
    return false;
  }
  if (this->dump_fp_ != nullptr) {
    ESP_LOGW(TAG, "dump %s: another dump (%s) is running", path.c_str(), this->dump_path_.c_str());
    return false;
  }
  if (!info_level && ESPHOME_LOG_LEVEL < ESPHOME_LOG_LEVEL_VERY_VERBOSE) {
    ESP_LOGW(TAG, "dump_file() logs at VERY_VERBOSE, which is compiled out (logger level)");
    return false;
  }
  FILE *f = fopen(path.c_str(), "rb");
  if (f == nullptr) {
    ESP_LOGW(TAG, "dump %s: cannot open", path.c_str());
    return false;
  }
  struct stat st {};
  const long size = stat(path.c_str(), &st) == 0 ? static_cast<long>(st.st_size) : -1;
  this->dump_fp_ = f;
  this->dump_path_ = path;
  this->dump_offset_ = 0;
  this->dump_info_ = info_level;
  // Line format is parsed by tools/read_identity.py --from-log.
  EXTFLASH_LOG(info_level ? ESPHOME_LOG_LEVEL_INFO : ESPHOME_LOG_LEVEL_VERY_VERBOSE, "DUMP BEGIN %s size=%ld",
               path.c_str(), size);
  this->enable_loop();
  return true;
}

void SungrowExtFlash::loop() {
  if (this->dump_fp_ == nullptr) {
    this->disable_loop();
    return;
  }
  const int level = this->dump_info_ ? ESPHOME_LOG_LEVEL_INFO : ESPHOME_LOG_LEVEL_VERY_VERBOSE;
  uint8_t buf[DUMP_BYTES_PER_LINE];
  char hex[DUMP_BYTES_PER_LINE * 2 + 1];
  for (int i = 0; i < DUMP_LINES_PER_LOOP; i++) {
    size_t n = fread(buf, 1, sizeof(buf), this->dump_fp_);
    if (n > 0) {
      for (size_t j = 0; j < n; j++)
        snprintf(&hex[j * 2], 3, "%02x", buf[j]);
      hex[n * 2] = 0;
      EXTFLASH_LOG(level, "DUMP %06" PRIx32 ": %s", this->dump_offset_, hex);
      this->dump_offset_ += n;
    }
    if (n < sizeof(buf)) {
      const bool err = ferror(this->dump_fp_) != 0;
      fclose(this->dump_fp_);
      this->dump_fp_ = nullptr;
      EXTFLASH_LOG(level, "DUMP END %s bytes=%" PRIu32 "%s", this->dump_path_.c_str(), this->dump_offset_,
                   err ? " READ ERROR" : "");
      this->disable_loop();
      return;
    }
  }
}

bool SungrowExtFlash::dump_identity() {
  this->log_identity_(ESPHOME_LOG_LEVEL_INFO);
  if (this->identity_source_.empty()) {
    ESP_LOGW(TAG, "No identity file to dump");
    return false;
  }
  return this->start_dump_(this->identity_source_, true);
}

void SungrowExtFlash::list_dir(const std::string &path, int max_depth) {
  if (!this->mounted_) {
    ESP_LOGW(TAG, "list: filesystem not mounted");
    return;
  }
  const std::string root = path.empty() ? this->mount_path_ : path;
  uint32_t count = 0;
  ESP_LOGI(TAG, "Listing %s:", root.c_str());
  this->list_dir_rec_(root, 0, max_depth, count);
  ESP_LOGI(TAG, "%" PRIu32 " entries", count);
}

void SungrowExtFlash::list_dir_rec_(const std::string &path, int depth, int max_depth, uint32_t &count) {
  DIR *d = opendir(path.c_str());
  if (d == nullptr) {
    ESP_LOGW(TAG, "  cannot open dir %s", path.c_str());
    return;
  }
  while (struct dirent *e = readdir(d)) {
    if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
      continue;
    const std::string full = path + "/" + e->d_name;
    struct stat st {};
    const bool is_dir = e->d_type == DT_DIR;
    if (!is_dir)
      stat(full.c_str(), &st);
    count++;
    if (is_dir) {
      ESP_LOGI(TAG, "  %*s%s/", depth * 2, "", e->d_name);
      if (depth + 1 < max_depth)
        this->list_dir_rec_(full, depth + 1, max_depth, count);
    } else {
      ESP_LOGI(TAG, "  %*s%s (%ld B)", depth * 2, "", e->d_name, static_cast<long>(st.st_size));
    }
    App.feed_wdt();
  }
  closedir(d);
}

void SungrowExtFlash::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sungrow external flash:\n"
                "  SPI host %d, MOSI %d, MISO %d, CLK %d, CS %d, %s @ %d MHz, DMA %s\n"
                "  Chip: %s, ID 0x%06" PRIX32 ", %" PRIu32 " KB",
                this->spi_host_ + 1, this->mosi_, this->miso_, this->clk_, this->cs_, io_mode_str(this->io_mode_),
                this->freq_mhz_, this->dma_channel_ == 0 ? "auto" : (this->dma_channel_ == 1 ? "ch1" : "ch2"),
                this->chip_ != nullptr ? "OK" : "NOT DETECTED", this->chip_id_, this->chip_size_ / 1024);
  ESP_LOGCONFIG(TAG,
                "  Partition '%s' (read-only), mount %s: %s\n"
                "  Superblock: %s\n"
                "  LittleFS: %u / %u B used\n"
                "  Sigdata record cap: 0x%X, default productType %u",
                this->partition_label_.c_str(), this->mount_path_.c_str(),
                this->mounted_ ? "mounted RO" : (this->chip_ != nullptr ? "MOUNT FAILED" : "-"),
                this->superblock_info_.empty() ? "-" : this->superblock_info_.c_str(), (unsigned) this->fs_used_,
                (unsigned) this->fs_total_, (unsigned) this->max_record_len_, this->default_product_type_);
  for (const auto &p : this->identity_files_)
    ESP_LOGCONFIG(TAG, "  Identity file%s: %s", p == this->identity_source_ ? " (used)" : "", p.c_str());
  ESP_LOGCONFIG(TAG, "  Identity: %s, SN '%s', broker %s:%u", this->identity_valid() ? "valid" : "NOT VALID",
                this->identity_.sn.c_str(), this->identity_.broker_host.c_str(), this->identity_.broker_port);
#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "SN", this->sn_text_);
  LOG_TEXT_SENSOR("  ", "Broker", this->broker_text_);
  LOG_TEXT_SENSOR("  ", "Identity source", this->source_text_);
  LOG_TEXT_SENSOR("  ", "Chip", this->chip_text_);
#endif
#ifdef USE_BINARY_SENSOR
  LOG_BINARY_SENSOR("  ", "Mounted", this->mounted_bs_);
  LOG_BINARY_SENSOR("  ", "Identity valid", this->identity_valid_bs_);
#endif
}

}  // namespace esphome::sungrow_extflash
