#pragma once
// ESPHome glue for the Modbus-TCP slave: listener + client sockets, unit map, entities.
// Protocol / register logic lives in the host-testable core (mbtcp_core.h).
//
// Threading: non-blocking sockets serviced from loop(), not a dedicated FreeRTOS task. The
// SignalStore (std::unordered_map) and the RS485 master's ad-hoc queue are not thread-safe and
// are only touched from the main loop; a separate task would need a lock around every store
// access (including the hub's ingest path) or a copy of the data. The sockets are created
// loop-monitored, so incoming data wakes the main loop; replies go out within one loop pass.
// Per request the work is one table pass (<= 498 rows) + <= 125 register encodes.
#include <functional>
#include <memory>
#include <vector>

#include "esphome/components/socket/socket.h"
#include "esphome/components/sungrow_datamodel/sungrow_datamodel.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "mbtcp_core.h"

#ifdef USE_SUNGROW_MODBUS_TCP_HUB
#include "esphome/components/sungrow_modbus/sungrow_modbus.h"
#endif
#ifdef USE_SUNGROW_MODBUS_TCP_CONTROL
#include "esphome/components/sungrow_control/sungrow_control.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

namespace esphome {
namespace sungrow_modbus_tcp {

class SungrowModbusTcp;

#ifdef USE_SWITCH
class EnableSwitch : public switch_::Switch, public Parented<SungrowModbusTcp> {
 protected:
  void write_state(bool state) override;
};
#endif

class SungrowModbusTcp : public Component, public Backend {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override { this->stop_(); }
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // ---- configuration (generated code) ------------------------------------------------------
  void set_datamodel(sungrow_datamodel::SungrowDataModel *dm) { this->dm_ = dm; }
#ifdef USE_SUNGROW_MODBUS_TCP_HUB
  void set_hub(sungrow_modbus::SungrowModbus *hub) { this->hub_ = hub; }
#endif
  void set_port(uint16_t port) { this->port_ = port; }
  void set_max_clients(uint8_t n) { this->max_clients_ = n; }
  void set_idle_timeout(uint32_t ms) { this->idle_timeout_ms_ = ms; }
  void set_stats_interval(uint32_t ms) { this->stats_interval_ms_ = ms; }
  void set_word_order(uint8_t wo) { this->opt_.word_order = static_cast<WordOrder>(wo); }
  void set_unknown_unit_exception(bool b) { this->opt_.unknown_unit_exception = b; }
  void set_passthrough(bool b) { this->opt_.passthrough = b; }
  void set_poll_fallback(bool b) { this->opt_.poll_fallback = b; }
  void add_allowed_ip(uint32_t addr, uint32_t mask) { this->allow_.push_back({addr & mask, mask}); }
  /// Clients allowed to WRITE (fn 6/16); empty = every connected client.
  void add_write_allowed_ip(uint32_t addr, uint32_t mask) { this->write_allow_.push_back({addr & mask, mask}); }
#ifdef USE_SUNGROW_MODBUS_TCP_CONTROL
  void set_control(sungrow_control::SungrowControl *c) {
    this->control_ = c;
    // A setup frame that writes the device's country register re-evaluates the :502 country rule.
    c->set_country_written_cb([this](uint16_t country) { this->on_country_written(country); });
  }
#endif
  /// Explicit unit map entry. dev_type -1 = auto (discovery / table family); address 0 = the
  /// hub device's RS485 address.
  void add_unit(uint8_t unit, uint8_t device, int16_t dev_type, uint8_t address) {
    this->units_.push_back({unit, device, dev_type, address});
  }
#ifdef USE_SENSOR
  void set_clients_sensor(sensor::Sensor *s) { this->clients_sensor_ = s; }
  void set_requests_sensor(sensor::Sensor *s) { this->requests_sensor_ = s; }
  void set_errors_sensor(sensor::Sensor *s) { this->errors_sensor_ = s; }
#endif
#ifdef USE_SWITCH
  void set_enable_switch(EnableSwitch *s) { this->enable_switch_ = s; }
#endif

  /// Runtime enable. Off closes the listener and all clients.
  void set_enabled(bool enabled);
  bool is_enabled() const { return this->enabled_; }
  /// User toggle (the enable switch): a change persists user_set, which stops the country rule
  /// from deciding the server state (stock user_set_flag, sig 0x1a0d).
  void set_enabled_by_user(bool enabled);
  /// Post-write rule (as stock): call after an acknowledged write frame that
  /// starts at the inverter's descriptor country register. Unless the user has toggled the
  /// switch (or its restore mode is ALWAYS_ON/OFF), enables the server for country 6, 99 or
  /// 114..117 and disables it otherwise, and
  /// stops the boot rule (sig 0x2001) from overriding that decision until reboot.
  void on_country_written(uint16_t country);
  bool user_set() const { return this->user_set_; }
  const Stats &stats() const { return this->stats_; }

