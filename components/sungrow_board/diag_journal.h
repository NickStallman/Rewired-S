#pragma once
// Reset journal and log tail: the evidence a reset leaves for the next boot.
//
// Both live in RTC slow memory (RTC_NOINIT, see diag.cpp), which survives every software reset
// and panic, and is not initialised by the bootloader or IDF startup. A power-on reset that still
// finds a valid journal had its RAM kept, so the chip was reset through its enable pin with power
// present ("RAM retained"). On the WiNet-S2 the supervisor's reset clears RTC memory, so there a
// supervisor reset reads like a power loss ("RAM lost").
//
// Writers:
//  - Heartbeat: the supervisor kick timer (500 ms, esp_timer task) and the main loop (loop_ms,
//    Wi-Fi). It keeps running while the loop hangs, so "loop stopped at X, kicks ran until Y" shows.
//  - StallRecord: an ISR-dispatched esp_timer that notices the periodic kick falling behind.
//  - CrashRecord: the panic entry wrapper (IRAM, before IDF's panic handler runs).
//  - LogRing: the ESPHome logger callback.
// The layout is fixed and versioned, so a different build reads it as long as the RTC address
// matches (it is the first .rtc_noinit object; see the symbol check in diag.cpp).
//
// Pure C++ (no IDF headers): host-tested by tests/board/test_diag_journal.cpp.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome::sungrow_board::diag {

static constexpr uint32_t JOURNAL_MAGIC = 0x4A444753u;  // "SGDJ"
static constexpr uint32_t STALL_MAGIC = 0x53534753u;    // "SGSS"
static constexpr uint32_t CRASH_MAGIC = 0x52434753u;    // "SGCR"
static constexpr uint32_t RING_MAGIC = 0x4C524753u;     // "SGRL"
static constexpr uint16_t JOURNAL_VERSION = 2;
static constexpr uint16_t RING_VERSION = 1;
static constexpr size_t BUILD_ID_LEN = 48;
static constexpr size_t TASK_NAME_LEN = 16;
static constexpr size_t BT_DEPTH = 8;
static constexpr size_t DETAIL_LEN = 48;
/// A kick gap above this counts as a stall (the periodic kick runs every 500 ms).
static constexpr uint32_t STALL_THRESHOLD_MS = 1000;
/// The main loop counts as stopped when its last pass is this much older than the last heartbeat.
static constexpr uint32_t LOOP_STOPPED_MS = 3000;

/// Mirrors esp_reset_reason_t (esp_system.h) so this header needs no IDF include.
enum ResetReason : int {
  RR_UNKNOWN = 0,
  RR_POWERON = 1,
  RR_EXT = 2,
  RR_SW = 3,
  RR_PANIC = 4,
  RR_INT_WDT = 5,
  RR_TASK_WDT = 6,
  RR_WDT = 7,
  RR_DEEPSLEEP = 8,
  RR_BROWNOUT = 9,
  RR_SDIO = 10,
  RR_USB = 11,
  RR_JTAG = 12,
  RR_EFUSE = 13,
  RR_PWR_GLITCH = 14,
  RR_CPU_LOCKUP = 15,
};

enum CrashKind : uint8_t {
  CRASH_EXCEPTION = 0,  ///< CPU exception (exccause = Xtensa EXCCAUSE)
  CRASH_ABORT = 1,      ///< abort()/assert/stack overflow (details in CrashRecord::detail)
  CRASH_TASK_WDT = 2,   ///< task watchdog
  CRASH_PSEUDO = 3,     ///< interrupt-level panic: exccause = PANIC_RSN_* (int WDT, cache error, ...)
};

struct Heartbeat {
  uint32_t uptime_ms;        ///< last heartbeat from the kick timer
  uint32_t loop_ms;          ///< uptime of the last main-loop pass
  uint32_t heap_free;        ///< internal heap free (bytes)
  uint32_t heap_min;         ///< internal heap minimum ever free (bytes)
  uint32_t psram_total;      ///< PSRAM in the heap (bytes); 0 = none (absent or failed to start)
  uint32_t psram_free;       ///< PSRAM heap free (bytes)
  uint32_t kicks;            ///< periodic supervisor kicks this boot
  uint32_t max_kick_gap_ms;  ///< longest gap between periodic kicks this boot
  uint32_t long_kick_gaps;   ///< periodic kick gaps above STALL_THRESHOLD_MS
  int8_t wifi_rssi;          ///< dBm, 0 = unknown
  uint8_t wifi_connected;
  uint8_t reserved[2];
};

