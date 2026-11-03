#include "board_io.h"

#ifdef USE_SUNGROW_BOARD_IO

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_NETWORK
#include "esphome/components/network/util.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace esphome::sungrow_board {

static const char *const TAG = "sungrow_board.io";

static constexpr uint32_t BUTTON_POLL_MS = 50;  // stock 50 ms esp_timer
static constexpr uint32_t LED_TICK_MS = 100;    // stock: every 2nd tick

void BoardIO::add_hold_mark(uint32_t ms) {
  if (this->n_marks_ < 4)
    this->marks_[this->n_marks_++] = ms;
}

void BoardIO::setup() {
  this->ap_pref_ = global_preferences->make_preference<bool>(fnv1_hash("sungrow_board_fallback_ap"));
  bool stored = true;
  if (this->ap_pref_.load(&stored))
    this->ap_enabled_ = stored;
  this->apply_ap_flag_();

  if (this->button_ != nullptr) {
    this->decoder_.set_hold_marks(this->marks_[0], this->marks_[1], this->marks_[2], this->marks_[3]);
    this->set_interval("button", BUTTON_POLL_MS, [this]() { this->button_tick_(); });
  }
  if (this->red_ != nullptr || this->green_ != nullptr || this->blue_ != nullptr)
    this->set_interval("leds", LED_TICK_MS, [this]() { this->led_tick_(); });
}

void BoardIO::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Board IO:\n"
                "  LEDs: red %s, green %s, blue %s\n"
                "  Button: %s\n"
                "  Fault grace after boot: %" PRIu32 " ms\n"
                "  Fallback AP: %s (ap_timeout %" PRIu32 " ms)",
                YESNO(this->red_), YESNO(this->green_), YESNO(this->blue_), YESNO(this->button_),
                this->fault_grace_ms_, this->ap_enabled_ ? "enabled" : "DISABLED by button", this->ap_timeout_ms_);
}

void BoardIO::write_(output::BinaryOutput *out, bool on, int8_t &last) {
  if (out == nullptr || last == static_cast<int8_t>(on))
    return;
  last = static_cast<int8_t>(on);
  if (on) {
    out->turn_on();
  } else {
    out->turn_off();
  }
}

void BoardIO::led_tick_() {
  const uint32_t now = millis();
  const bool cloud_up = this->cloud_connected_ ? this->cloud_connected_() : false;
  const bool cloud_wanted = this->cloud_enabled_ ? this->cloud_enabled_() : static_cast<bool>(this->cloud_connected_);
  const bool cloud_error = this->cloud_error_ ? this->cloud_error_()
                                              : this->cloud_err_.update(cloud_wanted, cloud_up, now);
  const bool past_grace = now >= this->fault_grace_ms_;
  const bool offline = past_grace && this->devices_offline_ && this->devices_offline_();
  const bool faults = offline || cloud_error;
  if (faults != this->faults_) {
    this->faults_ = faults;
    if (faults) {
      ESP_LOGW(TAG, "Fault indication on:%s%s", offline ? " RS485 device offline" : "",
               cloud_error ? " cloud uplink down" : "");
    } else {
      ESP_LOGI(TAG, "Fault indication off");
    }
  }

  logic::LedInputs in;
#ifdef USE_NETWORK
  in.network = network::is_connected();
#endif
  in.config_mode = this->fallback_ap_active();
  in.cloud = cloud_up;
  in.faults = faults;
  const logic::LedOutputs out = this->leds_.update(in, now);
  this->write_(this->red_, out.red, this->red_last_);
  this->write_(this->green_, out.green, this->green_last_);
  this->write_(this->blue_, out.blue, this->blue_last_);
}

void BoardIO::button_tick_() {
  using Kind = logic::ButtonDecoder::Kind;
  const auto ev = this->decoder_.update(this->button_->state, millis());
  switch (ev.kind) {
    case Kind::CLICKS:
      ESP_LOGI(TAG, "Button: %u click(s)", ev.clicks);
      this->click_trigger_.trigger(ev.clicks);
      break;
    case Kind::HOLD_REACHED:
      ESP_LOGI(TAG, "Button held %" PRIu32 " s (acts on release)", ev.held_ms / 1000);
      this->hold_reached_trigger_.trigger(ev.held_ms);
      break;
    case Kind::LONG_RELEASE:
      ESP_LOGI(TAG, "Button long press released after %" PRIu32 " ms", ev.held_ms);
      this->long_press_trigger_.trigger(ev.held_ms);
      break;
    case Kind::NONE:
      break;
  }
}

bool BoardIO::fallback_ap_active() const {
#if defined(USE_WIFI) && defined(USE_WIFI_AP)
  return wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_ap_active();
#else
  return false;
#endif
}

void BoardIO::apply_ap_flag_() {
#if defined(USE_WIFI) && defined(USE_WIFI_AP)
  if (wifi::global_wifi_component == nullptr)
    return;
  // ap_timeout 0 = ESPHome never starts the fallback AP (wifi_component.cpp check_connecting_finished).
  wifi::global_wifi_component->set_ap_timeout(this->ap_enabled_ ? this->ap_timeout_ms_ : 0);
#endif
}

void BoardIO::set_fallback_ap_enabled(bool enabled) {
#if defined(USE_WIFI) && defined(USE_WIFI_AP)
  if (enabled == this->ap_enabled_) {
    ESP_LOGI(TAG, "Fallback AP already %s", enabled ? "enabled" : "disabled");
    return;
  }
  this->ap_enabled_ = enabled;
  this->ap_pref_.save(&this->ap_enabled_);
  global_preferences->sync();
  this->apply_ap_flag_();
  ESP_LOGI(TAG, "Fallback AP %s (saved)", enabled ? "enabled" : "disabled");
  if (!enabled && this->fallback_ap_active()) {
    // ESPHome has no API to stop a running AP; a restart with the saved flag brings Wi-Fi up
    // without it.
    ESP_LOGW(TAG, "Fallback AP is running; restarting to stop it");
    this->set_timeout("ap_restart", 500, []() { App.safe_reboot(); });
  }
#else
  ESP_LOGI(TAG, "No Wi-Fi fallback AP configured; ignoring");
#endif
}

void BoardIO::reset_esphome_settings() {
  ESP_LOGW(TAG, "Erasing ESPHome's NVS namespace \"esphome\" and restarting (external flash untouched)");
  // Flush pending writes first so nothing lands after the erase; then restart directly (not
  // safe_reboot, whose shutdown hooks could save preferences again).
  global_preferences->sync();
  nvs_handle_t h;
  esp_err_t err = nvs_open("esphome", NVS_READWRITE, &h);
  if (err == ESP_OK) {
    err = nvs_erase_all(h);
    if (err == ESP_OK)
      err = nvs_commit(h);
    nvs_close(h);
  }
  if (err != ESP_OK)
    ESP_LOGE(TAG, "NVS erase failed: %s", esp_err_to_name(err));
  vTaskDelay(pdMS_TO_TICKS(200));  // let the log drain
  esp_restart();
}

}  // namespace esphome::sungrow_board

#endif  // USE_SUNGROW_BOARD_IO