  // ---- Backend -----------------------------------------------------------------------------
  Resolve resolve_unit(uint8_t unit, UnitTarget *out) override;
  const Signal *get_signal(uint8_t device, uint16_t sigid) override;
  uint8_t check_write(const UnitTarget &t, uint8_t fc, uint16_t start, const uint16_t *values,
                      uint16_t count) override;

#ifdef USE_SUNGROW_DEBUG
  /// Debug-build frame observer (components/sungrow_debug): what 0 = request ADU, 1 = reply ADU,
  /// 2 = request dropped without a reply. Compiled only in the debug build.
  using FrameObserver = std::function<void(uint8_t what, const char *peer, const uint8_t *adu, size_t len)>;
  void set_frame_observer(FrameObserver f) { this->frame_observer_ = std::move(f); }
#endif

 protected:
  struct UnitCfg {
    uint8_t unit;
    uint8_t device;
    int16_t dev_type;
    uint8_t address;
  };
  struct Client {
    std::unique_ptr<socket::Socket> sock;
    FrameAssembler rx;
    uint8_t tx[MAX_ADU_LEN];
    size_t tx_len{0};
    size_t tx_off{0};
    uint32_t last_activity{0};
    bool pending{false};
    uint32_t pend_id{0};
    ReqHeader pend_hdr;
    uint16_t pend_count{0};
    uint32_t pend_deadline{0};
    bool pend_write{false};      ///< pending request is a write (reply = echo)
    uint16_t pend_start{0};
    uint16_t pend_echo{0};       ///< fn 6: value, fn 16: count
    bool has_ip{false};
    uint32_t ip{0};              ///< IPv4 host order (when has_ip)
    char peer[socket::SOCKADDR_STR_LEN]{};
  };

  bool start_();
  void stop_();
  void accept_();
  /// Service one client; returns false if it must be closed.
  bool service_(Client &c, uint32_t now);
  bool flush_(Client &c);
  void queue_reply_(Client &c, const uint8_t *data, size_t len);
  void start_passthrough_(Client &c, const Frame &f, const Decision &d, uint32_t now);
  void on_passthrough_done_(uint32_t id, bool ok, const uint8_t *data, uint16_t nregs);
  void start_write_(Client &c, const Frame &f, const Decision &d, uint32_t now);
  void on_write_done_(uint32_t id, uint8_t exception);
  void publish_stats_();
  void publish_clients_();
  /// Load user_set; on the first boot without it, a stored switch OFF counts as a user choice
  /// (the switch default is ON), a stored ON leaves the decision to the country rule.
  void load_user_set_();
  /// True when `device` is the first inverter-family device (source of the country code).
  bool is_country_source_(uint8_t device) const;
  void on_country_signal_(const sungrow_datamodel::Signal &s);
  void apply_country_(uint16_t country, const char *source);

  sungrow_datamodel::SungrowDataModel *dm_{nullptr};
#ifdef USE_SUNGROW_MODBUS_TCP_HUB
  sungrow_modbus::SungrowModbus *hub_{nullptr};
#endif
  uint16_t port_{502};
  uint8_t max_clients_{4};
  // Idle client links close after 120 s by default.
  uint32_t idle_timeout_ms_{120000};
  uint32_t stats_interval_ms_{30000};
  Options opt_;
  std::vector<IpRule> allow_;
  std::vector<IpRule> write_allow_;
  const Client *current_{nullptr};  ///< client whose frame handle_frame() is processing
#ifdef USE_SUNGROW_MODBUS_TCP_CONTROL
  sungrow_control::SungrowControl *control_{nullptr};
#endif
  std::vector<UnitCfg> units_;

  bool enabled_{true};
  bool user_set_{false};
  bool pinned_{false};           ///< switch restore mode ALWAYS_ON/OFF: the YAML fixes the state
  bool follow_country_{false};   ///< boot rule active: sig 0x2001 decides the server state
  int32_t country_applied_{-1};  ///< last country the rule applied (-1 = none)
  ESPPreferenceObject user_set_pref_;
  std::unique_ptr<socket::ListenSocket> listener_;
  std::vector<std::unique_ptr<Client>> clients_;
  uint32_t next_listen_retry_{0};
  uint32_t next_pend_id_{1};
  uint32_t last_stats_{0};
  Stats stats_;
  uint8_t out_[MAX_ADU_LEN];
#ifdef USE_SUNGROW_DEBUG
  FrameObserver frame_observer_;
#endif

#ifdef USE_SENSOR
  sensor::Sensor *clients_sensor_{nullptr};
  sensor::Sensor *requests_sensor_{nullptr};
  sensor::Sensor *errors_sensor_{nullptr};
#endif
#ifdef USE_SWITCH
  EnableSwitch *enable_switch_{nullptr};
#endif
};

}  // namespace sungrow_modbus_tcp
}  // namespace esphome
