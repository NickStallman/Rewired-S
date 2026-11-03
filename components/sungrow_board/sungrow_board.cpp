#include "sungrow_board.h"

#include <atomic>
#include <cstring>

#include <driver/gpio.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#include <esp_private/startup_internal.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "diag.h"
#ifdef USE_SUNGROW_WDOG
#include <hal/gpio_ll.h>
#endif

namespace esphome::sungrow_board {

static const char *const TAG = "sungrow_board";

SungrowBoard *global_board = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// ---- external supervisor ("WDOG", WiNet-S2) ----
//
// Why esp_timer and not loop(): the supervisor must keep being fed while the main loop is busy
// (slow setup(), Wi-Fi connect, a blocking component). The esp_timer task runs independently of
// the loop task, so a long setup()/loop() won't reset the board, while a real hang (interrupts
// off, scheduler dead) still stops the edges and lets the supervisor reset us — same behaviour as
// stock (a 500 ms feed timer). A crash is IDF's to handle: with `diagnostics:`
// the panic hook switches the supervisor off (watchdog_panic_off) so the panic handler can record,
// dump and restart (diag.h).
//
// Two steps, both before app_main (stock also feeds before its NVS init):
//  1. The start sequence runs from a global constructor emitted by codegen
//     (sungrow_board/__init__.py), i.e. during IDF's do_global_ctors(). The 10 x 100 ms init
//     toggles busy-wait with esp_rom_delay_us because vTaskDelay needs a running scheduler.
//     esp_timer cannot be used there: esp_timer_create/start return ESP_ERR_INVALID_STATE until
//     esp_timer_init_os has run.
//  2. The periodic kick is armed by an IDF SECONDARY-stage init function (priority 104, after
//     esp_timer_init_os at 100), before the scheduler and app_main start.
// SungrowBoard::setup() arms it as a last resort if step 2 did not.

namespace {
bool wd_started = false;
int wd_kick = -1;
bool wd_kick_inv = false;
int wd_enable = -1;
bool wd_enable_inv = false;
uint32_t wd_interval_ms = 0;
bool wd_state = true;  // next logical level written to kick (stock: s starts at 1)
std::atomic<uint32_t> wd_edges{0};
esp_timer_handle_t wd_timer = nullptr;

void wd_set(int pin, bool inv, bool level) {
  gpio_set_level(static_cast<gpio_num_t>(pin), (level != inv) ? 1 : 0);
}

// Stock order: write s, then invert s.
void wd_kick_once() {
  wd_set(wd_kick, wd_kick_inv, wd_state);
  wd_state = !wd_state;
  wd_edges.fetch_add(1, std::memory_order_relaxed);
}

// Kick-gap diagnostics: the supervisor resets the board if the gap between periodic kicks exceeds
// its (unknown) timeout, which looks like a power-on reset with no crash record.
std::atomic<uint32_t> wd_max_gap_ms{0};
std::atomic<uint32_t> wd_long_gaps{0};  ///< periodic gaps over 1 s
int64_t wd_last_kick_us = 0;

void wd_timer_cb(void * /*arg*/) {
  const int64_t now = esp_timer_get_time();
  if (wd_last_kick_us != 0) {
    const uint32_t gap = static_cast<uint32_t>((now - wd_last_kick_us) / 1000);
    if (gap > wd_max_gap_ms.load(std::memory_order_relaxed))
      wd_max_gap_ms.store(gap, std::memory_order_relaxed);
    if (gap > 1000)
      wd_long_gaps.fetch_add(1, std::memory_order_relaxed);
  }
  wd_last_kick_us = now;
  wd_kick_once();
#ifdef USE_SUNGROW_DIAG
  diag_on_kick(now, wd_edges.load(std::memory_order_relaxed), wd_max_gap_ms.load(std::memory_order_relaxed),
               wd_long_gaps.load(std::memory_order_relaxed));
#endif
}

void wd_config_pin(int pin) {
  gpio_config_t io{};
  io.pin_bit_mask = 1ULL << pin;
  io.mode = GPIO_MODE_INPUT_OUTPUT;  // as stock; lets the level be read back
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);
}

// Runs inside esp_restart() (every software restart: OTA reboot, Restart button, safe mode). Stock
// switches the supervisor off before its post-upgrade restart ("Prepare to restart system!",
// 3 s, enable low, 50 ms, restart), so the next image can take
// its time to boot (a freshly installed image is verified and its otadata rewritten first) and
// re-enables it from its own start sequence. Left on, the supervisor resets that first boot and
// the bootloader rolls the new image back.
void wd_shutdown_handler() {
  if (wd_enable < 0)
    return;
  wd_set(wd_enable, wd_enable_inv, false);
  esp_rom_delay_us(50000);
}
}  // namespace

