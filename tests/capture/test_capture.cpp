// Host tests for components/sungrow_capture: YMODEM receiver and the I-V / grid-wave /
// fault-recording capture state machines against a fake inverter (Modbus slave + YMODEM sender
// on one fake bus).
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "../test_harness.h"
#include "capture_core.h"
#include "modbus_rtu.h"
#include "register_tables.h"

using namespace esphome::sungrow_capture;
using namespace esphome::sungrow_modbus;
namespace dm = esphome::sungrow_datamodel;

static uint32_t g_now = 0;

// ---------------------------------------------------------------------------------------------
// YMODEM sender as the inverter implements it (header SOH, data STX 1024 then SOH 128 for the
// tail, 0x1A padding, EOT, no closing header).
struct Sender {
  std::string name;
  std::vector<uint8_t> file;
  std::vector<std::vector<uint8_t>> blocks;  // [0] = header
  size_t next{0};
  bool armed{false}, started{false}, eot_sent{false}, done{false};
  int corrupt_block{-1};  // send this block once with a bad CRC
  int nak_seen{0};
  static std::vector<uint8_t> make(uint8_t start, uint8_t seq, const uint8_t *d, size_t n, size_t len) {
    std::vector<uint8_t> b = {start, seq, static_cast<uint8_t>(~seq)};
    b.insert(b.end(), d, d + n);
    b.resize(3 + len, start == SOH_ && seq == 0 ? 0x00 : 0x1A);
    const uint16_t crc = crc16_xmodem(b.data() + 3, len);
    b.push_back(crc >> 8);
    b.push_back(crc & 0xFF);
    return b;
  }
  static constexpr uint8_t SOH_ = 1;
  void arm(const std::string &n, const std::vector<uint8_t> &f) {
    name = n;
    file = f;
    blocks.clear();
    std::vector<uint8_t> hdr(n.begin(), n.end());
    hdr.push_back(0);
    std::string sz = std::to_string(f.size());
    hdr.insert(hdr.end(), sz.begin(), sz.end());
    hdr.push_back(0);
    blocks.push_back(make(1, 0, hdr.data(), hdr.size(), 128));
    size_t off = 0;
    uint8_t seq = 1;
    while (f.size() - off >= 1024) {
      blocks.push_back(make(2, seq++, f.data() + off, 1024, 1024));
      off += 1024;
    }
    while (off < f.size()) {
      const size_t n2 = f.size() - off < 128 ? f.size() - off : 128;
      blocks.push_back(make(1, seq++, f.data() + off, n2, 128));
      off += n2;
    }
    next = 0;
    armed = true;
    started = eot_sent = done = false;
  }
  void send(std::deque<uint8_t> *rx, size_t i) {
    std::vector<uint8_t> b = blocks[i];
    if (static_cast<int>(i) == corrupt_block) {
      corrupt_block = -1;
      b[10] ^= 0x55;
    }
    rx->insert(rx->end(), b.begin(), b.end());
  }
  // A raw byte from the dongle.
  void on_byte(uint8_t c, std::deque<uint8_t> *rx) {
    if (!armed || done)
      return;
    if (!started) {
      if (c != 0x43)
        return;
      started = true;
      send(rx, 0);
      return;
    }
    if (c == 0x15) {  // NAK: resend
      nak_seen++;
      if (eot_sent)
        rx->push_back(0x04);
      else
        send(rx, next);
      return;
    }
    if (c != 0x06)
      return;
    if (eot_sent) {
      done = true;
      armed = false;
      return;
    }
    next++;
    if (next < blocks.size()) {
      send(rx, next);
    } else {
      eot_sent = true;
      rx->push_back(0x04);
    }
  }
};

