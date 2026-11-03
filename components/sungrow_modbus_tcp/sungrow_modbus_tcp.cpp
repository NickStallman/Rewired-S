#include "sungrow_modbus_tcp.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/optional.h"

namespace esphome {
namespace sungrow_modbus_tcp {

static const char *const TAG = "sungrow_modbus_tcp";

#ifdef USE_SWITCH
void EnableSwitch::write_state(bool state) {
  this->parent_->set_enabled_by_user(state);
  this->publish_state(state);
}
#endif

// ---- lifecycle -----------------------------------------------------------------------------------

void SungrowModbusTcp::setup() {
  this->load_user_set_();
#ifdef USE_SWITCH
  if (this->enable_switch_ != nullptr) {
    auto initial = this->enable_switch_->get_initial_state_with_restore_mode();
    this->enabled_ = initial.has_value() ? *initial : true;
    this->enable_switch_->publish_state(this->enabled_);
    const uint8_t mode = this->enable_switch_->restore_mode;
    this->pinned_ = !(mode & (switch_::RESTORE_MODE_PERSISTENT_MASK | switch_::RESTORE_MODE_DISABLED_MASK));
  }
#endif
  // Boot rule (as stock): without a user toggle, the first inverter's region code
  // (sig 0x2001) decides the server state.
  this->follow_country_ = !this->user_set_ && !this->pinned_ && this->dm_ != nullptr;
  ESP_LOGD(TAG, "user_set %d, country rule %s", this->user_set_, this->follow_country_ ? "on" : "off");
  if (this->follow_country_) {
    auto &store = this->dm_->store();
    store.add_listener([this](const sungrow_datamodel::Signal &s) {
      if (s.sigid == SIG_COUNTRY_CODE)
        this->on_country_signal_(s);
    });
    store.for_each([this](const sungrow_datamodel::Signal &s) {
      if (s.sigid == SIG_COUNTRY_CODE)
        this->on_country_signal_(s);
    });
  }
  if (this->enabled_)
    this->start_();
  this->publish_clients_();
}

void SungrowModbusTcp::set_enabled(bool enabled) {
  if (enabled == this->enabled_)
    return;
  this->enabled_ = enabled;
  ESP_LOGI(TAG, "Server %s", enabled ? "enabled" : "disabled");
  if (enabled) {
    this->start_();
  } else {
    this->stop_();
  }
  this->publish_clients_();
}

// ---- :502 country rule ---------------------------------------------------------------------------

void SungrowModbusTcp::load_user_set_() {
  this->user_set_pref_ = global_preferences->make_preference<bool>(fnv1_hash("sungrow_modbus_tcp_user_set"), true);
  bool stored = false;
  if (this->user_set_pref_.load(&stored)) {
    this->user_set_ = stored;
    return;
  }
  optional<bool> sw_state;
#ifdef USE_SWITCH
  if (this->enable_switch_ != nullptr)
    sw_state = this->enable_switch_->get_initial_state();
#endif
  this->user_set_ = sw_state.has_value() && !*sw_state;
  this->user_set_pref_.save(&this->user_set_);
  ESP_LOGI(TAG, "user_set initialised to %d (stored switch state: %s)", this->user_set_,
           sw_state.has_value() ? ONOFF(*sw_state) : "none");
}

void SungrowModbusTcp::set_enabled_by_user(bool enabled) {
  if (enabled != this->enabled_ && !this->user_set_) {
    this->user_set_ = true;
    this->follow_country_ = false;
    this->user_set_pref_.save(&this->user_set_);
    ESP_LOGI(TAG, "Switch changed by the user: the user decides the server state from now on");
  }
  this->set_enabled(enabled);
}

bool SungrowModbusTcp::is_country_source_(uint8_t device) const {
#ifdef USE_SUNGROW_MODBUS_TCP_HUB
  if (this->hub_ != nullptr) {
    for (const auto &d : this->hub_->master().devices()) {
      const auto *t = d.table();
      if (t != nullptr && t->family == sungrow_datamodel::Family::INVERTER)
        return d.config().index == device;
    }
    return false;
  }
#endif
  for (const auto &u : this->units_) {
    if (u.dev_type == 1 || u.dev_type == 21 || u.dev_type == 35)
      return u.device == device;
  }
  return false;
}

void SungrowModbusTcp::on_country_signal_(const sungrow_datamodel::Signal &s) {
  if (!this->follow_country_ || !s.valid || !this->is_country_source_(s.device))
    return;
  const uint16_t country = static_cast<uint16_t>(s.bits());
  if (static_cast<int32_t>(country) == this->country_applied_)
    return;
  this->apply_country_(country, "inverter region code");
}

void SungrowModbusTcp::on_country_written(uint16_t country) {
  this->follow_country_ = false;  // stock 0x1a10: country set by web/cloud, boot rule done
  this->apply_country_(country, "country write");
}

void SungrowModbusTcp::apply_country_(uint16_t country, const char *source) {
  if (this->user_set_ || this->pinned_) {
    ESP_LOGI(TAG, "%s: country %u, %s -> server stays %s", source, country,
             this->user_set_ ? "user_set" : "restore mode ALWAYS_*", this->enabled_ ? "enabled" : "disabled");
    return;
  }
  this->country_applied_ = country;
  const bool en = tcp_enabled_for_country(country);
  ESP_LOGI(TAG, "%s: country %u -> server %s", source, country, en ? "enabled" : "disabled");
  this->set_enabled(en);
#ifdef USE_SWITCH
  if (this->enable_switch_ != nullptr && this->enable_switch_->state != en)
    this->enable_switch_->publish_state(en);
#endif
}

bool SungrowModbusTcp::start_() {
  if (this->listener_ != nullptr)
    return true;
  this->listener_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->listener_ == nullptr) {
    ESP_LOGW(TAG, "Socket create failed: errno %d", errno);
    this->next_listen_retry_ = millis() + 10000;
    this->status_set_warning();
    return false;
  }
  int enable = 1;
  this->listener_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
  bool ok = this->listener_->setblocking(false) == 0;
  struct sockaddr_storage server;
  socklen_t sl = socket::set_sockaddr_any((struct sockaddr *) &server, sizeof(server), this->port_);
  ok = ok && sl != 0;
  ok = ok && this->listener_->bind((struct sockaddr *) &server, sl) == 0;
  // Small backlog: connections above max_clients are accepted and closed at once.
  ok = ok && this->listener_->listen(2) == 0;
  if (!ok) {
    ESP_LOGW(TAG, "Listen on :%u failed: errno %d (retry in 10 s)", this->port_, errno);
    this->listener_.reset();
    this->next_listen_retry_ = millis() + 10000;
    this->status_set_warning();
    return false;
  }
  this->status_clear_warning();
  ESP_LOGI(TAG, "Listening on :%u", this->port_);
  return true;
}

void SungrowModbusTcp::stop_() {
  this->clients_.clear();  // socket destructors close
  this->listener_.reset();
}

void SungrowModbusTcp::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Sungrow Modbus-TCP slave:\n"
                "  Port: %u\n"
                "  Enabled: %s (user_set %s, country rule %s)\n"
                "  Max clients: %u\n"
                "  Idle timeout: %u ms\n"
                "  Word order: %u\n"
                "  Unknown unit: %s\n"
                "  fn 3 live read: %s\n"
                "  Poll-table fallback: %s\n"
                "  Allow-list entries: %u\n"
                "  Writes: %s (write allow-list entries: %u)",
                this->port_, YESNO(this->enabled_), YESNO(this->user_set_),
                this->follow_country_ ? "on" : "off", this->max_clients_, (unsigned) this->idle_timeout_ms_,
                (unsigned) this->opt_.word_order, this->opt_.unknown_unit_exception ? "exception 0x0B" : "drop",
                YESNO(this->opt_.passthrough), YESNO(this->opt_.poll_fallback), (unsigned) this->allow_.size(),
#ifdef USE_SUNGROW_MODBUS_TCP_CONTROL
                this->control_ != nullptr ? "gated by sungrow_control" : "refused (exception 1)",
#else
                "refused (exception 1)",
#endif
                (unsigned) this->write_allow_.size());
  if (this->units_.empty()) {
    ESP_LOGCONFIG(TAG, "  Units: auto (unit id = DM device id: inverter 1, SBR stacks 2, 3 ...)");
  }
  for (const auto &u : this->units_) {
    ESP_LOGCONFIG(TAG, "  Unit %u -> device %u, dev_type %d, RS485 address %u", u.unit, u.device, u.dev_type,
                  u.address);
  }
}

