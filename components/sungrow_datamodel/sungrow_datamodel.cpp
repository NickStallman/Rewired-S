#include "sungrow_datamodel.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace sungrow_datamodel {

static const char *const TAG = "sungrow_datamodel";
static const uint32_t TICK_MS = 100;

void DmBinding::attach(SungrowDataModel *parent) {
  this->parent_ = parent;
  parent->register_binding(this);
  parent->store().subscribe(this->device_, this->sigid_, [this](const Signal &) { this->dirty_ = true; });
  for (const auto &f : this->fallbacks_)
    parent->store().subscribe(this->device_, f.sigid, [this](const Signal &) { this->dirty_ = true; });
}

const Signal *DmBinding::pick_(const SignalStore &store, float *fallback_mult) const {
  if (fallback_mult != nullptr)
    *fallback_mult = 1.0f;
  const Signal *primary = store.get(this->device_, this->sigid_);
  if ((primary != nullptr && primary->valid) || this->fallbacks_.empty())
    return primary;
  for (const auto &f : this->fallbacks_) {
    const Signal *s = store.get(this->device_, f.sigid);
    if (s != nullptr && s->valid) {
      if (fallback_mult != nullptr)
        *fallback_mult = f.multiplier;
      return s;
    }
  }
  return primary;
}

void DmBinding::dm_tick(uint32_t now) {
  bool due = this->dirty_ && (!this->published_ || now - this->last_publish_ >= this->throttle_ms_);
  bool forced = this->force_ms_ != 0 && this->published_ && now - this->last_publish_ >= this->force_ms_;
  if (!due && !forced)
    return;
  this->dirty_ = false;
  this->published_ = true;
  this->last_publish_ = now;
  this->dm_publish(this->parent_->store());
}

void SungrowDataModel::setup() { this->store_.set_clock([]() { return millis(); }); }

void SungrowDataModel::loop() {
  uint32_t now = millis();
  if (now - this->last_tick_ < TICK_MS)
    return;
  this->last_tick_ = now;
  for (auto *b : this->bindings_)
    b->dm_tick(now);
}

void SungrowDataModel::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sungrow data model:\n"
                "  Tables: %u (%s ... %s)\n"
                "  Bound entities: %u",
                (unsigned) kTableCount, kTables[0].name, kTables[kTableCount - 1].name,
                (unsigned) this->bindings_.size());
}

}  // namespace sungrow_datamodel
}  // namespace esphome