struct StallRecord {
  uint32_t magic;     ///< STALL_MAGIC once a stall was seen this boot
  uint32_t count;     ///< stalls this boot
  uint32_t at_ms;     ///< uptime when the latest stall was detected
  uint32_t gap_ms;    ///< how long the kick had not run (kept growing until it ran again)
  uint8_t cache_off;  ///< flash cache was disabled (a flash operation) when detected
  uint8_t reserved[3];
  char task[2][TASK_NAME_LEN];  ///< tasks running on core 0 / core 1 when detected
};

struct CrashRecord {
  uint32_t magic;  ///< CRASH_MAGIC once recorded this boot
  uint32_t at_ms;
  uint8_t kind;    ///< CrashKind
  uint8_t core;
  uint8_t depth;   ///< valid entries in bt
  uint8_t reserved;
  uint32_t exccause;
  uint32_t pc;
  uint32_t excvaddr;
  uint32_t bt[BT_DEPTH];
  char task[TASK_NAME_LEN];
  char detail[DETAIL_LEN];  ///< abort details, when the text was readable
};

/// The stored core dump this boot reported. The next boot erases a dump it finds noted here, so
/// each dump is reported by exactly one boot and stays downloadable for the whole of that boot.
struct CoredumpNote {
  uint32_t fingerprint;  ///< coredump_fingerprint() of the reported image; 0 = none reported
  uint32_t boot;         ///< boot the dump belongs to (its crash ended that boot); 0 = not known
};

struct Journal {
  /// Keeps the record clear of a build without the journal: its RTC data starts at the segment
  /// base and would overwrite the header after a rollback.
  uint8_t pad[64];
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t boot_count;
  char build[BUILD_ID_LEN];  ///< "0x<config hash> <build time>"
  uint32_t header_check;     ///< FNV-1a over magic..build
  Heartbeat hb;
  StallRecord stall;
  CrashRecord crash;
  CoredumpNote coredump;
};

struct RingHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t cap;
  uint32_t boot_count;
  uint32_t check;  ///< FNV-1a over magic..boot_count
  uint32_t head;   ///< next write offset
  uint32_t used;   ///< bytes held (<= cap)
};

