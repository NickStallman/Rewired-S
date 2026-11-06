#pragma once

#include <cstdint>
#include <string>

#include <esp_err.h>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"

#include "boot_slot.h"
#include "cloud_version.h"
#include "net_fallback.h"
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif
#ifdef USE_SUNGROW_DIAG
#include "diag.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#endif

namespace esphome::sungrow_board {

// Start feeding the external supervisor. Called from a global constructor emitted by codegen,
// before Application/logger exist, so it must not use ESPHome pin objects or ESP_LOGx.
// Blocks for init_toggles * init_delay_ms (stock: 10 x 100 ms). enable_pin < 0 = none.
void watchdog_early_start(int kick_pin, bool kick_inverted, int enable_pin, bool enable_inverted,
                          uint32_t interval_ms, uint32_t init_toggles, uint32_t init_delay_ms);

// Arm the periodic kick after watchdog_early_start(). Needs esp_timer_init_os to have run
// (ESP_ERR_INVALID_STATE before that). ESP_OK when already running.
esp_err_t watchdog_start_timer();

// True once the periodic watchdog timer is running.
bool watchdog_running();
// Number of edges produced since boot (diagnostics).
uint32_t watchdog_toggle_count();
/// Longest gap between periodic kicks since boot (ms), and how many periodic gaps exceeded 1 s.
uint32_t watchdog_max_gap_ms();
uint32_t watchdog_long_gaps();

// Boot slots (boot_slot.h / boot_slot.cpp): the slot we run from, the other OTA slot, and the
// one otadata write that makes the other slot boot next (verifies its image first; the caller
// restarts). Shared with sungrow_debug's gated "Boot other slot (stock app)".
SlotInfo read_running_slot();
SlotInfo read_other_slot();
BootSwitchVerdict switch_boot_to_other_slot();

#ifdef USE_SWITCH
class BootSwitchArmSwitch;
#endif

class SungrowBoard : public Component {
 public:
  SungrowBoard();

  // Runs before every bus/hardware component; it does nothing time-critical itself (the
  // watchdog is already running), but other components may query the board in their setup().
  float get_setup_priority() const override { return setup_priority::BUS + 100.0f; }
  void setup() override;
  void dump_config() override;
  void loop() override;
#ifdef USE_SUNGROW_DIAG
  void set_diag_config(const DiagConfig &cfg) { this->diag_cfg_ = cfg; }
#ifdef USE_TEXT_SENSOR
  void set_last_reset_sensor(text_sensor::TextSensor *s) { this->last_reset_ = s; }
#endif
#endif

  void set_product_name(const char *name) { this->product_name_ = name; }
  void set_product_type(uint16_t type) { this->product_type_ = type; }
  /// The YAML version ("" = detect from the other OTA slot, else the board default).
  void set_firmware_version(const char *version) { this->yaml_version_ = version; }
  /// The stock version the board file is built against (VARIANT_DEFAULTS).
  void set_default_version(const char *version) { this->default_version_ = version; }
  void set_version_floor(const char *floor) { this->version_floor_ = floor; }

  const char *product_name() const { return this->product_name_; }
  uint16_t product_type() const { return this->product_type_; }
  /// The version reported to iSolarCloud (cloud_version.h precedence); read live by isolarcloud
  /// and analytics, so an override applies without a restart.
  const char *firmware_version() const { return this->firmware_version_.c_str(); }
  CloudVersionSource cloud_version_source() const { return this->version_source_; }
  const char *detected_version() const { return this->version_pref_data_.detected; }
  const char *override_version() const { return this->version_pref_data_.override; }
  const char *default_version() const { return this->default_version_; }
  const char *yaml_version() const { return this->yaml_version_; }
  /// Store ("" clears) the override and re-resolve. False with `why` when it is not a stock
  /// version string. Loop task only.
  bool set_cloud_version_override(const std::string &v, std::string *why);

  // ---- "Boot other slot" (boot_slot.h) ----
  /// Arm (or disarm) ONE boot switch; the "Allow boot switch" switch, never restored.
  void arm_boot_switch(bool on);
  bool boot_switch_armed() const { return this->boot_guard_.armed(); }
  /// Switch the boot partition to the other slot and restart 3 s later. Refused unless armed
  /// (`confirmed` = the web UI's confirm dialog stands in for the switch) and the other slot holds
  /// a valid app image. Disarms on every call. Loop task only.
  BootSwitchVerdict boot_other_slot(bool confirmed);
#ifdef USE_SWITCH
  void set_boot_switch(switch_::Switch *s) { this->boot_switch_ = s; }
#endif
  // ---- network fallback (net_fallback.h, common/ethernet_fallback.yaml) ----
  void set_network_fallback(uint32_t timeout_ms, uint32_t phy_failed_timeout_ms, bool allow_stock) {
    this->net_fallback_cfg_.timeout_ms = timeout_ms;
    this->net_fallback_cfg_.phy_failed_timeout_ms = phy_failed_timeout_ms;
    this->net_fallback_cfg_.allow_stock = allow_stock;
  }
  /// One-line state of the network fallback ("" when the build has none).
  std::string network_fallback_status() const;
  bool is_winet_s2() const {
#ifdef USE_WINET_S2
    return true;
#else
    return false;
#endif
  }

 protected:
  void load_cloud_version_();
  void detect_other_slot_version_();
  void resolve_cloud_version_();

  const char *product_name_{""};
  uint16_t product_type_{0};
  const char *yaml_version_{""};
  const char *default_version_{""};
  std::string firmware_version_;
  CloudVersionSource version_source_{CloudVersionSource::DEFAULT};
  ESPPreferenceObject version_pref_;
  CloudVersionPref version_pref_data_{};
  const char *version_floor_{""};
  BootSwitchGuard boot_guard_;
#ifdef USE_SWITCH
  switch_::Switch *boot_switch_{nullptr};
#endif
#ifdef USE_SUNGROW_NET_FALLBACK
  void net_fallback_setup_();
  void net_fallback_loop_(uint32_t now);
  bool net_fallback_connected_() const;
  bool net_fallback_phy_failed_() const;
  FallbackTarget net_fallback_target_() const;
  NetFallbackConfig net_fallback_cfg_;
  ESPPreferenceObject net_fallback_pref_;
  NetFallbackPref net_fallback_data_{};
  uint32_t net_fallback_last_ms_{0};
  bool net_fallback_fired_{false};
#else
  NetFallbackConfig net_fallback_cfg_;
#endif
#ifdef USE_SUNGROW_DIAG
  DiagConfig diag_cfg_;
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *last_reset_{nullptr};
#endif
#endif
};

extern SungrowBoard *global_board;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

#ifdef USE_SWITCH
/// "Allow boot switch": arms one "Boot other slot"; ALWAYS_OFF restore mode, off after every boot.
class BootSwitchArmSwitch : public switch_::Switch, public Component, public Parented<SungrowBoard> {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void write_state(bool state) override;
};
#endif

#ifdef USE_BUTTON
/// "Boot other slot": refused unless "Allow boot switch" is on and the other slot holds a valid image.
class BootOtherSlotButton : public button::Button, public Parented<SungrowBoard> {
 protected:
  void press_action() override;
};
#endif

}  // namespace esphome::sungrow_board
