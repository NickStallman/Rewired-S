#include "sungrow_modbus.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace sungrow_modbus {

static const char *const TAG = "sungrow_modbus";

[[maybe_unused]] static const char *word_order_name(uint8_t wo) {
  switch (wo) {
    case 0:
      return "high_word_first";
    case 1:
      return "raw";
    case 2:
      return "low_word_first";
    default:
      return "unswapped";
  }
}

void SungrowModbus::add_device(uint8_t index, uint8_t address, const char *table, const char *fallback,
                               uint8_t word_order, int8_t gate, bool sbr_cells, uint32_t sbr_interval_ms,
                               int32_t frame_gap_ms, uint8_t dev_type) {
  DeviceConfig c;
  c.index = index;
  c.address = address;
  c.table = (table == nullptr || std::strcmp(table, "auto") == 0) ? nullptr : sungrow_datamodel::find_table(table);
  c.fallback_table = fallback == nullptr ? nullptr : sungrow_datamodel::find_table(fallback);
  c.word_order = static_cast<WordOrder>(word_order);
  c.gate_capabilities = gate;
  c.sbr_cells = sbr_cells;
  c.sbr_interval_ms = sbr_interval_ms;
  c.frame_gap_ms = frame_gap_ms;
  c.dev_type = dev_type;
  this->dev_cfgs_.push_back(c);
  this->ent_.emplace_back();
}

void SungrowModbus::setup() {
  if (this->de_pin_ != nullptr) {
    this->de_pin_->setup();
    this->de_pin_->digital_write(false);
  }
  // Character time from the actual uart config: start + data + parity + stop bits.
  this->timing_.baud = this->parent_->get_baud_rate();
  this->timing_.bits_per_char = static_cast<uint8_t>(
      1 + this->parent_->get_data_bits() + (this->parent_->get_parity() != uart::UART_CONFIG_PARITY_NONE ? 1 : 0) +
      this->parent_->get_stop_bits());
  this->master_.set_timing(this->timing_);
  this->master_.set_transport(this);
  this->master_.set_store(&this->dm_->store());
  this->master_.set_clock([]() { return millis(); });
  this->master_.set_log([](int level, const char *msg) { esp_log_printf_(level, TAG, __LINE__, "%s", msg); });
  this->master_.set_listener(this);
  for (const auto &c : this->dev_cfgs_)
    this->master_.add_device(c);
  if (this->search_) {
    this->master_.set_search(true);
    this->remembered_pref_ = global_preferences->make_preference<RememberedPref>(fnv1_hash("sungrow_modbus_inverters"));
    RememberedPref p{};
    if (this->remembered_pref_.load(&p) && p.count <= ModbusMaster::MAX_REMEMBERED)
      this->master_.set_remembered(std::vector<uint8_t>(p.addr, p.addr + p.count));
  }
  if (this->scan_first_ >= 1) {
    this->master_.set_scan(this->scan_first_, this->scan_last_, this->scan_attempts_);
    if (this->scan_fallback_ != nullptr)
      this->master_.set_scan_fallback(sungrow_datamodel::find_table(this->scan_fallback_));
  }
  this->master_.start();
  for (size_t i = 0; i < this->ent_.size(); i++) {
#ifdef USE_BINARY_SENSOR
    if (this->ent_[i].online != nullptr)
      this->ent_[i].online->publish_initial_state(false);
#endif
    this->publish_info_(i);
  }
}

void SungrowModbus::loop() {
  this->master_.tick();
  if (this->fast_loop_enabled_ && this->master_.wants_fast_loop())
    this->fast_loop_.start();
  else
    this->fast_loop_.stop();
  uint32_t now = millis();
  if (now - this->last_stats_ >= this->stats_interval_ms_) {
    this->last_stats_ = now;
    this->publish_stats_();
  }
}

// ---- Transport -------------------------------------------------------------------------------
void SungrowModbus::send_frame(const uint8_t *data, size_t len) {
  if (this->de_pin_ != nullptr) {
    // Software DE: blocks for the frame time (8 B @ 9600 = 8.3 ms).
    this->de_pin_->digital_write(true);
    this->write_array(data, len);
    this->flush();
    this->de_pin_->digital_write(false);
  } else {
    // No local echo in RS485 half-duplex mode: IDF keeps the receiver off while DE is high. An
    // echo, should one appear, is skipped by the parser's CRC resync.
    // Hardware RS485 half duplex: with no TX ring buffer this returns once the bytes are in the
    // 128 B FIFO, i.e. immediately for read requests.
    this->write_array(data, len);
  }
}

