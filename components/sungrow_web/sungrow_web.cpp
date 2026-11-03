#include "sungrow_web.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "spa_handler.h"

namespace esphome {
namespace sungrow_web {

static const char *const TAG = "sungrow_web";

void SungrowWeb::setup() {
#ifdef SUNGROW_WEB_RUNTIME_LOGIN
  // The stored pair (or the placeholders) replaces web_server.auth's literals before the server
  // starts (web_server's own setup runs at 249, after us): the pointers it checks on every
  // request point into login_ from here on.
  this->load_login_();
  this->apply_login_pointers_();
#endif
  // Register BEFORE web_server adds its own handlers (we run at a higher setup priority), so the
  // SPA owns "/". Behind the web server's digest auth (add_handler wraps it in the middleware).
  if (web_server_base::global_web_server_base != nullptr) {
    web_server_base::global_web_server_base->add_handler(new SpaHandler(this));  // NOLINT
    ESP_LOGCONFIG(TAG, "SPA handler registered on web_server_base (owns '/').");
  } else {
    ESP_LOGE(TAG, "web_server_base not available; SPA not served.");
  }
}

void SungrowWeb::loop() {
  for (;;) {
    std::shared_ptr<WebJob> job;
    {
      std::lock_guard<std::mutex> lk(this->jobs_mutex_);
      if (this->jobs_.empty())
        return;
      job = std::move(this->jobs_.front());
      this->jobs_.pop_front();
    }
    int expected = WebJob::QUEUED;
    if (!job->state.compare_exchange_strong(expected, WebJob::RUNNING))
      continue;  // the httpd side gave up on it
    job->fn(job->reply);
    job->state.store(WebJob::DONE);
    xSemaphoreGive(job->done);
  }
}

bool SungrowWeb::run_on_loop(std::function<void(WebReply &)> fn, WebReply *out, uint32_t timeout_ms) {
  auto job = std::make_shared<WebJob>();
  if (job->done == nullptr)
    return false;
  job->fn = std::move(fn);
  {
    std::lock_guard<std::mutex> lk(this->jobs_mutex_);
    if (this->jobs_.size() >= 8)
      return false;
    this->jobs_.push_back(job);
  }
  if (xSemaphoreTake(job->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    int expected = WebJob::QUEUED;
    if (job->state.compare_exchange_strong(expected, WebJob::CANCELLED))
      return false;  // never started; the loop drops it
    xSemaphoreTake(job->done, portMAX_DELAY);  // already running: it finishes quickly
  }
  *out = std::move(job->reply);
  return true;
}

void SungrowWeb::load_login_() {
  this->login_pref_ = global_preferences->make_preference<WebLoginPref>(fnv1_hash("sungrow_web_login"), true);
  WebLoginPref p{};
  std::string user, pass;
  if (this->login_pref_.load(&p) && unpack_login(p, &user, &pass)) {
    this->login_.set(user, pass);
    this->default_credentials_ = false;
    ESP_LOGI(TAG, "Web login: stored pair for '%s'", user.c_str());
  } else {
    this->login_.set(this->default_user_, this->default_pass_);
    this->default_credentials_ = true;
    ESP_LOGW(TAG, "Web login: default credentials in use (change them on the Settings page)");
  }
}

void SungrowWeb::apply_login_pointers_() {
#if defined(SUNGROW_WEB_RUNTIME_LOGIN) && defined(USE_WEBSERVER_AUTH)
  if (web_server_base::global_web_server_base != nullptr) {
    web_server_base::global_web_server_base->set_auth_username(this->login_.user());
    web_server_base::global_web_server_base->set_auth_password(this->login_.pass());
  }
#endif
#ifdef SUNGROW_WEB_BIND_OTA
  // Native OTA uploads (plaintext, until the dongle is adopted) take the same password.
  if (this->ota_ != nullptr)
    this->ota_->set_auth_password(this->login_.pass());
#endif
}

LoginChange SungrowWeb::change_login(const std::string &current, const std::string &user, const std::string &pass,
                                     const std::string &confirm, std::string *why) {
  if (!this->runtime_login())
    return LoginChange::MANAGED_BY_YAML;
  const LoginChange verdict = check_login_change(this->login_.pass(), current, user, pass, confirm, why);
  if (verdict != LoginChange::OK)
    return verdict;
  WebLoginPref p;
  pack_login(user, pass, &p);
  if (!this->login_pref_.save(&p))
    return LoginChange::STORE_FAILED;
  global_preferences->sync();
  this->login_.set(user, pass);
  this->apply_login_pointers_();
  this->default_credentials_ = false;
  ESP_LOGI(TAG, "Web login changed (user '%s')", user.c_str());
  return LoginChange::OK;
}

void SungrowWeb::dump_config() {
  ESP_LOGCONFIG(TAG, "Sungrow Web UI:");
  ESP_LOGCONFIG(TAG, "  modbus master: %s", this->master() != nullptr ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  signal store:  %s", this->store() != nullptr ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  login: %s%s", this->runtime_login() ? "runtime (NVS)" : "yaml",
                this->runtime_login() && this->default_credentials_ ? ", DEFAULT credentials" : "");
}

}  // namespace sungrow_web
}  // namespace esphome
