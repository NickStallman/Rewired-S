// Host tests for components/sungrow_board/boot_slot.h: the one-shot arming guard, the switch
// verdict and the slots JSON. Build + run: `make -C tests/board`.
#include <cstdio>
#include <cstring>
#include <string>

#include "boot_slot.h"

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

static SlotInfo slot(const char *label, uint32_t addr, bool valid, const char *project = "", const char *version = "") {
  SlotInfo s;
  s.present = true;
  std::snprintf(s.label, sizeof(s.label), "%s", label);
  s.address = addr;
  s.valid_image = valid;
  std::snprintf(s.project, sizeof(s.project), "%s", project);
  std::snprintf(s.version, sizeof(s.version), "%s", version);
  return s;
}

static void test_guard_is_one_shot() {
  BootSwitchGuard g;
  CHECK(!g.armed());
  CHECK(!g.take());  // never armed: refused
  g.arm(true);
  CHECK(g.armed());
  CHECK(g.take());   // the one permitted attempt
  CHECK(!g.armed());  // disarmed by the attempt
  CHECK(!g.take());  // a second press is refused
  g.arm(true);
  g.arm(false);  // the switch turned off again before a press
  CHECK(!g.take());
}

static void test_verdicts() {
  const SlotInfo stock = slot("ota_1", 0x610000, true, "winet", "WINET-SV200.001.00.P046B001-dir");
  const SlotInfo blank = slot("ota_1", 0x610000, false);
  SlotInfo none;
  CHECK(check_boot_switch(false, stock) == BootSwitchVerdict::NOT_ARMED);
  CHECK(check_boot_switch(true, none) == BootSwitchVerdict::NO_OTHER_SLOT);
  CHECK(check_boot_switch(true, blank) == BootSwitchVerdict::NO_VALID_IMAGE);
  CHECK(check_boot_switch(true, stock) == BootSwitchVerdict::OK);
  // The arming check comes first: an unarmed request never reveals the slot state.
  CHECK(check_boot_switch(false, none) == BootSwitchVerdict::NOT_ARMED);
  CHECK(std::string(boot_switch_verdict_code(BootSwitchVerdict::NO_VALID_IMAGE)) == "NO_VALID_IMAGE");
  CHECK(std::string(boot_switch_verdict_code(BootSwitchVerdict::SET_FAILED)) == "SET_FAILED");
  CHECK(contains(boot_switch_verdict_text(BootSwitchVerdict::NOT_ARMED), "Allow boot switch"));
}

static void test_slots_json() {
  const SlotInfo run = slot("ota_0", 0x110000, true, "rewired-s", "2026.9.1");
  const SlotInfo stock = slot("ota_1", 0x610000, true, "winet", "WINET-SV200.001.00.P046B001-dir");
  std::string j = boot_slots_json(run, stock, false);
  CHECK(contains(j, "\"running\":{\"label\":\"ota_0\",\"address\":1114112,\"valid\":true,\"project\":\"rewired-s\""));
  CHECK(contains(j, "\"other\":{\"label\":\"ota_1\",\"address\":6356992,\"valid\":true"));
  CHECK(contains(j, "\"version\":\"WINET-SV200.001.00.P046B001-dir\""));
  CHECK(contains(j, "\"armed\":false"));
  CHECK(contains(j, "\"can_switch\":true"));
  SlotInfo none;
  j = boot_slots_json(run, none, true);
  CHECK(contains(j, "\"other\":null"));
  CHECK(contains(j, "\"armed\":true"));
  CHECK(contains(j, "\"can_switch\":false"));
  const SlotInfo blank = slot("ota_1", 0x610000, false);
  j = boot_slots_json(run, blank, false);
  CHECK(contains(j, "\"valid\":false,\"project\":\"\",\"version\":\"\""));
  CHECK(contains(j, "\"can_switch\":false"));
  // Descriptor text is escaped / sanitised.
  SlotInfo odd = slot("ota_1", 0x610000, true, "a\"b\\c", "v\x01");
  j = boot_slots_json(run, odd, false);
  CHECK(contains(j, "\"project\":\"a\\\"b\\\\c\""));
  CHECK(contains(j, "\"version\":\"v?\""));
}

int main() {
  test_guard_is_one_shot();
  test_verdicts();
  test_slots_json();
  std::printf("%d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
