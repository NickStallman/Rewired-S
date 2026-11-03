#pragma once
// sungrow_debug: bring-up / debug tooling for the WiNet replacement firmware (debug builds only).
//
// Everything goes through public APIs of the other components:
//  - RS485 raw trace: this component installs itself as the ModbusMaster's Transport
//    (hub->master().set_transport(this)) and forwards every call to the hub's own Transport
//    implementation (SungrowModbus::send_frame/receive/discard_input). It sees every TX frame and
//    every RX byte, runs its own ResponseParser on a copy to time and classify the reply, and
//    logs from its own loop(), never from inside the master's tick.
//  - Safety: with block_rtu_writes (default on) only fn 3/4 frames reach the bus. Anything else
//    (fn 5/6/15/16/0x60/0x77...) is dropped here and the master sees a timeout.
//  - Decode trace / dumps / ad-hoc reads: the register tables, decoder and SignalStore readers of
//    sungrow_datamodel, the hub's read_registers() ad-hoc queue, Device::jobs()/stats().
//  - Modbus-TCP trace: SungrowModbusTcp::set_frame_observer() (compiled only with
//    USE_SUNGROW_DEBUG, which this component's codegen defines).
//  - Ext flash: SungrowExtFlash::dump_file()/list_dir() (read-only).
//  - Log server: a logger callback, buffered and written to plain TCP clients from loop().
//
// Long outputs (decode rows, store / plan dumps) are emitted a few lines per loop() so the UART
// logger never stalls the main loop for long.
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "esphome/components/sungrow_datamodel/sungrow_datamodel.h"
#include "esphome/components/sungrow_modbus/sungrow_modbus.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "flash_tools.h"
#include "trace_core.h"

#ifdef USE_SUNGROW_DEBUG_TCP
#include "esphome/components/sungrow_modbus_tcp/sungrow_modbus_tcp.h"
#endif
#ifdef USE_SUNGROW_DEBUG_EXTFLASH
#include "esphome/components/sungrow_extflash/sungrow_extflash.h"
#endif
#ifdef USE_SUNGROW_DEBUG_LOG_SERVER
#include "esphome/components/socket/socket.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

namespace esphome {
namespace sungrow_debug {

class SungrowDebug;

enum class SwitchKind : uint8_t { RTU_TRACE, DECODE_TRACE, ONLY_CHANGED, TCP_TRACE, ALLOW_BOOT_SWITCH };

#ifdef USE_SWITCH
class DebugSwitch : public switch_::Switch, public Parented<SungrowDebug> {
 public:
  void set_kind(SwitchKind k) { this->kind_ = k; }
  SwitchKind kind() const { return this->kind_; }

 protected:
  void write_state(bool state) override;
  SwitchKind kind_{SwitchKind::RTU_TRACE};
};
#endif

/// A long output produced a line at a time from loop().
class LineJob {
 public:
  virtual ~LineJob() = default;
  /// Next line; false when done. *tag: log tag for the line.
  virtual bool next(std::string *line, const char **tag) = 0;
};

class SungrowDebug : public Component, public sungrow_modbus::Transport {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the RS485 hub (DATA) so its setup() has installed itself as the transport first.
  float get_setup_priority() const override { return setup_priority::DATA - 10.0f; }

