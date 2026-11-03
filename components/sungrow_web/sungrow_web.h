#pragma once
// sungrow_web: serves our own single-page app from the device, replacing ESPHome's web_server
// landing page, plus a JSON API over the live device inventory and signals and (with a bound
// sungrow_control) the write / Installer / Setup endpoints (spa_handler.h).
//
// It does NOT replace web_server: web_server still runs (and keeps /events, the REST API and
// everything else). This component only registers an extra handler that wins the few exact
// paths it claims (see spa_handler.h). api:/OTA/logs are unaffected.
//
//   sungrow_web:
//     id: web_ui
//     sungrow_modbus_id: rs485_hub      # live device inventory (/api/devices)
//     sungrow_datamodel_id: sungrow_dm  # live signals (/api/signals)
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"

#include "login_prefs.h"

#include "../sungrow_datamodel/sungrow_datamodel.h"
#include "../sungrow_modbus/sungrow_modbus.h"
#ifdef SUNGROW_WEB_BIND_CONTROL
#include "../sungrow_control/sungrow_control.h"
#endif
#ifdef SUNGROW_WEB_BIND_CAPTURE
#include "../sungrow_capture/sungrow_capture.h"
#endif
#ifdef SUNGROW_WEB_BIND_ANALYTICS
#include "../sungrow_analytics/sungrow_analytics.h"
#endif
#ifdef SUNGROW_WEB_BIND_MQTT
#include "../sungrow_mqtt/sungrow_mqtt.h"
#endif
#ifdef SUNGROW_WEB_BIND_CLOUD
#include "../isolarcloud/isolarcloud.h"
#endif
#ifdef SUNGROW_WEB_BIND_OTA
#include "esphome/components/esphome/ota/ota_esphome.h"
#endif

namespace esphome {
namespace sungrow_web {

/// A web response built on the main loop and sent by the httpd task.
struct WebReply {
  int code{500};
  const char *type{"text/plain"};
  bool no_store{false};
  std::string body{"no reply\n"};
  /// Non-empty: sent as an attachment with this file name.
  std::string filename;
};

/// One API request run on the main loop: the httpd task waits for it, then sends `reply`.
struct WebJob {
  enum State : int { QUEUED = 0, RUNNING = 1, DONE = 2, CANCELLED = 3 };
  std::function<void(WebReply &)> fn;
  WebReply reply;
  std::atomic<int> state{QUEUED};
  SemaphoreHandle_t done{xSemaphoreCreateBinary()};
  ~WebJob() {
    if (this->done != nullptr)
      vSemaphoreDelete(this->done);
  }
};

class SungrowWeb : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  /// Run `fn` on the main loop and wait for it (httpd task only). The signal store, the Modbus
  /// master and ControlCore belong to the loop task; the API handlers never touch them from httpd.
  /// False (and `fn` never runs) if the loop did not pick the job up within `timeout_ms`.
  bool run_on_loop(std::function<void(WebReply &)> fn, WebReply *out, uint32_t timeout_ms);
  // 251 > web_server's 249: our handler is buffered first and therefore flushed/registered
  // first by WebServerBase::init(), so it wins "/" under first-match-wins dispatch.
  float get_setup_priority() const override { return setup_priority::WIFI + 1.0f; }

  void set_modbus(sungrow_modbus::SungrowModbus *m) { this->modbus_ = m; }
  void set_datamodel(sungrow_datamodel::SungrowDataModel *dm) { this->dm_ = dm; }