esp_err_t watchdog_start_timer() {
  if (wd_timer != nullptr)
    return ESP_OK;
  if (!wd_started)
    return ESP_ERR_INVALID_STATE;
  esp_timer_create_args_t args{};
  args.callback = wd_timer_cb;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "sg_wdog";
  esp_timer_handle_t timer = nullptr;
  esp_err_t err = esp_timer_create(&args, &timer);
  if (err != ESP_OK)
    return err;
  err = esp_timer_start_periodic(timer, static_cast<uint64_t>(wd_interval_ms) * 1000ULL);
  if (err != ESP_OK) {
    esp_timer_delete(timer);
    return err;
  }
  wd_timer = timer;
  return ESP_OK;
}

// The stock start sequence (docs/board_pins.md): both pins INPUT_OUTPUT -> enable=0 ->
// init_toggles x (wait init_delay, kick=s, s=!s; s starts at 1) -> enable=1 -> periodic timer.
void watchdog_early_start(int kick_pin, bool kick_inverted, int enable_pin, bool enable_inverted,
                          uint32_t interval_ms, uint32_t init_toggles, uint32_t init_delay_ms) {
  if (wd_started || kick_pin < 0)
    return;
  wd_started = true;
  wd_kick = kick_pin;
  wd_kick_inv = kick_inverted;
  wd_enable = enable_pin;
  wd_enable_inv = enable_inverted;
  wd_interval_ms = interval_ms;

  wd_config_pin(kick_pin);
  if (enable_pin >= 0) {
    wd_config_pin(enable_pin);
    wd_set(enable_pin, enable_inverted, false);
  }
  const bool scheduler_running = xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED;
  for (uint32_t i = 0; i < init_toggles; i++) {
    if (scheduler_running)
      vTaskDelay(pdMS_TO_TICKS(init_delay_ms));
    else
      esp_rom_delay_us(static_cast<uint32_t>(init_delay_ms) * 1000u);
    wd_kick_once();
  }
  if (enable_pin >= 0) {
    wd_set(enable_pin, enable_inverted, true);
    esp_register_shutdown_handler(wd_shutdown_handler);
  }

  // Arms only when called after esp_timer_init_os; from the global constructor the
  // SECONDARY-stage init function at the end of this file arms it.
  watchdog_start_timer();
}

#ifdef USE_SUNGROW_WDOG
// Panic path (diag.cpp): same effect as wd_shutdown_handler without the wait, from IRAM with
// register writes only. IDF's panic handler takes far longer than the switch-off needs (it prints,
// writes the core dump) before it restarts.
void IRAM_ATTR watchdog_panic_off() {
  if (wd_enable < 0)
    return;
  gpio_ll_set_level(&GPIO, static_cast<uint32_t>(wd_enable), wd_enable_inv ? 1 : 0);
}
#endif

bool watchdog_running() { return wd_timer != nullptr; }
uint32_t watchdog_toggle_count() { return wd_edges.load(std::memory_order_relaxed); }
uint32_t watchdog_max_gap_ms() { return wd_max_gap_ms.load(std::memory_order_relaxed); }
uint32_t watchdog_long_gaps() { return wd_long_gaps.load(std::memory_order_relaxed); }

// ---- board component ----

SungrowBoard::SungrowBoard() { global_board = this; }

// ---- cloud version (cloud_version.h) ----

void SungrowBoard::load_cloud_version_() {
  this->version_pref_ =
      global_preferences->make_preference<CloudVersionPref>(fnv1_hash("sungrow_board_cloud_version"), true);
  if (!this->version_pref_.load(&this->version_pref_data_) || !cloud_version_pref_valid(this->version_pref_data_))
    cloud_version_pref_init(&this->version_pref_data_);
}

// The stock app stays in the other OTA slot after the first install (the stock updater wrote us
// into the free slot). Its esp_app_desc_t.version is the version the dongle reported before;
// noted once, because the next OTA overwrites that slot with Rewired-S.
void SungrowBoard::detect_other_slot_version_() {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  if (run == nullptr || other == nullptr || other->address == run->address)
    return;
  esp_app_desc_t d;
  if (esp_ota_get_partition_description(other, &d) != ESP_OK)
    return;
  char version[sizeof(d.version) + 1];
  std::memcpy(version, d.version, sizeof(d.version));
  version[sizeof(d.version)] = ' ';
  char project[sizeof(d.project_name) + 1];
  std::memcpy(project, d.project_name, sizeof(d.project_name));
  project[sizeof(d.project_name)] = ' ';
  const esp_app_desc_t *self = esp_app_get_description();
  const bool ours = self != nullptr && std::strncmp(self->project_name, project, sizeof(d.project_name)) == 0;
  if (ours || !is_stock_version_string(version)) {
    ESP_LOGD(TAG, "Other slot %s holds '%s' %s: not a stock image", other->label, project, version);
    return;
  }
  if (note_detected_version(&this->version_pref_data_, version)) {
    ESP_LOGI(TAG, "Stock image in %s reports %s: noted as the cloud version", other->label,
             this->version_pref_data_.detected);
    if (this->version_pref_.save(&this->version_pref_data_))
      global_preferences->sync();
  }
}