inline uint32_t fnv1a(const void *data, size_t len, uint32_t h = 2166136261u) {
  const auto *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

inline uint32_t journal_header_check(const Journal &j) {
  return fnv1a(&j.magic, offsetof(Journal, header_check) - offsetof(Journal, magic));
}

inline bool journal_valid(const Journal &j) {
  return j.magic == JOURNAL_MAGIC && j.version == JOURNAL_VERSION && j.size == sizeof(Journal) &&
         j.header_check == journal_header_check(j) && j.build[BUILD_ID_LEN - 1] == '\0';
}

/// Start the journal of a new boot (keeps the leading pad untouched).
inline void journal_begin(Journal &j, uint32_t boot_count, const char *build) {
  std::memset(reinterpret_cast<uint8_t *>(&j) + offsetof(Journal, magic), 0, sizeof(Journal) - offsetof(Journal, magic));
  j.magic = JOURNAL_MAGIC;
  j.version = JOURNAL_VERSION;
  j.size = static_cast<uint16_t>(sizeof(Journal));
  j.boot_count = boot_count;
  std::snprintf(j.build, sizeof(j.build), "%s", build != nullptr ? build : "");
  j.header_check = journal_header_check(j);
}

inline uint32_t ring_header_check(const RingHeader &h) {
  return fnv1a(&h.magic, offsetof(RingHeader, check) - offsetof(RingHeader, magic));
}

inline bool ring_valid(const RingHeader &h, size_t cap) {
  return h.magic == RING_MAGIC && h.version == RING_VERSION && h.cap == cap && h.check == ring_header_check(h) &&
         h.head < cap && h.used <= cap;
}

inline void ring_begin(RingHeader &h, size_t cap, uint32_t boot_count) {
  h.magic = RING_MAGIC;
  h.version = RING_VERSION;
  h.cap = static_cast<uint16_t>(cap);
  h.boot_count = boot_count;
  h.check = ring_header_check(h);
  h.head = 0;
  h.used = 0;
}

/// Append bytes (oldest bytes are overwritten once full).
inline void ring_append(RingHeader &h, char *data, size_t cap, const char *s, size_t n) {
  if (n >= cap) {  // keep the tail
    s += n - (cap - 1);
    n = cap - 1;
  }
  size_t head = h.head;
  for (size_t i = 0; i < n; i++) {
    data[head] = s[i];
    head = head + 1 == cap ? 0 : head + 1;
  }
  h.head = static_cast<uint32_t>(head);
  h.used = h.used + n > cap ? static_cast<uint32_t>(cap) : static_cast<uint32_t>(h.used + n);
}

/// Append one log line: ANSI colour sequences and CR dropped, other control characters become
/// spaces, cut at max_len, '\n' added.
inline void ring_append_line(RingHeader &h, char *data, size_t cap, const char *msg, size_t len, size_t max_len = 200) {
  char line[256];
  if (max_len > sizeof(line) - 1)
    max_len = sizeof(line) - 1;
  size_t o = 0;
  for (size_t i = 0; i < len && msg[i] != '\0' && o < max_len; i++) {
    const char c = msg[i];
    if (c == '\033') {  // ESC [ ... letter
      i++;
      if (i < len && msg[i] == '[')
        while (i + 1 < len && !((msg[i + 1] >= 'A' && msg[i + 1] <= 'Z') || (msg[i + 1] >= 'a' && msg[i + 1] <= 'z')))
          i++;
      i++;  // the final letter
      continue;
    }
    if (c == '\r' || c == '\n')
      continue;
    line[o++] = static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
  }
  line[o++] = '\n';
  ring_append(h, data, cap, line, o);
}

/// The ring's text, oldest first. When it has wrapped, the partial first line is dropped.
inline std::string ring_text(const RingHeader &h, const char *data, size_t cap) {
  std::string out;
  if (!ring_valid(h, cap) || h.used == 0)
    return out;
  out.reserve(h.used);
  size_t start = (h.head + cap - h.used) % cap;
  for (size_t i = 0; i < h.used; i++)
    out += data[(start + i) % cap];
  if (h.used == cap) {
    size_t nl = out.find('\n');
    out.erase(0, nl == std::string::npos ? out.size() : nl + 1);
  }
  return out;
}

inline const char *xtensa_exccause_name(uint32_t c) {
  switch (c) {
    case 0:
      return "IllegalInstruction";
    case 2:
      return "InstructionFetchError";
    case 3:
      return "LoadStoreError";
    case 6:
      return "IntegerDivideByZero";
    case 9:
      return "LoadStoreAlignment";
    case 20:
      return "InstFetchProhibited";
    case 28:
      return "LoadProhibited";
    case 29:
      return "StoreProhibited";
    default:
      return "Exception";
  }
}

/// PANIC_RSN_* (xtensa esp_private/panic_reason.h).
inline const char *panic_rsn_name(uint32_t r) {
  switch (r) {
    case 1:
      return "Debug exception";
    case 2:
      return "Double exception";
    case 3:
      return "Kernel exception";
    case 4:
      return "Coprocessor exception";
    case 5:
      return "Interrupt watchdog (core 0)";
    case 6:
      return "Interrupt watchdog (core 1)";
    case 7:
      return "Cache error";
    default:
      return "Interrupt-level panic";
  }
}

inline const char *reset_reason_name(int r) {
  switch (r) {
    case RR_POWERON:
      return "power-on";
    case RR_EXT:
      return "external pin";
    case RR_SW:
      return "software restart";
    case RR_PANIC:
      return "panic";
    case RR_INT_WDT:
      return "interrupt watchdog";
    case RR_TASK_WDT:
      return "task watchdog";
    case RR_WDT:
      return "other watchdog";
    case RR_DEEPSLEEP:
      return "deep sleep";
    case RR_BROWNOUT:
      return "brownout";
    case RR_PWR_GLITCH:
      return "power glitch";
    case RR_CPU_LOCKUP:
      return "CPU lockup";
    default:
      return "unknown";
  }
}

/// One-line description of a crash record ("LoadProhibited at 0x4200ABCD (addr 0x00000000), task
/// loopTask, core 0").
inline std::string crash_text(const CrashRecord &c) {
  char buf[200];
  const char *what;
  char tmp[48];
  switch (c.kind) {
    case CRASH_ABORT:
      what = "abort()";
      break;
    case CRASH_TASK_WDT:
      what = "Task watchdog";
      break;
    case CRASH_PSEUDO:
      what = panic_rsn_name(c.exccause);
      break;
    default:
      std::snprintf(tmp, sizeof(tmp), "%s (cause %u)", xtensa_exccause_name(c.exccause), static_cast<unsigned>(c.exccause));
      what = tmp;
      break;
  }
  char task[TASK_NAME_LEN + 1];
  std::memcpy(task, c.task, TASK_NAME_LEN);
  task[TASK_NAME_LEN] = '\0';
  int n = std::snprintf(buf, sizeof(buf), "%s at 0x%08X", what, static_cast<unsigned>(c.pc));
  if (c.kind == CRASH_EXCEPTION && (c.exccause == 28 || c.exccause == 29 || c.exccause == 9 || c.exccause == 3))
    n += std::snprintf(buf + n, sizeof(buf) - n, " (addr 0x%08X)", static_cast<unsigned>(c.excvaddr));
  std::snprintf(buf + n, sizeof(buf) - n, ", task %s, core %u", task[0] ? task : "?", static_cast<unsigned>(c.core));
  std::string s = buf;
  char det[DETAIL_LEN + 1];
  std::memcpy(det, c.detail, DETAIL_LEN);
  det[DETAIL_LEN] = '\0';
  if (det[0] != '\0') {
    s += ": ";
    s += det;
  }
  return s;
}

inline std::string backtrace_text(const CrashRecord &c) {
  std::string s;
  char b[16];
  const unsigned d = c.depth > BT_DEPTH ? BT_DEPTH : c.depth;
  for (unsigned i = 0; i < d; i++) {
    std::snprintf(b, sizeof(b), "%s0x%08X", i ? " " : "", static_cast<unsigned>(c.bt[i]));
    s += b;
  }
  return s;
}

inline std::string stall_text(const StallRecord &s) {
  char t0[TASK_NAME_LEN + 1], t1[TASK_NAME_LEN + 1];
  std::memcpy(t0, s.task[0], TASK_NAME_LEN);
  std::memcpy(t1, s.task[1], TASK_NAME_LEN);
  t0[TASK_NAME_LEN] = t1[TASK_NAME_LEN] = '\0';
  char buf[160];
  std::snprintf(buf, sizeof(buf), "kick stalled %u ms at %u.%u s (core 0: %s, core 1: %s%s)%s",
                static_cast<unsigned>(s.gap_ms), static_cast<unsigned>(s.at_ms / 1000),
                static_cast<unsigned>(s.at_ms % 1000 / 100), t0[0] ? t0 : "?", t1[0] ? t1 : "?",
                s.cache_off ? ", flash cache off" : "", s.count > 1 ? " [repeated]" : "");
  return buf;
}

/// Summary of how the previous boot ended, from this boot's reset reason and the previous boot's
/// journal (nullptr when no valid journal survived). <= 255 characters (a text sensor state).
inline std::string summarize(int reason, const Journal *prev, const char *current_build) {
  std::string s;
  char buf[160];
  const Heartbeat *hb = prev != nullptr ? &prev->hb : nullptr;
  auto secs = [](uint32_t ms) { return static_cast<unsigned>(ms / 1000); };
  const bool crash = prev != nullptr && prev->crash.magic == CRASH_MAGIC;
  switch (reason) {
    case RR_PANIC:
    case RR_INT_WDT:
    case RR_TASK_WDT:
    case RR_CPU_LOCKUP:
      if (crash) {
        std::snprintf(buf, sizeof(buf), "Crash after %u s: ", secs(prev->crash.at_ms));
        s = buf;
        s += crash_text(prev->crash);
      } else {
        std::snprintf(buf, sizeof(buf), "Crash (%s), no record", reset_reason_name(reason));
        s = buf;
        if (hb != nullptr) {
          std::snprintf(buf, sizeof(buf), "; last heartbeat at %u s", secs(hb->uptime_ms));
          s += buf;
        }
      }
      break;
    case RR_SW:
      if (hb != nullptr)
        std::snprintf(buf, sizeof(buf), "Software restart after %u s", secs(hb->uptime_ms));
      else
        std::snprintf(buf, sizeof(buf), "Software restart");
      s = buf;
      break;
    case RR_POWERON:
    case RR_EXT:
      if (hb != nullptr) {
        std::snprintf(buf, sizeof(buf), "Supervisor/EN reset after %u s (RAM retained)", secs(hb->uptime_ms));
        s = buf;
        if (hb->loop_ms + LOOP_STOPPED_MS < hb->uptime_ms) {
          std::snprintf(buf, sizeof(buf), "; loop stopped at %u s", secs(hb->loop_ms));
          s += buf;
        }
        if (prev->stall.magic == STALL_MAGIC) {
          s += "; ";
          s += stall_text(prev->stall);
        } else {
          std::snprintf(buf, sizeof(buf), "; max kick gap %u ms", static_cast<unsigned>(hb->max_kick_gap_ms));
          s += buf;
        }
        if (crash) {
          s += "; crash: ";
          s += crash_text(prev->crash);
        }
      } else {
        s = "Power-on reset, RAM lost (power loss, or the S2 supervisor)";
      }
      break;
    case RR_BROWNOUT:
    case RR_PWR_GLITCH:
      std::snprintf(buf, sizeof(buf), "%s", reason == RR_BROWNOUT ? "Brownout" : "Power glitch");
      s = buf;
      if (hb != nullptr) {
        std::snprintf(buf, sizeof(buf), " after %u s", secs(hb->uptime_ms));
        s += buf;
      }
      break;
    default:
      std::snprintf(buf, sizeof(buf), "Reset: %s", reset_reason_name(reason));
      s = buf;
      if (hb != nullptr) {
        std::snprintf(buf, sizeof(buf), " after %u s", secs(hb->uptime_ms));
        s += buf;
      }
      break;
  }
  if (prev != nullptr && current_build != nullptr && std::strncmp(prev->build, current_build, BUILD_ID_LEN) != 0) {
    s += " [build ";
    s += std::string(prev->build, strnlen(prev->build, BUILD_ID_LEN)).substr(0, 10);
    s += "]";
  }
  if (s.size() > 255)
    s.resize(255);
  return s;
}

inline bool reset_is_crash(int reason) {
  return reason == RR_PANIC || reason == RR_INT_WDT || reason == RR_TASK_WDT || reason == RR_CPU_LOCKUP;
}

/// Short class of a crash record with no addresses in it ("abort", "task_wdt", a panic reason
/// or an exception name): the error key usage reporting groups crashes by.
inline std::string crash_class(const CrashRecord &c) {
  switch (c.kind) {
    case CRASH_ABORT:
      return "abort";
    case CRASH_TASK_WDT:
      return "task_wdt";
    case CRASH_PSEUDO:
      return panic_rsn_name(c.exccause);
    default:
      return xtensa_exccause_name(c.exccause);
  }
}

/// Diagnostics-upload trigger implied by how the previous boot ended: "crash" (panic, CPU
/// lockup), "watchdog" (interrupt / task watchdog, or a supervisor reset with the RAM kept),
/// "stall" (a supervisor reset after a recorded kick stall), nullptr for a clean restart, a
/// power loss or a power fault. `prev` is the previous boot's journal, nullptr when none survived.
inline const char *reset_trigger(int reason, const Journal *prev) {
  const bool crash_rec = prev != nullptr && prev->crash.magic == CRASH_MAGIC;
  switch (reason) {
    case RR_INT_WDT:
    case RR_TASK_WDT:
      return "watchdog";
    case RR_PANIC:
    case RR_CPU_LOCKUP:
      return crash_rec && prev->crash.kind == CRASH_TASK_WDT ? "watchdog" : "crash";
    case RR_POWERON:
    case RR_EXT:
      if (prev == nullptr)
        return nullptr;
      if (crash_rec)
        return prev->crash.kind == CRASH_TASK_WDT ? "watchdog" : "crash";
      if (prev->stall.magic == STALL_MAGIC)
        return "stall";
      return "watchdog";
    default:
      return nullptr;
  }
}

/// Identity of a stored core dump image: its size, its trailing checksum word and the crashing PC.
/// Never 0 (0 marks "none reported" in CoredumpNote).
inline uint32_t coredump_fingerprint(uint32_t size, uint32_t checksum, uint32_t pc) {
  const uint32_t v[3] = {size, checksum, pc};
  const uint32_t h = fnv1a(v, sizeof(v));
  return h != 0 ? h : 1;
}

/// What a boot does with the stored core dump it finds.
struct CoredumpDecision {
  bool erase;     ///< the previous boot already reported this dump: erase it, report nothing
  bool previous;  ///< the dump was written by the crash that ended the previous boot
  uint32_t boot;  ///< boot the dump belongs to; 0 = not known
};

/// A dump noted in the previous boot's journal was reported there and is erased. Any other dump is
/// reported: after a crash reset it belongs to the previous boot (numbered when its journal
/// survived), otherwise to an earlier boot.
inline CoredumpDecision coredump_decide(int reason, const Journal *prev, uint32_t fingerprint) {
  if (prev != nullptr && prev->coredump.fingerprint != 0 && prev->coredump.fingerprint == fingerprint)
    return {true, false, prev->coredump.boot};
  if (reset_is_crash(reason))
    return {false, true, prev != nullptr ? prev->boot_count : 0};
  return {false, false, 0};
}

/// "boot #12", "the previous boot" or "an earlier boot".
inline std::string coredump_owner_text(const CoredumpDecision &d) {
  if (d.boot != 0) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "boot #%u", static_cast<unsigned>(d.boot));
    return buf;
  }
  return d.previous ? "the previous boot" : "an earlier boot";
}