size_t SungrowModbus::receive(uint8_t *buf, size_t cap) {
  size_t n = this->available();
  if (n == 0)
    return 0;
  if (n > cap)
    n = cap;
  return this->read_array(buf, n) ? n : 0;
}

void SungrowModbus::discard_input() {
  uint8_t tmp[64];
  for (int i = 0; i < 16; i++) {
    size_t n = this->receive(tmp, sizeof(tmp));
    if (n == 0)
      break;
    ESP_LOGV(TAG, "Discarded %u stale RX bytes", (unsigned) n);
  }
}

// ---- entities --------------------------------------------------------------------------------
void SungrowModbus::on_online_changed(const Device &dev) {
  // ent_ only covers the explicitly-configured devices; bus-scan devices have no entities.
  for (size_t i = 0; i < this->master_.devices().size() && i < this->ent_.size(); i++) {
    if (&this->master_.devices()[i] != &dev)
      continue;
#ifdef USE_BINARY_SENSOR
    if (this->ent_[i].online != nullptr)
      this->ent_[i].online->publish_state(dev.online());
#endif
  }
}

void SungrowModbus::on_discovered(const Device &dev) {
  for (size_t i = 0; i < this->master_.devices().size() && i < this->ent_.size(); i++)
    if (&this->master_.devices()[i] == &dev)
      this->publish_info_(i);
}

void SungrowModbus::on_identity_changed(const Device &dev) { this->on_discovered(dev); }

void SungrowModbus::on_unsupported(const Device &dev, const char *why) {
  const DevInfo &di = dev.devinfo();
  ESP_LOGW(TAG, "Device %u (addr %u): %s (dev_code 0x%04X, model \"%s\", protocol %u, dev_type %u)",
           dev.config().index, dev.config().address, why, di.dev_code, di.model, di.protocol, di.dev_type);
  for (auto &cb : this->unsupported_cbs_)
    cb(dev, why);
}

void SungrowModbus::on_remembered_changed() {
  if (!this->search_)
    return;
  RememberedPref p{};
  const auto &r = this->master_.remembered();
  p.count = static_cast<uint8_t>(r.size() < ModbusMaster::MAX_REMEMBERED ? r.size() : ModbusMaster::MAX_REMEMBERED);
  for (uint8_t i = 0; i < p.count; i++)
    p.addr[i] = r[i];
  this->remembered_pref_.save(&p);
  ESP_LOGI(TAG, "Remembered inverter addresses: %u", (unsigned) p.count);
}

static const char *dev_type_label(uint8_t dt) {
  switch (dt) {
    case 1:
      return "inverter";
    case 21:
      return "string";
    case 35:
      return "hybrid";
    case 18:
      return "meter";
    case 34:
      return "logger";
    case 44:
      return "battery";
    case 46:
      return "charger";
    case 92:
      return "energy bridge";
    default:
      return "other";
  }
}

void SungrowModbus::publish_info_(size_t i) {
#ifdef USE_TEXT_SENSOR
  const Device &d = this->master_.devices()[i];
  const Entities &e = this->ent_[i];
  char buf[64];
  if (d.identified()) {
    const DevInfo &info = d.devinfo();
    if (e.dev_code != nullptr && d.discovered()) {
      std::snprintf(buf, sizeof(buf), "0x%04X", info.dev_code);
      e.dev_code->publish_state(buf);
    }
    if (e.dev_type != nullptr) {
      // e.g. "21 (string, devinfo), family unit"
      std::snprintf(buf, sizeof(buf), "%u (%s, %s), family %s", info.dev_type, dev_type_label(info.dev_type),
                    d.dev_type_source(), xml_family_name(d.xml_family()));
      e.dev_type->publish_state(buf);
    }
    if (e.protocol != nullptr && info.protocol != 0) {
      // e.g. "3 (AA10), inv_type 1"; without a devinfo block the protocol stands for the table.
      char inv[24] = "";
      if (d.inv_type_known())
        std::snprintf(inv, sizeof(inv), ", inv_type %u", d.inv_type());
      std::snprintf(buf, sizeof(buf), "%u (%s)%s", info.protocol, d.discovered() ? info.proto_code : "table", inv);
      e.protocol->publish_state(buf);
    }
  }
  if (e.table != nullptr)
    e.table->publish_state(d.table() != nullptr ? d.table()->name : "discovering");
#endif
}