// ---- Backend -------------------------------------------------------------------------------------

const Signal *SungrowModbusTcp::get_signal(uint8_t device, uint16_t sigid) {
  return this->dm_ != nullptr ? this->dm_->store().get(device, sigid) : nullptr;
}

Backend::Resolve SungrowModbusTcp::resolve_unit(uint8_t unit, UnitTarget *out) {
  const UnitCfg *cfg = nullptr;
  UnitCfg auto_cfg{};
#ifdef USE_SUNGROW_MODBUS_TCP_HUB
  const sungrow_modbus::Device *dev = nullptr;
#endif
  if (!this->units_.empty()) {
    for (const auto &u : this->units_) {
      if (u.unit == unit) {
        cfg = &u;
        break;
      }
    }
    if (cfg == nullptr)
      return Resolve::UNKNOWN;
#ifdef USE_SUNGROW_MODBUS_TCP_HUB
    if (this->hub_ != nullptr) {
      for (const auto &d : this->hub_->master().devices()) {
        if (d.config().index == cfg->device) {
          dev = &d;
          break;
        }
      }
    }
#endif
  } else {
    // Auto map (stock behaviour): unit id = the device's DM device id (Master::device_id: inverter
    // 1, then the SBR stacks it reports, 2, 3 ...), the same numbers the cloud uses as devCode.
    // The stock logger-remap flag (sig 0x10D on dev 0xF7: unit 1 <-> 247) is not applied: unit 1
    // is always the inverter.
#ifdef USE_SUNGROW_MODBUS_TCP_HUB
    if (this->hub_ != nullptr && unit != 0) {
      for (const auto &d : this->hub_->master().devices()) {
        if (this->hub_->master().device_id(d.config().index) == unit) {
          dev = &d;
          break;
        }
      }
    }
    if (dev == nullptr)
      return Resolve::UNKNOWN;
    auto_cfg = {unit, dev->config().index, -1, 0};
    cfg = &auto_cfg;
#else
    return Resolve::UNKNOWN;
#endif
  }

  int dev_type = cfg->dev_type;
  const RegisterTable *poll = nullptr;
  uint8_t address = cfg->address;
#ifdef USE_SUNGROW_MODBUS_TCP_HUB
  if (dev != nullptr) {
    poll = dev->table();
    if (address == 0)
      address = dev->config().address;
    // Device identity: devinfo, the hub's per-device `dev_type:` or the table family.
    if (dev_type < 0 && dev->identified())
      dev_type = dev->devinfo().dev_type;
  }
#endif
  const RegisterTable *t = dev_type >= 0 ? sungrow_datamodel::tcpslave_table_for_dev_type(dev_type) : nullptr;
  // No tcpslave map for this dev_type -> the device's own RTU table (as stock).
  if (t == nullptr)
    t = poll;
  if (t == nullptr)
    return Resolve::NOT_READY;
  out->device = cfg->device;
  out->table = t;
  out->poll_table = poll;
  out->rtu_address = address;
#ifndef USE_SUNGROW_MODBUS_TCP_HUB
  out->rtu_address = 0;  // no RS485 hub: no live reads
#endif
  return Resolve::OK;
}