  // ---- web login (login: runtime) ----
  /// The compile-time placeholders of web_server.auth: what applies until a pair is stored.
  void set_default_login(const char *user, const char *pass) {
    this->default_user_ = user;
    this->default_pass_ = pass;
  }
  /// True when the login is runtime-managed (SUNGROW_WEB_RUNTIME_LOGIN); else web_server.auth
  /// from YAML applies and /api/web_login answers MANAGED_BY_YAML.
  bool runtime_login() const {
#ifdef SUNGROW_WEB_RUNTIME_LOGIN
    return true;
#else
    return false;
#endif
  }
  /// The placeholders are still in use (no pair stored): the UI shows a banner.
  bool default_credentials() const { return this->default_credentials_; }
#ifdef SUNGROW_WEB_BIND_OTA
  /// The native OTA whose password follows the login password (pre-built images).
  void set_ota(esphome::ESPHomeOTAComponent *o) { this->ota_ = o; }
#endif
  const char *login_username() const { return this->login_.user(); }
  /// Change the pair (loop task): checks the current password, validates, stores in NVS and
  /// swaps the live buffers. `why` carries the detail for INVALID.
  LoginChange change_login(const std::string &current, const std::string &user, const std::string &pass,
                           const std::string &confirm, std::string *why);

  // Accessors for the handler (nullptr-safe).
  sungrow_modbus::ModbusMaster *master() { return this->modbus_ != nullptr ? &this->modbus_->master() : nullptr; }
  sungrow_datamodel::SignalStore *store() { return this->dm_ != nullptr ? &this->dm_->store() : nullptr; }

#ifdef SUNGROW_WEB_BIND_CONTROL
  void set_control(sungrow_control::SungrowControl *c) { this->control_ = c; }
  sungrow_control::SungrowControl *control() { return this->control_; }
#else
  // No control bound at compile time: the write endpoints always report "unavailable".
  void *control() { return nullptr; }
#endif
#ifdef SUNGROW_WEB_BIND_CAPTURE
  void set_capture(sungrow_capture::SungrowCapture *c) { this->capture_ = c; }
  sungrow_capture::SungrowCapture *capture() { return this->capture_; }
#endif
#ifdef SUNGROW_WEB_BIND_ANALYTICS
  void set_analytics(sungrow_analytics::SungrowAnalytics *a) { this->analytics_ = a; }
  sungrow_analytics::SungrowAnalytics *analytics() { return this->analytics_; }
#endif
#ifdef SUNGROW_WEB_BIND_MQTT
  void set_mqtt(sungrow_mqtt::SungrowMqttComponent *m) { this->mqtt_ = m; }
  sungrow_mqtt::SungrowMqttComponent *mqtt() { return this->mqtt_; }
#endif
#ifdef SUNGROW_WEB_BIND_CLOUD
  void set_cloud(isolarcloud::IsolarcloudComponent *c) { this->cloud_ = c; }
  isolarcloud::IsolarcloudComponent *cloud() { return this->cloud_; }
#endif

 protected:
  sungrow_modbus::SungrowModbus *modbus_{nullptr};
  sungrow_datamodel::SungrowDataModel *dm_{nullptr};
#ifdef SUNGROW_WEB_BIND_CONTROL
  sungrow_control::SungrowControl *control_{nullptr};
#endif
#ifdef SUNGROW_WEB_BIND_CAPTURE
  sungrow_capture::SungrowCapture *capture_{nullptr};
#endif
#ifdef SUNGROW_WEB_BIND_ANALYTICS
  sungrow_analytics::SungrowAnalytics *analytics_{nullptr};
#endif
#ifdef SUNGROW_WEB_BIND_MQTT
  sungrow_mqtt::SungrowMqttComponent *mqtt_{nullptr};
#endif
#ifdef SUNGROW_WEB_BIND_CLOUD
  isolarcloud::IsolarcloudComponent *cloud_{nullptr};
#endif
  std::mutex jobs_mutex_;
  std::deque<std::shared_ptr<WebJob>> jobs_;

#ifdef SUNGROW_WEB_BIND_OTA
  esphome::ESPHomeOTAComponent *ota_{nullptr};
#endif
  const char *default_user_{"admin"};
  const char *default_pass_{"rewired-s"};
  bool default_credentials_{true};
  LoginBuffers login_;
  ESPPreferenceObject login_pref_;
  void load_login_();
  void apply_login_pointers_();
};

}  // namespace sungrow_web
}  // namespace esphome
