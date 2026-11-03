// READ-ONLY flash tools of the debug build. Only flash READ APIs are used in this file (see
// flash_tools.h; enforced by tests/test_debug_readonly.py). Never add a write/erase call here.
#include "flash_tools.h"

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>

#include "esphome/core/log.h"

#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>

#ifdef USE_SUNGROW_DEBUG_DUMP
#include <esp_http_server.h>
#endif

namespace esphome {
namespace sungrow_debug {

static const char *const TAG = "sgdbg.flash";
static constexpr size_t CHUNK = 4096;

static std::string sfmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
static std::string sfmt(const char *f, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof(buf), f, ap);
  va_end(ap);
  return buf;
}

static bool read_internal(uint32_t addr, void *buf, size_t len) {
  return esp_flash_read(esp_flash_default_chip, buf, addr, len) == ESP_OK;
}

std::vector<PartInfo> list_partitions() {
  std::vector<PartInfo> out;
  const esp_partition_t *run = esp_ota_get_running_partition();
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  for (; it != nullptr; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    PartInfo pi;
    pi.label = p->label;
    pi.type = static_cast<uint8_t>(p->type);
    pi.subtype = static_cast<uint8_t>(p->subtype);
    pi.offset = p->address;
    pi.size = p->size;
    pi.external = p->flash_chip != esp_flash_default_chip;
    pi.running = run != nullptr && p->address == run->address && !pi.external;
    out.push_back(pi);
  }
  esp_partition_iterator_release(it);
  return out;
}

std::vector<Region> device_regions() {
  uint32_t fs = 0;
  esp_flash_get_physical_size(esp_flash_default_chip, &fs);
#ifdef CONFIG_IDF_TARGET_ESP32
  return plan_regions(list_partitions(), fs, BOOTLOADER_OFFSET_ESP32);
#else
  return plan_regions(list_partitions(), fs, 0x0);
#endif
}

std::vector<std::string> flash_report_lines() {
  std::vector<std::string> L;
  uint32_t id = 0, phys = 0, hdr = 0;
  esp_flash_read_id(esp_flash_default_chip, &id);
  esp_flash_get_physical_size(esp_flash_default_chip, &phys);
  esp_flash_get_size(esp_flash_default_chip, &hdr);
  L.push_back(sfmt("flash_id: JEDEC 0x%06" PRIX32 " (manufacturer 0x%02X, device 0x%04X), physical %" PRIu32
                   " MB, usable (from app header) %" PRIu32 " MB%s",
                   id, (unsigned) (id >> 16) & 0xFF, (unsigned) (id & 0xFFFF), phys >> 20, hdr >> 20,
                   phys != hdr ? "  <-- MISMATCH: build with the real flash_size" : ""));

  // Bootloader header.
#ifdef CONFIG_IDF_TARGET_ESP32
  const uint32_t boot_off = BOOTLOADER_OFFSET_ESP32;
#else
  const uint32_t boot_off = 0x0;
#endif
  uint8_t b[IMAGE_HEADER_LEN + SEGMENT_HEADER_LEN + 80];
  if (read_internal(boot_off, b, sizeof(b))) {
    ImageHeader h = parse_image_header(b, sizeof(b));
    L.push_back(sfmt("bootloader @0x%" PRIX32 ": ", boot_off) + describe_image_header(h));
    if (h.ok) {
      uint32_t off = boot_off + IMAGE_HEADER_LEN;
      for (uint8_t s = 0; s < h.segments && s < 16; s++) {
        uint8_t sh[8];
        if (!read_internal(off, sh, 8))
          break;
        uint32_t load, len;
        std::memcpy(&load, sh, 4);
        std::memcpy(&len, sh + 4, 4);
        L.push_back(sfmt("  segment %u: load 0x%08" PRIX32 " len 0x%" PRIX32 " (file +0x%" PRIX32 ")", s, load, len,
                         off - boot_off));
        if (len > 0x100000)
          break;
        off += 8 + len;
      }
      BootloaderDesc d = parse_bootloader_desc(b + IMAGE_HEADER_LEN + SEGMENT_HEADER_LEN, 80);
      if (d.ok)
        L.push_back(sfmt("  bootloader desc: version %" PRIu32 ", secure_version %u, IDF '%s', built '%s'", d.version,
                         d.secure_version, d.idf_ver.c_str(), d.date_time.c_str()));
      else
        L.push_back("  bootloader desc: none (IDF < 5.2 bootloader, or a non-IDF bootloader)");
    }
  } else {
    L.push_back("bootloader: read failed");
  }

  // Raw partition table @0x8000.
  std::unique_ptr<uint8_t[]> pt(new uint8_t[PTABLE_MAX_LEN]);
  if (read_internal(PTABLE_OFFSET, pt.get(), PTABLE_MAX_LEN)) {
    PartTable t = parse_partition_table(pt.get(), PTABLE_MAX_LEN);
    L.push_back(sfmt("partition table @0x8000: %u entries, MD5 %s", (unsigned) t.entries.size(),
                     t.md5_present ? (t.md5_ok ? "OK" : "MISMATCH") : "absent"));
    for (auto &l : partition_csv(t))
      L.push_back("  " + l);
    for (auto &n : check_partition_table(t, phys))
      L.push_back("  WARNING: " + n);
  } else {
    L.push_back("partition table: read failed");
  }

  // esp_partition view (includes the runtime-registered ext flash partition).
  for (const auto &p : list_partitions()) {
    std::string extra;
    if (p.type == 0 && !p.external) {
      uint8_t ah[IMAGE_HEADER_LEN + SEGMENT_HEADER_LEN + 0x90];
      if (read_internal(p.offset, ah, sizeof(ah))) {
        ImageHeader ih = parse_image_header(ah, sizeof(ah));
        AppDesc ad = parse_app_desc(ah + IMAGE_HEADER_LEN + SEGMENT_HEADER_LEN, 0x90);
        if (ih.ok)
          extra = sfmt(" | header %s %s %s", flash_mode_str(ih.spi_mode), flash_freq_str(ih.spi_speed),
                       flash_size_str(ih.spi_size));
        else
          extra = " | no app image";
        if (ad.ok)
          extra += sfmt(" | app '%s' %s (%s %s, IDF %s)", ad.project_name.c_str(), ad.version.c_str(), ad.date.c_str(),
                        ad.time.c_str(), ad.idf_ver.c_str());
      }
    }
    L.push_back(sfmt("  esp_partition %-12s type %u sub 0x%02X @0x%06" PRIX32 " size 0x%06" PRIX32 "%s%s", p.label.c_str(),
                     p.type, p.subtype, p.offset, p.size, p.external ? " [EXT FLASH]" : "",
                     p.running ? " <running>" : "") +
                extra);
  }

  // otadata.
  const esp_partition_t *od = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
  if (od != nullptr && od->size >= 0x2000) {
    std::unique_ptr<uint8_t[]> buf(new uint8_t[0x2000]);
    if (esp_partition_read(od, 0, buf.get(), 0x2000) == ESP_OK) {
      OtaData o = parse_otadata(buf.get(), 0x2000, esp_ota_get_app_partition_count());
      L.push_back(sfmt("otadata @0x%" PRIX32 ": ", od->address) + describe_otadata(o));
    }
  } else {
    L.push_back("otadata: no partition (boots factory/ota_0)");
  }
  return L;
}

#ifdef USE_SUNGROW_DEBUG_DUMP
// ---- HTTP dump handler --------------------------------------------------------------------------
static std::atomic<bool> g_dump_busy{false};

/// Reader for one region: internal flash (esp_flash_read) or an external partition
/// (esp_partition_read on the registered read-only partition).
struct RegionReader {
  const esp_partition_t *ext{nullptr};
  uint32_t base{0};
  bool read(uint32_t off, void *buf, size_t n) const {
    if (this->ext != nullptr)
      return esp_partition_read(this->ext, off, buf, n) == ESP_OK;
    return esp_flash_read(esp_flash_default_chip, buf, this->base + off, n) == ESP_OK;
  }
};

static bool make_reader(const Region &r, RegionReader *out) {
  out->ext = nullptr;
  out->base = r.offset;
  if (!r.external)
    return true;
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, r.label.c_str());
  for (; it != nullptr; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    if (p->flash_chip != esp_flash_default_chip) {
      out->ext = p;
      break;
    }
  }
  esp_partition_iterator_release(it);
  return out->ext != nullptr;
}

