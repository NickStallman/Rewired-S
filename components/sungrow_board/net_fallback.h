#pragma once
// Network fallback (common/ethernet_fallback.yaml): a build that has never had a network boots
// the other OTA slot. Pure part, host-tested in tests/board/test_net_fallback.cpp; the glue in
// sungrow_board.cpp reads the network state, the other slot and the NVS record.
//  - Armed until the running build has connected once (NVS record keyed by the build id).
//  - Fires `timeout` after boot without a connection, or `phy_failed_timeout` when the Ethernet
//    driver failed to start.
//  - Target: a Rewired-S image that is not an Ethernet build ("+ethernet" / "+wifi" marker in
//    the app descriptor's version; unmarked counts); the stock app only with `allow_stock`.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "boot_slot.h"
#include "cloud_version.h"

namespace esphome {
namespace sungrow_board {

/// Network kind of a Rewired-S image from the "+wifi" / "+ethernet" suffix of its version.
enum class SlotNetwork : uint8_t { UNKNOWN, WIFI, ETHERNET };

inline SlotNetwork slot_network_kind(const char *version) {
  if (version == nullptr)
    return SlotNetwork::UNKNOWN;
  const char *plus = std::strrchr(version, '+');
  if (plus == nullptr)
    return SlotNetwork::UNKNOWN;
  if (std::strcmp(plus, "+ethernet") == 0)
    return SlotNetwork::ETHERNET;
  if (std::strcmp(plus, "+wifi") == 0)
    return SlotNetwork::WIFI;
  return SlotNetwork::UNKNOWN;
}

inline const char *slot_network_name(SlotNetwork k) {
  switch (k) {
    case SlotNetwork::WIFI:
      return "wifi";
    case SlotNetwork::ETHERNET:
      return "ethernet";
    default:
      return "unknown";
  }
}

/// NVS record: the build that last armed the fallback and whether it has connected since.
struct NetFallbackPref {
  uint32_t build_id;
  uint8_t connected_once;
  uint8_t reserved[3];
};

struct NetFallbackConfig {
  uint32_t timeout_ms{180000};
  uint32_t phy_failed_timeout_ms{60000};
  bool allow_stock{false};
};

enum class FallbackTarget : uint8_t { OK, NO_OTHER_SLOT, NO_VALID_IMAGE, OTHER_IS_ETHERNET, OTHER_IS_STOCK, OTHER_IS_FOREIGN };

inline const char *fallback_target_text(FallbackTarget t) {
  switch (t) {
    case FallbackTarget::OK:
      return "the other slot holds a Rewired-S image that is not an Ethernet build";
    case FallbackTarget::NO_OTHER_SLOT:
      return "no other OTA slot";
    case FallbackTarget::NO_VALID_IMAGE:
      return "the other slot holds no valid app image";
    case FallbackTarget::OTHER_IS_ETHERNET:
      return "the other slot holds another Ethernet build";
    case FallbackTarget::OTHER_IS_STOCK:
      return "the other slot holds the stock app (allow_stock is off)";
    default:
      return "the other slot holds an unknown image";
  }
}

/// Whether the other slot may be booted as the fallback. `our_project` is the running image's
/// esp_app_desc_t.project_name (every Rewired-S entry built from the same name shares it).
inline FallbackTarget check_fallback_target(const SlotInfo &other, const char *our_project, bool allow_stock) {
  if (!other.present)
    return FallbackTarget::NO_OTHER_SLOT;
  if (!other.valid_image)
    return FallbackTarget::NO_VALID_IMAGE;
  const bool ours = our_project != nullptr && our_project[0] != '\0' && std::strcmp(other.project, our_project) == 0;
  if (ours)
    return slot_network_kind(other.version) == SlotNetwork::ETHERNET ? FallbackTarget::OTHER_IS_ETHERNET
                                                                     : FallbackTarget::OK;
  if (is_stock_version_string(other.version))
    return allow_stock ? FallbackTarget::OK : FallbackTarget::OTHER_IS_STOCK;
  return FallbackTarget::OTHER_IS_FOREIGN;
}

/// At boot: a record from another build is reset to "this build, never connected". True when the
/// record changed and should be stored.
inline bool net_fallback_begin(NetFallbackPref *p, uint32_t build_id) {
  if (p->build_id == build_id)
    return false;
  *p = NetFallbackPref{};
  p->build_id = build_id;
  return true;
}

inline bool net_fallback_armed(const NetFallbackPref &p) { return p.connected_once == 0; }

enum class NetFallbackAction : uint8_t { NONE, CONNECTED, FALL_BACK };

/// One poll. CONNECTED: the record was updated (store it) and the fallback is disarmed for this
/// build. FALL_BACK: boot the other slot now (the caller checks the target first).
inline NetFallbackAction net_fallback_step(const NetFallbackConfig &c, NetFallbackPref *p, bool connected,
                                           bool phy_failed, uint32_t uptime_ms) {
  if (!net_fallback_armed(*p))
    return NetFallbackAction::NONE;
  if (connected) {
    p->connected_once = 1;
    return NetFallbackAction::CONNECTED;
  }
  if (phy_failed && uptime_ms >= c.phy_failed_timeout_ms)
    return NetFallbackAction::FALL_BACK;
  if (uptime_ms >= c.timeout_ms)
    return NetFallbackAction::FALL_BACK;
  return NetFallbackAction::NONE;
}

/// One-line status for the config dump, the diagnostics report and /api/boot_slot.
inline void net_fallback_status(const NetFallbackConfig &c, const NetFallbackPref &p, bool connected, bool phy_failed,
                                uint32_t uptime_ms, FallbackTarget target, char *buf, size_t n) {
  if (!net_fallback_armed(p)) {
    std::snprintf(buf, n, "disarmed: this build has connected before");
    return;
  }
  if (connected) {
    std::snprintf(buf, n, "disarmed: connected");
    return;
  }
  const uint32_t limit = phy_failed ? c.phy_failed_timeout_ms : c.timeout_ms;
  const uint32_t left = uptime_ms >= limit ? 0 : (limit - uptime_ms + 999) / 1000;
  if (target != FallbackTarget::OK) {
    std::snprintf(buf, n, "armed but no target: %s", fallback_target_text(target));
    return;
  }
  std::snprintf(buf, n, "armed: boots the other slot in %lu s unless the network comes up%s",
                static_cast<unsigned long>(left), phy_failed ? " (Ethernet driver failed to start)" : "");
}

}  // namespace sungrow_board
}  // namespace esphome
