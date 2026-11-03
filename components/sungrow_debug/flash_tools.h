#pragma once
// sungrow_debug flash tools (ESP-IDF side): flash layout report and the /dump/<region> HTTP
// download handler. READ-ONLY BY CONSTRUCTION: this file and flash_tools.cpp only call flash /
// partition READ APIs (esp_flash_read, esp_flash_read_id, esp_flash_get_*size,
// esp_partition_find*/get/next/read, esp_ota_get_*). tests/test_debug_readonly.py fails the build
// if a write/erase/OTA-write symbol appears here. The one flash write of the debug build (switching
// the boot partition back to the stock slot) lives in boot_switch.cpp, behind a confirm switch.
#include <string>
#include <vector>

#include "esphome/core/defines.h"
#include "flash_layout.h"

#ifdef USE_SUNGROW_DEBUG_DUMP
#include "esphome/components/web_server_base/web_server_base.h"
#endif

namespace esphome {
namespace sungrow_debug {

/// Partition table @0x8000 (raw + MD5 check, gen_esp32part CSV), esp_partition listing, bootloader
/// header @0x1000 (+ bootloader desc), running app header, otadata entries, flash id / sizes.
std::vector<std::string> flash_report_lines();
/// Every partition the app can see (internal table + runtime-registered external ones).
std::vector<PartInfo> list_partitions();
/// Region plan of this device (plan_regions() over list_partitions()).
std::vector<Region> device_regions();

#ifdef USE_SUNGROW_DEBUG_DUMP
/// GET /dump/index           JSON: regions with offset, size, sha256 (?sha=0 skips hashing)
/// GET /dump/<region>        raw bytes, Content-Length + X-SHA256 (computed before the body)
/// GET /dump/raw?offset=&len= internal flash range
/// Registered with web_server_base::add_handler(), i.e. behind the web server's auth.
class DumpHandler : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  bool isRequestHandlerTrivial() const override { return false; }
};
#endif

}  // namespace sungrow_debug
}  // namespace esphome
