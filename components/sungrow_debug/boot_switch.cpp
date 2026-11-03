// Debug build's "Boot other slot (stock app)": switching the boot partition back to the other OTA
// slot (normally the stock app that stock OTA left there). The otadata write itself lives in
// components/sungrow_board/boot_slot.cpp (esp_ota_set_boot_partition verifies the target image and
// rewrites one otadata sector; app slots, nvs, the partition table, the bootloader and the external
// flash are never touched); this file only gates it: the "Allow boot switch to other slot" switch
// must be on (never restored, always off after boot), one shot, logged loudly.
// tests/test_debug_readonly.py checks that components/sungrow_debug holds no write symbol at all.
#include "sungrow_debug.h"

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include "esphome/components/sungrow_board/sungrow_board.h"

namespace esphome {
namespace sungrow_debug {

static const char *const TAG = "sungrow_debug";

bool SungrowDebug::boot_other_slot() {
  if (!this->allow_boot_switch_) {
    ESP_LOGW(TAG, "boot_other_slot refused: turn on the 'Allow boot switch to other slot' switch first");
    return false;
  }
  this->set_flag(SwitchKind::ALLOW_BOOT_SWITCH, false);  // one shot
  if (sungrow_board::global_board == nullptr) {
    ESP_LOGE(TAG, "boot_other_slot: no sungrow_board");
    return false;
  }
  // The debug switch above stands in for the board's own "Allow boot switch".
  return sungrow_board::global_board->boot_other_slot(/*confirmed=*/true) == sungrow_board::BootSwitchVerdict::OK;
}

}  // namespace sungrow_debug
}  // namespace esphome
