// IDF side of "Boot other slot" (boot_slot.h): slot descriptors and THE otadata write.
// esp_ota_set_boot_partition() verifies the target image and then rewrites one otadata sector; it
// never touches the app slots, nvs, the partition table, the bootloader or the external flash.
#include "boot_slot.h"
#include "sungrow_board.h"

#include <cinttypes>
#include <cstdio>

#include <esp_ota_ops.h>

#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome {
namespace sungrow_board {

static const char *const TAG = "sungrow_board";

static void fill_slot(const esp_partition_t *p, SlotInfo *out) {
  *out = SlotInfo{};
  if (p == nullptr)
    return;
  out->present = true;
  std::snprintf(out->label, sizeof(out->label), "%s", p->label);
  out->address = p->address;
  esp_app_desc_t d;
  if (esp_ota_get_partition_description(p, &d) != ESP_OK)
    return;
  out->valid_image = true;
  std::snprintf(out->project, sizeof(out->project), "%.*s", static_cast<int>(sizeof(d.project_name)), d.project_name);
  std::snprintf(out->version, sizeof(out->version), "%.*s", static_cast<int>(sizeof(d.version)), d.version);
  std::snprintf(out->idf, sizeof(out->idf), "%.*s", static_cast<int>(sizeof(d.idf_ver)), d.idf_ver);
}

SlotInfo read_running_slot() {
  SlotInfo s;
  fill_slot(esp_ota_get_running_partition(), &s);
  return s;
}

SlotInfo read_other_slot() {
  SlotInfo s;
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  if (run == nullptr || other == nullptr || other->address == run->address)
    return s;  // no other OTA slot
  fill_slot(other, &s);
  return s;
}

BootSwitchVerdict switch_boot_to_other_slot() {
  const SlotInfo other = read_other_slot();
  const BootSwitchVerdict v = check_boot_switch(true, other);
  if (v != BootSwitchVerdict::OK) {
    ESP_LOGE(TAG, "boot_other_slot refused: %s", boot_switch_verdict_text(v));
    return v;
  }
  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t *run = esp_ota_get_running_partition();
  ESP_LOGW(TAG, "boot_other_slot: switching boot partition %s -> %s @0x%" PRIX32 " (app '%s' %s, IDF %s)",
           run != nullptr ? run->label : "?", other.label, other.address, other.project, other.version, other.idf);
  const esp_err_t err = esp_ota_set_boot_partition(target);  // verifies the image, writes otadata only
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "boot_other_slot: esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    return BootSwitchVerdict::SET_FAILED;
  }
  ESP_LOGW(TAG, "boot_other_slot: otadata updated; the caller restarts the board", other.label);
  return BootSwitchVerdict::OK;
}

// ---- SungrowBoard glue ----

void SungrowBoard::arm_boot_switch(bool on) {
  this->boot_guard_.arm(on);
  if (on)
    ESP_LOGW(TAG, "Boot switch ARMED: the next 'Boot other slot' rewrites otadata and reboots");
}

BootSwitchVerdict SungrowBoard::boot_other_slot(bool confirmed) {
  const bool armed = this->boot_guard_.take() || confirmed;
  const BootSwitchVerdict pre = check_boot_switch(armed, read_other_slot());
#ifdef USE_SWITCH
  if (this->boot_switch_ != nullptr && this->boot_switch_->state)
    this->boot_switch_->publish_state(false);  // one shot, whatever the outcome
#endif
  if (pre != BootSwitchVerdict::OK) {
    ESP_LOGW(TAG, "boot_other_slot refused: %s", boot_switch_verdict_text(pre));
    return pre;
  }
  const BootSwitchVerdict v = switch_boot_to_other_slot();
  if (v == BootSwitchVerdict::OK) {
    ESP_LOGW(TAG, "Rebooting into the other slot in 3 s");
    this->set_timeout("boot_switch", 3000, []() { App.safe_reboot(); });
  }
  return v;
}

#ifdef USE_SWITCH
void BootSwitchArmSwitch::setup() {
  // ALWAYS_OFF restore mode: the switch starts off on every boot (get_initial_state_with_restore_mode
  // yields false); never arm from a restored state.
  this->publish_state(false);
}
void BootSwitchArmSwitch::dump_config() { LOG_SWITCH("", "Allow boot switch", this); }
void BootSwitchArmSwitch::write_state(bool state) {
  this->parent_->arm_boot_switch(state);
  this->publish_state(state);
}
#endif

#ifdef USE_BUTTON
void BootOtherSlotButton::press_action() { this->parent_->boot_other_slot(false); }
#endif

}  // namespace sungrow_board
}  // namespace esphome