void SungrowModbus::publish_stats_() {
  for (size_t i = 0; i < this->master_.devices().size(); i++) {
    const Device &d = this->master_.devices()[i];
    // Abandoned slots (a probed address that never answered, a remembered address the search
    // replaced) are inert: not logged, not treated as devices.
    if (d.abandoned())
      continue;
    const LinkStats &s = d.stats();
    ESP_LOGD(TAG, "dev %u addr %u %s: sent %" PRIu32 " recv %" PRIu32 " timeouts %" PRIu32 " crc %" PRIu32
                  " exc %" PRIu32 " loss %.1f%%",
             d.config().index, d.config().address, d.online() ? "online" : "offline", s.sent, s.received,
             s.timeouts, s.crc_errors, s.exceptions, s.loss_pct());
#ifdef USE_SENSOR
    if (i < this->ent_.size() && this->ent_[i].packet_loss != nullptr)
      this->ent_[i].packet_loss->publish_state(s.loss_pct());
#endif
  }
}

void SungrowModbus::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sungrow RS485 Modbus master:\n"
                "  Line: %" PRIu32 " baud, %u bits/char, DE: %s\n"
                "  Frame gap: %" PRIu32 " ms (battery/charger/EB %" PRIu32 " ms, >= 3.5 chars), retries: %u\n"
                "  Timeouts: cyclic reads wire time + %" PRIu32 " ms, other reads %" PRIu32 " ms, tunnelled %" PRIu32
                " ms\n"
                "  Refresh tiers: fast %" PRIu32 " ms, live %" PRIu32 " ms, slow %" PRIu32 " ms, static %" PRIu32
                " ms\n"
                "  Offline after %u failures over %" PRIu32 " ms, probe every %" PRIu32 " ms\n"
                "  Writes: timeout %" PRIu32 " ms, settle %" PRIu32 " ms, rate limit %" PRIu32 " ms per register\n"
                "  Inverter search: %s; range probe: %s\n"
                "  Fast loop while a transaction is pending: %s",
                this->timing_.baud, this->timing_.bits_per_char,
                this->de_pin_ != nullptr ? "software (de_pin)" : "uart flow_control_pin (hardware RS485)",
                this->timing_.frame_gap_ms, this->timing_.frame_gap_fast_ms, this->timing_.retries,
                this->timing_.poll_timeout_margin_ms, this->timing_.response_timeout_ms, this->timing_.tunnel_timeout_ms,
                this->master_.tier_period(PollTier::FAST), this->master_.tier_period(PollTier::LIVE),
                this->master_.tier_period(PollTier::SLOW), this->master_.tier_period(PollTier::STATIC),
                this->timing_.offline_after, this->timing_.offline_silence_ms, this->timing_.offline_probe_ms,
                this->timing_.write_timeout_ms,
                this->timing_.write_settle_ms, this->timing_.write_rate_limit_ms,
                this->search_ ? "broadcast 30035, remembered addresses" : "off",
                this->scan_first_ >= 1 ? (this->search_ ? "fallback" : "at start") : "off",
                this->fast_loop_enabled_ ? "on" : "off");
  for (const auto &d : this->master_.devices()) {
    const auto &c = d.config();
    ESP_LOGCONFIG(TAG, "  Device %u: address %u, table %s (fallback %s), word order %s, %u requests%s, dev_type %u (%s)",
                  c.index, c.address, c.table != nullptr ? c.table->name : "auto",
                  c.fallback_table != nullptr ? c.fallback_table->name : "-", word_order_name((uint8_t) c.word_order),
                  (unsigned) d.jobs().size(), c.sbr_cells ? ", SBR cell block" : "", d.devinfo().dev_type,
                  d.dev_type_source());
  }
}

}  // namespace sungrow_modbus
}  // namespace esphome