uint8_t SungrowModbusTcp::check_write(const UnitTarget &t, uint8_t fc, uint16_t start, const uint16_t *values,
                                      uint16_t count) {
#ifdef USE_SUNGROW_MODBUS_TCP_CONTROL
  if (this->control_ == nullptr)
    return EX_ILLEGAL_FUNCTION;
  if (!this->write_allow_.empty() &&
      (this->current_ == nullptr || !this->current_->has_ip || !ip_allowed(this->write_allow_, this->current_->ip))) {
    ESP_LOGW(TAG, "%s: write %u+%u refused (not in write_allowed_ips)",
             this->current_ != nullptr ? this->current_->peer : "?", start, count);
    this->stats_.writes_rejected++;
    return EX_ILLEGAL_FUNCTION;
  }
  sungrow_control::Check c = this->control_->core().check(t.device, start, values, count);
  if (c.verdict != sungrow_control::Verdict::OK) {
    ESP_LOGI(TAG, "%s: fn %u write %s refused: %s", this->current_ != nullptr ? this->current_->peer : "?", fc,
             sungrow_control::format_write(start, values, count).c_str(), sungrow_control::verdict_name(c.verdict));
    this->stats_.writes_rejected++;
  }
  return sungrow_control::exception_for(c.verdict);
#else
  (void) t;
  (void) fc;
  (void) start;
  (void) values;
  (void) count;
  return EX_ILLEGAL_FUNCTION;  // no sungrow_control: read-only
#endif
}

