#include "diag.h"

#ifdef USE_SUNGROW_DIAG

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_debug_helpers.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <esp_private/cache_utils.h>
#include <esp_private/startup_internal.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <xtensa_context.h>

#ifdef USE_SUNGROW_COREDUMP
#include <esp_core_dump.h>
#include <esp_flash.h>
#endif

#include "esphome/core/build_info_data.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_LOGGER
#include "esphome/components/logger/logger.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_SUNGROW_DIAG_WEB
#include <esp_http_server.h>
#include "esphome/components/web_server_base/web_server_base.h"
#endif

#include "diag_journal.h"
#include "sungrow_board.h"

// The task currently running on each core (FreeRTOS kernel global). Read directly in the panic
// and stall paths: the FreeRTOS accessors live in flash, which may be unreachable there.
extern "C" void *volatile pxCurrentTCBs[];
// Set by IDF before an abort()- or task-watchdog-initiated panic (panic.c, task_wdt.c).
extern "C" bool g_panic_abort __attribute__((weak));
extern "C" char *g_panic_abort_details __attribute__((weak));
extern "C" bool g_twdt_isr __attribute__((weak));

namespace esphome::sungrow_board {

static const char *const TAG = "diag";

namespace {

using namespace diag;

#ifndef SUNGROW_DIAG_RING_SIZE
#define SUNGROW_DIAG_RING_SIZE 6144
#endif
static constexpr size_t RING_SIZE = SUNGROW_DIAG_RING_SIZE;

// RTC slow memory: one object, so the journal is always the first .rtc_noinit data (at the
// segment base, 0x50000000 on the ESP32-S3) and builds with the same layout read each other's
// record. Nothing else in these builds uses RTC_NOINIT/RTC_DATA; check with
//   nm firmware.elf | grep sg_diag_rtc        (expected: 50000000)
struct DiagRtc {
  Journal journal;
  RingHeader ring_hdr;
  char ring[RING_SIZE];
};
RTC_NOINIT_ATTR DiagRtc sg_diag_rtc;

// Previous boot, captured before this boot's journal starts.
DRAM_ATTR Journal s_prev;
bool s_prev_valid = false;
int s_reason = 0;
std::string s_last_reset;
char *s_prev_log = nullptr;  // previous boot's log tail (PSRAM when available)
size_t s_prev_log_len = 0;
bool s_prev_ring_valid = false;
char s_build[BUILD_ID_LEN];
uint8_t s_log_level = 5;
bool s_capture = false;
portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
#ifdef USE_SUNGROW_COREDUMP
std::string s_coredump;  // summary of the stored core dump this boot reports (empty: none)
#endif

// Kick-stall detector state (ISR).
volatile uint32_t s_last_kick_ms = 0;
volatile bool s_stall_active = false;
esp_timer_handle_t s_stall_timer = nullptr;
esp_timer_handle_t s_hb_timer = nullptr;
uint32_t s_last_wifi_ms = 0;

inline uint32_t IRAM_ATTR uptime_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Task name of the TCB running on `core`; "?" when the TCB is not readable right now.
void IRAM_ATTR copy_task_name(int core, char *out, bool cache_on) {
  out[0] = '?';
  out[1] = '\0';
  const uint8_t *tcb = static_cast<const uint8_t *>(pxCurrentTCBs[core]);
  if (tcb == nullptr || !(esp_ptr_internal(tcb) || cache_on))
    return;
  const uint8_t *name = tcb + offsetof(StaticTask_t, ucDummy7);
  size_t i = 0;
  for (; i < TASK_NAME_LEN - 1 && i < configMAX_TASK_NAME_LEN && name[i] != 0; i++)
    out[i] = static_cast<char>(name[i]);
  out[i] = '\0';
}

// Xtensa return address -> call site (top bits carry the window size).
inline uint32_t IRAM_ATTR stack_pc(uint32_t pc) {
  if (pc & 0x80000000u)
    pc = (pc & 0x3fffffffu) | 0x40000000u;
  return pc - 3;
}

// Panic entry, before IDF's handler (which re-enables the flash cache, prints, writes the core
// dump and restarts). Register writes and IRAM/DRAM/RTC data only. The crash record is written
// once per boot (the first core to panic).
void IRAM_ATTR panic_entry(void *frame, bool pseudo) {
#ifdef USE_SUNGROW_WDOG
  watchdog_panic_off();
#endif
  Journal &j = sg_diag_rtc.journal;
  if (j.magic != JOURNAL_MAGIC || j.crash.magic == CRASH_MAGIC)
    return;
  CrashRecord &c = j.crash;
  const bool cache_on = spi_flash_cache_enabled();
  c.at_ms = uptime_ms();
  c.core = static_cast<uint8_t>(esp_cpu_get_core_id());
  if (&g_twdt_isr != nullptr && g_twdt_isr)
    c.kind = CRASH_TASK_WDT;
  else if (&g_panic_abort != nullptr && g_panic_abort)
    c.kind = CRASH_ABORT;
  else
    c.kind = pseudo ? CRASH_PSEUDO : CRASH_EXCEPTION;
  copy_task_name(c.core, c.task, cache_on);
  c.detail[0] = '\0';
  if (c.kind == CRASH_ABORT && &g_panic_abort_details != nullptr && g_panic_abort_details != nullptr &&
      (cache_on || esp_ptr_internal(g_panic_abort_details))) {
    size_t i = 0;
    for (; i < DETAIL_LEN - 1 && g_panic_abort_details[i] != '\0'; i++)
      c.detail[i] = g_panic_abort_details[i];
    c.detail[i] = '\0';
  }
  c.depth = 0;
  const auto *f = static_cast<const XtExcFrame *>(frame);
  if (f != nullptr && (esp_ptr_internal(f) || cache_on)) {
    c.exccause = static_cast<uint32_t>(f->exccause);
    c.pc = static_cast<uint32_t>(f->pc);
    c.excvaddr = static_cast<uint32_t>(f->excvaddr);
    esp_backtrace_frame_t fr{};
    fr.pc = static_cast<uint32_t>(f->pc);
    fr.sp = static_cast<uint32_t>(f->a1);
    fr.next_pc = static_cast<uint32_t>(f->a0);
    fr.exc_frame = f;
    c.bt[c.depth++] = fr.pc;
    while (c.depth < BT_DEPTH && fr.next_pc != 0) {
      if (!esp_stack_ptr_is_sane(fr.sp) || !(esp_ptr_internal(reinterpret_cast<void *>(fr.sp)) || cache_on))
        break;
      if (!esp_backtrace_get_next_frame(&fr))
        break;
      c.bt[c.depth++] = stack_pc(fr.pc);
    }
  }
  c.magic = CRASH_MAGIC;
}

#ifdef USE_SUNGROW_WDOG
// Kick-stall detector (esp_timer ISR dispatch, 200 ms): records, never kicks and never panics.
void IRAM_ATTR stall_check(void * /*arg*/) {
  const uint32_t last = s_last_kick_ms;
  if (last == 0)
    return;
  const uint32_t now = uptime_ms();
  const uint32_t gap = now - last;
  StallRecord &s = sg_diag_rtc.journal.stall;
  if (gap <= STALL_THRESHOLD_MS) {
    s_stall_active = false;
    return;
  }
  if (!s_stall_active) {
    s_stall_active = true;
    const bool cache_on = spi_flash_cache_enabled();
    s.count++;
    s.at_ms = now;
    s.cache_off = cache_on ? 0 : 1;
    copy_task_name(0, s.task[0], cache_on);
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    copy_task_name(1, s.task[1], cache_on);
#else
    s.task[1][0] = '\0';
#endif
    s.magic = STALL_MAGIC;
  }
  s.gap_ms = gap;
}
#endif  // USE_SUNGROW_WDOG

void heartbeat_common(uint32_t now_ms) {
  Heartbeat &h = sg_diag_rtc.journal.hb;
  h.uptime_ms = now_ms;
  h.heap_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  h.heap_min = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  h.psram_total = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
  h.psram_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// Boards without the supervisor kick: the same heartbeat from a plain timer.
void heartbeat_timer_cb(void * /*arg*/) { heartbeat_common(uptime_ms()); }

void log_callback(void * /*self*/, uint8_t level, const char * /*tag*/, const char *message, size_t len) {
  if (!s_capture || level > s_log_level)
    return;
  char line[240];
  const uint32_t t = millis();
  int n = std::snprintf(line, sizeof(line), "%5u.%03u ", static_cast<unsigned>(t / 1000), static_cast<unsigned>(t % 1000));
  size_t k = len < sizeof(line) - n - 1 ? len : sizeof(line) - n - 1;
  std::memcpy(line + n, message, k);
  portENTER_CRITICAL(&s_ring_mux);
  ring_append_line(sg_diag_rtc.ring_hdr, sg_diag_rtc.ring, RING_SIZE, line, n + k);
  portEXIT_CRITICAL(&s_ring_mux);
}

#ifdef USE_SUNGROW_COREDUMP
// Erases the core dump partition (1 MB, seconds of flash erase) off the main loop, so the loop's
// task watchdog keeps running.
void coredump_erase_task(void * /*arg*/) {
  const esp_err_t err = esp_core_dump_image_erase();
  if (err == ESP_OK)
    ESP_LOGI(TAG, "Core dump erased");
  else
    ESP_LOGW(TAG, "Core dump erase failed: %s", esp_err_to_name(err));
  vTaskDelete(nullptr);
}

// A stored core dump is reported by one boot only: the first boot that finds it puts its summary
// in "Last reset" and /api/diag, serves the image at /api/coredump and notes it in its journal; the
// next boot finds the note and erases the dump (coredump_decide in diag_journal.h).
void read_coredump() {
  if (esp_core_dump_image_check() != ESP_OK)
    return;
  size_t addr = 0, size = 0;
  if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size < sizeof(uint32_t))
    return;
  uint32_t checksum = 0;
  if (esp_flash_read(nullptr, &checksum, addr + size - sizeof(checksum), sizeof(checksum)) != ESP_OK)
    checksum = 0;
  auto *sum = static_cast<esp_core_dump_summary_t *>(heap_caps_malloc(sizeof(esp_core_dump_summary_t), MALLOC_CAP_8BIT));
  const bool have_sum = sum != nullptr && esp_core_dump_get_summary(sum) == ESP_OK;
  const uint32_t fp = coredump_fingerprint(static_cast<uint32_t>(size), checksum, have_sum ? sum->exc_pc : 0);
  const CoredumpDecision d = coredump_decide(s_reason, s_prev_valid ? &s_prev : nullptr, fp);
  if (d.erase) {
    ESP_LOGI(TAG, "Core dump of %s was reported by the previous boot: erasing it", coredump_owner_text(d).c_str());
    if (xTaskCreate(coredump_erase_task, "sg_cd_erase", 3072, nullptr, 1, nullptr) != pdPASS)
      ESP_LOGW(TAG, "Core dump erase task not started");
  } else {
    sg_diag_rtc.journal.coredump.fingerprint = fp;
    sg_diag_rtc.journal.coredump.boot = d.boot;
    s_coredump = "Core dump of " + coredump_owner_text(d);
    if (have_sum) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), ": task %.16s, pc 0x%08" PRIX32 ", backtrace", sum->exc_task, sum->exc_pc);
      s_coredump += buf;
      for (uint32_t i = 0; i < sum->exc_bt_info.depth && i < 16; i++) {
        std::snprintf(buf, sizeof(buf), " 0x%08" PRIX32, sum->exc_bt_info.bt[i]);
        s_coredump += buf;
      }
      if (sum->exc_bt_info.corrupted)
        s_coredump += " (corrupted)";
    } else {
      s_coredump += " (no summary)";
    }
  }
  heap_caps_free(sum);
}
#endif

