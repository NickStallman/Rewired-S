#pragma once
// External SPI NOR ("ext_flash") + READ-ONLY LittleFS mount of /usrdataout, and the cloud identity
// stored on it (docs/cloud_identity.md).
//
// SAFETY: this flash holds the dongle's irreplaceable SN/cloud identity. Stock firmware reformats
// it when a mount fails. This component never formats, erases or writes it:
//   - the io mode is restricted to non-quad modes, so esp_flash_init() never writes the status
//     register (QE bit);
//   - the registered partition is flagged readonly, so esp_partition_write/erase_range refuse;
//   - LittleFS is mounted with read_only=1, format_if_mount_failed=0, grow_on_mount=0;
//   - the VFS is registered with ESP_VFS_FLAG_READONLY_FS (done by esp_littlefs for read_only);
//   - files are only ever opened with "rb".

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"

#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif

#include "sigdata.h"

#include <esp_flash.h>
#include <esp_partition.h>

namespace esphome::sungrow_extflash {

enum class IoMode : uint8_t { SLOWRD = 0, FASTRD, DOUT, DIO };

class SungrowExtFlash : public Component {
 public:
  SungrowExtFlash();

  // Before any consumer (isolarcloud) runs its setup(); logger is already up by then.
  float get_setup_priority() const override { return setup_priority::BUS; }
  void setup() override;
  void loop() override;  // only enabled while a file dump is in progress
  void dump_config() override;

  // ---- config (codegen) ----
  void set_spi_host(int host) { this->spi_host_ = host; }
  void set_pins(int mosi, int miso, int clk, int cs) {
    this->mosi_ = mosi;
    this->miso_ = miso;
    this->clk_ = clk;
    this->cs_ = cs;
  }
  void set_frequency_mhz(int mhz) { this->freq_mhz_ = mhz; }
  void set_io_mode(IoMode mode) { this->io_mode_ = mode; }
  void set_dma_channel(int ch) { this->dma_channel_ = ch; }  // 0 = auto
  void set_mount_path(const char *path) { this->mount_path_ = path; }
  void set_partition_label(const char *label) { this->partition_label_ = label; }
  void set_partition_size(uint32_t size) { this->partition_size_ = size; }  // 0 = whole chip
  void add_identity_file(const char *path) { this->identity_files_.emplace_back(path); }
  void set_default_product_type(uint16_t type) { this->default_product_type_ = type; }
  // Sigdata record length cap: WiNet-S 0x100, WiNet-S2 0x420 (set from sungrow_board's variant).
  void set_max_record_len(uint16_t len) { this->max_record_len_ = len; }

  // ---- C++ API (for isolarcloud and friends) ----
  bool is_mounted() const { return this->mounted_; }
  uint32_t chip_id() const { return this->chip_id_; }      // JEDEC id (mfr<<16 | type<<8 | cap)
  uint32_t chip_size() const { return this->chip_size_; }  // bytes, as reported by esp_flash_get_size
  const std::string &mount_path() const { return this->mount_path_; }

  // Identity is loaded synchronously in setup(). It is always populated (with stock defaults
  // for missing records), but only trustworthy when identity_valid().
  bool identity_loaded() const { return this->identity_loaded_; }
  bool identity_valid() const { return this->identity_loaded_ && this->identity_.valid(); }
  const Identity &identity() const { return this->identity_; }
  // Raw parsed file the identity came from (for other 0xF7 sigs, e.g. Wi-Fi creds 0x1053/0x1054).
  const SigdataFile &sigdata() const { return this->sigdata_; }
  // Which file the identity came from ("" if none could be read).
  const std::string &identity_source() const { return this->identity_source_; }

  // Called once after the identity load attempt (also if no valid SN was found: check
  // identity.valid()). If that already happened, the callback runs immediately.
  void add_on_identity_callback(std::function<void(const Identity &)> &&callback);

