#pragma once
// Button decoder + LED pattern logic of the stock WiNet dongles (docs/board_pins.md "Button / LED
// behaviour"). Pure C++17, no ESP-IDF / ESPHome dependencies,
// so it is used by board_io.cpp and by the host tests in tests/board/.
//
// Times are milliseconds from any monotonic clock (millis()). Callers may tick at any rate; stock
// polls the button every 50 ms and updates the LEDs every 100 ms, and so does board_io.cpp.

#include <cstdint>

namespace esphome::sungrow_board::logic {

// ---- button (as stock) ----
//
// - A press of at most CLICK_MAX_MS is a click. Clicks are counted and dispatched CLICK_DISPATCH_MS
//   after the last release (stock: 20 ticks).
// - A press longer than CLICK_MAX_MS is a long press, never a click; it also drops clicks still
//   pending. It only counts if the button was idle (released) for at least LONG_IDLE_MS before it
//   was pressed (stock: > 60 ticks idle).
// - Stock acts on long-press thresholds while the button is still held. We report the crossing
//   (HOLD_REACHED, for logging) and act on release (LONG_RELEASE with the held time), so a 30 s
//   hold can't fire the 5 s action first.
class ButtonDecoder {
 public:
  static constexpr uint32_t CLICK_MAX_MS = 1000;       // > 0x14 ticks = long-press flag
  static constexpr uint32_t CLICK_DISPATCH_MS = 1000;  // 20 ticks after release
  static constexpr uint32_t LONG_IDLE_MS = 3000;       // > 0x3c idle ticks before the press

  enum class Kind : uint8_t { NONE, CLICKS, HOLD_REACHED, LONG_RELEASE };
  struct Event {
    Kind kind{Kind::NONE};
    uint8_t clicks{0};     // CLICKS: count (saturates at 255)
    uint32_t held_ms{0};   // HOLD_REACHED: threshold; LONG_RELEASE: total press time
  };

  // Hold thresholds to report while held (HOLD_REACHED). Ascending; 0-terminated or up to 4.
  void set_hold_marks(uint32_t a, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
    this->marks_[0] = a;
    this->marks_[1] = b;
    this->marks_[2] = c;
    this->marks_[3] = d;
  }

  // Feed the debounced button state. Returns at most one event per call.
  Event update(bool pressed, uint32_t now) {
    Event ev;
    if (!this->started_) {
      // The idle time before the first press counts from the first sample.
      this->started_ = true;
      this->released_at_ = now;
      if (pressed) {  // held at boot: never a valid long press, never a click
        this->pressed_ = true;
        this->pressed_at_ = now;
        this->long_ok_ = false;
        this->long_ = true;
      }
      return ev;
    }
    if (pressed && !this->pressed_) {
      this->pressed_ = true;
      this->pressed_at_ = now;
      this->long_ok_ = (now - this->released_at_) >= LONG_IDLE_MS;
      this->long_ = false;
      this->next_mark_ = 0;
      return ev;
    }
    if (pressed) {
      const uint32_t held = now - this->pressed_at_;
      if (!this->long_ && held > CLICK_MAX_MS) {
        this->long_ = true;
        this->clicks_ = 0;  // a long press cancels clicks still waiting for dispatch
      }
      if (this->long_ok_ && this->next_mark_ < 4 && this->marks_[this->next_mark_] != 0 &&
          held >= this->marks_[this->next_mark_]) {
        ev.kind = Kind::HOLD_REACHED;
        ev.held_ms = this->marks_[this->next_mark_];
        this->next_mark_++;
      }
      return ev;
    }
    if (this->pressed_) {  // release edge
      this->pressed_ = false;
      this->released_at_ = now;
      const uint32_t held = now - this->pressed_at_;
      if (!this->long_ && held <= CLICK_MAX_MS) {
        if (this->clicks_ < 255)
          this->clicks_++;
      } else if (this->long_ok_) {
        ev.kind = Kind::LONG_RELEASE;
        ev.held_ms = held;
      }
      this->long_ = false;
      return ev;
    }
    if (this->clicks_ != 0 && now - this->released_at_ >= CLICK_DISPATCH_MS) {
      ev.kind = Kind::CLICKS;
      ev.clicks = this->clicks_;
      this->clicks_ = 0;
    }
    return ev;
  }