#ifdef USE_SUNGROW_DIAG_WEB
// GET /api/diag (text report) and /api/coredump (raw core dump image). Registered with
// add_handler, so the web server's login applies (the core dump holds RAM contents).
class DiagHandler : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() != HTTP_GET)
      return false;
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    StringRef url = request->url_to(buf);
    return std::strcmp(url.c_str(), "/api/diag") == 0
#ifdef USE_SUNGROW_COREDUMP
           || std::strcmp(url.c_str(), "/api/coredump") == 0
#endif
        ;
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    StringRef url = request->url_to(buf);
    httpd_req_t *req = *request;
    if (std::strcmp(url.c_str(), "/api/diag") == 0) {
      std::string body = diag_report();
      httpd_resp_set_status(req, "200 OK");
      httpd_resp_set_type(req, "text/plain; charset=utf-8");
      httpd_resp_set_hdr(req, "Cache-Control", "no-store");
      httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
      return;
    }
#ifdef USE_SUNGROW_COREDUMP
    size_t addr = 0, size = 0;
    if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
      httpd_resp_set_status(req, "404 Not Found");
      httpd_resp_set_type(req, "text/plain");
      httpd_resp_send(req, "no valid core dump\n", HTTPD_RESP_USE_STRLEN);
      return;
    }
    static constexpr size_t CHUNK = 2048;
    auto *chunk = static_cast<char *>(heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (chunk == nullptr) {
      httpd_resp_set_status(req, "503 Service Unavailable");
      httpd_resp_send(req, "out of memory\n", HTTPD_RESP_USE_STRLEN);
      return;
    }
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.elf\"");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    for (size_t off = 0; off < size; off += CHUNK) {
      const size_t n = size - off < CHUNK ? size - off : CHUNK;
      if (esp_flash_read(nullptr, chunk, addr + off, n) != ESP_OK || httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
        ESP_LOGW(TAG, "core dump download aborted at +0x%X", static_cast<unsigned>(off));
        break;
      }
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    heap_caps_free(chunk);
#endif
  }
};
#endif

}  // namespace

