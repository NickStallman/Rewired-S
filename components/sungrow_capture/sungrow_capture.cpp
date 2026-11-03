#include "sungrow_capture.h"

#include <cstdio>

#include <esp_heap_caps.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace sungrow_capture {

static const char *const TAG = "sungrow_capture";

void SungrowCapture::setup() {
  this->core_.set_master(&this->modbus_->master());
  this->core_.set_store(&this->dm_->store());
  this->core_.set_clock([]() { return millis(); });
#ifdef USE_TIME
  if (this->time_ != nullptr) {
    this->core_.set_wall_clock([this]() -> int64_t {
      const ESPTime t = this->time_->utcnow();
      return t.is_valid() ? static_cast<int64_t>(t.timestamp) : 0;
    });
  }
#endif
  this->core_.set_log([](int level, const char *msg) { esp_log_printf_(level, TAG, __LINE__, "%s", msg); });
  this->core_.set_large_ok(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0);
  this->core_.set_writer([this](uint16_t start, const uint16_t *v, uint16_t n, CaptureCore::DoneFn done,
                                std::string *why) -> bool {
    const uint8_t dev = this->core_device_;
    const sungrow_control::Verdict verdict = this->control_->core().write_diag(
        dev, start, v, n, "capture", [done](const sungrow_modbus::WriteResult &r) {
          done(r.status == sungrow_modbus::WriteStatus::OK, r.exception);
        });
    if (verdict != sungrow_control::Verdict::OK) {
      *why = sungrow_control::verdict_name(verdict);
      return false;
    }
    return true;
  });
}

void SungrowCapture::loop() {
  this->core_.tick();
  // The YMODEM blocks arrive at ~1 byte/ms: drain the UART often while the bus is ours.
  if (this->core_.running() && this->modbus_->master().raw())
    this->fast_loop_.start();
  else
    this->fast_loop_.stop();
}

void SungrowCapture::dump_config() {
  ESP_LOGCONFIG(TAG, "Sungrow capture (I-V scan, grid waveform, fault recordings): large captures %s",
                heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0 ? "on (PSRAM)" : "off (no PSRAM)");
}

bool SungrowCapture::resolve_(uint8_t device, uint8_t *addr, std::string *label, std::string *why) {
  for (const auto &d : this->modbus_->master().devices()) {
    if (d.config().index != device || d.abandoned())
      continue;
    if (d.table() == nullptr || d.table()->family != sungrow_datamodel::Family::INVERTER) {
      *why = "not an inverter";
      return false;
    }
    *addr = d.config().address;
    char b[64];
    std::snprintf(b, sizeof(b), "%s (device %u, address %u)", d.devinfo().model[0] ? d.devinfo().model : "inverter",
                  device, d.config().address);
    *label = b;
    return true;
  }
  *why = "unknown device";
  return false;
}

bool SungrowCapture::start_iv(uint8_t device, std::string *why) {
  uint8_t addr = 0;
  std::string label;
  if (!this->resolve_(device, &addr, &label, why))
    return false;
  this->core_device_ = device;
  return this->core_.start_iv(device, addr, label, why);
}

bool SungrowCapture::start_grid(uint8_t device, uint8_t cycles, std::string *why) {
  uint8_t addr = 0;
  std::string label;
  if (!this->resolve_(device, &addr, &label, why))
    return false;
  this->core_device_ = device;
  return this->core_.start_grid(device, addr, cycles, label, why);
}

bool SungrowCapture::start_fault(uint8_t device, std::string *why) {
  uint8_t addr = 0;
  std::string label;
  if (!this->resolve_(device, &addr, &label, why))
    return false;
  this->core_device_ = device;
  return this->core_.start_fault(device, addr, label, why);
}

static void append_escaped(std::string *o, const std::string &s) {
  o->push_back('"');
  for (char c : s) {
    if (c == '"' || c == '\\') {
      o->push_back('\\');
      o->push_back(c);
    } else if (static_cast<unsigned char>(c) >= 0x20) {
      o->push_back(c);
    }
  }
  o->push_back('"');
}

std::string SungrowCapture::status_json() {
  std::string o = "{\"available\":true,\"running\":";
  o += this->core_.running() ? "true" : "false";
  char b[200];
  if (this->core_.running()) {
    std::snprintf(b, sizeof(b), ",\"device\":%u,\"kind\":\"%s\",\"progress\":%d,\"elapsed_ms\":%u,\"step\":",
                  this->core_device_, kind_name(this->core_.running_kind()), this->core_.progress(),
                  (unsigned) this->core_.elapsed_ms());
    o += b;
    append_escaped(&o, this->core_.step_text());
    if (this->core_.running_kind() == Kind::IV) {
      const uint16_t fault = this->core_.iv_fault();
      std::snprintf(b, sizeof(b), ",\"scan_ms\":%u,\"iv_fault\":%u,\"iv_fault_text\":",
                    (unsigned) this->core_.scan_ms(), fault);
      o += b;
      const char *text = fault != 0 ? CaptureCore::iv_fault_text(fault) : nullptr;
      if (text != nullptr)
        append_escaped(&o, text);
      else
        o += "null";
    }
  }
  std::snprintf(b, sizeof(b), ",\"iv_starts\":%u,\"generation\":%u,\"devices\":[", (unsigned) this->core_.iv_starts(),
                (unsigned) this->core_.generation());
  o += b;
  bool first = true;
  for (const auto &d : this->modbus_->master().devices()) {
    if (d.abandoned() || d.table() == nullptr || d.table()->family != sungrow_datamodel::Family::INVERTER)
      continue;
    const uint8_t idx = d.config().index;
    std::snprintf(b, sizeof(b), "%s{\"device\":%u,\"online\":%s,\"iv\":%s,\"grid\":%s,\"fault\":%s,\"mppt\":%u,\"model\":",
                  first ? "" : ",", idx, d.online() ? "true" : "false", this->core_.iv_supported(idx) ? "true" : "false",
                  this->core_.grid_supported(idx) ? "true" : "false", this->core_.fault_supported(idx) ? "true" : "false",
                  this->core_.mppt_count(idx));
    o += b;
    append_escaped(&o, d.devinfo().model);
    o += "}";
    first = false;
  }
  o += "],\"results\":[";
  first = true;
  // Newest first.
  const auto &res = this->core_.results();
  for (auto it = res.rbegin(); it != res.rend(); ++it) {
    const Capture &c = **it;
    std::snprintf(b, sizeof(b),
                  "%s{\"id\":%u,\"kind\":\"%s\",\"ok\":%s,\"size\":%u,\"cycles\":%u,\"records\":%u,"
                  "\"wall_start\":%lld,\"duration_ms\":%u,\"result\":",
                  first ? "" : ",", (unsigned) c.id, kind_name(c.kind), c.ok ? "true" : "false",
                  (unsigned) c.data.size(), c.cycles, c.records, (long long) c.wall_start,
                  (unsigned) (c.finished_ms - c.started_ms));
    o += b;
    append_escaped(&o, c.result);
    o += ",\"file_name\":";
    append_escaped(&o, c.file_name);
    o += ",\"inverter\":";
    append_escaped(&o, c.inverter);
    o += "}";
    first = false;
  }
  o += "]}";
  return o;
}

}  // namespace sungrow_capture
}  // namespace esphome