struct FakeInverter : Transport {
  uint8_t addr{1};
  std::map<uint32_t, uint16_t> regs;  // fn << 16 | doc
  std::deque<uint8_t> rx;
  std::vector<std::vector<uint8_t>> frames;
  Sender sender;
  // I-V scan model.
  int iv_state{0};  // 0 idle, 1 success, 2 scanning, 3 failed
  uint32_t iv_done_at{0};
  uint32_t iv_scan_ms{20000};
  bool iv_fail{false};
  bool refuse_iv{false};
  bool high_word_first{false};
  std::vector<uint8_t> iv_file;
  // Grid model (the progress ramp also serves the fault-record copy).
  uint16_t progress{100};
  uint32_t grid_started{0};
  bool grid_running{false};
  bool grid_ignore_start{false};
  std::vector<uint8_t> grid_file;
  // Fault-recording model: 30034 = 0xAA makes the catalogue visible fault_scan_ms later; the
  // fetch is refused (exception 04) until then or while the catalogue is empty.
  uint16_t fault_records{0};
  uint32_t fault_scan_ms{500};
  uint32_t fault_scan_at{0};
  bool fault_scanned{false};
  bool fault_ignore_fetch{false};  // accept the fetch but never copy (progress stays 100)
  int fetch_refused{0};
  std::vector<uint8_t> fault_file;
  int raw_bytes{0};

  bool catalogue_ready() const { return fault_scanned && g_now >= fault_scan_at + fault_scan_ms; }

  uint16_t get(uint8_t fn, uint16_t a) {
    if (fn == 3 && a >= 32005 && a <= 32009) {
      if (iv_state == 2 && g_now >= iv_done_at)
        iv_state = iv_fail ? 3 : 1;
      static const uint32_t bits[4] = {0x20000, 0x8000, 0x10000, 0x40000};
      const uint32_t s = bits[iv_state] | 0x0200;  // plus a general status bit
      const uint16_t lo = s & 0xFFFF, hi = s >> 16;
      if (a == 32005)
        return high_word_first ? hi : lo;
      if (a == 32006)
        return high_word_first ? lo : hi;
      if (a == 32009)
        return iv_state == 3 ? 5 : 0;
      return 0;
    }
    if (fn == 4 && a == 30078)
      return catalogue_ready() ? fault_records : 0;
    if (fn == 4 && a == 30079) {
      if (grid_running) {
        const uint32_t t = g_now - grid_started;
        progress = t < 3000 ? 0 : (t < 4000 ? 20 : (t < 9000 ? static_cast<uint16_t>(20 + (t - 4000) * 80 / 5000) : 100));
        if (progress == 100)
          grid_running = false;
      }
      return progress;
    }
    auto it = regs.find(static_cast<uint32_t>(fn) << 16 | a);
    return it == regs.end() ? 0 : it->second;
  }
  void reply(std::vector<uint8_t> f) {
    append_crc(f.data(), f.size() - 2);
    rx.insert(rx.end(), f.begin(), f.end());
  }
  void send_frame(const uint8_t *d, size_t n) override {
    frames.emplace_back(d, d + n);
    if (n < 4 || !crc_ok(d, n)) {
      for (size_t i = 0; i < n; i++) {
        raw_bytes++;
        sender.on_byte(d[i], &rx);
      }
      return;
    }
    if (d[0] != addr)
      return;
    const uint8_t fn = d[1];
    const uint16_t start = static_cast<uint16_t>((d[2] << 8 | d[3]) + 1);
    const uint16_t count = static_cast<uint16_t>(d[4] << 8 | d[5]);
    if (fn == 3 || fn == 4) {
      std::vector<uint8_t> f = {addr, fn, static_cast<uint8_t>(2 * count)};
      for (uint16_t i = 0; i < count; i++) {
        const uint16_t v = get(fn, static_cast<uint16_t>(start + i));
        f.push_back(v >> 8);
        f.push_back(v & 0xFF);
      }
      f.resize(f.size() + 2);
      reply(f);
      return;
    }
    if (fn == 16) {
      for (uint16_t i = 0; i < count; i++) {
        const uint16_t reg = static_cast<uint16_t>(start + i);
        const uint16_t v = static_cast<uint16_t>(d[7 + 2 * i] << 8 | d[8 + 2 * i]);
        if (reg == 32000 && v == 0x01A0) {
          if (refuse_iv || iv_state == 2) {
            reply({addr, 0x90, 4, 0, 0});
            return;
          }
          iv_state = 2;
          iv_done_at = g_now + iv_scan_ms;
        } else if (reg == 32000 && v == 0x00A0) {
          sender.arm("VI_ALL.txt 261030120000", iv_file);
        } else if (reg == 32000 && v == 0x01B0) {
          sender.arm("GridWaveRecord.txt", grid_file);
        } else if (reg == 32000 && v == 0x01A1) {
          if (!catalogue_ready() || fault_records == 0) {
            fetch_refused++;
            reply({addr, 0x90, 4, 0, 0});
            return;
          }
          if (!fault_ignore_fetch) {
            grid_running = true;
            grid_started = g_now;
            progress = 0;
          }
        } else if (reg == 32000 && v == 0x00B0) {
          sender.arm("FaultUpload_packet_01.txt", fault_file);
        } else if (reg == 30034 && v == 0xAA) {
          fault_scanned = true;
          fault_scan_at = g_now;
        } else if (reg == 32011 && !grid_ignore_start) {
          grid_running = true;
          grid_started = g_now;
          progress = 0;
        }
        regs[3u << 16 | reg] = v;
      }
      std::vector<uint8_t> f(d, d + 6);
      f.resize(8);
      reply(f);
    }
  }
  size_t receive(uint8_t *buf, size_t cap) override {
    size_t n = 0;
    // ~1 byte per ms at 9600 baud: the rig ticks every 5 ms.
    while (n < cap && n < 6 && !rx.empty()) {
      buf[n++] = rx.front();
      rx.pop_front();
    }
    return n;
  }
  void discard_input() override { rx.clear(); }
};