/// SHA-256 of [0, size) of a region, CHUNK at a time, yielding to other tasks.
static bool region_sha256(const RegionReader &rd, uint32_t size, uint8_t *buf, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  bool ok = true;
  uint32_t n = 0;
  for (uint32_t off = 0; off < size; off += CHUNK) {
    const size_t k = size - off < CHUNK ? size - off : CHUNK;
    if (!rd.read(off, buf, k)) {
      ok = false;
      break;
    }
    mbedtls_sha256_update(&ctx, buf, k);
    if ((++n & 15) == 0)
      vTaskDelay(1);  // let IDLE run (task watchdog) and the main loop keep polling RS485
  }
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
  return ok;
}

static std::string hex32(const uint8_t d[32]) {
  static const char *h = "0123456789abcdef";
  std::string s;
  for (int i = 0; i < 32; i++) {
    s += h[d[i] >> 4];
    s += h[d[i] & 15];
  }
  return s;
}

static bool send_all(httpd_req_t *req, const char *d, size_t n) {
  int retries = 0;
  while (n > 0) {
    int k = httpd_send(req, d, n);
    if (k == HTTPD_SOCK_ERR_TIMEOUT && ++retries < 20) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (k <= 0)
      return false;
    retries = 0;
    d += k;
    n -= static_cast<size_t>(k);
  }
  return true;
}

