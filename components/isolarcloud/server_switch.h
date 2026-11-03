#pragma once
// Broker migration ("svr_convert"). Host-buildable, time injected.
//
//   trigger:  cmdType 10384, devCode 247, point 10457 = "host;port;key" (+ sslFlag). Ignored when
//             host, port and key all equal the current ones (an ssl-only change is ignored too).
//             The cloud gets its reply immediately.
//   apply:    about 10 s later the broker settings are overwritten and the link reconnects.
//   confirm:  connected for more than 5 s: the new settings are copied into the backup ("old")
//             settings and devinfo is published; both 38338 and 38337 then name the new broker.
//   timeout:  not connected after 600 s: with the remote-control mask at its variant default the
//             old server is restored and the link reconnects, otherwise the unit stays.

#include <cstdint>

#include "cloud_types.h"

namespace esphome::isolarcloud {

class ServerSwitch {
 public:
  enum class State : uint8_t { IDLE = 0, WAIT_APPLY = 1, CONFIRMING = 2 };
  enum class Action : uint8_t { NONE, APPLY, CONFIRMED, ROLLBACK, STAY };

  ServerSwitch(uint32_t confirm_after_ms = 5000, uint32_t timeout_ms = 600000, uint32_t delay_ms = 10000)
      : confirm_after_ms_(confirm_after_ms), timeout_ms_(timeout_ms), delay_ms_(delay_ms) {}

  /// Request a switch to `next`. False (ignored, like stock) when host/port/key are unchanged.
  bool request(const CloudIdentity &id, const ServerEndpoint &next, uint32_t now_ms);
  /// Resume a switch that was confirming when we rebooted (state persisted). Timer restarts.
  void resume(uint32_t now_ms) {
    this->state_ = State::CONFIRMING;
    this->applied_ms_ = now_ms;
  }

  /// Call periodically. APPLY: old_server := server, server := target (caller persists and
  /// reconnects). CONFIRMED: old_server := server (commit; caller persists, publishes devinfo with
  /// the committed settings). ROLLBACK: server := old_server (caller persists, reconnects).
  /// STAY: keep the new server.
  Action update(CloudIdentity &id, bool connected, uint32_t now_ms);

  State state() const { return this->state_; }
  bool pending() const { return this->state_ != State::IDLE; }
  bool confirming() const { return this->state_ == State::CONFIRMING; }

 protected:
  uint32_t confirm_after_ms_;
  uint32_t timeout_ms_;
  uint32_t delay_ms_;
  State state_{State::IDLE};
  uint32_t requested_ms_{0};
  uint32_t applied_ms_{0};
  ServerEndpoint target_;
};

}  // namespace esphome::isolarcloud