static std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++)
    v[i] = static_cast<uint8_t>(seed + i * 7 + (i >> 8));
  return v;
}

struct Rig {
  FakeInverter inv;
  dm::SignalStore store;
  ModbusMaster m;
  CaptureCore cap;
  std::vector<std::string> logs;
  int writes{0};
  Rig(uint32_t function_enum = 0x025040DDu | (1u << 19)) {
    g_now = 1000;
    store.set_clock([] { return g_now; });
    m.set_transport(&inv);
    m.set_store(&store);
    m.set_clock([] { return g_now; });
    DeviceConfig d;
    d.index = 0;
    d.address = 1;
    d.table = dm::find_table("other_dev18_st6200");
    m.add_device(d);
    m.start();
    cap.set_master(&m);
    cap.set_store(&store);
    cap.set_clock([] { return g_now; });
    cap.set_wall_clock([] { return (int64_t) 1793000000; });
    cap.set_log([this](int, const char *msg) {
      logs.emplace_back(msg);
      if (getenv("CAPTURE_TEST_LOG"))
        std::printf("    %s\n", msg);
    });
    cap.set_writer([this](uint16_t start, const uint16_t *v, uint16_t n, CaptureCore::DoneFn done, std::string *why) {
      writes++;
      WriteOptions o;
      o.fn = FN_WRITE_MULTIPLE;
      const WriteStatus st = m.submit_write(1, start, v, n, o, [done](const WriteResult &r) {
        done(r.status == WriteStatus::OK, r.exception);
      });
      if (st != WriteStatus::QUEUED) {
        *why = write_status_name(st);
        return false;
      }
      return true;
    });
    store.set_scalar(0, 0x2173, dm::TYPE_U32, 0.0f, function_enum, 4);
    inv.iv_file = pattern(2 * 3 * 258, 3);
    inv.grid_file = pattern(6 + 9000, 9);
    inv.grid_file[0] = 0;  // prefix: 1 record of 9000 bytes
    inv.grid_file[1] = 1;
    inv.grid_file[2] = inv.grid_file[3] = 0;
    inv.grid_file[4] = 0x23;
    inv.grid_file[5] = 0x28;
    // Fault file: prefix (3 records of 400 bytes) + the records back to back.
    inv.fault_file = {0, 3, 0, 0, 0x01, 0x90};
    for (uint8_t k = 0; k < 3; k++) {
      const auto rec = pattern(400, static_cast<uint8_t>(40 + k));
      inv.fault_file.insert(inv.fault_file.end(), rec.begin(), rec.end());
    }
    run(500);
  }
  /// fn 16 writes to the inverter as (doc register, first value), in order.
  std::vector<std::pair<uint16_t, uint16_t>> writes_sent() const {
    std::vector<std::pair<uint16_t, uint16_t>> w;
    for (const auto &f : inv.frames)
      if (f.size() > 8 && f[1] == 16)
        w.emplace_back(static_cast<uint16_t>((f[2] << 8 | f[3]) + 1), static_cast<uint16_t>(f[7] << 8 | f[8]));
    return w;
  }
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 5) {
      m.tick();
      cap.tick();
      g_now += 5;
    }
  }
  /// Run until the capture finishes (or the limit).
  void finish(uint32_t limit_ms) {
    for (uint32_t t = 0; t < limit_ms && cap.running(); t += 5) {
      m.tick();
      cap.tick();
      g_now += 5;
    }
  }
  /// Frames to the inverter while the bus was held: anything that is not the capture's own.
  size_t polls_between(size_t a, size_t b) const {
    size_t n = 0;
    for (size_t i = a; i < b && i < inv.frames.size(); i++) {
      const auto &f = inv.frames[i];
      if (f.size() >= 8 && f[1] == 4)
        n++;  // the st6200 table polls fn 4
    }
    return n;
  }
};