static void send_text(AsyncWebServerRequest *request, int code, const char *type, const std::string &body) {
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, code == 200 ? "200 OK" : code == 400 ? "400 Bad Request" : code == 404 ? "404 Not Found"
                                                     : code == 503 ? "503 Service Unavailable"
                                                                   : "500 Internal Server Error");
  httpd_resp_set_type(req, type);
  httpd_resp_send(req, body.data(), body.size());
}

static std::string json_escape(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      o += ' ';
    } else {
      o += c;
    }
  }
  return o;
}

static void stream_region(AsyncWebServerRequest *request, const Region &r, uint8_t *buf) {
  RegionReader rd;
  if (!make_reader(r, &rd)) {
    send_text(request, 500, "text/plain", "external partition not found\n");
    return;
  }
  uint8_t sha[32];
  const uint32_t t0 = static_cast<uint32_t>(xTaskGetTickCount());
  if (!region_sha256(rd, r.size, buf, sha)) {
    send_text(request, 500, "text/plain", "flash read failed\n");
    return;
  }
  ESP_LOGI(TAG, "dump %s @0x%" PRIX32 " size 0x%" PRIX32 "%s sha256 %s", r.name.c_str(), r.offset, r.size,
           r.external ? " (ext)" : "", hex32(sha).c_str());
  httpd_req_t *req = *request;
  std::string h = sfmt("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %" PRIu32
                       "\r\nContent-Disposition: attachment; filename=\"%s.bin\"\r\n",
                       r.size, r.name.c_str());
  h += sfmt("X-Region: %s\r\nX-Region-Label: %s\r\nX-Region-Offset: 0x%" PRIX32 "\r\nX-Region-Size: 0x%" PRIX32
            "\r\nX-Flash: %s\r\n",
            r.name.c_str(), r.label.c_str(), r.offset, r.size, r.external ? "external" : "internal");
  h += "X-SHA256: " + hex32(sha) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  if (!send_all(req, h.data(), h.size()))
    return;
  uint32_t n = 0;
  for (uint32_t off = 0; off < r.size; off += CHUNK) {
    const size_t k = r.size - off < CHUNK ? r.size - off : CHUNK;
    if (!rd.read(off, buf, k) || !send_all(req, reinterpret_cast<const char *>(buf), k)) {
      ESP_LOGW(TAG, "dump %s aborted at +0x%" PRIX32, r.name.c_str(), off);
      return;
    }
    if ((++n & 15) == 0)
      vTaskDelay(1);
  }
  ESP_LOGI(TAG, "dump %s done in %" PRIu32 " ms", r.name.c_str(),
           static_cast<uint32_t>((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));
}

bool DumpHandler::canHandle(AsyncWebServerRequest *request) const {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(buf);
  return request->method() == HTTP_GET && url.size() >= 5 && std::strncmp(url.c_str(), "/dump", 5) == 0;
}

void DumpHandler::handleRequest(AsyncWebServerRequest *request) {
  char ubuf[AsyncWebServerRequest::URL_BUF_SIZE];
  std::string url = request->url_to(ubuf).c_str();
  std::string name = url.size() > 6 ? url.substr(6) : "index";  // "/dump/<name>"
  if (name.empty() || url == "/dump")
    name = "index";
  bool expected = false;
  if (!g_dump_busy.compare_exchange_strong(expected, true)) {
    send_text(request, 503, "text/plain", "another dump is running\n");
    return;
  }
  uint8_t *buf = static_cast<uint8_t *>(heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (buf == nullptr) {
    g_dump_busy = false;
    send_text(request, 503, "text/plain", "out of memory\n");
    return;
  }
  const std::vector<Region> regions = device_regions();
  uint32_t fs = 0;
  esp_flash_get_physical_size(esp_flash_default_chip, &fs);
  if (name == "index") {
    const bool with_sha = request->arg("sha") != "0";
    uint32_t id = 0;
    esp_flash_read_id(esp_flash_default_chip, &id);
    const esp_partition_t *run = esp_ota_get_running_partition();
    std::string j = sfmt("{\"flash_size\":%" PRIu32 ",\"flash_id\":\"0x%06" PRIX32 "\",\"running\":\"%s\",\"running_offset\":%" PRIu32
                         ",\"regions\":[",
                         fs, id, run ? run->label : "", run ? run->address : 0);
    for (size_t i = 0; i < regions.size(); i++) {
      const Region &r = regions[i];
      std::string sha;
      if (with_sha && r.name != "flash") {
        RegionReader rd;
        uint8_t d[32];
        if (make_reader(r, &rd) && region_sha256(rd, r.size, buf, d))
          sha = hex32(d);
      }
      j += sfmt("%s{\"name\":\"%s\",\"label\":\"%s\",\"offset\":%" PRIu32 ",\"size\":%" PRIu32 ",\"external\":%s,",
                i ? "," : "", r.name.c_str(), json_escape(r.label).c_str(), r.offset, r.size,
                r.external ? "true" : "false");
      j += "\"sha256\":\"" + sha + "\",\"note\":\"" + json_escape(r.note) + "\"}";
    }
    j += "]}\n";
    send_text(request, 200, "application/json", j);
  } else if (name == "raw") {
    uint32_t off = 0, len = 0;
    if (!parse_u32(request->arg("offset"), &off) || !parse_u32(request->arg("len"), &len) ||
        !validate_raw_range(off, len, fs)) {
      send_text(request, 400, "text/plain", "need offset=&len= inside the internal flash\n");
    } else {
      Region r{sfmt("raw_%06" PRIx32 "_%06" PRIx32, off, len), "", off, len, false, "raw range"};
      stream_region(request, r, buf);
    }
  } else if (name == "ptable.csv") {
    std::string out;
    if (esp_flash_read(esp_flash_default_chip, buf, PTABLE_OFFSET, PTABLE_MAX_LEN) == ESP_OK)
      for (const auto &l : partition_csv(parse_partition_table(buf, PTABLE_MAX_LEN)))
        out += l + "\n";
    send_text(request, 200, "text/plain", out);
  } else if (name == "report") {
    std::string out;
    for (const auto &l : flash_report_lines())
      out += l + "\n";
    send_text(request, 200, "text/plain", out);
  } else {
    const Region *r = find_region(regions, name);
    if (r == nullptr)
      send_text(request, 404, "text/plain", "unknown region; see /dump/index\n");
    else
      stream_region(request, *r, buf);
  }
  heap_caps_free(buf);
  g_dump_busy = false;
}
#endif

}  // namespace sungrow_debug
}  // namespace esphome