  // Read a whole file from the mounted fs (read-only). Fails if not mounted or larger than max.
  bool read_file(const std::string &path, std::vector<uint8_t> &out, size_t max_size = SIGDATA_MAX_FILE_SIZE);
  // Stream any file to the log as hex at VERY_VERBOSE (useful for backups). Non-blocking:
  // the dump runs in loop(), a few lines per iteration. Returns false if it can't start.
  bool dump_file(const std::string &path) { return this->start_dump_(path, false); }
  // Hex-dump the identity file at INFO level (button). Also logs the decoded table.
  bool dump_identity();
  // Log the directory tree under path (default: mount path) at INFO, up to max_depth.
  void list_dir(const std::string &path = "", int max_depth = 4);

#ifdef USE_TEXT_SENSOR
  void set_sn_text_sensor(text_sensor::TextSensor *s) { this->sn_text_ = s; }
  void set_broker_text_sensor(text_sensor::TextSensor *s) { this->broker_text_ = s; }
  void set_identity_source_text_sensor(text_sensor::TextSensor *s) { this->source_text_ = s; }
  void set_chip_text_sensor(text_sensor::TextSensor *s) { this->chip_text_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_mounted_binary_sensor(binary_sensor::BinarySensor *s) { this->mounted_bs_ = s; }
  void set_identity_valid_binary_sensor(binary_sensor::BinarySensor *s) { this->identity_valid_bs_ = s; }
#endif

 protected:
  bool init_bus_();
  bool register_partition_();
  void log_superblock_();
  bool mount_();
  void load_identity_();
  void publish_states_();
  void log_identity_(int level) const;
  bool start_dump_(const std::string &path, bool info_level);
  void list_dir_rec_(const std::string &path, int depth, int max_depth, uint32_t &count);

  // config
  int spi_host_{1};  // SPI2_HOST
  int mosi_{-1}, miso_{-1}, clk_{-1}, cs_{-1};
  int freq_mhz_{40};
  IoMode io_mode_{IoMode::DIO};
  int dma_channel_{0};
  std::string mount_path_{"/usrdataout"};
  std::string partition_label_{"usrdataout"};
  uint32_t partition_size_{0};
  uint8_t partition_subtype_{0x81};  // as stock (FAT subtype; esp_littlefs mounts by label)
  std::vector<std::string> identity_files_;
  uint16_t default_product_type_{0x720};
  size_t max_record_len_{SIGDATA_MAX_RECORD_LEN};

  // state
  bool bus_initialized_{false};
  esp_flash_t *chip_{nullptr};
  const esp_partition_t *partition_{nullptr};
  uint32_t chip_id_{0};
  uint32_t chip_size_{0};
  bool mounted_{false};
  int mount_err_{0};
  size_t fs_total_{0}, fs_used_{0};
  std::string superblock_info_;

  SigdataFile sigdata_;
  Identity identity_;
  std::string identity_source_;
  IdentityRule identity_rule_{IdentityRule::NONE};  // which rule chose identity_source_
  bool identity_loaded_{false};
  CallbackManager<void(const Identity &)> identity_callbacks_;

  // file dump state (loop())
  FILE *dump_fp_{nullptr};
  std::string dump_path_;
  uint32_t dump_offset_{0};
  bool dump_info_{false};

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *sn_text_{nullptr};
  text_sensor::TextSensor *broker_text_{nullptr};
  text_sensor::TextSensor *source_text_{nullptr};
  text_sensor::TextSensor *chip_text_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *mounted_bs_{nullptr};
  binary_sensor::BinarySensor *identity_valid_bs_{nullptr};
#endif
};

// Set by the constructor; nullptr if the component isn't configured. Prefer use_id() in YAML.
extern SungrowExtFlash *global_extflash;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

class IdentityTrigger : public Trigger<> {
 public:
  explicit IdentityTrigger(SungrowExtFlash *parent) {
    parent->add_on_identity_callback([this](const Identity &) { this->trigger(); });
  }
};

#ifdef USE_BUTTON
class DumpIdentityButton : public button::Button, public Parented<SungrowExtFlash> {
 protected:
  void press_action() override { this->parent_->dump_identity(); }
};
class ListFilesButton : public button::Button, public Parented<SungrowExtFlash> {
 protected:
  void press_action() override { this->parent_->list_dir(); }
};
#endif

}  // namespace esphome::sungrow_extflash
