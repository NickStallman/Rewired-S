#pragma once
// HTTPS POST off the main loop: one FreeRTOS task per job (the TLS handshake and the request
// take seconds; the loop task has a 5 s watchdog and the inverter polling runs on it).
//
// The loop builds an AnalyticsJob, start_job() runs it on a worker task, and the loop polls
// `state` until it is DONE / FAILED / UNAUTHORIZED, then deletes the job. Nothing in here
// touches ESPHome state; results come back through the atomics and the fixed buffers only.
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace sungrow_analytics {

struct AnalyticsJob {
  enum State : int { QUEUED = 0, RUNNING, DONE, FAILED, UNAUTHORIZED };
  static constexpr int ATTEMPTS = 3;
  static constexpr uint32_t RETRY_DELAY_MS = 3000;
  static constexpr uint32_t TIMEOUT_MS = 15000;

  std::string url;
  std::string content_type;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  /// Written by the worker before the release-store on `state`; read by the loop after it.
  std::atomic<int> state{QUEUED};
  std::atomic<int> http_status{0};
  char id[48]{};     ///< "id" field of a JSON reply, when there is one
  char error[64]{};  ///< short failure text (no addresses)
};

/// Runs `job` on a worker task. False when the task could not be created (the job is untouched).
bool start_job(AnalyticsJob *job);

/// Extracts "id":"..." from a small JSON reply into `out` (capacity `cap`); false when absent.
bool parse_reply_id(const char *reply, char *out, size_t cap);

}  // namespace sungrow_analytics
}  // namespace esphome
