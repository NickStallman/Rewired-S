#pragma once
// SpaHandler: the AsyncWebHandler that owns the device's single-page app and its JSON API.
//
// It is registered on web_server_base BEFORE ESPHome's own web_server (SungrowWeb runs at
// setup priority 251 > web_server's 249, and WebServerBase::init() flushes buffered handlers
// in registration order), so for the handful of paths below it is matched first and can own
// "/". Dispatch is first-match-wins, so canHandle() is a STRICT exact-path whitelist: every
// other path (/events, /0.js, /0.css, /<domain>/<id>, ...) returns false and falls through to
// web_server unchanged.
//
// Sits behind the existing digest auth automatically (add_handler wraps it in the auth
// middleware). Responses are served through the IDF esp_http_server shim (httpd_req_t), the
// same approach as components/sungrow_debug/flash_tools.cpp.
//
// Threading: handleRequest runs on the httpd task. Static assets are sent from there; every /api
// request is run on the main loop (SungrowWeb::run_on_loop), which owns the signal store, the
// Modbus master and ControlCore, with its reply captured and then sent from httpd.
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "esphome/components/web_server_base/web_server_base.h"
#include "sungrow_web.h"

namespace esphome {
namespace sungrow_web {

class SungrowWeb;

class SpaHandler : public AsyncWebHandler {
 public:
  explicit SpaHandler(SungrowWeb *parent) : parent_(parent) {}

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  // Non-form POST bodies (setup_write / grid_read_start are sent as text/plain) arrive here in
  // chunks before handleRequest; accumulated per request in bodies_.
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  SungrowWeb *parent_;

  // Writes policy: every write endpoint here calls ControlCore with authorized = true. The digest
  // login authorises /api/write; the Setup and Installer endpoints (/api/setup_write,
  // /api/setup_apply, /api/setup_start) additionally need a short-lived, single-use confirm token
  // (POST /api/confirm -> token) bound to the device and the endpoint's sentinel address below.
  // The `writes_enabled` switch does not apply to these; it gates MQTT, the Modbus-TCP :502
  // server and the HA action. ControlCore still enforces the allow-list (per XML family for
  // protection registers), value ranges / relations and the online check.
  struct ConfirmToken {
    uint32_t token{0};  // 0 = empty slot
    uint32_t expiry_ms{0};
    uint8_t device{0};
    uint16_t address{0};
  };
  static constexpr unsigned CONFIRM_SLOTS = 4;
  static constexpr uint32_t CONFIRM_TTL_MS = 20000;
  ConfirmToken tokens_[CONFIRM_SLOTS]{};
  // Sentinel "addresses" the batch confirm tokens are bound to (a batch has no single register):
  // /api/confirm?device=D&address=65535 -> /api/setup_write (Installer batch),
  // 65534 -> /api/setup_apply (Setup profile), 65533 -> /api/setup_start.
  static constexpr uint16_t SETUP_CONFIRM_ADDR = 0xFFFF;
  static constexpr uint16_t SETUP_APPLY_CONFIRM_ADDR = 0xFFFE;
  static constexpr uint16_t SETUP_START_CONFIRM_ADDR = 0xFFFD;
  /// 65532 -> /api/capture_start (I-V scan or grid-waveform recording).
  static constexpr uint16_t CAPTURE_CONFIRM_ADDR = 0xFFFC;
  /// 65531 -> /api/boot_slot (boot the other OTA slot; the confirm stands in for the arming switch).
  static constexpr uint16_t BOOT_SLOT_CONFIRM_ADDR = 0xFFFB;

  uint32_t issue_confirm(uint8_t device, uint16_t address, uint32_t now_ms);
  bool consume_confirm(uint8_t device, uint16_t address, uint32_t token, uint32_t now_ms);

  /// Longest the httpd task waits for the loop to pick an API request up (then 503).
  static constexpr uint32_t API_LOOP_TIMEOUT_MS = 4000;
  /// The API proper; runs on the main loop with replies captured (SungrowWeb::run_on_loop).
  void dispatch_api_(AsyncWebServerRequest *request, const std::string &url, const std::string &body);
  void handle_write_(AsyncWebServerRequest *request);
  void handle_confirm_(AsyncWebServerRequest *request);
  // Installer: confirmed batch apply + on-demand read-back for the diff.
  void handle_setup_write_(AsyncWebServerRequest *request, const std::string &body);
  void handle_grid_read_start_(AsyncWebServerRequest *request, const std::string &body);
  void handle_grid_read_(AsyncWebServerRequest *request);
  // Setup: option picker, server-resolved preview, confirmed apply, start.
  void handle_setup_options_(AsyncWebServerRequest *request);
  void handle_setup_preview_(AsyncWebServerRequest *request);
  void handle_setup_apply_(AsyncWebServerRequest *request);
  void handle_setup_start_(AsyncWebServerRequest *request);
  // Diagnostic captures: confirmed start, cancel, metadata (on the loop), raw file (streamed).
  void handle_capture_start_(AsyncWebServerRequest *request);
  void handle_capture_cancel_(AsyncWebServerRequest *request);
  void handle_capture_meta_(AsyncWebServerRequest *request);
  void handle_capture_file_(AsyncWebServerRequest *request);
  // Diagnostics report (sungrow_analytics): manual upload start, upload status, report download.
  void handle_diag_send_(AsyncWebServerRequest *request);
  void handle_diag_status_(AsyncWebServerRequest *request);
  void handle_diag_report_(AsyncWebServerRequest *request);
  // Settings page (pre-built images): the web login, the MQTT broker and the cloud version.
  // All form-encoded POSTs (request->arg); mutations run on the loop like every API call.
  void handle_web_login_get_(AsyncWebServerRequest *request);
  void handle_web_login_post_(AsyncWebServerRequest *request);
  void handle_mqtt_settings_get_(AsyncWebServerRequest *request);
  void handle_mqtt_settings_post_(AsyncWebServerRequest *request);
  void handle_cloud_version_get_(AsyncWebServerRequest *request);
  void handle_cloud_version_post_(AsyncWebServerRequest *request);
  // Boot slots: running / other slot descriptors, and the confirmed switch to the other slot.
  void handle_boot_slot_get_(AsyncWebServerRequest *request);
  void handle_boot_slot_post_(AsyncWebServerRequest *request);

  /// A raw text/plain POST body (setup_write / grid_read_start) while it is being received.
  struct PostBody {
    std::string data;
    size_t total{0};         ///< Content-Length
    uint32_t started_ms{0};  ///< first chunk
  };
  static constexpr size_t BODY_MAX = 8192;
  static constexpr uint32_t BODY_TTL_MS = 30000;  ///< a transfer that broke off is forgotten after this
  std::map<AsyncWebServerRequest *, PostBody> bodies_;  ///< keyed by the request being received
  std::mutex bodies_mutex_;
  std::string take_body_(AsyncWebServerRequest *request);
};

}  // namespace sungrow_web
}  // namespace esphome