TEST(crc16_xmodem_vector) {
  const char *s = "123456789";
  CHECK_EQ(crc16_xmodem(reinterpret_cast<const uint8_t *>(s), 9), 0x31C3);
}

TEST(ymodem_receive_happy_path_and_trim) {
  Sender s;
  const auto file = pattern(2500, 1);  // 2 STX blocks + 4 SOH blocks (452 B tail)
  s.arm("FaultRecord.txt", file);
  YmodemReceiver r;
  r.begin(0, 64 * 1024);
  std::deque<uint8_t> wire;
  std::vector<uint8_t> tx;
  uint32_t now = 0;
  for (int i = 0; i < 2000 && !r.finished(); i++, now += 5) {
    r.tick(now, &tx);
    for (uint8_t b : tx)
      s.on_byte(b, &wire);
    tx.clear();
    std::vector<uint8_t> chunk;
    while (!wire.empty() && chunk.size() < 37) {
      chunk.push_back(wire.front());
      wire.pop_front();
    }
    r.feed(chunk.data(), chunk.size(), now, &tx);
  }
  for (uint8_t b : tx)
    s.on_byte(b, &wire);
  CHECK(r.status() == YmodemReceiver::Status::DONE);
  CHECK(r.file_name() == "FaultRecord.txt");
  CHECK_EQ(r.announced_size(), 2500u);
  CHECK(r.data() == file);
  CHECK_EQ(r.blocks(), 6u);
  CHECK(s.done);
}

TEST(ymodem_crc_error_nak_and_resend) {
  Sender s;
  const auto file = pattern(1500, 2);
  s.arm("x.txt", file);
  s.corrupt_block = 1;
  YmodemReceiver r;
  r.begin(0, 64 * 1024);
  std::deque<uint8_t> wire;
  std::vector<uint8_t> tx;
  uint32_t now = 0;
  for (int i = 0; i < 4000 && !r.finished(); i++, now += 5) {
    r.tick(now, &tx);
    for (uint8_t b : tx)
      s.on_byte(b, &wire);
    tx.clear();
    std::vector<uint8_t> all(wire.begin(), wire.end());
    wire.clear();
    r.feed(all.data(), all.size(), now, &tx);
  }
  CHECK(r.status() == YmodemReceiver::Status::DONE);
  CHECK(r.data() == file);
  CHECK_EQ(r.naks(), 1u);
  CHECK_EQ(s.nak_seen, 1);
}

