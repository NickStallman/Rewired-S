#pragma once
// ESPHome hub for the RS485 Modbus-RTU master. Glue only: the protocol, scheduler and discovery
// live in the host-testable core (modbus_rtu / discovery / master).
//
// RS485 direction control: ESPHome 2026.9's IDF UART switches the port into
// UART_MODE_RS485_HALF_DUPLEX with DE on RTS whenever `uart: flow_control_pin:` is set (the
// same mode stock uses), so the hardware drives DE and nothing here has to fight the UART
// component over pins or mode. `de_pin:` on this hub is the software fallback (raise, write,
// wait for TX done, lower) for a uart without flow_control_pin.
#include <functional>
#include <vector>

#include "esphome/components/sungrow_datamodel/sungrow_datamodel.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "master.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome {
namespace sungrow_modbus {

class SungrowModbus : public Component, public uart::UARTDevice, public Transport, public MasterListener {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After the uart bus (BUS) and the data model hub (BUS + 1).
  float get_setup_priority() const override { return setup_priority::DATA; }

  // ---- configuration (generated code) ------------------------------------------------------
  void set_datamodel(sungrow_datamodel::SungrowDataModel *dm) { this->dm_ = dm; }
  void set_de_pin(GPIOPin *pin) { this->de_pin_ = pin; }
  void set_frame_gap(uint32_t ms) { this->timing_.frame_gap_ms = ms; }
  void set_frame_gap_fast(uint32_t ms) { this->timing_.frame_gap_fast_ms = ms; }
  void set_tier_period(PollTier t, uint32_t ms) { this->master_.set_tier_period(t, ms); }
  void set_poll_timeout_margin(uint32_t ms) { this->timing_.poll_timeout_margin_ms = ms; }
  void set_response_timeout(uint32_t ms) { this->timing_.response_timeout_ms = ms; }
  void set_tunnel_timeout(uint32_t ms) { this->timing_.tunnel_timeout_ms = ms; }
  void set_offline_silence(uint32_t ms) { this->timing_.offline_silence_ms = ms; }
  void set_retries(uint8_t n) { this->timing_.retries = n; }
  void set_offline_after(uint8_t n) { this->timing_.offline_after = n; }
  void set_offline_probe_interval(uint32_t ms) { this->timing_.offline_probe_ms = ms; }
  void set_write_timeout(uint32_t ms) { this->timing_.write_timeout_ms = ms; }
  void set_write_settle(uint32_t ms) { this->timing_.write_settle_ms = ms; }
  void set_write_rate_limit(uint32_t ms) { this->timing_.write_rate_limit_ms = ms; }
  void set_stats_interval(uint32_t ms) { this->stats_interval_ms_ = ms; }
  void set_child_discovery(bool on) { this->master_.set_child_discovery(on); }
  /// Run the main loop without its idle delay while the master waits for a reply, a retry, a
  /// queued write / ad-hoc read or the settle gap (ModbusMaster::wants_fast_loop), so the frame
  /// gap and the reply detection are not quantised to the ~16 ms loop.
  void set_fast_loop(bool on) { this->fast_loop_enabled_ = on; }
  /// Broadcast inverter search + remembered inverter addresses (persisted in NVS).
  void set_inverter_search(bool on) { this->search_ = on; }
  /// Range probe over Modbus addresses [first, last] (1-based), `attempts` probes per slot; with
  /// the search on it is only the fallback when the search gets no answer.
  void set_scan(uint8_t first, uint8_t last, uint8_t attempts) {
    this->scan_first_ = first;
    this->scan_last_ = last;
    this->scan_attempts_ = attempts;
  }
  /// Register-table name applied to scanned devices whose devinfo read is rejected. Resolved
  /// with find_table() in setup() (like add_device), since the table registry is a runtime thing.
  void set_scan_fallback(const char *name) { this->scan_fallback_ = name; }
  /// table/fallback: register table names (find_table), nullptr/"auto" = discovery.
  /// dev_type: stock dev_type override, 0 = devinfo / table family.
  void add_device(uint8_t index, uint8_t address, const char *table, const char *fallback, uint8_t word_order,
                  int8_t gate, bool sbr_cells, uint32_t sbr_interval_ms, int32_t frame_gap_ms = -1,
                  uint8_t dev_type = 0);
#ifdef USE_BINARY_SENSOR
  void set_online_sensor(binary_sensor::BinarySensor *s) { this->ent_.back().online = s; }
#endif
#ifdef USE_SENSOR
  void set_packet_loss_sensor(sensor::Sensor *s) { this->ent_.back().packet_loss = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_dev_code_sensor(text_sensor::TextSensor *s) { this->ent_.back().dev_code = s; }
  void set_dev_type_sensor(text_sensor::TextSensor *s) { this->ent_.back().dev_type = s; }
  void set_protocol_sensor(text_sensor::TextSensor *s) { this->ent_.back().protocol = s; }
  void set_table_sensor(text_sensor::TextSensor *s) { this->ent_.back().table = s; }
#endif

  // ---- API for other components (cloud getparam, SBR, scripts) ------------------------------
  /// Ad-hoc read, queued ahead of the cyclic polls. start = 1-based table address.
  bool read_registers(uint8_t dev_addr, uint8_t fn, uint16_t start, uint16_t count, AdhocCallback cb) {
    return this->master_.read_registers(dev_addr, fn, start, count, std::move(cb));
  }
  /// Raw register write through the master's write queue, NOT gated: use sungrow_control
  /// (enable switch + allow-list) for anything user-facing.
  WriteStatus submit_write(uint8_t dev_addr, uint16_t start, const uint16_t *values, uint16_t count,
                           const WriteOptions &opt, WriteCallback cb) {
    return this->master_.submit_write(dev_addr, start, values, count, opt, std::move(cb));
  }
  ModbusMaster &master() { return this->master_; }
  /// Search again and re-probe every silent auto-discovered address (Rescan button).
  void rescan() { this->master_.rescan(); }

  // ---- Transport ---------------------------------------------------------------------------
  void send_frame(const uint8_t *data, size_t len) override;
  size_t receive(uint8_t *buf, size_t cap) override;
  void discard_input() override;

  // ---- MasterListener ----------------------------------------------------------------------
  void on_online_changed(const Device &dev) override;
  void on_discovered(const Device &dev) override;
  void on_identity_changed(const Device &dev) override;
  void on_remembered_changed() override;
  void on_unsupported(const Device &dev, const char *why) override;
  /// Called when discovery could not classify a device (MasterListener::on_unsupported), after
  /// the log line; usage reporting subscribes here.
  void add_on_unsupported_callback(std::function<void(const Device &, const char *)> &&cb) {
    this->unsupported_cbs_.push_back(std::move(cb));
  }

 protected:
  struct Entities {
#ifdef USE_BINARY_SENSOR
    binary_sensor::BinarySensor *online{nullptr};
#endif
#ifdef USE_SENSOR
    sensor::Sensor *packet_loss{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
    text_sensor::TextSensor *dev_code{nullptr};
    text_sensor::TextSensor *dev_type{nullptr};
    text_sensor::TextSensor *protocol{nullptr};
    text_sensor::TextSensor *table{nullptr};
#endif
  };
  void publish_info_(size_t i);
  void publish_stats_();
  /// NVS record of the remembered inverter addresses.
  struct RememberedPref {
    uint8_t count;
    uint8_t addr[ModbusMaster::MAX_REMEMBERED];
  };
  ESPPreferenceObject remembered_pref_;
  bool search_{false};

  sungrow_datamodel::SungrowDataModel *dm_{nullptr};
  GPIOPin *de_pin_{nullptr};
  ModbusMaster master_;
  MasterTiming timing_;
  std::vector<DeviceConfig> dev_cfgs_;
  std::vector<std::function<void(const Device &, const char *)>> unsupported_cbs_;
  std::vector<Entities> ent_;
  uint32_t stats_interval_ms_{30000};
  uint32_t last_stats_{0};
  bool fast_loop_enabled_{false};
  HighFrequencyLoopRequester fast_loop_;  ///< held while fast_loop_enabled_ && master_.wants_fast_loop()
  // Range probe (0 = disabled); applied to the master in setup() before start().
  uint8_t scan_first_{0};
  uint8_t scan_last_{0};
  uint8_t scan_attempts_{2};
  const char *scan_fallback_{nullptr};  ///< table name for scanned devices; resolved in setup()
};

}  // namespace sungrow_modbus
}  // namespace esphome
