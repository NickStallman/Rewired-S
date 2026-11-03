#include "analytics_http.h"

#include <cstdio>
#include <cstring>

#include "esphome/core/defines.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif
#endif

namespace esphome {
namespace sungrow_analytics {

static const char *const TAG = "analytics.http";

bool parse_reply_id(const char *reply, char *out, size_t cap) {
  const char *p = std::strstr(reply, "\"id\"");
  if (p == nullptr)
    return false;
  p = std::strchr(p + 4, '"');
  if (p == nullptr)
    return false;
  p++;
  size_t n = 0;
  while (p[n] != 0 && p[n] != '"' && n + 1 < cap) {
    out[n] = p[n];
    n++;
  }
  out[n] = 0;
  return n > 0;
}

#ifdef USE_ESP32

static void set_error(AnalyticsJob *job, const char *text) { std::snprintf(job->error, sizeof(job->error), "%s", text); }

/// One attempt. Returns the HTTP status, or a negative value for a transport failure.
static int attempt(AnalyticsJob *job) {
  esp_http_client_config_t cfg = {};
  cfg.url = job->url.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = AnalyticsJob::TIMEOUT_MS;
  cfg.disable_auto_redirect = true;
  cfg.max_redirection_count = 0;
  cfg.keep_alive_enable = false;
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 1024;
  cfg.user_agent = "rewired-s";
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) {
    set_error(job, "client init failed");
    return -1;
  }
  esp_http_client_set_header(client, "Content-Type", job->content_type.c_str());
  for (const auto &h : job->headers)
    esp_http_client_set_header(client, h.first.c_str(), h.second.c_str());

  int status = -1;
  esp_err_t err = esp_http_client_open(client, static_cast<int>(job->body.size()));
  if (err != ESP_OK) {
    set_error(job, esp_err_to_name(err));
  } else {
    size_t off = 0;
    bool ok = true;
    while (off < job->body.size()) {
      const size_t chunk = job->body.size() - off < 1024 ? job->body.size() - off : 1024;
      const int w = esp_http_client_write(client, job->body.data() + off, static_cast<int>(chunk));
      if (w <= 0) {
        set_error(job, "write failed");
        ok = false;
        break;
      }
      off += static_cast<size_t>(w);
    }
    if (ok) {
      if (esp_http_client_fetch_headers(client) < 0) {
        set_error(job, "no response");
      } else {
        status = esp_http_client_get_status_code(client);
        char reply[256];
        const int n = esp_http_client_read_response(client, reply, sizeof(reply) - 1);
        reply[n > 0 ? n : 0] = 0;
        if (status >= 200 && status < 300)
          parse_reply_id(reply, job->id, sizeof(job->id));
      }
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return status;
}

static void job_task(void *arg) {
  auto *job = static_cast<AnalyticsJob *>(arg);
  int final_state = AnalyticsJob::FAILED;
  for (int i = 0; i < AnalyticsJob::ATTEMPTS; i++) {
    const int status = attempt(job);
    job->http_status.store(status);
    if (status >= 200 && status < 300) {
      final_state = AnalyticsJob::DONE;
      break;
    }
    if (status >= 400 && status < 500) {
      // The collector refused the request: retrying will not change its mind.
      std::snprintf(job->error, sizeof(job->error), "http %d", status);
      final_state = status == 401 ? AnalyticsJob::UNAUTHORIZED : AnalyticsJob::FAILED;
      break;
    }
    if (status >= 500)
      std::snprintf(job->error, sizeof(job->error), "http %d", status);
    if (i + 1 < AnalyticsJob::ATTEMPTS)
      vTaskDelay(pdMS_TO_TICKS(AnalyticsJob::RETRY_DELAY_MS));
  }
  ESP_LOGD(TAG, "%s: %s (http %d, stack headroom %u)", job->url.c_str(),
           final_state == AnalyticsJob::DONE ? "sent" : job->error, job->http_status.load(),
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  job->state.store(final_state, std::memory_order_release);
  vTaskDelete(nullptr);
}

bool start_job(AnalyticsJob *job) {
  job->state.store(AnalyticsJob::RUNNING);
  // 8 KB of internal RAM: the TLS buffers come from the heap (PSRAM when present), the body is
  // streamed from wherever it lives, so the stack holds only the client and mbedtls contexts.
  if (xTaskCreate(job_task, "sg_analytics", 8192, job, 1, nullptr) != pdPASS) {
    job->state.store(AnalyticsJob::QUEUED);
    return false;
  }
  return true;
}

#else

bool start_job(AnalyticsJob *job) {
  std::snprintf(job->error, sizeof(job->error), "no transport on this platform");
  job->state.store(AnalyticsJob::FAILED, std::memory_order_release);
  return true;
}

#endif

}  // namespace sungrow_analytics
}  // namespace esphome