TEST(ymodem_sequence_wrap) {
  Sender s;
  const auto file = pattern(300 * 1024 / 1024 * 1024, 5);  // 300 STX blocks: wraps at 256
  s.arm("big", file);
  YmodemReceiver r;
  r.begin(0, 512 * 1024);
  std::deque<uint8_t> wire;
  std::vector<uint8_t> tx;
  uint32_t now = 0;
  for (int i = 0; i < 100000 && !r.finished(); i++, now += 5) {
    r.tick(now, &tx);
    for (uint8_t b : tx)
      s.on_byte(b, &wire);
    tx.clear();
    std::vector<uint8_t> all(wire.begin(), wire.end());
    wire.clear();
    r.feed(all.data(), all.size(), now, &tx);
  }
  CHECK(r.status() == YmodemReceiver::Status::DONE);
  CHECK(r.data() == file);
  CHECK_EQ(r.blocks(), 300u);
}

TEST(ymodem_empty_file_and_no_header) {
  Sender s;
  s.arm("StandbyRecord.txt", {});
  YmodemReceiver r;
  r.begin(0, 1024);
  std::deque<uint8_t> wire;
  std::vector<uint8_t> tx;
  uint32_t now = 0;
  for (int i = 0; i < 400 && !r.finished(); i++, now += 5) {
    r.tick(now, &tx);
    for (uint8_t b : tx)
      s.on_byte(b, &wire);
    tx.clear();
    std::vector<uint8_t> all(wire.begin(), wire.end());
    wire.clear();
    r.feed(all.data(), all.size(), now, &tx);
  }
  CHECK(r.status() == YmodemReceiver::Status::EMPTY);
  // Silent sender: 'C' three times, then a failure.
  YmodemReceiver q;
  q.begin(0, 1024);
  tx.clear();
  for (uint32_t t = 0; t < 12000 && !q.finished(); t += 5)
    q.tick(t, &tx);
  CHECK(q.status() == YmodemReceiver::Status::FAILED);
  size_t cs = 0;
  for (uint8_t b : tx)
    cs += b == 0x43;
  CHECK_EQ(cs, 3u);
}

TEST(ymodem_oversized_file_cancelled) {
  Sender s;
  s.arm("big", pattern(5000, 1));
  YmodemReceiver r;
  r.begin(0, 4096);
  std::deque<uint8_t> wire;
  std::vector<uint8_t> tx;
  r.tick(0, &tx);
  for (uint8_t b : tx)
    s.on_byte(b, &wire);
  tx.clear();
  std::vector<uint8_t> all(wire.begin(), wire.end());
  r.feed(all.data(), all.size(), 5, &tx);
  CHECK(r.status() == YmodemReceiver::Status::FAILED);
  CHECK(!tx.empty() && tx[0] == 0x18);
}