  // ---- configuration (codegen) -------------------------------------------------------------
  void set_hub(sungrow_modbus::SungrowModbus *hub) { this->hub_ = hub; }
  void set_datamodel(sungrow_datamodel::SungrowDataModel *dm) { this->dm_ = dm; }
#ifdef USE_SUNGROW_DEBUG_TCP
  void set_tcp(sungrow_modbus_tcp::SungrowModbusTcp *tcp) { this->tcp_ = tcp; }
#endif
#ifdef USE_SUNGROW_DEBUG_EXTFLASH
  void set_extflash(sungrow_extflash::SungrowExtFlash *f) { this->extflash_ = f; }
#endif
  void set_block_rtu_writes(bool b) { this->block_writes_ = b; }
  void set_lines_per_loop(uint8_t n) { this->lines_per_loop_ = n ? n : 1; }
  void set_stats_interval(uint32_t ms) { this->stats_interval_ms_ = ms; }
  void set_trace_min_interval(uint32_t ms) { this->trace_min_interval_ms_ = ms; }
  void set_log_server_port(uint16_t p) { this->log_port_ = p; }
  void set_initial_filter(const char *addresses, const char *registers);
  void add_stats_device(uint8_t address);
#ifdef USE_SENSOR
  enum StatKind : uint8_t { ST_REQUESTS, ST_RESPONSES, ST_TIMEOUTS, ST_CRC, ST_EXCEPTIONS, ST_LAT_AVG, ST_LAT_MAX,
                            ST_AGE, ST_LOSS, ST_COUNT };
  void set_stat_sensor(uint8_t kind, sensor::Sensor *s) { this->stats_.back().sensors[kind] = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_last_result_sensor(text_sensor::TextSensor *s) { this->last_result_ts_ = s; }
  void set_scan_result_sensor(text_sensor::TextSensor *s) { this->scan_result_ts_ = s; }
  void set_partition_sensor(text_sensor::TextSensor *s) { this->partition_ts_ = s; }
  void set_idf_version_sensor(text_sensor::TextSensor *s) { this->idf_ts_ = s; }
  void set_filter_sensor(text_sensor::TextSensor *s) { this->filter_ts_ = s; }
#endif
#ifdef USE_SWITCH
  void add_switch(DebugSwitch *s) { this->switches_.push_back(s); }
#endif

  // ---- runtime toggles ---------------------------------------------------------------------
  void set_flag(SwitchKind k, bool on);
  bool flag(SwitchKind k) const;
  void set_rtu_trace(bool on) { this->set_flag(SwitchKind::RTU_TRACE, on); }
  void set_decode_trace(bool on) { this->set_flag(SwitchKind::DECODE_TRACE, on); }
  void set_only_changed(bool on) { this->set_flag(SwitchKind::ONLY_CHANGED, on); }
  void set_tcp_trace(bool on) { this->set_flag(SwitchKind::TCP_TRACE, on); }
  /// "" = all. addresses: "1,3-5"; registers: "5000-5100,13000+50" (1-based table addresses).
  bool set_trace_filter(const std::string &addresses, const std::string &registers);
  /// Per-tag log level at runtime (needs logger runtime_tag_levels). level: NONE..VERY_VERBOSE.
  bool set_tag_level(const std::string &tag, const std::string &level);

  // ---- on-demand tools (API actions / buttons) ---------------------------------------------
  /// Ad-hoc read; logs raw hex + decoded rows (device's table, or `table` if given).
  bool debug_read(int address, int fn, int start, int count, const std::string &table = "");
  /// Probe slave addresses first..last with fn 4 start/count (one at a time, interval_ms of bus
  /// time left to the normal polls between probes). timeout_ms > 0 temporarily lowers the master's
  /// response timeout for the whole bus while the scan runs (restored afterwards).
  bool debug_scan(int first, int last, int start = 5000, int count = 1, int interval_ms = 300, int timeout_ms = 0);
  void stop_scan();
  /// device = SignalStore device index; -1 = all.
  void dump_signal_store(int device);
  void dump_poll_plan(int device);
  void dump_system();
  void dump_stats();
  /// Ext flash (read-only): hex dump of a file under the mount path (VERY_VERBOSE, tag sungrow_extflash).
  bool dump_file(const std::string &path);
  void list_files(const std::string &path);
  void dump_identity();
  /// Partition table @0x8000 (raw, MD5-checked, CSV), partitions, bootloader header, otadata, flash id.
  void dump_flash_layout();
  /// THE ONLY FLASH WRITE: esp_ota_set_boot_partition(other OTA slot) + reboot. Needs the
  /// ALLOW_BOOT_SWITCH switch on (one shot) and a valid app image in the other slot (boot_switch.cpp).
  bool boot_other_slot();

  // ---- Transport (wrapper around the hub) --------------------------------------------------
  void send_frame(const uint8_t *data, size_t len) override;
  size_t receive(uint8_t *buf, size_t cap) override;
  void discard_input() override;

 protected:
  enum class Outcome : uint8_t { OK, EXCEPTION, TIMEOUT, CRC_ERROR, BLOCKED };
  struct Record {
    uint32_t seq{0};
    uint32_t t_tx{0};
    uint32_t latency{0};
    Outcome outcome{Outcome::TIMEOUT};
    uint8_t exc{0};
    uint16_t crc_errors{0};
    uint16_t dropped{0};
    int dev{-1};
    RtuInfo req;
    std::vector<uint8_t> tx;
    std::vector<uint8_t> rx;
    std::vector<uint8_t> payload;  ///< OK fn 3/4: register data (2 * nregs)
  };
  struct LastOutcome {
    uint32_t seq{0};
    uint8_t addr{0};
    Outcome outcome{Outcome::TIMEOUT};
    uint8_t exc{0};
  };
  struct LatAcc {
    uint32_t sum{0};
    uint32_t n{0};
    uint32_t max{0};
  };
  struct StatsDev {
    uint8_t address{0};
#ifdef USE_SENSOR
    sensor::Sensor *sensors[ST_COUNT]{};
#endif
  };

  void finalize_(uint32_t now, Outcome o);
  void process_record_(Record &r);
  void log_discovery_(const Record &r);
  void queue_decode_(int dev_index, const RegisterTable *table, uint8_t fn, uint16_t start,
                     const std::vector<uint8_t> &payload, bool forced, const char *title);
  void push_job_(std::unique_ptr<LineJob> job);
  void run_jobs_();
  void scan_tick_(uint32_t now);
  void scan_finish_();
  void publish_stats_(uint32_t now);
  void set_last_result_(const std::string &s);
  std::string partition_summary_() const;
  int device_index_for_address_(uint8_t addr) const;
#ifdef USE_SUNGROW_DEBUG_TCP
  void on_tcp_frame_(uint8_t what, const char *peer, const uint8_t *adu, size_t len);
#endif
#ifdef USE_SUNGROW_DEBUG_LOG_SERVER
  static void on_log_(void *self, uint8_t level, const char *tag, const char *msg, size_t len);
  void log_server_loop_();
#endif

  sungrow_modbus::SungrowModbus *hub_{nullptr};
  sungrow_datamodel::SungrowDataModel *dm_{nullptr};
#ifdef USE_SUNGROW_DEBUG_TCP
  sungrow_modbus_tcp::SungrowModbusTcp *tcp_{nullptr};
#endif
#ifdef USE_SUNGROW_DEBUG_EXTFLASH
  sungrow_extflash::SungrowExtFlash *extflash_{nullptr};
#endif

  // config / runtime flags
  bool block_writes_{true};
  bool rtu_trace_{true};
  bool decode_trace_{true};
  bool only_changed_{true};
  bool tcp_trace_{true};
  bool allow_boot_switch_{false};  ///< never restored: always off after boot
  uint8_t lines_per_loop_{6};
  uint32_t stats_interval_ms_{10000};
  uint32_t trace_min_interval_ms_{0};
  uint16_t log_port_{0};
  TraceFilter filter_;
  ChangeTracker changes_;
  RateLimiter rate_;
  std::set<std::string> tag_names_;  ///< interned tags for logger::set_log_level(tag, ...)

  // transaction in flight
  bool open_{false};
  Record cur_;
  sungrow_modbus::ResponseParser parser_;
  uint32_t seq_{0};
  LastOutcome last_;
  std::deque<Record> records_;
  uint32_t records_dropped_{0};
  uint32_t blocked_frames_{0};
  std::vector<uint8_t> stray_;
  std::unordered_map<uint8_t, LatAcc> lat_;

  std::deque<std::unique_ptr<LineJob>> jobs_;
  uint32_t jobs_dropped_{0};

  // scan
  ScanPlan scan_;
  uint16_t scan_start_{5000};
  uint16_t scan_count_{1};
  bool scan_timeout_override_{false};
  uint32_t scan_saved_timeout_{0};

  std::vector<StatsDev> stats_;
  uint32_t last_stats_{0};

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *last_result_ts_{nullptr};
  text_sensor::TextSensor *scan_result_ts_{nullptr};
  text_sensor::TextSensor *partition_ts_{nullptr};
  text_sensor::TextSensor *idf_ts_{nullptr};
  text_sensor::TextSensor *filter_ts_{nullptr};
#endif
#ifdef USE_SWITCH
  std::vector<DebugSwitch *> switches_;
#endif

#ifdef USE_SUNGROW_DEBUG_LOG_SERVER
  struct LogClient {
    std::unique_ptr<socket::Socket> sock;
    std::string out;
    uint32_t dropped{0};
    char peer[socket::SOCKADDR_STR_LEN]{};
  };
  std::unique_ptr<socket::ListenSocket> log_listener_;
  std::vector<std::unique_ptr<LogClient>> log_clients_;
  uint32_t log_retry_{0};
  bool in_log_cb_{false};
#endif
};

}  // namespace sungrow_debug
}  // namespace esphome
