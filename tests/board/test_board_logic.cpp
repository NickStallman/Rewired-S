// Host tests for components/sungrow_board/board_logic.h. Build + run: `make -C tests/board`.
#include <cstdio>
#include <vector>

#include "board_logic.h"

using namespace esphome::sungrow_board::logic;
using Kind = ButtonDecoder::Kind;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    g_checks++;                                                   \
    if (!(cond)) {                                                \
      g_fail++;                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                             \
  } while (0)

// Drives a decoder at 50 ms ticks and records events with their timestamps.
struct Sim {
  ButtonDecoder d;
  uint32_t t{0};
  std::vector<std::pair<uint32_t, ButtonDecoder::Event>> ev;
  Sim() {
    d.set_hold_marks(5000, 30000);
    d.update(false, t);
  }
  void run(bool pressed, uint32_t ms) {
    for (uint32_t end = t + ms; t < end;) {
      t += 50;
      auto e = d.update(pressed, t);
      if (e.kind != Kind::NONE)
        ev.push_back({t, e});
    }
  }
  int count(Kind k) const {
    int n = 0;
    for (auto &e : ev)
      n += e.second.kind == k;
    return n;
  }
  const ButtonDecoder::Event *first(Kind k) const {
    for (auto &e : ev)
      if (e.second.kind == k)
        return &e.second;
    return nullptr;
  }
  uint32_t first_t(Kind k) const {
    for (auto &e : ev)
      if (e.second.kind == k)
        return e.first;
    return 0;
  }
};

static void test_clicks() {
  for (int n = 1; n <= 4; n++) {
    Sim s;
    s.run(false, 5000);
    uint32_t last_release = 0;
    for (int i = 0; i < n; i++) {
      s.run(true, 200);
      last_release = s.t;
      s.run(false, 300);
    }
    s.run(false, 2000);
    CHECK(s.count(Kind::CLICKS) == 1);
    CHECK(s.first(Kind::CLICKS) && s.first(Kind::CLICKS)->clicks == n);
    // Dispatched 1 s after the last release (within one tick).
    const uint32_t dt = s.first_t(Kind::CLICKS) - last_release;
    CHECK(dt >= 1000 && dt <= 1050);
    CHECK(s.count(Kind::LONG_RELEASE) == 0);
  }
  // Two separate single clicks (gap > 1 s) are two events.
  Sim s;
  s.run(false, 4000);
  s.run(true, 100);
  s.run(false, 1500);
  s.run(true, 100);
  s.run(false, 1500);
  CHECK(s.count(Kind::CLICKS) == 2);
}

static void test_long_press() {
  // 6 s hold after >= 3 s idle: mark 5 s while held, LONG_RELEASE on release, no click.
  Sim s;
  s.run(false, 4000);
  s.run(true, 6000);
  CHECK(s.count(Kind::HOLD_REACHED) == 1);
  CHECK(s.first(Kind::HOLD_REACHED)->held_ms == 5000);
  CHECK(s.count(Kind::LONG_RELEASE) == 0);  // nothing until release
  s.run(false, 2000);
  CHECK(s.count(Kind::LONG_RELEASE) == 1);
  CHECK(s.first(Kind::LONG_RELEASE)->held_ms >= 6000);
  CHECK(s.count(Kind::CLICKS) == 0);

  // 31 s hold: both marks, one release event with >= 30 s.
  Sim s2;
  s2.run(false, 4000);
  s2.run(true, 31000);
  s2.run(false, 100);
  CHECK(s2.count(Kind::HOLD_REACHED) == 2);
  CHECK(s2.count(Kind::LONG_RELEASE) == 1);
  CHECK(s2.first(Kind::LONG_RELEASE)->held_ms >= 30000);

  // 2 s press: not a click, a (short) long release; caller ignores < 5 s.
  Sim s3;
  s3.run(false, 4000);
  s3.run(true, 2000);
  s3.run(false, 2000);
  CHECK(s3.count(Kind::CLICKS) == 0);
  CHECK(s3.count(Kind::LONG_RELEASE) == 1);
  CHECK(s3.first(Kind::LONG_RELEASE)->held_ms < 5000);
}