TEST(iv_capture_end_to_end) {
  Rig r;
  CHECK(r.m.devices()[0].online());
  CHECK(r.cap.iv_supported(0));
  std::string why;
  CHECK(r.cap.start_iv(0, 1, "SG5.0RS", &why));
  CHECK(r.cap.running());
  r.run(3000);
  CHECK(r.cap.step_text() == "scanning");
  CHECK(r.cap.scan_ms() > 0 && r.cap.scan_ms() <= 3000);
  CHECK_EQ(r.cap.iv_fault(), 0);
  CHECK(!r.m.exclusive());  // normal polling continues while the inverter scans
  const size_t f0 = r.inv.frames.size();
  r.run(10000);
  CHECK(r.polls_between(f0, r.inv.frames.size()) > 0);
  r.finish(60000);
  CHECK(!r.cap.running());
  CHECK(!r.m.exclusive());
  CHECK(!r.m.raw());
  CHECK_EQ(r.cap.results().size(), 1u);
  const Capture &c = *r.cap.results().back();
  CHECK(c.ok);
  CHECK(c.result == "ok");
  CHECK(c.data == r.inv.iv_file);
  CHECK(c.file_name == "VI_ALL.txt 261030120000");
  CHECK_EQ(c.announced_size, 1548u);
  CHECK(c.snapshot_before.size() >= 2);
  CHECK(!c.timeline.empty());
  CHECK_EQ(r.inv.regs[3u << 16 | 31999], 0);  // format flag written before the select
  CHECK_EQ(r.inv.regs[3u << 16 | 32000], 0x00A0);
  // No poll frames between the select write and the end of the transfer.
  size_t sel = 0, last_raw = 0;
  for (size_t i = 0; i < r.inv.frames.size(); i++) {
    const auto &f = r.inv.frames[i];
    if (f.size() > 8 && f[1] == 16 && f[7] == 0x00 && f[8] == 0xA0)
      sel = i;
    if (f.size() < 4)
      last_raw = i;
  }
  CHECK(sel > 0 && last_raw > sel);
  CHECK_EQ(r.polls_between(sel, last_raw), 0u);
  // Polling resumes and the inverter stays online.
  const size_t f1 = r.inv.frames.size();
  r.run(10000);
  CHECK(r.polls_between(f1, r.inv.frames.size()) > 0);
  CHECK(r.m.devices()[0].online());
  // Metadata JSON carries the essentials.
  const std::string js = CaptureCore::metadata_json(c);
  CHECK(js.find("\"kind\":\"iv\"") != std::string::npos);
  CHECK(js.find("\"size\":1548") != std::string::npos);
  CHECK(c.scan_ms >= 15000 && c.scan_ms <= 21000);
  CHECK_EQ(r.cap.scan_ms(), 0u);  // idle again
  CHECK(std::string(CaptureCore::iv_fault_text(21)) == "the inverter's scan watchdog expired");
  CHECK(CaptureCore::iv_fault_text(999) == nullptr);
}

TEST(mppt_count_from_store) {
  Rig r;
  CHECK_EQ(r.cap.mppt_count(0), 0);
  r.store.set_scalar(0, 0x2040, dm::TYPE_U16, 0.1f, 3000, 2);
  r.store.set_scalar(0, 0x2042, dm::TYPE_U16, 0.1f, 2900, 2);
  r.store.set_scalar(0, 0x2041, dm::TYPE_U16, 0.01f, 500, 2);  // an MPPT current: not counted
  CHECK_EQ(r.cap.mppt_count(0), 2);
  CHECK_EQ(r.cap.mppt_count(1), 0);
}

TEST(iv_capture_high_word_first_state) {
  Rig r;
  r.inv.high_word_first = true;
  std::string why;
  CHECK(r.cap.start_iv(0, 1, "x", &why));
  r.finish(80000);
  CHECK(r.cap.results().back()->ok);
}

TEST(iv_capture_failure_refusal_and_guards) {
  Rig r;
  r.inv.iv_fail = true;
  std::string why;
  CHECK(r.cap.start_iv(0, 1, "x", &why));
  CHECK(!r.cap.start_iv(0, 1, "x", &why));  // one at a time
  r.finish(60000);
  const Capture &c = *r.cap.results().back();
  CHECK(!c.ok);
  CHECK(c.result.find("fault code 5: not grid-tied running") != std::string::npos);
  CHECK(c.data.empty());
  // Minimum interval between I-V starts.
  CHECK(!r.cap.start_iv(0, 1, "x", &why));
  CHECK(why.find("wait") != std::string::npos);
  r.run(CaptureCore::IV_MIN_INTERVAL_MS);
  r.inv.iv_fail = false;
  r.inv.refuse_iv = true;
  CHECK(r.cap.start_iv(0, 1, "x", &why));
  r.finish(10000);
  CHECK(r.cap.results().back()->result.find("1000-scans-per-boot") != std::string::npos);
  CHECK_EQ(r.cap.results().size(), 1u);  // one I-V kept
}

TEST(iv_capture_unsupported_refused) {
  Rig r(0x025040D5u);  // bit 3 clear
  std::string why;
  CHECK(!r.cap.start_iv(0, 1, "x", &why));
  CHECK(why.find("bit 3") != std::string::npos);
  CHECK(!r.cap.start_grid(0, 1, 1, "x", &why));
}

