#pragma once
// Reset diagnostics (sungrow_board `diagnostics:`): the reset journal, the log tail and the boot
// report of how the previous boot ended. Data layout and classification: diag_journal.h.
//
// What runs where:
//  - SECONDARY init (priority 103, before the supervisor kick is armed): read the previous boot's
//    journal and reset reason, start this boot's journal.
//  - Panic entry (--wrap of panicHandler / xt_unhandled_exception, IRAM): switch the external
//    supervisor off and record the crash, before IDF's panic handler prints, writes the core dump
//    and restarts. Without the switch-off the supervisor fires during that slow path or the next
//    boot and the crash surfaces as a "power-on" reset with no record.
//  - Kick timer (500 ms): heartbeat. ISR-dispatched esp_timer (200 ms): kick-stall detector.
//  - SungrowBoard::setup(): copy the previous log tail out of RTC, publish the "Last reset" text
//    sensor, register the logger callback that feeds the new log tail.
#include <cstddef>
#include <cstdint>
#include <string>

#include "esphome/core/defines.h"

namespace esphome::sungrow_board {

namespace diag {
struct Journal;
}

#ifdef USE_SUNGROW_DIAG
/// Kick timer hook: one periodic supervisor kick happened at now_us (esp_timer task context).
void diag_on_kick(int64_t now_us, uint32_t kicks, uint32_t max_gap_ms, uint32_t long_gaps);

struct DiagConfig {
  uint8_t log_level{5};  ///< highest ESPHome log level kept in the log tail (5 = DEBUG)
};

/// Board-side half of the diagnostics (called from SungrowBoard).
void diag_setup(const DiagConfig &cfg);
void diag_loop();
void diag_dump_config();
/// Summary of how the previous boot ended (the "Last reset" text sensor state).
const std::string &diag_last_reset();
/// PSRAM in the heap (bytes); 0 = none (absent, or failed init with ignore_not_found).
uint32_t psram_size_bytes();
/// Plain-text report: this boot, the previous boot's journal and log tail, the core dump summary.
std::string diag_report();
/// This boot's log tail (the RTC ring), oldest line first.
std::string diag_ring_text();
/// esp_reset_reason() of this boot (diag_journal.h ResetReason numbering).
int diag_reset_reason();
/// The previous boot's journal; nullptr when none survived the reset.
const diag::Journal *diag_previous_journal();
/// This boot reports a stored core dump (its summary is in diag_report()).
bool diag_coredump_reported();
/// Boot number of this boot (1 when no journal survived).
uint32_t diag_boot_count();
/// "0x<config hash> <build time>" of this build.
const char *diag_build();
#endif

#ifdef USE_SUNGROW_WDOG
/// Drive the supervisor enable pin to "off" (IRAM, register writes only; panic-safe).
void watchdog_panic_off();
#endif

}  // namespace esphome::sungrow_board
