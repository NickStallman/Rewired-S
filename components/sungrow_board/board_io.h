#pragma once
// sungrow_board `io:` block: stock-like button + status LEDs (common/board_io.yaml).
// The decision logic is in board_logic.h (host-tested in tests/board/); this is the ESPHome glue.

#include "esphome/core/defines.h"

#ifdef USE_SUNGROW_BOARD_IO

#include <cstdint>
#include <functional>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/output/binary_output.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include "board_logic.h"

namespace esphome::sungrow_board {

class BoardIO : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ---- config (codegen) ----
  void set_leds(output::BinaryOutput *red, output::BinaryOutput *green, output::BinaryOutput *blue) {
    this->red_ = red;
    this->green_ = green;
    this->blue_ = blue;
  }
  void set_cloud_connected(std::function<bool()> &&f) { this->cloud_connected_ = std::move(f); }
  void set_cloud_enabled(std::function<bool()> &&f) { this->cloud_enabled_ = std::move(f); }
  void set_cloud_error(std::function<bool()> &&f) { this->cloud_error_ = std::move(f); }
  void set_devices_offline(std::function<bool()> &&f) { this->devices_offline_ = std::move(f); }
  void set_fault_grace(uint32_t ms) { this->fault_grace_ms_ = ms; }
  void set_button(binary_sensor::BinarySensor *b) { this->button_ = b; }
  void add_hold_mark(uint32_t ms);
  // ESPHome's configured wifi ap_timeout, restored when the fallback AP is re-enabled. 0 = no AP.
  void set_fallback_ap_timeout(uint32_t ms) { this->ap_timeout_ms_ = ms; }

  Trigger<uint8_t> *get_click_trigger() { return &this->click_trigger_; }
  Trigger<uint32_t> *get_long_press_trigger() { return &this->long_press_trigger_; }
  Trigger<uint32_t> *get_hold_reached_trigger() { return &this->hold_reached_trigger_; }

  // ---- runtime API (YAML lambdas) ----
  // Fallback AP on/off, persisted in ESPHome's own NVS preferences (never on the external flash).
  // "Off" sets wifi ap_timeout to 0 so ESPHome never starts the fallback AP. ESPHome can't stop an
  // AP that is already running, so if it is up we restart (the flag is saved first).
  void set_fallback_ap_enabled(bool enabled);
  bool fallback_ap_active() const;
  // Erase ESPHome's own NVS namespace ("esphome": Wi-Fi creds from the captive portal, restored
  // switch/number states, isolarcloud's persisted server switch, our AP flag) and restart.
  // Leaves every other NVS namespace, the internal app/OTA partitions and the external flash
  // (/usrdataout identity) alone.
  void reset_esphome_settings();

  bool faults() const { return this->faults_; }

 protected:
  void button_tick_();
  void led_tick_();
  void write_(output::BinaryOutput *out, bool on, int8_t &last);
  void apply_ap_flag_();

  output::BinaryOutput *red_{nullptr}, *green_{nullptr}, *blue_{nullptr};
  int8_t red_last_{-1}, green_last_{-1}, blue_last_{-1};
  std::function<bool()> cloud_connected_;
  std::function<bool()> cloud_enabled_;
  std::function<bool()> cloud_error_;  // if set, replaces the connected/enabled heuristic
  std::function<bool()> devices_offline_;
  uint32_t fault_grace_ms_{60000};
  bool faults_{false};
  logic::LedLogic leds_;
  logic::CloudErrorTracker cloud_err_;

  binary_sensor::BinarySensor *button_{nullptr};
  logic::ButtonDecoder decoder_;
  uint32_t marks_[4]{0, 0, 0, 0};
  uint8_t n_marks_{0};
  Trigger<uint8_t> click_trigger_;
  Trigger<uint32_t> long_press_trigger_;
  Trigger<uint32_t> hold_reached_trigger_;

  uint32_t ap_timeout_ms_{0};
  bool ap_enabled_{true};
  ESPPreferenceObject ap_pref_;
};

}  // namespace esphome::sungrow_board

#endif  // USE_SUNGROW_BOARD_IO