  bool pressed() const { return this->pressed_; }
  uint8_t pending_clicks() const { return this->clicks_; }

 protected:
  uint32_t marks_[4]{0, 0, 0, 0};
  bool started_{false};
  bool pressed_{false};
  bool long_{false};
  bool long_ok_{false};
  uint8_t clicks_{0};
  uint8_t next_mark_{0};
  uint32_t pressed_at_{0};
  uint32_t released_at_{0};
};

// ---- cloud error: uplink wanted but not connected for CLOUD_ERROR_MS ----
class CloudErrorTracker {
 public:
  static constexpr uint32_t CLOUD_ERROR_MS = 60000;

  bool update(bool wanted, bool connected, uint32_t now) {
    if (!this->started_ || !wanted || connected) {
      this->started_ = true;
      this->ok_at_ = now;
      return false;
    }
    return now - this->ok_at_ >= CLOUD_ERROR_MS;
  }

 protected:
  bool started_{false};
  uint32_t ok_at_{0};
};

// ---- LEDs (as stock) ----
//
// Blue (network / cloud):
//   config mode (our fallback AP is up)    : toggles every 200 ms
//   network not connected                  : off
//   network up, cloud up                   : solid
//   network up, no cloud for < 60 s        : solid
//   network up, no cloud for >= 60 s       : 3 s off, 1 s on (a 1 s flash every 4 s)
//   (stock's 1 s slow blink for wifi_data_state != 0 is not mirrored: meaning unknown, C19)
// Green: toggles every 1 s (heartbeat) with no faults, off with faults.
// Red: on with faults.
struct LedInputs {
  bool network{false};      // STA (or Ethernet) connected
  bool config_mode{false};  // fallback AP active
  bool cloud{false};        // cloud uplink connected
  bool faults{false};
};
struct LedOutputs {
  bool red{false};
  bool green{false};
  bool blue{false};
};

class LedLogic {
 public:
  static constexpr uint32_t FAST_BLINK_MS = 200;
  static constexpr uint32_t HEARTBEAT_MS = 1000;
  static constexpr uint32_t NO_CLOUD_SOLID_MS = 60000;
  static constexpr uint32_t NO_CLOUD_PERIOD_MS = 4000;
  static constexpr uint32_t NO_CLOUD_ON_MS = 1000;

  LedOutputs update(const LedInputs &in, uint32_t now) {
    if (!this->started_) {
      this->started_ = true;
      this->fast_at_ = this->beat_at_ = now;
      this->no_cloud_since_ = now;
    }
    // "No cloud since" = the later of the network coming up and the cloud going down.
    if (in.cloud || !in.network)
      this->no_cloud_since_ = now;

    if (now - this->fast_at_ >= FAST_BLINK_MS) {
      this->fast_at_ = now;
      this->fast_ = !this->fast_;
    }
    if (now - this->beat_at_ >= HEARTBEAT_MS) {
      this->beat_at_ = now;
      this->beat_ = !this->beat_;
    }

    LedOutputs out;
    if (in.config_mode) {
      out.blue = this->fast_;
    } else if (!in.network) {
      out.blue = false;
    } else if (in.cloud) {
      out.blue = true;
    } else {
      const uint32_t t = now - this->no_cloud_since_;
      if (t < NO_CLOUD_SOLID_MS) {
        out.blue = true;
      } else {
        out.blue = ((t - NO_CLOUD_SOLID_MS) % NO_CLOUD_PERIOD_MS) >= (NO_CLOUD_PERIOD_MS - NO_CLOUD_ON_MS);
      }
    }
    out.red = in.faults;
    out.green = !in.faults && this->beat_;
    return out;
  }

 protected:
  bool started_{false};
  bool fast_{false};
  bool beat_{true};
  uint32_t fast_at_{0};
  uint32_t beat_at_{0};
  uint32_t no_cloud_since_{0};
};

}  // namespace esphome::sungrow_board::logic