// ---- sockets -------------------------------------------------------------------------------------

static bool peer_ipv4(const struct sockaddr_storage &ss, uint32_t *out) {
  if (ss.ss_family == AF_INET) {
    const auto *sin = reinterpret_cast<const struct sockaddr_in *>(&ss);
    *out = ntohl(sin->sin_addr.s_addr);
    return true;
  }
#if USE_NETWORK_IPV6
  if (ss.ss_family == AF_INET6) {
    const auto *sin6 = reinterpret_cast<const struct sockaddr_in6 *>(&ss);
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&sin6->sin6_addr);
    static const uint8_t kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(b, kMapped, 12) == 0) {
      *out = (uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16) | (uint32_t(b[14]) << 8) | b[15];
      return true;
    }
  }
#endif
  return false;
}

void SungrowModbusTcp::accept_() {
  while (true) {
    struct sockaddr_storage addr;
    socklen_t alen = sizeof(addr);
    auto sock = this->listener_->accept_loop_monitored((struct sockaddr *) &addr, &alen);
    if (!sock)
      return;
    char peer[socket::SOCKADDR_STR_LEN];
    sock->getpeername_to(peer);
    if (!this->allow_.empty()) {
      uint32_t ip = 0;
      // IPv6 peers (not v4-mapped) are always refused when an allow-list is set.
      if (!peer_ipv4(addr, &ip) || !ip_allowed(this->allow_, ip)) {
        ESP_LOGW(TAG, "Refused %s (not in allowed_ips)", peer);
        this->stats_.rejected++;
        continue;  // destructor closes
      }
    }
    if (this->clients_.size() >= this->max_clients_) {
      ESP_LOGW(TAG, "Max clients (%u), refusing %s", this->max_clients_, peer);
      this->stats_.rejected++;
      continue;
    }
    if (sock->setblocking(false) != 0) {
      ESP_LOGW(TAG, "setblocking failed for %s", peer);
      continue;
    }
    int one = 1;
    sock->setsockopt(IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    auto c = make_unique<Client>();
    c->has_ip = peer_ipv4(addr, &c->ip);
    c->sock = std::move(sock);
    c->last_activity = millis();
    std::memcpy(c->peer, peer, sizeof(peer));
    ESP_LOGD(TAG, "Client %s connected", c->peer);
    this->clients_.push_back(std::move(c));
    this->publish_clients_();
  }
}

void SungrowModbusTcp::queue_reply_(Client &c, const uint8_t *data, size_t len) {
  if (len == 0 || len > sizeof(c.tx))
    return;
  std::memcpy(c.tx, data, len);
  c.tx_len = len;
  c.tx_off = 0;
#ifdef USE_SUNGROW_DEBUG
  if (this->frame_observer_)
    this->frame_observer_(1, c.peer, data, len);
#endif
}

bool SungrowModbusTcp::flush_(Client &c) {
  while (c.tx_off < c.tx_len) {
    ssize_t n = c.sock->write(c.tx + c.tx_off, c.tx_len - c.tx_off);
    if (n > 0) {
      c.tx_off += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return true;  // retry next loop
    return false;
  }
  c.tx_len = c.tx_off = 0;
  return true;
}

void SungrowModbusTcp::start_passthrough_(Client &c, const Frame &f, const Decision &d, uint32_t now) {
#ifdef USE_SUNGROW_MODBUS_TCP_HUB
  if (this->hub_ != nullptr) {
    const uint32_t id = this->next_pend_id_++;
    if (this->next_pend_id_ == 0)
      this->next_pend_id_ = 1;
    bool queued = this->hub_->read_registers(
        d.rtu_address, d.fn, d.start, d.count,
        [this, id](bool ok, const uint8_t *data, uint16_t nregs) { this->on_passthrough_done_(id, ok, data, nregs); });
    if (queued) {
      c.pending = true;
      c.pend_id = id;
      c.pend_hdr = f.hdr;
      c.pend_count = d.count;
      // A pending request times out after 5 s (exception 4, as stock), even when the RS485
      // queue is longer than that under load.
      c.pend_deadline = now + PASSTHROUGH_TIMEOUT_MS;
      this->stats_.passthrough++;
      ESP_LOGV(TAG, "%s: live read addr %u fn %u start %u count %u", c.peer, d.rtu_address, d.fn, d.start, d.count);
      return;
    }
  }
#endif
  // The single live-read context is in use: exception 6 (busy).
  size_t n = build_exception(f.hdr, EX_BUSY, this->out_);
  this->stats_.exceptions++;
  this->queue_reply_(c, this->out_, n);
}

void SungrowModbusTcp::start_write_(Client &c, const Frame &f, const Decision &d, uint32_t now) {
  const uint16_t echo = d.fn == FC_WRITE_SINGLE ? d.values[0] : d.count;
#ifdef USE_SUNGROW_MODBUS_TCP_CONTROL
  if (this->control_ != nullptr) {
    const uint32_t id = this->next_pend_id_++;
    if (this->next_pend_id_ == 0)
      this->next_pend_id_ = 1;
    char src[24 + socket::SOCKADDR_STR_LEN];
    std::snprintf(src, sizeof(src), "mbtcp %s unit %u", c.peer, f.hdr.unit);
    // The unit was resolved by resolve_unit() (DM device id by default), d.device is its device.
    sungrow_modbus::WriteOptions wo;
    wo.fn = d.fn;  // stock keeps the client's function code (equip cmd 0x76)
    // :502 writes are not verified here: the reply follows the inverter's echo, like stock.
    sungrow_control::Verdict v = this->control_->write(
        d.device, d.start, d.values, d.count, false, src,
        [this, id](const sungrow_modbus::WriteResult &r) { this->on_write_done_(id, sungrow_control::exception_for(r)); },
        wo);
    if (v == sungrow_control::Verdict::OK) {
      c.pending = true;
      c.pend_write = true;
      c.pend_id = id;
      c.pend_hdr = f.hdr;
      c.pend_start = d.start;
      c.pend_echo = echo;
      c.pend_deadline = now + PASSTHROUGH_TIMEOUT_MS;  // stock: same 5 s context timeout as reads
      this->stats_.writes++;
      return;
    }
    size_t n = build_write_reply(f.hdr, d.start, echo, sungrow_control::exception_for(v), this->out_);
    this->stats_.exceptions++;
    this->stats_.writes_rejected++;
    this->queue_reply_(c, this->out_, n);
    return;
  }
#endif
  (void) now;
  size_t n = build_write_reply(f.hdr, d.start, echo, EX_ILLEGAL_FUNCTION, this->out_);
  this->stats_.exceptions++;
  this->queue_reply_(c, this->out_, n);
}

void SungrowModbusTcp::on_write_done_(uint32_t id, uint8_t exception) {
  for (auto &cp : this->clients_) {
    Client &c = *cp;
    if (!c.pending || c.pend_id != id)
      continue;
    c.pending = false;
    c.pend_write = false;
    size_t n = build_write_reply(c.pend_hdr, c.pend_start, c.pend_echo, exception, this->out_);
    if (exception == EX_NONE)
      this->stats_.requests++;
    else
      this->stats_.exceptions++;
    this->queue_reply_(c, this->out_, n);
    this->flush_(c);
    return;
  }
  // Client gone or timed out (it got exception 4); the write itself still happened or failed.
}

void SungrowModbusTcp::on_passthrough_done_(uint32_t id, bool ok, const uint8_t *data, uint16_t nregs) {
  for (auto &cp : this->clients_) {
    Client &c = *cp;
    if (!c.pending || c.pend_id != id)
      continue;
    if (c.pend_write)
      continue;
    c.pending = false;
    size_t n = build_passthrough_reply(c.pend_hdr, ok, data, nregs, c.pend_count, this->out_);
    if (ok && nregs == c.pend_count)
      this->stats_.requests++;
    else
      this->stats_.exceptions++;
    this->queue_reply_(c, this->out_, n);
    this->flush_(c);  // a write error is picked up by the next service_()
    return;
  }
  // Client gone or request timed out: drop the late reply.
}

bool SungrowModbusTcp::service_(Client &c, uint32_t now) {
  // Read everything available (non-blocking) while there is room.
  while (c.rx.space() > 0) {
    ssize_t n = c.sock->read(c.rx.write_ptr(), c.rx.space());
    if (n > 0) {
      c.rx.commit(static_cast<size_t>(n));
      c.last_activity = now;
      continue;
    }
    if (n == 0) {
      ESP_LOGD(TAG, "Client %s closed", c.peer);
      return false;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;
    ESP_LOGD(TAG, "Client %s read error %d", c.peer, errno);
    return false;
  }

  if (c.pending && (int32_t) (now - c.pend_deadline) >= 0) {
    if (c.pend_write) {
      ESP_LOGW(TAG, "%s: write timed out", c.peer);
    } else {
      ESP_LOGD(TAG, "%s: live read timed out", c.peer);
    }
    c.pending = false;
    c.pend_write = false;
    size_t n = build_exception(c.pend_hdr, EX_DEVICE_FAILURE, this->out_);
    this->stats_.exceptions++;
    this->queue_reply_(c, this->out_, n);
  }

  if (!this->flush_(c))
    return false;

  // One request in flight per client: pipelined frames wait in the assembler.
  while (!c.pending && c.tx_len == 0) {
    Frame f;
    FrameAssembler::Status st = c.rx.peek(&f);
    if (st == FrameAssembler::Status::NEED_MORE)
      break;
    if (st == FrameAssembler::Status::BAD) {
      ESP_LOGW(TAG, "Client %s: bad MBAP header, closing", c.peer);
      this->stats_.dropped++;
      return false;
    }
    size_t out_len = 0;
#ifdef USE_SUNGROW_DEBUG
    if (this->frame_observer_)
      this->frame_observer_(0, c.peer, f.pdu - MBAP_HEADER_LEN, f.total_len);
#endif
    this->current_ = &c;
    Decision d = handle_frame(f, *this, this->opt_, this->out_, &out_len);
    this->current_ = nullptr;
    switch (d.kind) {
      case Decision::REPLY:
        if (d.exception != EX_NONE) {
          this->stats_.exceptions++;
          ESP_LOGV(TAG, "%s: unit %u fn %u -> exception %u", c.peer, f.hdr.unit, f.hdr.fc, d.exception);
        } else {
          this->stats_.requests++;
        }
        this->queue_reply_(c, this->out_, out_len);
        break;
      case Decision::PASSTHROUGH:
        this->start_passthrough_(c, f, d, now);
        break;
      case Decision::WRITE:
        this->start_write_(c, f, d, now);
        break;
      case Decision::DROP:
      default:
        this->stats_.dropped++;
#ifdef USE_SUNGROW_DEBUG
        if (this->frame_observer_)
          this->frame_observer_(2, c.peer, f.pdu - MBAP_HEADER_LEN, f.total_len);
#endif
        break;
    }
    c.rx.pop(f);
    if (!this->flush_(c))
      return false;
  }

  if (this->idle_timeout_ms_ != 0 && !c.pending && c.tx_len == 0 &&
      now - c.last_activity > this->idle_timeout_ms_) {
    ESP_LOGD(TAG, "Client %s idle, closing", c.peer);
    return false;
  }
  return true;
}

void SungrowModbusTcp::loop() {
  const uint32_t now = millis();
  if (this->enabled_) {
    if (this->listener_ == nullptr && (int32_t) (now - this->next_listen_retry_) >= 0)
      this->start_();
    if (this->listener_ != nullptr)
      this->accept_();
    bool changed = false;
    for (size_t i = 0; i < this->clients_.size();) {
      if (this->service_(*this->clients_[i], now)) {
        i++;
      } else {
        this->clients_.erase(this->clients_.begin() + i);
        changed = true;
      }
    }
    if (changed)
      this->publish_clients_();
  }
  if (now - this->last_stats_ >= this->stats_interval_ms_) {
    this->last_stats_ = now;
    this->publish_stats_();
  }
}

void SungrowModbusTcp::publish_clients_() {
#ifdef USE_SENSOR
  if (this->clients_sensor_ != nullptr)
    this->clients_sensor_->publish_state(static_cast<float>(this->clients_.size()));
#endif
}

void SungrowModbusTcp::publish_stats_() {
#ifdef USE_SENSOR
  if (this->requests_sensor_ != nullptr)
    this->requests_sensor_->publish_state(static_cast<float>(this->stats_.requests));
  if (this->errors_sensor_ != nullptr)
    this->errors_sensor_->publish_state(static_cast<float>(this->stats_.errors()));
#endif
}

}  // namespace sungrow_modbus_tcp
}  // namespace esphome
