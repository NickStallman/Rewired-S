#include "server_switch.h"

namespace esphome::isolarcloud {

bool ServerSwitch::request(const CloudIdentity &id, const ServerEndpoint &next, uint32_t now_ms) {
  const ServerEndpoint &cur = id.server;
  if (next.host == cur.host && next.port == cur.port && next.key == cur.key)
    return false;  // an ssl-only change is ignored
  this->target_ = next;
  this->requested_ms_ = now_ms;
  this->state_ = State::WAIT_APPLY;
  return true;
}

ServerSwitch::Action ServerSwitch::update(CloudIdentity &id, bool connected, uint32_t now_ms) {
  switch (this->state_) {
    case State::IDLE:
      return Action::NONE;
    case State::WAIT_APPLY:
      if (now_ms - this->requested_ms_ < this->delay_ms_)
        return Action::NONE;
      id.old_server = id.server;  // rollback target
      id.server = this->target_;
      this->state_ = State::CONFIRMING;
      this->applied_ms_ = now_ms;
      return Action::APPLY;
    case State::CONFIRMING: {
      const uint32_t elapsed = now_ms - this->applied_ms_;
      if (connected && elapsed > this->confirm_after_ms_) {
        id.old_server = id.server;  // commit into the backup settings
        this->state_ = State::IDLE;
        return Action::CONFIRMED;
      }
      if (!connected && elapsed >= this->timeout_ms_) {
        this->state_ = State::IDLE;
        if (id.remote_ctl_mask != default_remote_ctl_mask(id.product_type))
          return Action::STAY;  // a customised remote-control mask keeps the new server
        id.server = id.old_server;
        return Action::ROLLBACK;
      }
      return Action::NONE;
    }
  }
  return Action::NONE;
}

}  // namespace esphome::isolarcloud