void SungrowBoard::resolve_cloud_version_() {
  const CloudVersionChoice c =
      choose_cloud_version(&this->version_pref_data_, this->yaml_version_, this->default_version_);
  this->firmware_version_ = c.value;
  this->version_source_ = c.source;
}

bool SungrowBoard::set_cloud_version_override(const std::string &v, std::string *why) {
  if (!v.empty() && !validate_cloud_version_override(v, why))
    return false;
  if (esphome::sungrow_board::set_cloud_version_override(&this->version_pref_data_, v)) {
    if (!this->version_pref_.save(&this->version_pref_data_)) {
      if (why != nullptr)
        *why = "could not store the override";
      return false;
    }
    global_preferences->sync();
  }
  this->resolve_cloud_version_();
  ESP_LOGI(TAG, "Cloud version %s: %s (%s)", v.empty() ? "override cleared" : "override set",
           this->firmware_version_.c_str(), cloud_version_source_name(this->version_source_));
  return true;
}

void SungrowBoard::setup() {
  this->load_cloud_version_();
  this->detect_other_slot_version_();
  this->resolve_cloud_version_();
#ifdef USE_SUNGROW_DIAG
  diag_setup(this->diag_cfg_);
#ifdef USE_TEXT_SENSOR
  if (this->last_reset_ != nullptr)
    this->last_reset_->publish_state(diag_last_reset());
#endif
#endif
#ifdef USE_SUNGROW_WDOG
  if (!watchdog_running()) {
    esp_err_t err = watchdog_start_timer();
    if (err == ESP_OK) {
      ESP_LOGW(TAG, "External watchdog timer was not armed before setup(); armed now");
    } else {
      ESP_LOGE(TAG, "External watchdog timer failed to start (%s); board will reset", esp_err_to_name(err));
    }
  }
#endif
}

#ifdef USE_SUNGROW_DIAG
void SungrowBoard::loop() { diag_loop(); }
#endif

void SungrowBoard::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sungrow board:\n"
                "  Variant: %s\n"
                "  Product: %s (type 0x%04X)\n"
                "  Reported version: %s (%s; floor %s)",
                this->is_winet_s2() ? "WiNet-S2 (esp32s3)" : "WiNet-S (esp32)", this->product_name_,
                this->product_type_, this->firmware_version_.c_str(),
                cloud_version_source_name(this->version_source_), this->version_floor_);
  if (this->version_pref_data_.detected[0] != '\0')
    ESP_LOGCONFIG(TAG, "  Stock version detected in the other slot: %s", this->version_pref_data_.detected);
#ifdef USE_SUNGROW_WDOG
  ESP_LOGCONFIG(TAG, "  External watchdog: %s, kick GPIO%d, enable GPIO%d, every %" PRIu32 " ms, %" PRIu32 " edges",
                watchdog_running() ? "running" : "FAILED", wd_kick, wd_enable, wd_interval_ms,
                watchdog_toggle_count());
#else
  ESP_LOGCONFIG(TAG, "  External watchdog: not configured");
#endif
  // OTA image state: PENDING_VERIFY means the bootloader will roll back unless safe_mode marks
  // the image valid (safe_mode boot_is_good_after; see README "Rollback").
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK) {
    const char *s = state == ESP_OTA_IMG_VALID            ? "valid"
                    : state == ESP_OTA_IMG_PENDING_VERIFY ? "pending verify"
                    : state == ESP_OTA_IMG_NEW            ? "new"
                    : state == ESP_OTA_IMG_UNDEFINED      ? "undefined"
                                                          : "invalid/aborted";
    ESP_LOGCONFIG(TAG, "  Running partition: %s @0x%06" PRIX32 " (%s)", running->label, running->address, s);
  }
#ifdef USE_SUNGROW_DIAG
  diag_dump_config();
#endif
}

}  // namespace esphome::sungrow_board

#ifdef USE_SUNGROW_WDOG
// Arms the periodic supervisor kick as soon as esp_timer works (SECONDARY stage, after
// esp_timer_init_os at priority 100), before the scheduler and app_main. Always returns ESP_OK:
// a failing init function aborts the boot, and SungrowBoard::setup() retries.
ESP_SYSTEM_INIT_FN(sungrow_wdog_timer, SECONDARY, BIT(0), 104) {
  esphome::sungrow_board::watchdog_start_timer();
  return ESP_OK;
}
#endif
