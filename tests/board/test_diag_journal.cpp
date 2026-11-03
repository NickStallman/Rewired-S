// Host tests for components/sungrow_board/diag_journal.h (reset journal, log tail ring, verdicts).
// Build + run: `make -C tests/board`.
#include <cstdio>
#include <cstring>
#include <string>

#include "diag_journal.h"

using namespace esphome::sungrow_board::diag;

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

static bool contains(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

static void test_journal_header() {
  Journal j;
  std::memset(&j, 0xA5, sizeof(j));  // RAM after a cold power-on: random
  CHECK(!journal_valid(j));
  journal_begin(j, 7, "0x12345678 2026-10-21 09:00:00");
  CHECK(journal_valid(j));
  CHECK(j.boot_count == 7);
  CHECK(j.pad[0] == 0xA5);  // the pad is not touched
  CHECK(j.crash.magic == 0 && j.stall.magic == 0 && j.hb.uptime_ms == 0);
  // Heartbeat / stall / crash fields are outside the header check.
  j.hb.uptime_ms = 12345;
  j.crash.magic = CRASH_MAGIC;
  CHECK(journal_valid(j));
  // Header corruption is caught.
  j.boot_count = 8;
  CHECK(!journal_valid(j));
  journal_begin(j, 8, "b");
  j.build[3] = 'x';
  CHECK(!journal_valid(j));
  // An over-long build id is cut and stays NUL-terminated.
  std::string longb(100, 'b');
  journal_begin(j, 1, longb.c_str());
  CHECK(journal_valid(j));
  CHECK(std::strlen(j.build) == BUILD_ID_LEN - 1);
}

static void test_ring() {
  static constexpr size_t CAP = 64;
  RingHeader h;
  char data[CAP];
  std::memset(&h, 0x5A, sizeof(h));
  CHECK(!ring_valid(h, CAP));
  CHECK(ring_text(h, data, CAP).empty());
  ring_begin(h, CAP, 3);
  CHECK(ring_valid(h, CAP));
  CHECK(!ring_valid(h, CAP + 1));  // a build with another ring size ignores it
  CHECK(ring_text(h, data, CAP).empty());
  ring_append_line(h, data, CAP, "hello", 5);
  ring_append_line(h, data, CAP, "world", 5);
  CHECK(ring_text(h, data, CAP) == "hello\nworld\n");
  // Wrap: only whole lines survive, newest last.
  for (int i = 0; i < 20; i++) {
    char b[16];
    int n = std::snprintf(b, sizeof(b), "line %02d", i);
    ring_append_line(h, data, CAP, b, n);
  }
  std::string t = ring_text(h, data, CAP);
  CHECK(t.size() <= CAP);
  CHECK(t.substr(t.size() - 8) == "line 19\n");
  CHECK(t.rfind("line ", 0) == 0);  // starts at a line boundary
  CHECK(!contains(t, "hello"));
  CHECK(h.used == CAP && h.head < CAP);
  // Corrupt indices are rejected.
  RingHeader bad = h;
  bad.head = CAP;
  CHECK(!ring_valid(bad, CAP));
  bad = h;
  bad.used = CAP + 1;
  CHECK(!ring_valid(bad, CAP));
  // A line longer than the ring keeps its tail.
  ring_begin(h, CAP, 4);
  std::string big(300, 'z');
  big += "END";
  ring_append(h, data, CAP, big.data(), big.size());
  t = ring_text(h, data, CAP);
  CHECK(h.used == CAP - 1);
  CHECK(t.substr(t.size() - 3) == "END");
}

static void test_line_sanitising() {
  static constexpr size_t CAP = 256;
  RingHeader h;
  char data[CAP];
  ring_begin(h, CAP, 1);
  const char msg[] = "\033[0;33m[W][tag:12]: warn\tx\r\n\033[0m";
  ring_append_line(h, data, CAP, msg, sizeof(msg) - 1);
  CHECK(ring_text(h, data, CAP) == "[W][tag:12]: warn x\n");
  ring_begin(h, CAP, 1);
  ring_append_line(h, data, CAP, "abcdefghij", 10, 4);
  CHECK(ring_text(h, data, CAP) == "abcd\n");
}

static Journal make_prev() {
  Journal j;
  std::memset(&j, 0, sizeof(j));
  journal_begin(j, 5, "0xaaaaaaaa 2026-10-21 08:00:00");
  j.hb.uptime_ms = 92300;
  j.hb.loop_ms = 92290;
  j.hb.max_kick_gap_ms = 512;
  return j;
}

static void test_summaries() {
  const char *cur = "0xaaaaaaaa 2026-10-21 08:00:00";
  // Power-on with no journal: power loss, or a supervisor reset that cleared RTC memory.
  std::string s = summarize(RR_POWERON, nullptr, cur);
  CHECK(contains(s, "RAM lost"));
  // Power-on with RAM retained: the supervisor / EN pin.
  Journal j = make_prev();
  s = summarize(RR_POWERON, &j, cur);
  CHECK(contains(s, "Supervisor/EN reset after 92 s (RAM retained)"));
  CHECK(contains(s, "max kick gap 512 ms"));
  CHECK(!contains(s, "loop stopped"));
  CHECK(!contains(s, "[build"));
  // ... with a hung loop and a kick stall.
  j.hb.loop_ms = 61000;
  j.stall.magic = STALL_MAGIC;
  j.stall.count = 1;
  j.stall.at_ms = 91500;
  j.stall.gap_ms = 1800;
  std::strcpy(j.stall.task[0], "wifi");
  std::strcpy(j.stall.task[1], "loopTask");
  s = summarize(RR_POWERON, &j, cur);
  CHECK(contains(s, "loop stopped at 61 s"));
  CHECK(contains(s, "kick stalled 1800 ms at 91.5 s (core 0: wifi, core 1: loopTask)"));
  // Crash with a record.
  j = make_prev();
  j.crash.magic = CRASH_MAGIC;
  j.crash.at_ms = 65000;
  j.crash.kind = CRASH_EXCEPTION;
  j.crash.exccause = 28;
  j.crash.pc = 0x4200ABCD;
  j.crash.excvaddr = 0;
  std::strcpy(j.crash.task, "loopTask");
  s = summarize(RR_PANIC, &j, cur);
  CHECK(contains(s, "Crash after 65 s: LoadProhibited (cause 28) at 0x4200ABCD (addr 0x00000000), task loopTask, core 0"));
  // abort() with details; another build wrote the record.
  j.crash.kind = CRASH_ABORT;
  std::strcpy(j.crash.detail, "abort() was called at PC 0x420E1234 on core 1");
  s = summarize(RR_PANIC, &j, "0xbbbbbbbb 2026-10-21 09:00:00");
  CHECK(contains(s, "abort() at 0x4200ABCD"));
  CHECK(contains(s, ": abort() was called at PC 0x420E1234"));
  CHECK(contains(s, "[build 0xaaaaaaaa]"));
  // Task watchdog / interrupt-level panic.
  j.crash.kind = CRASH_TASK_WDT;
  j.crash.detail[0] = '\0';
  CHECK(contains(summarize(RR_TASK_WDT, &j, cur), "Task watchdog at"));
  j.crash.kind = CRASH_PSEUDO;
  j.crash.exccause = 5;
  CHECK(contains(summarize(RR_INT_WDT, &j, cur), "Interrupt watchdog (core 0)"));
  // Crash without a record (e.g. a build without the journal).
  CHECK(contains(summarize(RR_PANIC, nullptr, cur), "Crash (panic), no record"));
  // Software restart, brownout.
  j = make_prev();
  CHECK(summarize(RR_SW, &j, cur) == "Software restart after 92 s");
  CHECK(summarize(RR_BROWNOUT, &j, cur) == "Brownout after 92 s");
  // Always a valid text sensor state.
  j.crash.magic = CRASH_MAGIC;
  j.stall.magic = STALL_MAGIC;
  j.hb.loop_ms = 0;
  std::memset(j.crash.detail, 'd', DETAIL_LEN);  // no NUL: must not over-read
  std::memset(j.stall.task, 't', sizeof(j.stall.task));
  CHECK(summarize(RR_POWERON, &j, "other").size() <= 255);
}

static void test_journal_text() {
  Journal j = make_prev();
  j.crash.magic = CRASH_MAGIC;
  j.crash.depth = 3;
  j.crash.bt[0] = 0x4037541C;
  j.crash.bt[1] = 0x4200AAAA;
  j.crash.bt[2] = 0x4200BBBB;
  std::string t = journal_text(j);
  CHECK(contains(t, "boot #5, build 0xaaaaaaaa"));
  CHECK(contains(t, "backtrace: 0x4037541C 0x4200AAAA 0x4200BBBB"));
  j.crash.depth = 200;  // corrupt depth is clamped
  CHECK(backtrace_text(j.crash).size() == BT_DEPTH * 11 - 1);
}

static void test_coredump() {
  const uint32_t fp = coredump_fingerprint(0x12000, 0xDEADBEEF, 0x4200ABCD);
  CHECK(fp != 0);
  CHECK(fp != coredump_fingerprint(0x12000, 0xDEADBEEE, 0x4200ABCD));  // another crash, another image
  CHECK(fp == coredump_fingerprint(0x12000, 0xDEADBEEF, 0x4200ABCD));

  Journal prev;
  journal_begin(prev, 12, "b");
  CHECK(prev.coredump.fingerprint == 0 && prev.coredump.boot == 0);

  // Crash reset with the previous journal: the dump is the previous boot's, numbered.
  CoredumpDecision d = coredump_decide(RR_PANIC, &prev, fp);
  CHECK(!d.erase && d.previous && d.boot == 12);
  CHECK(coredump_owner_text(d) == "boot #12");
  // Crash reset, journal lost: the previous boot, number unknown.
  d = coredump_decide(RR_TASK_WDT, nullptr, fp);
  CHECK(!d.erase && d.previous && d.boot == 0);
  CHECK(coredump_owner_text(d) == "the previous boot");
  // Other reset with no note: an older dump, reported once.
  d = coredump_decide(RR_POWERON, nullptr, fp);
  CHECK(!d.erase && !d.previous && d.boot == 0);
  CHECK(coredump_owner_text(d) == "an earlier boot");
  d = coredump_decide(RR_SW, &prev, fp);
  CHECK(!d.erase && !d.previous);

  // The reporting boot notes the dump; the next boot erases it, whatever its reset reason.
  prev.coredump.fingerprint = fp;
  prev.coredump.boot = 11;
  CHECK(journal_valid(prev));  // the note is outside the header check
  d = coredump_decide(RR_SW, &prev, fp);
  CHECK(d.erase && d.boot == 11);
  d = coredump_decide(RR_PANIC, &prev, fp);  // a crash whose dump was not written: the old one goes
  CHECK(d.erase);
  // A new crash overwrote the dump: reported, not erased.
  d = coredump_decide(RR_PANIC, &prev, coredump_fingerprint(0x13000, 1, 2));
  CHECK(!d.erase && d.previous && d.boot == 12);

  CHECK(contains(journal_text(prev), "reported the core dump of boot #11 (the next boot erases it)"));
  prev.coredump.boot = 0;
  CHECK(contains(journal_text(prev), "reported the core dump of an earlier boot"));
  journal_begin(prev, 13, "b");  // a new boot starts without a note
  CHECK(prev.coredump.fingerprint == 0);
  CHECK(!contains(journal_text(prev), "core dump"));
}

static void test_crash_class_and_reset_trigger() {
  Journal prev{};
  journal_begin(prev, 5, "b");
  // Clean restarts and power events ask for no upload.
  CHECK(reset_trigger(RR_SW, &prev) == nullptr);
  CHECK(reset_trigger(RR_BROWNOUT, &prev) == nullptr);
  CHECK(reset_trigger(RR_POWERON, nullptr) == nullptr);  // RAM lost: power loss
  // Watchdog resets.
  CHECK(std::string(reset_trigger(RR_TASK_WDT, &prev)) == "watchdog");
  CHECK(std::string(reset_trigger(RR_INT_WDT, nullptr)) == "watchdog");
  // A panic without a record is a crash; with a task-watchdog record it is a watchdog.
  CHECK(std::string(reset_trigger(RR_PANIC, nullptr)) == "crash");
  prev.crash.magic = CRASH_MAGIC;
  prev.crash.kind = CRASH_TASK_WDT;
  CHECK(std::string(reset_trigger(RR_PANIC, &prev)) == "watchdog");
  prev.crash.kind = CRASH_EXCEPTION;
  prev.crash.exccause = 28;
  CHECK(std::string(reset_trigger(RR_PANIC, &prev)) == "crash");
  CHECK(crash_class(prev.crash) == "LoadProhibited");
  prev.crash.kind = CRASH_ABORT;
  CHECK(crash_class(prev.crash) == "abort");
  prev.crash.kind = CRASH_PSEUDO;
  prev.crash.exccause = 5;
  CHECK(crash_class(prev.crash) == "Interrupt watchdog (core 0)");
  prev.crash.kind = CRASH_TASK_WDT;
  CHECK(crash_class(prev.crash) == "task_wdt");
  // Supervisor / EN resets with the RAM kept: crash record first, then stall, else watchdog.
  CHECK(std::string(reset_trigger(RR_POWERON, &prev)) == "watchdog");
  prev.crash.kind = CRASH_ABORT;
  CHECK(std::string(reset_trigger(RR_EXT, &prev)) == "crash");
  prev.crash.magic = 0;
  prev.stall.magic = STALL_MAGIC;
  CHECK(std::string(reset_trigger(RR_POWERON, &prev)) == "stall");
  prev.stall.magic = 0;
  CHECK(std::string(reset_trigger(RR_POWERON, &prev)) == "watchdog");
}

int main() {
  test_journal_header();
  test_ring();
  test_line_sanitising();
  test_summaries();
  test_journal_text();
  test_coredump();
  test_crash_class_and_reset_trigger();
  std::printf("%s: %d checks, %d failed\n", g_fail ? "FAILED" : "OK", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