static void test_idle_precondition() {
  // Released only 1 s before a 6 s hold: long press does not count, and no marks.
  Sim s;
  s.run(false, 4000);
  s.run(true, 100);   // a click...
  s.run(false, 1000); // ...whose dispatch is pre-empted by the next press
  s.run(true, 6000);
  s.run(false, 2000);
  CHECK(s.count(Kind::LONG_RELEASE) == 0);
  CHECK(s.count(Kind::HOLD_REACHED) == 0);
  // The long press cancelled the pending click.
  CHECK(s.count(Kind::CLICKS) == 0);

  // Held at boot: never a long press, never a click.
  ButtonDecoder d;
  d.set_hold_marks(5000);
  d.update(true, 0);
  bool any = false;
  for (uint32_t t = 50; t <= 40000; t += 50)
    any |= d.update(true, t).kind != Kind::NONE;
  for (uint32_t t = 40050; t <= 45000; t += 50)
    any |= d.update(false, t).kind != Kind::NONE;
  CHECK(!any);

  // Boot counts as idle: a press 3 s after the first sample is a valid long press.
  Sim s2;
  s2.run(false, 3000);
  s2.run(true, 5500);
  s2.run(false, 100);
  CHECK(s2.count(Kind::LONG_RELEASE) == 1);
}

static void test_cloud_error() {
  CloudErrorTracker c;
  CHECK(!c.update(true, false, 0));
  CHECK(!c.update(true, false, 59000));
  CHECK(c.update(true, false, 60000));
  CHECK(!c.update(true, true, 61000));   // connected clears it
  CHECK(!c.update(true, false, 100000)); // 39 s down
  CHECK(c.update(true, false, 121000));
  CHECK(!c.update(false, false, 122000)); // uplink switched off: not an error
  CHECK(!c.update(true, false, 150000));
}

static void test_leds() {
  LedLogic l;
  LedInputs in;
  // No network: blue off; no faults: green heartbeat toggles every 1 s.
  int toggles = 0;
  bool last_green = l.update(in, 0).green;
  for (uint32_t t = 100; t <= 10000; t += 100) {
    auto o = l.update(in, t);
    CHECK(!o.blue);
    CHECK(!o.red);
    toggles += o.green != last_green;
    last_green = o.green;
  }
  CHECK(toggles == 10);

  // Faults: red on, green off.
  in.faults = true;
  auto o = l.update(in, 10100);
  CHECK(o.red && !o.green);
  in.faults = false;

  // Network + cloud: solid blue.
  in.network = true;
  in.cloud = true;
  for (uint32_t t = 10200; t <= 20000; t += 100)
    CHECK(l.update(in, t).blue);

  // Cloud lost at 20 s: solid until 80 s, then 3 s off / 1 s on.
  in.cloud = false;
  for (uint32_t t = 20100; t < 80000; t += 100)
    CHECK(l.update(in, t).blue);
  int on = 0, total = 0;
  for (uint32_t t = 80100; t < 80100 + 40000; t += 100) {
    on += l.update(in, t).blue;
    total++;
  }
  CHECK(on == total / 4);
  CHECK(!l.update(in, 80100 + 40000 + 100).blue);  // 0.1 s into a new period: off

  // Network drop resets the 60 s window.
  in.network = false;
  CHECK(!l.update(in, 130000).blue);
  in.network = true;
  CHECK(l.update(in, 130100).blue);
  CHECK(l.update(in, 189900).blue);

  // Config mode (fallback AP up): 200 ms blink regardless of the rest.
  in.config_mode = true;
  toggles = 0;
  bool last = l.update(in, 200000).blue;
  for (uint32_t t = 200100; t <= 202000; t += 100) {
    bool b = l.update(in, t).blue;
    toggles += b != last;
    last = b;
  }
  CHECK(toggles == 10);
}

int main() {
  test_clicks();
  test_long_press();
  test_idle_precondition();
  test_cloud_error();
  test_leds();
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