// ---- public ---------------------------------------------------------------------------------

void diag_on_kick(int64_t now_us, uint32_t kicks, uint32_t max_gap_ms, uint32_t long_gaps) {
  const uint32_t now_ms = static_cast<uint32_t>(now_us / 1000);
  s_last_kick_ms = now_ms;
  Heartbeat &h = sg_diag_rtc.journal.hb;
  h.kicks = kicks;
  h.max_kick_gap_ms = max_gap_ms;
  h.long_kick_gaps = long_gaps;
  heartbeat_common(now_ms);
}

const std::string &diag_last_reset() { return s_last_reset; }

uint32_t psram_size_bytes() { return static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)); }

std::string diag_report() {
  std::string s;
  char buf[160];
  std::snprintf(buf, sizeof(buf), "This boot: #%u, build %s, uptime %u s, reset reason %s, PSRAM %u KB\n",
                static_cast<unsigned>(sg_diag_rtc.journal.boot_count), s_build, static_cast<unsigned>(millis() / 1000),
                reset_reason_name(s_reason), static_cast<unsigned>(psram_size_bytes() / 1024));
  s += buf;
  s += journal_text(sg_diag_rtc.journal);
  s += "\nPrevious boot: " + s_last_reset + "\n";
  if (s_prev_valid)
    s += journal_text(s_prev);
  else
    s += "(no journal survived)\n";
#ifdef USE_SUNGROW_COREDUMP
  if (!s_coredump.empty())
    s += s_coredump + "\n";
#endif
  s += "\n--- log tail of the previous boot ---\n";
  if (s_prev_log != nullptr && s_prev_log_len > 0)
    s.append(s_prev_log, s_prev_log_len);
  else
    s += s_prev_ring_valid ? "(empty)\n" : "(none survived)\n";
  return s;
}

