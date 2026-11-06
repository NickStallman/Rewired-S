// Host tests for components/sungrow_board/net_fallback.h: the slot kind marker, the target check,
// the per-build arming record and the timing. Build + run: `make -C tests/board`.
#include <cstdio>
#include <cstring>
#include <string>

#include "net_fallback.h"

using namespace esphome::sungrow_board;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    g_checks++;                                                   \
    if (!(cond)) {                                                \
      g_fail++;                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                             \
  } while (0)

static bool contains(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

static SlotInfo slot(bool valid, const char *project, const char *version) {
  SlotInfo s;
  s.present = true;
  std::snprintf(s.label, sizeof(s.label), "ota_1");
  s.address = 0x610000;
  s.valid_image = valid;
  std::snprintf(s.project, sizeof(s.project), "%s", project);
  std::snprintf(s.version, sizeof(s.version), "%s", version);
  return s;
}

static void test_slot_kind() {
  CHECK(slot_network_kind("0.9.1+ethernet") == SlotNetwork::ETHERNET);
  CHECK(slot_network_kind("0.9.1+wifi") == SlotNetwork::WIFI);
  CHECK(slot_network_kind("0.9.0") == SlotNetwork::UNKNOWN);               // 0.9.0 images carry no marker
  CHECK(slot_network_kind("0.9.1+ethernet-dirty") == SlotNetwork::UNKNOWN);  // the marker is the whole suffix
  CHECK(slot_network_kind("WINET-SV300.001.03.P033B001-dir") == SlotNetwork::UNKNOWN);
  CHECK(slot_network_kind("") == SlotNetwork::UNKNOWN && slot_network_kind(nullptr) == SlotNetwork::UNKNOWN);
  CHECK(std::string(slot_network_name(SlotNetwork::ETHERNET)) == "ethernet");
  CHECK(std::string(slot_network_name(SlotNetwork::UNKNOWN)) == "unknown");
}

static void test_target() {
  SlotInfo none;
  CHECK(check_fallback_target(none, "rewired-s", false) == FallbackTarget::NO_OTHER_SLOT);
  CHECK(check_fallback_target(slot(false, "", ""), "rewired-s", false) == FallbackTarget::NO_VALID_IMAGE);
  // The Wi-Fi image the user installed through: the normal target.
  CHECK(check_fallback_target(slot(true, "rewired-s", "0.9.1+wifi"), "rewired-s", false) == FallbackTarget::OK);
  // A 0.9.0 image (no marker) is a Rewired-S image of unknown kind: still a target.
  CHECK(check_fallback_target(slot(true, "rewired-s", "0.9.0"), "rewired-s", false) == FallbackTarget::OK);
  // Another Ethernet build would be just as unreachable.
  CHECK(check_fallback_target(slot(true, "rewired-s", "0.9.1+ethernet"), "rewired-s", false) ==
        FallbackTarget::OTHER_IS_ETHERNET);
  // The stock app: only when asked for.
  const SlotInfo stock = slot(true, "winet", "WINET-SV300.001.03.P033B001-dir");
  CHECK(check_fallback_target(stock, "rewired-s", false) == FallbackTarget::OTHER_IS_STOCK);
  CHECK(check_fallback_target(stock, "rewired-s", true) == FallbackTarget::OK);
  // Anything else is never booted, allow_stock or not.
  CHECK(check_fallback_target(slot(true, "other", "1.0"), "rewired-s", true) == FallbackTarget::OTHER_IS_FOREIGN);
  // A compiled entry has its own project name (the node name): same rule, by equality.
  CHECK(check_fallback_target(slot(true, "winet-s2", "0.9.1+wifi"), "winet-s2-eth", false) ==
        FallbackTarget::OTHER_IS_FOREIGN);
  CHECK(check_fallback_target(slot(true, "winet-s2-eth", "0.9.1+wifi"), "winet-s2-eth", false) == FallbackTarget::OK);
  CHECK(check_fallback_target(slot(true, "", "0.9.1+wifi"), "", false) == FallbackTarget::OTHER_IS_FOREIGN);
  CHECK(contains(fallback_target_text(FallbackTarget::OTHER_IS_STOCK), "stock"));
}

static void test_record() {
  NetFallbackPref p{};
  // A fresh record (NVS empty) or another build's record arms the fallback.
  CHECK(net_fallback_begin(&p, 0x1234));
  CHECK(p.build_id == 0x1234 && net_fallback_armed(p));
  p.connected_once = 1;
  CHECK(!net_fallback_begin(&p, 0x1234));  // same build: the connected mark stays
  CHECK(!net_fallback_armed(p));
  CHECK(net_fallback_begin(&p, 0x5678));  // a new OTA: armed again
  CHECK(p.build_id == 0x5678 && net_fallback_armed(p));
}

static void test_timing() {
  NetFallbackConfig c;
  c.timeout_ms = 180000;
  c.phy_failed_timeout_ms = 60000;
  NetFallbackPref p{};
  net_fallback_begin(&p, 1);
  // No link yet: nothing until the timeout.
  CHECK(net_fallback_step(c, &p, false, false, 1000) == NetFallbackAction::NONE);
  CHECK(net_fallback_step(c, &p, false, false, 179999) == NetFallbackAction::NONE);
  CHECK(net_fallback_step(c, &p, false, false, 180000) == NetFallbackAction::FALL_BACK);
  // A failed driver falls back sooner.
  CHECK(net_fallback_step(c, &p, false, true, 59999) == NetFallbackAction::NONE);
  CHECK(net_fallback_step(c, &p, false, true, 60000) == NetFallbackAction::FALL_BACK);
  // The link comes up: the record is marked and the fallback is off for good (for this build).
  CHECK(net_fallback_step(c, &p, true, false, 20000) == NetFallbackAction::CONNECTED);
  CHECK(!net_fallback_armed(p) && p.connected_once == 1);
  CHECK(net_fallback_step(c, &p, false, false, 400000) == NetFallbackAction::NONE);  // a later outage
  CHECK(net_fallback_step(c, &p, false, true, 400000) == NetFallbackAction::NONE);
  // Status lines.
  char buf[160];
  NetFallbackPref q{};
  net_fallback_begin(&q, 1);
  net_fallback_status(c, q, false, false, 60000, FallbackTarget::OK, buf, sizeof(buf));
  CHECK(std::string(buf) == "armed: boots the other slot in 120 s unless the network comes up");
  net_fallback_status(c, q, false, true, 1000, FallbackTarget::OK, buf, sizeof(buf));
  CHECK(contains(buf, "in 59 s") && contains(buf, "driver failed"));
  net_fallback_status(c, q, false, false, 1000, FallbackTarget::OTHER_IS_STOCK, buf, sizeof(buf));
  CHECK(contains(buf, "no target") && contains(buf, "stock"));
  net_fallback_status(c, q, true, false, 1000, FallbackTarget::OK, buf, sizeof(buf));
  CHECK(std::string(buf) == "disarmed: connected");
  net_fallback_status(c, p, false, false, 1000, FallbackTarget::OK, buf, sizeof(buf));
  CHECK(contains(buf, "connected before"));
}

int main() {
  test_slot_kind();
  test_target();
  test_record();
  test_timing();
  std::printf("%d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