TEST(grid_capture_end_to_end) {
  Rig r;
  std::string why;
  CHECK(r.cap.grid_supported(0));
  CHECK(!r.cap.start_grid(0, 1, 9, "x", &why));
  CHECK(r.cap.start_grid(0, 1, 4, "SH10RS", &why));
  r.run(6000);
  CHECK(r.cap.step_text().find("recording") == 0);
  r.finish(120000);
  CHECK(!r.m.exclusive());
  const Capture &c = *r.cap.results().back();
  CHECK(c.ok);
  CHECK(c.kind == Kind::GRID);
  CHECK_EQ(c.cycles, 4);
  CHECK(c.file_name == "GridWaveRecord.txt");
  CHECK(c.data == r.inv.grid_file);
  CHECK_EQ(r.inv.regs[3u << 16 | 32010], 0xAA);
  CHECK_EQ(r.inv.regs[3u << 16 | 32011], 4);
  CHECK_EQ(r.inv.regs[3u << 16 | 32000], 0x01B0);
  CHECK_EQ(c.records, 1);
}

TEST(grid_capture_stale_100_not_exported) {
  Rig r;
  r.inv.grid_ignore_start = true;  // the inverter silently ignores the start; progress stays 100
  std::string why;
  CHECK(r.cap.start_grid(0, 1, 1, "x", &why));
  r.finish(60000);
  const Capture &c = *r.cap.results().back();
  CHECK(!c.ok);
  CHECK(c.result.find("did not start") != std::string::npos);
  CHECK_EQ(r.inv.regs.count(3u << 16 | 32000), 0u);  // no file select sent
  CHECK_EQ(r.inv.raw_bytes, 0);
}

TEST(grid_capture_keeps_newest_four) {
  Rig r;
  std::string why;
  for (int i = 1; i <= 6; i++) {
    CHECK(r.cap.start_grid(0, 1, static_cast<uint8_t>(i > 8 ? 8 : (i % 8) + 1), "x", &why));
    r.finish(120000);
    r.run(100);
  }
  size_t grids = 0;
  for (const auto &c : r.cap.results())
    grids += c->kind == Kind::GRID;
  CHECK_EQ(grids, 4u);
  CHECK_EQ(r.cap.results().front()->id, 3u);
}

TEST(fault_capture_end_to_end) {
  Rig r;
  r.inv.fault_records = 3;
  std::string why;
  CHECK(r.cap.fault_supported(0));
  CHECK(std::string(kind_name(Kind::FAULT)) == "fault");
  CHECK(r.cap.start_fault(0, 1, "SH10RS", &why));
  CHECK(r.cap.running());
  CHECK(r.cap.running_kind() == Kind::FAULT);
  r.run(200);
  CHECK(r.cap.step_text() == "waiting for the record list");
  r.run(5000);
  CHECK(r.cap.step_text().find("copying the record store") == 0);
  CHECK(!r.m.exclusive());  // normal polling continues while the inverter copies
  r.finish(120000);
  CHECK(!r.cap.running());
  CHECK(!r.m.exclusive());
  CHECK(!r.m.raw());
  const Capture &c = *r.cap.results().back();
  CHECK(c.ok);
  CHECK(c.kind == Kind::FAULT);
  CHECK_EQ(c.records, 3);
  CHECK_EQ(c.last_regs[1], 3);
  CHECK(c.file_name == "FaultUpload_packet_01.txt");
  CHECK(c.data == r.inv.fault_file);
  // Write order: catalogue re-read, the fetch repeated until accepted, the file select.
  const auto w = r.writes_sent();
  size_t fetches = 0;
  CHECK(w.size() >= 4);
  CHECK(w[0].first == 30034 && w[0].second == 0xAA);
  for (size_t i = 1; i + 1 < w.size(); i++) {
    CHECK(w[i].first == 32000 && w[i].second == 0x01A1);
    fetches++;
  }
  CHECK(fetches >= 2);
  CHECK(r.inv.fetch_refused >= 1);
  CHECK(w.back().first == 32000 && w.back().second == 0x00B0);
  bool refused_noted = false;
  for (const auto &t : c.timeline)
    refused_noted |= t.find("fetch refused: no records ready") != std::string::npos;
  CHECK(refused_noted);
  // No poll frames between the select write and the end of the transfer.
  size_t sel = 0, last_raw = 0;
  for (size_t i = 0; i < r.inv.frames.size(); i++) {
    const auto &f = r.inv.frames[i];
    if (f.size() > 8 && f[1] == 16 && f[7] == 0x00 && f[8] == 0xB0)
      sel = i;
    if (f.size() < 4)
      last_raw = i;
  }
  CHECK(sel > 0 && last_raw > sel);
  CHECK_EQ(r.polls_between(sel, last_raw), 0u);
  const std::string js = CaptureCore::metadata_json(c);
  CHECK(js.find("\"kind\":\"fault\"") != std::string::npos);
  CHECK(js.find("\"records\":3") != std::string::npos);
}