/// Multi-line dump of a journal (boot log, /api/diag).
inline std::string journal_text(const Journal &j) {
  std::string s;
  char buf[200];
  const Heartbeat &h = j.hb;
  std::snprintf(buf, sizeof(buf), "boot #%u, build %s\n", static_cast<unsigned>(j.boot_count),
                std::string(j.build, strnlen(j.build, BUILD_ID_LEN)).c_str());
  s += buf;
  std::snprintf(buf, sizeof(buf),
                "last heartbeat %u ms, loop %u ms, internal heap %u free / %u min, PSRAM %u KB (%u KB free)\n",
                static_cast<unsigned>(h.uptime_ms), static_cast<unsigned>(h.loop_ms), static_cast<unsigned>(h.heap_free),
                static_cast<unsigned>(h.heap_min), static_cast<unsigned>(h.psram_total / 1024),
                static_cast<unsigned>(h.psram_free / 1024));
  s += buf;
  std::snprintf(buf, sizeof(buf), "kicks %u, max kick gap %u ms, %u long gaps, wifi %s %d dBm\n",
                static_cast<unsigned>(h.kicks), static_cast<unsigned>(h.max_kick_gap_ms),
                static_cast<unsigned>(h.long_kick_gaps), h.wifi_connected ? "up" : "down", h.wifi_rssi);
  s += buf;
  if (j.stall.magic == STALL_MAGIC) {
    s += "stall: " + stall_text(j.stall);
    std::snprintf(buf, sizeof(buf), ", %u this boot\n", static_cast<unsigned>(j.stall.count));
    s += buf;
  }
  if (j.crash.magic == CRASH_MAGIC) {
    std::snprintf(buf, sizeof(buf), "crash at %u ms: ", static_cast<unsigned>(j.crash.at_ms));
    s += buf;
    s += crash_text(j.crash);
    s += "\nbacktrace: " + backtrace_text(j.crash) + "\n";
  }
  if (j.coredump.fingerprint != 0) {
    const CoredumpDecision d{false, false, j.coredump.boot};
    s += "reported the core dump of " + coredump_owner_text(d) + " (the next boot erases it)\n";
  }
  return s;
}

}  // namespace esphome::sungrow_board::diag