std::string diag_ring_text() {
  // Snapshot under the ring lock (the logger callback appends from any task), format outside it.
  auto *copy = static_cast<char *>(heap_caps_malloc(RING_SIZE, MALLOC_CAP_8BIT));
  if (copy == nullptr)
    return "(no memory for the log tail)\n";
  RingHeader hdr;
  portENTER_CRITICAL(&s_ring_mux);
  hdr = sg_diag_rtc.ring_hdr;
  std::memcpy(copy, sg_diag_rtc.ring, RING_SIZE);
  portEXIT_CRITICAL(&s_ring_mux);
  std::string t = ring_text(hdr, copy, RING_SIZE);
  heap_caps_free(copy);
  return t;
}

int diag_reset_reason() { return s_reason; }

const Journal *diag_previous_journal() { return s_prev_valid ? &s_prev : nullptr; }

bool diag_coredump_reported() {
#ifdef USE_SUNGROW_COREDUMP
  return !s_coredump.empty();
#else
  return false;
#endif
}

uint32_t diag_boot_count() { return sg_diag_rtc.journal.boot_count; }

const char *diag_build() { return s_build; }

void diag_setup(const DiagConfig &cfg) {
  s_log_level = cfg.log_level;
#ifdef USE_SUNGROW_COREDUMP
  read_coredump();
#endif
  s_last_reset = summarize(s_reason, s_prev_valid ? &s_prev : nullptr, s_build);
#ifdef USE_SUNGROW_COREDUMP
  if (!s_coredump.empty() && s_last_reset.size() < 200) {
    s_last_reset += "; ";
    s_last_reset += s_coredump.substr(0, 255 - s_last_reset.size());
  }
#endif
  // Previous log tail -> heap (PSRAM when present), then restart the ring for this boot.
  RingHeader &rh = sg_diag_rtc.ring_hdr;
  s_prev_ring_valid = ring_valid(rh, RING_SIZE);
  if (s_prev_ring_valid) {
    std::string t = ring_text(rh, sg_diag_rtc.ring, RING_SIZE);
    if (!t.empty()) {
      s_prev_log = static_cast<char *>(heap_caps_malloc(t.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (s_prev_log == nullptr)
        s_prev_log = static_cast<char *>(heap_caps_malloc(t.size(), MALLOC_CAP_8BIT));
      if (s_prev_log != nullptr) {
        std::memcpy(s_prev_log, t.data(), t.size());
        s_prev_log_len = t.size();
      }
    }
  }
  ring_begin(rh, RING_SIZE, sg_diag_rtc.journal.boot_count);

  ESP_LOGW(TAG, "Previous boot: %s", s_last_reset.c_str());
  if (psram_size_bytes() == 0)
    ESP_LOGW(TAG, "No PSRAM in the heap: running on internal RAM only");

#ifdef USE_LOGGER
  if (logger::global_logger != nullptr)
    logger::global_logger->add_log_callback(nullptr, log_callback);
#endif
  s_capture = true;

  if (!watchdog_running()) {
    esp_timer_create_args_t a{};
    a.callback = heartbeat_timer_cb;
    a.dispatch_method = ESP_TIMER_TASK;
    a.name = "sg_diag_hb";
    if (esp_timer_create(&a, &s_hb_timer) == ESP_OK)
      esp_timer_start_periodic(s_hb_timer, 500000);
  }

#ifdef USE_SUNGROW_DIAG_WEB
  if (web_server_base::global_web_server_base != nullptr)
    web_server_base::global_web_server_base->add_handler(new DiagHandler());  // NOLINT
#endif
}

void diag_loop() {
  const uint32_t now = millis();
  Heartbeat &h = sg_diag_rtc.journal.hb;
  h.loop_ms = now;
  if (now - s_last_wifi_ms < 1000)
    return;
  s_last_wifi_ms = now;
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr) {
    h.wifi_connected = wifi::global_wifi_component->is_connected() ? 1 : 0;
    h.wifi_rssi = h.wifi_connected ? static_cast<int8_t>(wifi::global_wifi_component->wifi_rssi()) : 0;
  }
#endif
}

void diag_dump_config() {
  ESP_LOGCONFIG(TAG,
                "Reset diagnostics:\n"
                "  Journal: RTC @%p (%u B), log tail %u B, level %u\n"
                "  This boot: #%u, build %s\n"
                "  PSRAM: %u KB (%u KB free), internal heap %u B free",
                static_cast<void *>(&sg_diag_rtc), static_cast<unsigned>(sizeof(Journal)), static_cast<unsigned>(RING_SIZE),
                s_log_level, static_cast<unsigned>(sg_diag_rtc.journal.boot_count), s_build,
                static_cast<unsigned>(psram_size_bytes() / 1024),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  // The previous-boot summary, its journal, the core dump note and the replayed log tail log at
  // INFO so they reach every log client at the default level (dump_config runs per API log
  // subscription); the previous log tail is replayed only after an abnormal end (not a software
  // restart, not a cold power-on).
  ESP_LOGI(TAG, "  Previous boot: %s", s_last_reset.c_str());
  if (s_prev_valid) {
    std::string j = journal_text(s_prev);
    size_t p = 0;
    while (p < j.size()) {
      size_t e = j.find('\n', p);
      if (e == std::string::npos)
        e = j.size();
      ESP_LOGI(TAG, "  | %.*s", static_cast<int>(e - p), j.c_str() + p);
      p = e + 1;
    }
  }
#ifdef USE_SUNGROW_COREDUMP
  if (!s_coredump.empty())
    ESP_LOGI(TAG, "  %s", s_coredump.c_str());
#endif
  const bool abnormal = s_reason != RR_SW && (s_prev_valid || s_reason != RR_POWERON);
  if (abnormal && s_prev_log != nullptr) {
    const bool was = s_capture;
    s_capture = false;  // don't copy the replay into this boot's tail
    ESP_LOGI(TAG, "  Log tail of the previous boot:");
    size_t p = 0;
    while (p < s_prev_log_len) {
      const char *nl = static_cast<const char *>(std::memchr(s_prev_log + p, '\n', s_prev_log_len - p));
      size_t e = nl != nullptr ? static_cast<size_t>(nl - s_prev_log) : s_prev_log_len;
      ESP_LOGI(TAG, "  > %.*s", static_cast<int>(e - p), s_prev_log + p);
      p = e + 1;
    }
    s_capture = was;
  }
}

}  // namespace esphome::sungrow_board

// ---- early init and panic entry ------------------------------------------------------------

// Before the supervisor kick timer is armed (104), which writes the heartbeat: keep the previous
// boot's journal, then start this boot's.
ESP_SYSTEM_INIT_FN(sungrow_diag_journal, SECONDARY, BIT(0), 103) {
  using namespace esphome::sungrow_board;
  using namespace esphome::sungrow_board::diag;
  s_reason = static_cast<int>(esp_reset_reason());
  Journal &j = sg_diag_rtc.journal;
  s_prev_valid = journal_valid(j);
  if (s_prev_valid)
    std::memcpy(&s_prev, &j, sizeof(Journal));
  std::snprintf(s_build, sizeof(s_build), "0x%08" PRIx32 " %s", esphome::ESPHOME_CONFIG_HASH, esphome::ESPHOME_BUILD_TIME_STR);
  journal_begin(j, s_prev_valid ? s_prev.boot_count + 1 : 1, s_build);
  return ESP_OK;
}

#ifdef USE_SUNGROW_WDOG
// After the kick timer (104): the stall detector compares against the kick's own timestamps.
ESP_SYSTEM_INIT_FN(sungrow_diag_stall, SECONDARY, BIT(0), 105) {
  using namespace esphome::sungrow_board;
  esp_timer_create_args_t a{};
  a.callback = stall_check;
  a.dispatch_method = ESP_TIMER_ISR;
  a.name = "sg_stall";
  if (esp_timer_create(&a, &s_stall_timer) == ESP_OK)
    esp_timer_start_periodic(s_stall_timer, 200000);
  return ESP_OK;
}
#endif

// Linked with -Wl,--wrap=panicHandler,--wrap=xt_unhandled_exception (sungrow_board codegen): the
// two Xtensa panic entry points (interrupt-level faults and watchdogs / CPU exceptions, abort()
// and the task watchdog). Both re-enable the flash cache and then run IDF's panic handler.
extern "C" {
void __real_panicHandler(void *frame);
void __real_xt_unhandled_exception(void *frame);

void IRAM_ATTR __wrap_panicHandler(void *frame) {
  esphome::sungrow_board::panic_entry(frame, true);
  __real_panicHandler(frame);
}

void IRAM_ATTR __wrap_xt_unhandled_exception(void *frame) {
  esphome::sungrow_board::panic_entry(frame, false);
  __real_xt_unhandled_exception(frame);
}
}

#endif  // USE_SUNGROW_DIAG
