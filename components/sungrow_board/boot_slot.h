#pragma once
// "Boot other slot": switch the boot partition to the other OTA slot (normally the stock app the
// stock updater left there), the way back to stock without a cable (docs/back_to_stock.md).
//
// This header is the pure part (host-tested in tests/board/test_boot_slot.cpp): the one-shot
// arming guard, the verdict of a switch request and the JSON the web UI shows. boot_slot.cpp
// holds the IDF calls: reading both slots' app descriptors and the single otadata write
// (esp_ota_set_boot_partition, which verifies the target image first). It never touches the app
// slots, NVS, the partition table, the bootloader or the external flash.
//
// Gate: the "Allow boot switch" switch (never restored, off after every boot) arms ONE attempt;
// the web UI's confirm dialog arms it the same way. The target must hold a valid app image.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace esphome {
namespace sungrow_board {

/// One OTA slot as the web UI sees it.
struct SlotInfo {
  bool present{false};      ///< the slot exists (false = no other OTA slot in the table)
  char label[17]{};         ///< partition label ("ota_0")
  uint32_t address{0};      ///< flash offset
  bool valid_image{false};  ///< esp_ota_get_partition_description succeeded (image header + magic ok)
  char project[33]{};       ///< esp_app_desc_t.project_name, "" when no valid image
  char version[33]{};       ///< esp_app_desc_t.version
  char idf[33]{};           ///< esp_app_desc_t.idf_ver
};

enum class BootSwitchVerdict : uint8_t { OK, NOT_ARMED, NO_OTHER_SLOT, NO_VALID_IMAGE, SET_FAILED };

inline const char *boot_switch_verdict_code(BootSwitchVerdict v) {
  switch (v) {
    case BootSwitchVerdict::OK:
      return "OK";
    case BootSwitchVerdict::NOT_ARMED:
      return "NOT_ARMED";
    case BootSwitchVerdict::NO_OTHER_SLOT:
      return "NO_OTHER_SLOT";
    case BootSwitchVerdict::NO_VALID_IMAGE:
      return "NO_VALID_IMAGE";
    default:
      return "SET_FAILED";
  }
}

inline const char *boot_switch_verdict_text(BootSwitchVerdict v) {
  switch (v) {
    case BootSwitchVerdict::OK:
      return "boot partition switched; restarting";
    case BootSwitchVerdict::NOT_ARMED:
      return "turn on 'Allow boot switch' first";
    case BootSwitchVerdict::NO_OTHER_SLOT:
      return "no other OTA slot";
    case BootSwitchVerdict::NO_VALID_IMAGE:
      return "the other slot holds no valid app image";
    default:
      return "could not rewrite the boot record";
  }
}

/// The one-shot arming guard: arm() (the switch turning on, or a confirmed web request) permits
/// exactly one take(); the flag is never restored across boots and clears on every attempt,
/// whatever its outcome.
class BootSwitchGuard {
 public:
  void arm(bool on) { this->armed_ = on; }
  bool armed() const { return this->armed_; }
  /// Consume the arming: true when a switch may proceed. Always leaves the guard disarmed.
  bool take() {
    const bool was = this->armed_;
    this->armed_ = false;
    return was;
  }

 private:
  bool armed_{false};
};

/// Verdict of a switch request before the otadata write: `armed` = take() returned true.
inline BootSwitchVerdict check_boot_switch(bool armed, const SlotInfo &other) {
  if (!armed)
    return BootSwitchVerdict::NOT_ARMED;
  if (!other.present)
    return BootSwitchVerdict::NO_OTHER_SLOT;
  if (!other.valid_image)
    return BootSwitchVerdict::NO_VALID_IMAGE;
  return BootSwitchVerdict::OK;
}

namespace boot_slot_detail {
inline void json_str(std::string &o, const char *s) {
  o += '"';
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(s); p != nullptr && *p != '\0'; ++p) {
    const unsigned char c = *p;
    if (c == '"' || c == '\\') {
      o += '\\';
      o += static_cast<char>(c);
    } else if (c < 0x20 || c > 0x7e) {
      o += '?';  // app descriptors are ASCII; anything else is not worth escaping
    } else {
      o += static_cast<char>(c);
    }
  }
  o += '"';
}
inline void slot_json(std::string &o, const SlotInfo &s) {
  if (!s.present) {
    o += "null";
    return;
  }
  char buf[24];
  o += "{\"label\":";
  json_str(o, s.label);
  std::snprintf(buf, sizeof(buf), ",\"address\":%lu", static_cast<unsigned long>(s.address));
  o += buf;
  o += ",\"valid\":";
  o += s.valid_image ? "true" : "false";
  o += ",\"project\":";
  json_str(o, s.project);
  o += ",\"version\":";
  json_str(o, s.version);
  o += ",\"idf\":";
  json_str(o, s.idf);
  o += '}';
}
}  // namespace boot_slot_detail

/// GET /api/boot_slot: {"running":{..},"other":{..}|null,"armed":bool,"can_switch":bool[,"fallback":".."]}
/// `fallback`: the network fallback's state line (net_fallback.h), omitted when nullptr or "".
inline std::string boot_slots_json(const SlotInfo &running, const SlotInfo &other, bool armed,
                                   const char *fallback = nullptr) {
  std::string j = "{\"running\":";
  boot_slot_detail::slot_json(j, running);
  j += ",\"other\":";
  boot_slot_detail::slot_json(j, other);
  j += ",\"armed\":";
  j += armed ? "true" : "false";
  j += ",\"can_switch\":";
  j += other.present && other.valid_image ? "true" : "false";
  if (fallback != nullptr && fallback[0] != '\0') {
    j += ",\"fallback\":";
    boot_slot_detail::json_str(j, fallback);
  }
  j += '}';
  return j;
}

}  // namespace sungrow_board
}  // namespace esphome