TEST(fault_capture_no_records_times_out) {
  Rig r;
  r.inv.fault_records = 0;
  std::string why;
  CHECK(r.cap.start_fault(0, 1, "x", &why));
  r.finish(60000);
  const Capture &c = *r.cap.results().back();
  CHECK(!c.ok);
  CHECK(c.result.find("no stored fault recordings") != std::string::npos);
  const uint32_t took = c.finished_ms - c.started_ms;
  CHECK(took >= CaptureCore::FAULT_START_MS && took < CaptureCore::FAULT_START_MS + 3000);
  CHECK(r.inv.fetch_refused >= 25);
  CHECK_EQ(r.inv.regs.count(3u << 16 | 32000), 0u);  // no accepted command write
  CHECK_EQ(r.inv.raw_bytes, 0);
}

TEST(fault_capture_stale_100_not_exported) {
  Rig r;
  r.inv.fault_records = 2;
  r.inv.fault_ignore_fetch = true;  // the fetch is accepted but nothing is copied
  std::string why;
  CHECK(r.cap.start_fault(0, 1, "x", &why));
  r.finish(60000);
  const Capture &c = *r.cap.results().back();
  CHECK(!c.ok);
  CHECK(c.result.find("did not start") != std::string::npos);
  CHECK_EQ(r.inv.regs[3u << 16 | 32000], 0x01A1);  // the fetch was the last command: no file select
  CHECK_EQ(r.inv.raw_bytes, 0);
}

TEST(fault_capture_keeps_one_and_grid_four) {
  Rig r;
  r.inv.fault_records = 3;
  std::string why;
  for (int i = 1; i <= 5; i++) {
    CHECK(r.cap.start_grid(0, 1, 1, "x", &why));
    r.finish(120000);
    r.run(100);
  }
  for (int i = 0; i < 2; i++) {
    r.inv.fault_scanned = false;
    CHECK(r.cap.start_fault(0, 1, "x", &why));
    r.finish(120000);
    r.run(100);
  }
  size_t grids = 0, faults = 0;
  for (const auto &c : r.cap.results()) {
    grids += c->kind == Kind::GRID;
    faults += c->kind == Kind::FAULT;
  }
  CHECK_EQ(grids, 4u);
  CHECK_EQ(faults, 1u);
  CHECK_EQ(r.cap.results().front()->id, 2u);  // the oldest grid capture went
  CHECK_EQ(r.cap.results().back()->id, 7u);   // the newest fault capture stays
  CHECK(r.cap.results().back()->ok);
}

TEST(capture_cancel_releases_bus) {
  Rig r;
  std::string why;
  r.inv.iv_scan_ms = 2000;
  CHECK(r.cap.start_iv(0, 1, "x", &why));
  // Stop mid-transfer.
  for (int i = 0; i < 4000 && !r.m.raw(); i++)
    r.run(5);
  CHECK(r.m.raw());
  r.cap.cancel();
  CHECK(!r.cap.running());
  CHECK(!r.m.exclusive());
  CHECK(r.cap.results().back()->result == "cancelled");
}

int main() { return th::run_all(); }
