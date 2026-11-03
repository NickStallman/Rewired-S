#include "spa_handler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "esphome/core/hal.h"  // millis()
#include "esphome/core/log.h"

#include <esp_http_server.h>
#include <esp_random.h>

#include "../sungrow_datamodel/signal_store.h"
#include "../sungrow_modbus/master.h"
#include "../sungrow_modbus/poll_plan.h"
#include "sungrow_web.h"
#if defined(USE_WINET_S) || defined(USE_WINET_S2)
#include "../sungrow_board/sungrow_board.h"
#define SUNGROW_WEB_HAVE_BOARD
#endif
// web_assets.h has internal-linkage (static) definitions, so it is included from exactly this
// one translation unit (see tools/gen_web_assets.py / web_assets.h header note).
#include "web_assets.h"

namespace esphome {
namespace sungrow_web {

using sungrow_datamodel::Family;
using sungrow_datamodel::Signal;
using sungrow_datamodel::SignalStore;
using sungrow_datamodel::ValueKind;
using sungrow_modbus::Device;
using sungrow_modbus::DevInfo;
using sungrow_modbus::ModbusMaster;

static const char *const TAG = "sungrow_web";

// Family -> the lowercase key used by the SPA's baked meta table (web/src/meta.gen.ts,
// keyed by the register_meta.py FAMILIES names). "" when the device has no table yet.
static const char *family_name(const Device &d) {
  const sungrow_datamodel::RegisterTable *t = d.table();
  if (t == nullptr)
    return "";
  switch (t->family) {
    case Family::INVERTER:
      return "inverter";
    case Family::BATTERY:
      return "battery";
    case Family::CHARGER:
      return "charger";
    case Family::ENERGY_BRIDGE:
      return "energy_bridge";
    case Family::METER:
      return "meter";
    case Family::LOGGER:
      return "logger";
  }
  return "";
}

// ---- low-level response helpers (mirror flash_tools.cpp) ----------------------------------------
// API handlers run on the main loop (SungrowWeb::run_on_loop); while one runs, g_reply is set and
// the helpers below fill it in instead of writing to the socket. The httpd task sends it after.
static WebReply *g_reply = nullptr;

static const char *status_text(int code) {
  return code == 200   ? "200 OK"
         : code == 400 ? "400 Bad Request"
         : code == 404 ? "404 Not Found"
         : code == 409 ? "409 Conflict"
         : code == 503 ? "503 Service Unavailable"
                       : "500 Internal Server Error";
}

static void send_reply(AsyncWebServerRequest *request, const WebReply &r) {
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, status_text(r.code));
  httpd_resp_set_type(req, r.type);
  if (r.no_store)
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  std::string disposition;
  if (!r.filename.empty()) {
    disposition = "attachment; filename=\"" + r.filename + "\"";
    httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
  }
  httpd_resp_send(req, r.body.data(), static_cast<ssize_t>(r.body.size()));
}

static void send_plain(AsyncWebServerRequest *request, int code, const char *body) {
  if (g_reply != nullptr) {
    g_reply->code = code;
    g_reply->type = "text/plain";
    g_reply->no_store = false;
    g_reply->body = body;
    return;
  }
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, code == 200   ? "200 OK"
                             : code == 400 ? "400 Bad Request"
                             : code == 404 ? "404 Not Found"
                                           : "500 Internal Server Error");
  httpd_resp_set_type(req, "text/plain");
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static void send_json(AsyncWebServerRequest *request, std::string body) {
  if (g_reply != nullptr) {
    g_reply->code = 200;
    g_reply->type = "application/json";
    g_reply->no_store = true;
    g_reply->body = std::move(body);
    return;
  }
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}

// JSON with an explicit status (loop side only: the settings endpoints answer 409 MANAGED_BY_YAML).
static void send_json_status(AsyncWebServerRequest *request, int code, std::string body) {
  send_json(request, std::move(body));
  if (g_reply != nullptr)
    g_reply->code = code;
}

// Serve one embedded gzip asset. `cache` must outlive the send (string literal).
static void send_gzip(AsyncWebServerRequest *request, const uint8_t *data, size_t len, const char *type,
                      const char *cache) {
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", cache);
  httpd_resp_send(req, reinterpret_cast<const char *>(data), static_cast<ssize_t>(len));
}

// Append one control character as a \u00XX JSON escape.
static void append_u_escape(std::string &o, unsigned char c) {
  static const char HEX[] = "0123456789abcdef";
  o += "\\u00";
  o += HEX[(c >> 4) & 0xF];
  o += HEX[c & 0xF];
}

// JSON-escape a C string into `o`, GUARANTEEING valid UTF-8 output. String-typed Modbus registers
// can hold arbitrary bytes (raw/uninitialised registers such as 0x221A, or the 0x2181 capability
// bitmap), and a single byte >= 0x80 that is not part of a well-formed UTF-8 sequence would make
// the whole /api/signals body invalid UTF-8 and break JSON.parse in the browser, taking down the
// entire response for one bad value. Here: " and \ are backslash-escaped, control chars become
// \u00XX, well-formed UTF-8 sequences pass through verbatim, and any invalid / overlong / surrogate
// / truncated byte is replaced with U+FFFD (0xEF 0xBF 0xBD), which is itself valid UTF-8.
static void json_escape_into(std::string &o, const char *s) {
  const auto *p = reinterpret_cast<const unsigned char *>(s);
  while (*p != '\0') {
    unsigned char c = *p;
    if (c < 0x80) {
      if (c == '"' || c == '\\') {
        o += '\\';
        o += static_cast<char>(c);
      } else if (c < 0x20) {
        append_u_escape(o, c);
      } else {
        o += static_cast<char>(c);
      }
      ++p;
      continue;
    }
    // Multi-byte lead byte: pick the expected length + the tighter range for the FIRST
    // continuation byte (rejects overlong forms and UTF-16 surrogates), rest are 0x80..0xBF.
    int len = 0;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      if (c == 0xE0)
        lo = 0xA0;  // reject overlong
      else if (c == 0xED)
        hi = 0x9F;  // reject surrogates U+D800..DFFF
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      if (c == 0xF0)
        lo = 0x90;  // reject overlong
      else if (c == 0xF4)
        hi = 0x8F;  // reject > U+10FFFF
    }
    bool ok = len > 0;
    for (int i = 1; ok && i < len; ++i) {
      unsigned char cc = p[i];
      const unsigned char min = (i == 1) ? lo : 0x80;
      const unsigned char max = (i == 1) ? hi : 0xBF;
      if (cc < min || cc > max)
        ok = false;  // also catches the NUL terminator (0x00 < 0x80)
    }
    if (ok) {
      for (int i = 0; i < len; ++i)
        o += static_cast<char>(p[i]);
      p += len;
    } else {
      o += "\xEF\xBF\xBD";  // U+FFFD
      ++p;
    }
  }
}

static std::string fmt_num(double v) {
  if (!std::isfinite(v))
    return "null";
  char buf[32];
  // Whole numbers (counters, u32 codes such as module date codes) print exactly; others
  // keep 7 significant digits (the float scale makes later digits noise).
  if (std::fabs(v) < 1e15 && v == std::floor(v))
    std::snprintf(buf, sizeof(buf), "%.0f", v);
  else
    std::snprintf(buf, sizeof(buf), "%.7g", v);
  return buf;
}

// ---- JSON builders ------------------------------------------------------------------------------

// {"inv_type":0,"protocol":3,"country_reg":32310,"grid_reg":32311,"power_switch_reg":5006} or null.
static void append_descriptor(std::string &j, const sungrow_modbus::FamilyDescriptor *d) {
  if (d == nullptr) {
    j += "null";
    return;
  }
  char b[160];
  std::snprintf(b, sizeof(b),
                "{\"inv_type\":%u,\"protocol\":%u,\"country_reg\":%u,\"grid_reg\":%u,\"power_switch_reg\":%u}",
                static_cast<unsigned>(d->inv_type), static_cast<unsigned>(d->protocol),
                static_cast<unsigned>(d->country_reg), static_cast<unsigned>(d->grid_reg),
                static_cast<unsigned>(d->power_switch_reg));
  j += b;
}

// "control":{"profile":"hybrid"|"string"|null,"expect":{...}|null}: the control profile attached
// to the device (ControlCore::profile_name; null without one, so the SPA offers no controls) and
// the follow-up of its last start/stop command (ControlCore::expect_status), null without one or
// without a bound control component.
static void append_control(std::string &j, const void *core, uint8_t device) {
  j += "\"control\":{\"profile\":";
#ifdef SUNGROW_WEB_BIND_CONTROL
  const auto *cc = static_cast<const sungrow_control::ControlCore *>(core);
  const char *profile = cc != nullptr ? cc->profile_name(device) : nullptr;
  if (profile != nullptr) {
    j += '"';
    json_escape_into(j, profile);
    j += '"';
  } else {
    j += "null";
  }
#else
  j += "null";
#endif
  j += ",\"expect\":";
#ifdef SUNGROW_WEB_BIND_CONTROL
  sungrow_control::ExpectStatus st;
  if (cc != nullptr && cc->expect_status(device, &st)) {
    char head[96];
    std::snprintf(head, sizeof(head), "{\"kind\":\"%s\",\"key\":\"%s\",\"final\":%s,\"ok\":%s,\"age_ms\":%u,",
                  st.kind == sungrow_control::ExpectKind::STOP ? "stop" : "start", st.key, st.final ? "true" : "false",
                  st.ok ? "true" : "false", static_cast<unsigned>(st.age_ms));
    j += head;
    j += "\"text\":\"";
    json_escape_into(j, st.text.c_str());
    j += "\",\"state\":";
    if (st.state_valid) {
      char code[12];
      std::snprintf(code, sizeof(code), "\"0x%04X\"", st.state);
      j += code;
    } else {
      j += "null";
    }
    j += "}}";
    return;
  }
#else
  (void) core;
  (void) device;
#endif
  j += "null}";
}

static std::string build_devices(ModbusMaster *master, const SignalStore *store, const void *core) {
  std::string j = "[";
  if (master != nullptr) {
    bool first = true;
    for (const Device &d : master->devices()) {
      // Skip abandoned slots (a probed address that never answered, a remembered address the
      // search replaced) so the SPA never sees phantom devices.
      if (d.abandoned())
        continue;
      const DevInfo &di = d.devinfo();
      const sungrow_modbus::LinkStats &st = d.stats();
      if (!first)
        j += ',';
      first = false;
      char head[160];
      std::snprintf(head, sizeof(head),
                    "{\"index\":%u,\"address\":%u,\"dev_type\":%u,\"dev_code\":%u,\"protocol\":%u,",
                    static_cast<unsigned>(d.config().index), static_cast<unsigned>(d.config().address),
                    static_cast<unsigned>(di.dev_type), static_cast<unsigned>(di.dev_code),
                    static_cast<unsigned>(di.protocol));
      j += head;
      j += "\"model\":\"";
      json_escape_into(j, di.model);
      j += "\",\"serial\":\"";
      // Device S/N (sig 0x0002: inverter devinfo, SBR 10711, charger 21201, Energy Bridge 4097).
      if (store != nullptr && store->is_valid(d.config().index, 0x0002))
        json_escape_into(j, store->get_string(d.config().index, 0x0002).c_str());
      j += "\",";
      j += d.online() ? "\"online\":true," : "\"online\":false,";
      j += d.discovered() ? "\"discovered\":true," : "\"discovered\":false,";
      j += d.state() == Device::State::RUN ? "\"state\":\"RUN\"," : "\"state\":\"DISCOVER\",";
      j += "\"family\":\"";
      j += family_name(d);
      j += "\",";
      char ident[200];
      std::snprintf(ident, sizeof(ident),
                    "\"xml_family\":\"%s\",\"inv_type\":%u,\"inv_type_known\":%s,\"dev_type_source\":\"%s\",",
                    sungrow_modbus::xml_family_name(d.xml_family()), static_cast<unsigned>(d.inv_type()),
                    d.inv_type_known() ? "true" : "false", d.dev_type_source());
      j += ident;
      j += "\"proto_code\":\"";
      json_escape_into(j, di.proto_code);
      j += "\",\"descriptor\":";
      append_descriptor(j, d.descriptor());
      j += ',';
      char stats[200];
      const float loss = st.loss_pct();
      std::snprintf(stats, sizeof(stats),
                    "\"stats\":{\"sent\":%u,\"received\":%u,\"timeouts\":%u,\"exceptions\":%u,\"loss_pct\":%s},",
                    static_cast<unsigned>(st.sent), static_cast<unsigned>(st.received),
                    static_cast<unsigned>(st.timeouts), static_cast<unsigned>(st.exceptions),
                    std::isnan(loss) ? "-1" : fmt_num(loss).c_str());
      j += stats;
      // Poll plan + freshness: reads per refresh tier (unserved = no values, polled at the static
      // period) and the age of the most frequently refreshed values.
      unsigned tiers[sungrow_modbus::POLL_TIER_COUNT] = {0, 0, 0, 0};
      unsigned unserved = 0;
      for (const auto &jb : d.jobs()) {
        tiers[static_cast<size_t>(jb.tier)]++;
        unserved += jb.unserved;
      }
      char poll[220];
      std::snprintf(poll, sizeof(poll),
                    "\"poll\":{\"initial_pass_done\":%s,\"fresh_age_ms\":%ld,\"reads\":{\"fast\":%u,\"live\":%u,"
                    "\"slow\":%u,\"static\":%u},\"unserved\":%u},",
                    d.first_round_done() ? "true" : "false",
                    d.fresh_ms() != 0 ? static_cast<long>(millis() - d.fresh_ms()) : -1L, tiers[0], tiers[1],
                    tiers[2], tiers[3], unserved);
      j += poll;
      append_control(j, core, d.config().index);
      j += '}';
    }
  }
  j += "]";
  return j;
}

// /api/signals?keys=0x2080,0x2086&class=fast. `keys` lists up to MAX_KEYS sigids (hex or
// decimal); `class` names a poll tier of the device's register table. Both combine as AND.
// Synthetic signals (sigid >= 0xF000: derived values with no register of their own) always pass
// the tier filter.
struct SignalFilter {
  static constexpr size_t MAX_KEYS = 64;
  static constexpr uint16_t SYNTHETIC_FIRST = 0xF000;
  std::vector<uint16_t> keys;  ///< sorted; empty = every key
  int tier{-1};                ///< -1 = every tier, else a sungrow_modbus::PollTier
};

/// False on a malformed list, an unknown class or more than MAX_KEYS keys.
static bool parse_signal_filter(const std::string &keys, const std::string &cls, SignalFilter *out) {
  out->keys.clear();
  out->tier = -1;
  if (!cls.empty()) {
    static const char *const NAMES[] = {"fast", "live", "slow", "static"};
    for (size_t i = 0; i < sungrow_modbus::POLL_TIER_COUNT; i++)
      if (cls == NAMES[i])
        out->tier = static_cast<int>(i);
    if (out->tier < 0)
      return false;
  }
  size_t pos = 0;
  while (pos < keys.size()) {
    size_t end = keys.find(',', pos);
    if (end == std::string::npos)
      end = keys.size();
    const std::string tok = keys.substr(pos, end - pos);
    pos = end + 1;
    if (tok.empty())
      continue;
    const bool hex = tok.size() > 2 && tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X');
    char *e = nullptr;
    const long v = std::strtol(tok.c_str() + (hex ? 2 : 0), &e, hex ? 16 : 10);
    if (e == tok.c_str() + (hex ? 2 : 0) || *e != '\0' || v < 0 || v > 0xFFFF || out->keys.size() >= SignalFilter::MAX_KEYS)
      return false;
    out->keys.push_back(static_cast<uint16_t>(v));
  }
  std::sort(out->keys.begin(), out->keys.end());
  return true;
}

/// Sorted sigids of the rows of `t` polled at `tier`, plus the children of the bitfield rows
/// among them. Empty when the device has no active table yet.
static std::vector<uint16_t> tier_sigids(const sungrow_datamodel::RegisterTable *t, sungrow_modbus::PollTier tier) {
  std::vector<uint16_t> out;
  if (t == nullptr)
    return out;
  for (uint16_t i = 0; i < t->count; i++) {
    const sungrow_datamodel::RegisterRow &r = t->rows[i];
    if (r.sigid == 0 || sungrow_modbus::poll_tier(*t, r) != tier)
      continue;
    out.push_back(r.sigid);
    if (r.nbits == 0)
      continue;
    for (const auto &b : sungrow_datamodel::kBitfields)
      if (b.table == t->id && b.parent_sigid == r.sigid)
        for (uint8_t bit = 0; bit < b.nbits; bit++)
          out.push_back(static_cast<uint16_t>(b.child_base + bit));
  }
  std::sort(out.begin(), out.end());
  return out;
}

/// Active register table of store device `device` (nullptr while discovering / unknown).
static const sungrow_datamodel::RegisterTable *device_table(ModbusMaster *master, int device) {
  if (master == nullptr)
    return nullptr;
  for (const auto &d : master->devices())
    if (d.config().index == device && !d.abandoned())
      return d.table();
  return nullptr;
}

// The signals body of a hybrid with battery stacks is tens of KB: one reserve up front (malloc puts
// >= 1 KB in PSRAM) instead of repeated doubling.
static std::string build_signals(SignalStore *store, ModbusMaster *master, int device, const SignalFilter &f) {
  std::vector<uint16_t> tier_sigs;
  if (f.tier >= 0)
    tier_sigs = tier_sigids(device_table(master, device), static_cast<sungrow_modbus::PollTier>(f.tier));
  const auto passes = [&](const Signal &s) {
    if (s.device != device || !s.valid)
      return false;
    if (!f.keys.empty() && !std::binary_search(f.keys.begin(), f.keys.end(), s.sigid))
      return false;
    if (f.tier >= 0 && s.sigid < SignalFilter::SYNTHETIC_FIRST &&
        !std::binary_search(tier_sigs.begin(), tier_sigs.end(), s.sigid))
      return false;
    return true;
  };
  std::string j;
  size_t n = 0;
  if (store != nullptr)
    store->for_each([&](const Signal &s) { n += passes(s); });
  j.reserve(64 + n * 72);
  j = "{\"device\":";
  j += std::to_string(device);
  j += ",\"now_ms\":";
  j += std::to_string(store != nullptr ? store->now() : 0);
  j += ",\"signals\":[";
  if (store != nullptr) {
    bool first = true;
    store->for_each([&](const Signal &s) {
      // Only signals that hold a valid value: the SPA labels/filters per device family, and
      // never-populated entries are noise.
      if (!passes(s))
        return;
      if (!first)
        j += ',';
      first = false;
      char key[16];
      std::snprintf(key, sizeof(key), "0x%04X", s.sigid);
      j += "{\"key\":\"";
      j += key;
      j += "\",\"value\":";
      if (s.kind == ValueKind::STRING) {
        j += '"';
        json_escape_into(j, s.str().c_str());
        j += '"';
      } else if (s.kind == ValueKind::ARRAY) {
        j += "null";  // raw multi-register blob; no scalar representation
      } else {
        j += fmt_num(s.number());
      }
      char tail[48];
      std::snprintf(tail, sizeof(tail), ",\"valid\":true,\"updated_ms\":%u}", static_cast<unsigned>(s.updated_ms));
      j += tail;
    });
  }
  j += "]}";
  return j;
}

// ---- handler ------------------------------------------------------------------------------------

// STRICT exact-path whitelist. Anything not listed returns false and falls through to web_server
// (so /events, the hashed web_server chunks /0.js /0.css, and /<domain>/<id> work unchanged).
static bool is_our_get_path(const char *url) {
  return std::strcmp(url, "/") == 0 || std::strcmp(url, "/index.html") == 0 ||
         std::strcmp(url, APP_JS_PATH) == 0 || std::strcmp(url, APP_CSS_PATH) == 0 ||
         std::strcmp(url, "/api/devices") == 0 || std::strcmp(url, "/api/signals") == 0 ||
         std::strcmp(url, "/api/grid_read") == 0 || std::strcmp(url, "/api/setup_options") == 0 ||
         std::strcmp(url, "/api/setup_preview") == 0 || std::strcmp(url, "/api/capture") == 0 ||
         std::strcmp(url, "/api/capture_meta") == 0 || std::strcmp(url, "/api/capture_file") == 0 ||
         std::strcmp(url, "/api/diag_send") == 0 || std::strcmp(url, "/api/diag_report") == 0 ||
         std::strcmp(url, "/api/web_login") == 0 || std::strcmp(url, "/api/mqtt_settings") == 0 ||
         std::strcmp(url, "/api/cloud_version") == 0 || std::strcmp(url, "/api/boot_slot") == 0;
}

// POST endpoints (inverter writes). Added to the whitelist; still behind digest auth.
static bool is_our_post_path(const char *url) {
  return std::strcmp(url, "/api/write") == 0 || std::strcmp(url, "/api/confirm") == 0 ||
         std::strcmp(url, "/api/setup_write") == 0 || std::strcmp(url, "/api/grid_read_start") == 0 ||
         std::strcmp(url, "/api/setup_apply") == 0 || std::strcmp(url, "/api/setup_start") == 0 ||
         std::strcmp(url, "/api/capture_start") == 0 || std::strcmp(url, "/api/capture_cancel") == 0 ||
         std::strcmp(url, "/api/diag_send") == 0 || std::strcmp(url, "/api/web_login") == 0 ||
         std::strcmp(url, "/api/mqtt_settings") == 0 || std::strcmp(url, "/api/cloud_version") == 0 ||
         std::strcmp(url, "/api/boot_slot") == 0;
}

bool SpaHandler::canHandle(AsyncWebServerRequest *request) const {
  const http_method m = request->method();
  if (m != HTTP_GET && m != HTTP_POST)
    return false;
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(buf);
  return m == HTTP_GET ? is_our_get_path(url.c_str()) : is_our_post_path(url.c_str());
}

// Confirm-token helpers ---------------------------------------------------------------------------
uint32_t SpaHandler::issue_confirm(uint8_t device, uint16_t address, uint32_t now_ms) {
  uint32_t tok = esp_random();
  if (tok == 0)
    tok = 1;
  // Reuse an expired/empty slot, else the oldest (soonest to expire).
  ConfirmToken *slot = &this->tokens_[0];
  for (auto &t : this->tokens_) {
    if (t.token == 0 || now_ms >= t.expiry_ms) {
      slot = &t;
      break;
    }
    if (t.expiry_ms < slot->expiry_ms)
      slot = &t;
  }
  slot->token = tok;
  slot->expiry_ms = now_ms + CONFIRM_TTL_MS;
  slot->device = device;
  slot->address = address;
  return tok;
}

bool SpaHandler::consume_confirm(uint8_t device, uint16_t address, uint32_t token, uint32_t now_ms) {
  if (token == 0)
    return false;
  for (auto &t : this->tokens_) {
    if (t.token == token && t.device == device && t.address == address && now_ms < t.expiry_ms) {
      t.token = 0;  // single use
      return true;
    }
  }
  return false;
}

// Parse an integer query arg into [lo, hi]. Returns false if missing or malformed.
static bool arg_int(AsyncWebServerRequest *request, const char *name, long lo, long hi, long *out) {
  const std::string s = request->arg(name);
  if (s.empty())
    return false;
  char *end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0' || v < lo || v > hi)
    return false;
  *out = v;
  return true;
}

// `note` (optional): items an accepted Setup batch skipped (ControlCore::setup_note).
static void send_write_result(AsyncWebServerRequest *request, bool ok, const char *verdict, const char *message,
                              const char *note = nullptr) {
  std::string j = "{\"ok\":";
  j += ok ? "true" : "false";
  j += ",\"verdict\":\"";
  j += verdict;
  j += "\",\"message\":\"";
  json_escape_into(j, message);
  j += '"';
  if (note != nullptr && note[0] != '\0') {
    j += ",\"note\":\"";
    json_escape_into(j, note);
    j += '"';
  }
  j += '}';
  send_json(request, j);
}

#ifdef SUNGROW_WEB_BIND_CONTROL
// Write-result JSON of a ControlCore verdict: verdict = stable code, message = the setup detail
// ("range:32323", "relation:32319", ...) when there is one, else the verdict text.
static void send_verdict(AsyncWebServerRequest *request, sungrow_control::Verdict v, const std::string &detail,
                         const char *note = nullptr) {
  const bool ok = v == sungrow_control::Verdict::OK;
  send_write_result(request, ok, sungrow_control::verdict_code(v),
                    ok ? "queued" : (!detail.empty() ? detail.c_str() : sungrow_control::verdict_name(v)), note);
}
#endif

static uint32_t parse_token(const std::string &s) {
  char *end = nullptr;
  return s.empty() ? 0u : static_cast<uint32_t>(std::strtoul(s.c_str(), &end, 16));
}

void SpaHandler::handle_confirm_(AsyncWebServerRequest *request) {
  long device = 0, address = 0;
  if (!arg_int(request, "device", 0, 255, &device) ||
      !arg_int(request, "address", 1, 0xFFFF, &address)) {
    send_plain(request, 400, "need ?device=N&address=A\n");
    return;
  }
  const uint32_t tok = this->issue_confirm(static_cast<uint8_t>(device), static_cast<uint16_t>(address),
                                           millis());
  char body[96];
  std::snprintf(body, sizeof(body), "{\"token\":\"%08x\",\"ttl_ms\":%u}", static_cast<unsigned>(tok),
                static_cast<unsigned>(CONFIRM_TTL_MS));
  send_json(request, body);
}

// POST /api/write?device=D&key=K&value=V or ?device=D&address=A&value=V: one routine register
// through the gate with authorized = true (the web login authorises it; writes_enabled does not
// apply). A rule key resolves to the register bound to the device's protocol type
// (ControlCore::address_for_key: start/stop is 5006, 13000 or 30045); an address is used as
// given.
void SpaHandler::handle_write_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_CONTROL
  sungrow_control::SungrowControl *ctl = this->parent_->control();
  if (ctl == nullptr) {
    send_write_result(request, false, "UNAVAILABLE", "writes not configured");
    return;
  }
  long device = 0, address = 0, value = 0;
  if (!arg_int(request, "device", 0, 255, &device) || !arg_int(request, "value", -32768, 0xFFFF, &value)) {
    send_write_result(request, false, "INVALID", "need device, key or address, value");
    return;
  }
  const std::string key = request->arg("key");
  if (!key.empty()) {
    uint16_t bound = 0;
    if (!ctl->core().address_for_key(static_cast<uint8_t>(device), key.c_str(), &bound)) {
      send_write_result(request, false, "NOT_ALLOWED", "no such control on this device");
      return;
    }
    address = bound;
  } else if (!arg_int(request, "address", 1, 0xFFFF, &address)) {
    send_write_result(request, false, "INVALID", "need device, key or address, value");
    return;
  }
  const uint16_t raw = static_cast<uint16_t>(value & 0xFFFF);
  const sungrow_control::Verdict v =
      ctl->core().write(static_cast<uint8_t>(device), static_cast<uint16_t>(address), &raw, 1, ctl->default_verify(),
                        "api:web", {}, {}, /*authorized=*/true);
  send_verdict(request, v, "");
#else
  send_write_result(request, false, "UNAVAILABLE", "writes not configured");
#endif
}

// Accumulate a non-form POST body (text/plain) across chunks, keyed by the request object that
// handleRequest receives for the same request; handleRequest takes the entry. The server does not
// call handleRequest for a transfer that broke off, so an entry BODY_TTL_MS old is dropped here
// (the request object's address can be reused by a later request).
void SpaHandler::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
  const uint32_t now = millis();
  std::lock_guard<std::mutex> lk(this->bodies_mutex_);
  if (index == 0) {
    for (auto it = this->bodies_.begin(); it != this->bodies_.end();) {
      if (now - it->second.started_ms >= BODY_TTL_MS)
        it = this->bodies_.erase(it);
      else
        ++it;
    }
    PostBody &b = this->bodies_[request];
    b.data.clear();
    b.total = total;
    b.started_ms = now;
  }
  auto it = this->bodies_.find(request);
  if (it == this->bodies_.end() || total > BODY_MAX)
    return;  // oversize: the body stays short of its length, so the handler reports INVALID
  if (data != nullptr && len > 0 && it->second.data.size() + len <= BODY_MAX)
    it->second.data.append(reinterpret_cast<const char *>(data), len);
}

// The text/plain body of `request`, complete; "" for a form POST, an oversize body or an
// incomplete one. The entry is removed.
std::string SpaHandler::take_body_(AsyncWebServerRequest *request) {
  std::string body;
  std::lock_guard<std::mutex> lk(this->bodies_mutex_);
  auto it = this->bodies_.find(request);
  if (it != this->bodies_.end()) {
    if (it->second.data.size() == it->second.total)
      body = std::move(it->second.data);
    this->bodies_.erase(it);
  }
  return body;
}

// POST /api/setup_write?device=D&confirm=XXXX  body (text/plain): "addr:val,addr:val,..." (decimal,
// 1-based document addresses, raw 16-bit values; a U32 setting is two consecutive addr:val pairs,
// low word first). The confirm token (bound to device + SETUP_CONFIRM_ADDR) authorises the whole
// Installer batch; ControlCore::setup_apply validates it (family allow-list, ranges, relations).
void SpaHandler::handle_setup_write_(AsyncWebServerRequest *request, const std::string &body) {
#ifdef SUNGROW_WEB_BIND_CONTROL
  sungrow_control::SungrowControl *ctl = this->parent_->control();
  if (ctl == nullptr) {
    send_write_result(request, false, "UNAVAILABLE", "writes not configured");
    return;
  }
  long device = 0;
  if (!arg_int(request, "device", 0, 255, &device)) {
    send_write_result(request, false, "INVALID", "need device");
    return;
  }
  if (body.empty() || body.size() > 8192) {
    send_write_result(request, false, "INVALID", "missing or oversize body (send text/plain with Content-Length)");
    return;
  }
  // Validate the confirm token only after we have a body (so a bad request never burns a token).
  if (!this->consume_confirm(static_cast<uint8_t>(device), SETUP_CONFIRM_ADDR, parse_token(request->arg("confirm")),
                             millis())) {
    send_write_result(request, false, "CONFIRM_REQUIRED",
                      "this change needs confirmation (POST /api/confirm?address=65535 first)");
    return;
  }
  // Parse "addr:val,addr:val,..." (whitespace ignored).
  std::vector<sungrow_control::ControlCore::SetupReg> regs;
  const char *p = body.c_str();
  while (*p != '\0') {
    while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
      ++p;
    if (*p == '\0')
      break;
    char *e = nullptr;
    const long a = std::strtol(p, &e, 10);
    if (e == p || *e != ':') {
      send_write_result(request, false, "INVALID", "bad pair (want addr:value)");
      return;
    }
    p = e + 1;
    const long v = std::strtol(p, &e, 10);
    if (e == p || a < 1 || a > 0xFFFF || v < -32768 || v > 0xFFFF) {
      send_write_result(request, false, "INVALID", "bad pair value");
      return;
    }
    p = e;
    if (regs.size() >= sungrow_control::ControlCore::SETUP_MAX_REGS) {
      send_write_result(request, false, "INVALID", "too many registers (max 128)");
      return;
    }
    regs.push_back({static_cast<uint16_t>(a), static_cast<uint16_t>(v & 0xFFFF)});
  }
  if (regs.empty()) {
    send_write_result(request, false, "INVALID", "empty batch");
    return;
  }
  sungrow_control::ControlCore &core = ctl->core();
  const sungrow_control::Verdict v = core.setup_apply(static_cast<uint8_t>(device), regs, "api:installer");
  send_verdict(request, v, core.setup_detail());
#else
  (void) body;
  send_write_result(request, false, "UNAVAILABLE", "writes not configured");
#endif
}

// POST /api/grid_read_start?device=D  body: "addr,addr,addr,..." (decimal, 1-based). Starts a
// coalesced on-demand fn 3 read of those registers into the ControlCore cache (off-list ones stay
// in it as invalid with reason "off-list"); the SPA then polls GET /api/grid_read.
void SpaHandler::handle_grid_read_start_(AsyncWebServerRequest *request, const std::string &body) {
#ifdef SUNGROW_WEB_BIND_CONTROL
  sungrow_control::SungrowControl *ctl = this->parent_->control();
  if (ctl == nullptr) {
    send_write_result(request, false, "UNAVAILABLE", "control not configured");
    return;
  }
  long device = 0;
  if (!arg_int(request, "device", 0, 255, &device)) {
    send_write_result(request, false, "INVALID", "need device");
    return;
  }
  if (body.empty() || body.size() > 8192) {
    send_write_result(request, false, "INVALID", "missing or oversize body");
    return;
  }
  std::vector<uint16_t> addrs;
  const char *p = body.c_str();
  while (*p != '\0') {
    while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')
      ++p;
    if (*p == '\0')
      break;
    char *e = nullptr;
    const long a = std::strtol(p, &e, 10);
    if (e == p)
      break;
    p = e;
    if (a >= 1 && a <= 0xFFFF && addrs.size() < 256)
      addrs.push_back(static_cast<uint16_t>(a));
  }
  ctl->core().grid_read_request(static_cast<uint8_t>(device), addrs.data(), static_cast<uint16_t>(addrs.size()));
  send_json(request, "{\"ok\":true}");
#else
  (void) body;
  send_write_result(request, false, "UNAVAILABLE", "control not configured");
#endif
}

#ifdef SUNGROW_WEB_BIND_CONTROL
// "setup":{"device":D,"busy":..,"total":..,"done":..,"ok":..,"failed":..,"deadline_ms":..,
//          "frames":[{"start":..,"n":..,"status":"ok","ex":0},...]}
static void append_setup_progress(std::string &j, const sungrow_control::ControlCore &core) {
  char sb[200];
  std::snprintf(sb, sizeof(sb),
                "\"setup\":{\"device\":%u,\"busy\":%s,\"total\":%u,\"done\":%u,\"ok\":%u,\"failed\":%u,"
                "\"deadline_ms\":%u,\"frames\":[",
                static_cast<unsigned>(core.setup_device()), core.setup_busy() ? "true" : "false",
                static_cast<unsigned>(core.setup_total()), static_cast<unsigned>(core.setup_done()),
                static_cast<unsigned>(core.setup_ok()), static_cast<unsigned>(core.setup_failed_n()),
                static_cast<unsigned>(core.setup_deadline_left()));
  j += sb;
  bool first = true;
  for (const auto &f : core.setup_frames()) {
    char fb[96];
    std::snprintf(fb, sizeof(fb), "%s{\"start\":%u,\"n\":%u,\"status\":\"%s\",\"ex\":%u}", first ? "" : ",",
                  static_cast<unsigned>(f.start), static_cast<unsigned>(f.count),
                  sungrow_control::frame_status_name(f.status, f.expired), static_cast<unsigned>(f.exception));
    j += fb;
    first = false;
  }
  j += "]}";
}
#endif

// GET /api/grid_read?device=D -> {"device":D,"complete":..,"values":[{"a":..,"v":..,"valid":true} |
// {"a":..,"v":null,"valid":false,"reason":"off-list"|"exception"|"timeout"|"pending"}],"setup":{...}}.
// The cache belongs to the device of the last grid_read_start; another device gets
// {"device":D,"complete":true,"values":[],"stale":true,"setup":{...}}.
void SpaHandler::handle_grid_read_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_CONTROL
  sungrow_control::SungrowControl *ctl = this->parent_->control();
  if (ctl == nullptr) {
    send_json(request, "{\"complete\":true,\"values\":[]}");
    return;
  }
  const sungrow_control::ControlCore &core = ctl->core();
  long device = core.grid_read_device();
  if (!request->arg("device").empty() && !arg_int(request, "device", 0, 255, &device)) {
    send_plain(request, 400, "bad device\n");
    return;
  }
  std::string j = "{\"device\":" + std::to_string(device);
  if (device != core.grid_read_device()) {
    j += ",\"complete\":true,\"values\":[],\"stale\":true,";
  } else {
    j += ",\"complete\":";
    j += core.grid_read_complete() ? "true" : "false";
    j += ",\"values\":[";
    bool first = true;
    for (const auto &e : core.grid_read_results()) {
      char buf[96];
      if (e.valid)
        std::snprintf(buf, sizeof(buf), "%s{\"a\":%u,\"v\":%u,\"valid\":true}", first ? "" : ",",
                      static_cast<unsigned>(e.address), static_cast<unsigned>(e.value));
      else
        std::snprintf(buf, sizeof(buf), "%s{\"a\":%u,\"v\":null,\"valid\":false,\"reason\":\"%s\"}", first ? "" : ",",
                      static_cast<unsigned>(e.address), sungrow_control::read_reason_name(e.reason));
      j += buf;
      first = false;
    }
    j += "],";
  }
  append_setup_progress(j, core);
  j += '}';
  send_json(request, j);
#else
  send_json(request, "{\"complete\":true,\"values\":[]}");
#endif
}

#ifdef SUNGROW_WEB_BIND_CONTROL
// Number or null from a read-set value.
static void append_read(std::string &j, const sungrow_control::ControlCore::ReadSet &rs, uint8_t fn, uint16_t addr) {
  const auto *e = addr != 0 ? rs.find(fn, addr) : nullptr;
  if (e != nullptr && e->valid)
    j += std::to_string(e->value);
  else
    j += "null";
}

static const char *item_kind_name(sungrow_control::ItemKind k) {
  switch (k) {
    case sungrow_control::ItemKind::COUNTRY:
      return "country";
    case sungrow_control::ItemKind::COMPANY:
      return "company";
    case sungrow_control::ItemKind::GRID_TYPE:
      return "grid_type";
    case sungrow_control::ItemKind::SET:
      return "set";
  }
  return "?";
}
#endif

// GET /api/setup_options?device=D -> the option picker of the device's family:
// {"device":D,"family":"unit","descriptor":{..}|null,"feature":{"w32b5":..,"w29b12":..},
//  "complete":..,"reg5479":1|null,"current":{"country":6|null,"grid_code":..|null,"company":13|null,
//  "grid_type":..|null},"countries":[{"code":6,"label":"Australia"}],
//  "options":[{"code":13,"label":"..","needparam":1,"needreadtype":1,
//              "variants":[{"ref5479":[1,2],"nsets":39,"nregs":111}]}]}
// Start-or-poll: the first call (or one 30 s after the last read) starts the reads of 5479 and the
// country / company registers and answers "complete":false until they are in.
void SpaHandler::handle_setup_options_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_CONTROL
  sungrow_control::SungrowControl *ctl = this->parent_->control();
  long device = 0;
  if (ctl == nullptr || !arg_int(request, "device", 0, 255, &device)) {
    send_plain(request, ctl == nullptr ? 404 : 400, ctl == nullptr ? "control not configured\n" : "need ?device=N\n");
    return;
  }
  sungrow_control::ControlCore &core = ctl->core();
  const uint8_t dev = static_cast<uint8_t>(device);
  if (!core.setup_info_fresh(dev, 30000))
    core.setup_info_request(dev);
  const sungrow_control::DeviceIdentity id = core.identity(dev);
  const auto &info = core.setup_info();
  std::string j = "{\"device\":" + std::to_string(device) + ",\"family\":\"";
  j += sungrow_modbus::xml_family_name(id.family);
  j += "\",\"descriptor\":";
  append_descriptor(j, id.desc);
  j += ",\"feature\":{\"w32b5\":";
  j += core.feature_w32b5(dev) ? "true" : "false";
  j += ",\"w29b12\":";
  j += core.feature_w29b12(dev) ? "true" : "false";
  j += "},\"complete\":";
  j += info.complete() ? "true" : "false";
  j += ",\"reg5479\":";
  append_read(j, info, sungrow_modbus::FN_READ_INPUT, 5479);
  const bool unit = id.family == sungrow_modbus::XmlFamily::UNIT;
  j += ",\"current\":{\"country\":";
  append_read(j, info, sungrow_modbus::FN_READ_HOLDING, id.desc != nullptr ? id.desc->country_reg : 0);
  j += ",\"grid_code\":";
  append_read(j, info, sungrow_modbus::FN_READ_HOLDING, id.desc != nullptr ? id.desc->grid_reg : 0);
  j += ",\"company\":";
  append_read(j, info, sungrow_modbus::FN_READ_HOLDING, unit ? sungrow_control::GP_UNIT_COMPANY_REG : 0);
  j += ",\"grid_type\":";
  append_read(j, info, sungrow_modbus::FN_READ_HOLDING, unit ? sungrow_control::GP_UNIT_GRIDTYPE_REG : 0);
  j += "},\"countries\":[{\"code\":6,\"label\":\"Australia\"}],\"options\":[";
  size_t n = 0;
  const sungrow_control::GpOption *opts = sungrow_control::setup_options_for(id.family, &n);
  for (size_t i = 0; i < n; i++) {
    const sungrow_control::GpOption &o = opts[i];
    char ob[96];
    std::snprintf(ob, sizeof(ob), "%s{\"code\":%u,\"needparam\":%u,\"needreadtype\":%u,\"label\":\"", i ? "," : "",
                  static_cast<unsigned>(o.code), static_cast<unsigned>(o.needparam),
                  static_cast<unsigned>(o.needreadtype));
    j += ob;
    json_escape_into(j, o.label);
    j += "\",\"variants\":[";
    for (uint8_t k = 0; k < o.nvariants; k++) {
      const sungrow_control::GpVariant &v = sungrow_control::GP_VARIANTS[o.variant_off + k];
      j += k ? ",{\"ref5479\":[" : "{\"ref5479\":[";
      for (uint8_t r = 0; r < v.nref; r++) {
        if (r)
          j += ',';
        j += std::to_string(sungrow_control::GP_REF5479[v.ref_off + r]);
      }
      unsigned nregs = 0;
      for (uint8_t r = 0; r < v.nruns; r++)
        nregs += sungrow_control::GP_RUNS[sungrow_control::GP_RUN_INDEX[v.run_off + r]].nwords;
      char vb[48];
      std::snprintf(vb, sizeof(vb), "],\"nsets\":%u,\"nregs\":%u}", static_cast<unsigned>(v.nruns), nregs);
      j += vb;
    }
    j += "]}";
  }
  j += "]}";
  send_json(request, j);
#else
  send_plain(request, 404, "control not configured\n");
#endif
}

// GET /api/setup_preview?device=D&option=O. Start-or-poll: without a prepared context for
// (D, O) younger than 60 s it starts one and answers {"device":D,"option":O,"complete":false};
// while the reads run, the same; then
// {"device":D,"option":O,"complete":true,"status":"ok"|"NEED_5479"|...,"reg5479":1|null,
//  "variant":0|null,"items":[{"a":32310,"new":6,"cur":6|null,"valid":true,"kind":"country",
//  "changed":false},...],"frames":[{"start":32310,"n":1},...],"n_changed":N,"note":"..."}
// (note: items the apply will skip because the model's power stage would not accept them, "").
// A device that cannot be prepared: {"device":D,"option":O,"complete":true,"status":"error",
// "error":"OFFLINE"|"BUSY","message":".."}.
void SpaHandler::handle_setup_preview_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_CONTROL
  sungrow_control::SungrowControl *ctl = this->parent_->control();
  long device = 0, option = 0;
  if (ctl == nullptr || !arg_int(request, "device", 0, 255, &device) ||
      !arg_int(request, "option", 0, 0xFFFF, &option)) {
    send_plain(request, ctl == nullptr ? 404 : 400,
               ctl == nullptr ? "control not configured\n" : "need ?device=N&option=O\n");
    return;
  }
  sungrow_control::ControlCore &core = ctl->core();
  const uint8_t dev = static_cast<uint8_t>(device);
  const uint16_t opt = static_cast<uint16_t>(option);
  std::string j = "{\"device\":" + std::to_string(device) + ",\"option\":" + std::to_string(option);
  const sungrow_control::ControlCore::Prepared *p = core.setup_prepared(dev, opt);
  if (p == nullptr) {
    const sungrow_control::Verdict v = core.setup_prepare(dev, opt);
    if (v != sungrow_control::Verdict::OK) {
      j += ",\"complete\":true,\"status\":\"error\",\"error\":\"";
      j += sungrow_control::verdict_code(v);
      j += "\",\"message\":\"";
      json_escape_into(j, core.setup_detail().c_str());
      j += "\"}";
      send_json(request, j);
      return;
    }
    p = core.setup_prepared(dev, opt);
  }
  if (p == nullptr || !p->complete()) {
    j += ",\"complete\":false}";
    send_json(request, j);
    return;
  }
  j += ",\"complete\":true,\"status\":\"";
  j += p->status == sungrow_control::ResolveStatus::OK ? "ok" : sungrow_control::resolve_status_name(p->status);
  j += "\",\"reg5479\":";
  j += p->have_5479 ? std::to_string(p->reg5479) : std::string("null");
  j += ",\"variant\":";
  j += p->variant >= 0 ? std::to_string(p->variant) : std::string("null");
  j += ",\"items\":[";
  std::vector<sungrow_control::ControlCore::SetupReg> regs;
  for (size_t i = 0; i < p->items.size(); i++) {
    const sungrow_control::ResolvedItem &it = p->items[i];
    regs.push_back({it.address, it.value});
    uint16_t cur = 0;
    const bool valid = p->current(it.address, &cur);
    char ib[160];
    std::snprintf(ib, sizeof(ib), "%s{\"a\":%u,\"new\":%u,\"cur\":%s,\"valid\":%s,\"kind\":\"%s\",\"changed\":%s}",
                  i ? "," : "", static_cast<unsigned>(it.address), static_cast<unsigned>(it.value),
                  valid ? std::to_string(cur).c_str() : "null", valid ? "true" : "false", item_kind_name(it.kind),
                  !valid || cur != it.value ? "true" : "false");
    j += ib;
  }
  j += "],\"frames\":[";
  bool first = true;
  for (const auto &f : core.plan_frames(dev, regs)) {
    char fb[48];
    std::snprintf(fb, sizeof(fb), "%s{\"start\":%u,\"n\":%u}", first ? "" : ",", static_cast<unsigned>(f.start),
                  static_cast<unsigned>(f.count));
    j += fb;
    first = false;
  }
  j += "],\"n_changed\":" + std::to_string(p->n_changed()) + ",\"note\":\"";
  json_escape_into(j, core.setup_plan_note(dev, regs).c_str());
  j += "\"}";
  send_json(request, j);
#else
  send_plain(request, 404, "control not configured\n");
#endif
}

// POST /api/setup_apply?device=D&option=O&confirm=XXXX: queue the previewed profile (every item,
// resolver order). Token bound to (D, SETUP_APPLY_CONFIRM_ADDR). PREVIEW_REQUIRED without a
// complete preview of (D, O) younger than 60 s.
void SpaHandler::handle_setup_apply_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_CONTROL
  sungrow_control::SungrowControl *ctl = this->parent_->control();
  if (ctl == nullptr) {
    send_write_result(request, false, "UNAVAILABLE", "writes not configured");
    return;
  }
  long device = 0, option = 0;
  if (!arg_int(request, "device", 0, 255, &device) || !arg_int(request, "option", 0, 0xFFFF, &option)) {
    send_write_result(request, false, "INVALID", "need device, option");
    return;
  }
  if (!this->consume_confirm(static_cast<uint8_t>(device), SETUP_APPLY_CONFIRM_ADDR,
                             parse_token(request->arg("confirm")), millis())) {
    send_write_result(request, false, "CONFIRM_REQUIRED",
                      "this change needs confirmation (POST /api/confirm?address=65534 first)");
    return;
  }
  sungrow_control::ControlCore &core = ctl->core();
  const sungrow_control::Verdict v =
      core.setup_apply_profile(static_cast<uint8_t>(device), static_cast<uint16_t>(option), "api:setup");
  send_verdict(request, v, core.setup_detail(), core.setup_note().c_str());
#else
  send_write_result(request, false, "UNAVAILABLE", "writes not configured");
#endif
}

// POST /api/setup_start?device=D&confirm=XXXX: 0xCF to the descriptor's power_switch register.
// Token bound to (D, SETUP_START_CONFIRM_ADDR).
void SpaHandler::handle_setup_start_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_CONTROL
  sungrow_control::SungrowControl *ctl = this->parent_->control();
  if (ctl == nullptr) {
    send_write_result(request, false, "UNAVAILABLE", "writes not configured");
    return;
  }
  long device = 0;
  if (!arg_int(request, "device", 0, 255, &device)) {
    send_write_result(request, false, "INVALID", "need device");
    return;
  }
  if (!this->consume_confirm(static_cast<uint8_t>(device), SETUP_START_CONFIRM_ADDR,
                             parse_token(request->arg("confirm")), millis())) {
    send_write_result(request, false, "CONFIRM_REQUIRED",
                      "this change needs confirmation (POST /api/confirm?address=65533 first)");
    return;
  }
  sungrow_control::ControlCore &core = ctl->core();
  const sungrow_control::Verdict v = core.setup_start(static_cast<uint8_t>(device), "api:setup");
  send_verdict(request, v, core.setup_detail());
#else
  send_write_result(request, false, "UNAVAILABLE", "writes not configured");
#endif
}

// ---- diagnostic captures (sungrow_capture) --------------------------------------------------------
#ifdef SUNGROW_WEB_BIND_CAPTURE
// "<kind>_<id>_<UTC start>.<ext>"
static std::string capture_filename(const sungrow_capture::Capture &c, const char *ext) {
  char ts[24] = "nodate";
  if (c.wall_start > 0) {
    const time_t t = static_cast<time_t>(c.wall_start);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%Y%m%dT%H%M%SZ", &tmv);
  }
  char b[64];
  std::snprintf(b, sizeof(b), "%s_%u_%s.%s", sungrow_capture::kind_name(c.kind), (unsigned) c.id, ts, ext);
  return b;
}
#endif

// POST /api/capture_start?device=D&kind=iv|grid|fault[&cycles=1..8]&confirm=XXXX. Token bound to
// (D, CAPTURE_CONFIRM_ADDR). The capture runs in the background; poll GET /api/capture.
void SpaHandler::handle_capture_start_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_CAPTURE
  sungrow_capture::SungrowCapture *cap = this->parent_->capture();
  if (cap == nullptr) {
    send_write_result(request, false, "UNAVAILABLE", "captures not configured");
    return;
  }
  long device = 0, cycles = 1;
  const std::string kind = request->arg("kind");
  if (!arg_int(request, "device", 0, 255, &device) || (kind != "iv" && kind != "grid" && kind != "fault") ||
      (kind == "grid" && !arg_int(request, "cycles", 1, 8, &cycles))) {
    send_write_result(request, false, "INVALID", "need device, kind=iv|grid|fault, cycles=1..8 for grid");
    return;
  }
  if (!this->consume_confirm(static_cast<uint8_t>(device), CAPTURE_CONFIRM_ADDR, parse_token(request->arg("confirm")),
                             millis())) {
    send_write_result(request, false, "CONFIRM_REQUIRED",
                      "a capture needs confirmation (POST /api/confirm?address=65532 first)");
    return;
  }
  std::string why;
  const bool ok = kind == "iv"     ? cap->start_iv(static_cast<uint8_t>(device), &why)
                  : kind == "grid" ? cap->start_grid(static_cast<uint8_t>(device), static_cast<uint8_t>(cycles), &why)
                                   : cap->start_fault(static_cast<uint8_t>(device), &why);
  send_write_result(request, ok, ok ? "OK" : "REFUSED", ok ? "started" : why.c_str());
#else
  send_write_result(request, false, "UNAVAILABLE", "captures not configured");
#endif
}

void SpaHandler::handle_capture_cancel_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_CAPTURE
  sungrow_capture::SungrowCapture *cap = this->parent_->capture();
  if (cap != nullptr) {
    cap->cancel();
    send_write_result(request, true, "OK", "cancelled");
    return;
  }
#endif
  send_write_result(request, false, "UNAVAILABLE", "captures not configured");
}

// GET /api/capture_meta?id=N: metadata, timeline and signal snapshots as a JSON attachment.
void SpaHandler::handle_capture_meta_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_CAPTURE
  sungrow_capture::SungrowCapture *cap = this->parent_->capture();
  long id = 0;
  if (cap != nullptr && arg_int(request, "id", 1, 0x7FFFFFFF, &id)) {
    std::shared_ptr<const sungrow_capture::Capture> c = cap->find(static_cast<uint32_t>(id));
    if (c != nullptr) {
      send_json(request, sungrow_capture::CaptureCore::metadata_json(*c));
      if (g_reply != nullptr)
        g_reply->filename = capture_filename(*c, "json");
      return;
    }
  }
#endif
  send_plain(request, 404, "no such capture\n");
}

// GET /api/capture_file?id=N: the raw file as received. The capture is looked up on the loop;
// the bytes (immutable once stored) are streamed from this task.
void SpaHandler::handle_capture_file_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_CAPTURE
  long id = 0;
  std::shared_ptr<const sungrow_capture::Capture> c;
  if (arg_int(request, "id", 1, 0x7FFFFFFF, &id)) {
    WebReply unused;
    this->parent_->run_on_loop(
        [this, id, &c](WebReply &) {
          sungrow_capture::SungrowCapture *cap = this->parent_->capture();
          if (cap != nullptr)
            c = cap->find(static_cast<uint32_t>(id));
        },
        &unused, API_LOOP_TIMEOUT_MS);
  }
  if (c != nullptr) {
    httpd_req_t *req = *request;
    const std::string disposition = "attachment; filename=\"" + capture_filename(*c, "bin") + "\"";
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition.c_str());
    const auto &d = c->data;
    for (size_t off = 0; off < d.size(); off += 4096) {
      const size_t n = d.size() - off < 4096 ? d.size() - off : 4096;
      if (httpd_resp_send_chunk(req, reinterpret_cast<const char *>(d.data() + off), static_cast<ssize_t>(n)) != ESP_OK)
        return;
    }
    httpd_resp_send_chunk(req, nullptr, 0);
    return;
  }
#endif
  send_plain(request, 404, "no such capture\n");
}

// ---- diagnostics report (sungrow_analytics) -------------------------------------------------------
#ifdef SUNGROW_WEB_BIND_ANALYTICS
static const char *bundle_state_name(sungrow_analytics::BundleState s) {
  switch (s) {
    case sungrow_analytics::BundleState::RUNNING:
      return "running";
    case sungrow_analytics::BundleState::DONE:
      return "done";
    case sungrow_analytics::BundleState::FAILED:
      return "failed";
    case sungrow_analytics::BundleState::IDLE:
      break;
  }
  return "idle";
}
#endif

// POST /api/diag_send: build the diagnostics report and upload it to the collector (trigger
// "manual"). Works whether or not automatic reporting is enabled; authorised by the web login.
void SpaHandler::handle_diag_send_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_ANALYTICS
  sungrow_analytics::SungrowAnalytics *an = this->parent_->analytics();
  if (an != nullptr) {
    std::string why;
    if (an->request_bundle("manual", &why))
      send_write_result(request, true, "OK", "queued");
    else
      send_write_result(request, false, "BUSY", why.empty() ? "cannot send now" : why.c_str());
    return;
  }
#endif
  send_write_result(request, false, "UNAVAILABLE", "diagnostics upload not configured");
}

// GET /api/diag_send: state of the last upload. `telemetry` = automatic reporting is on.
void SpaHandler::handle_diag_status_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_ANALYTICS
  sungrow_analytics::SungrowAnalytics *an = this->parent_->analytics();
  if (an != nullptr) {
    const sungrow_analytics::BundleStatus st = an->bundle_status();
    std::string j = "{\"available\":true,\"telemetry\":";
    j += an->enabled() ? "true" : "false";
    j += ",\"state\":\"";
    j += bundle_state_name(st.state);
    j += "\",\"id\":\"";
    json_escape_into(j, st.id.c_str());
    j += "\",\"error\":\"";
    json_escape_into(j, st.error.c_str());
    j += "\",\"trigger\":\"";
    json_escape_into(j, st.trigger.c_str());
    j += "\",\"age_ms\":";
    j += std::to_string(st.age_ms);
    j += '}';
    send_json(request, j);
    return;
  }
#endif
  send_json(request, "{\"available\":false}");
}

// GET /api/diag_report: the same scrubbed report as a text download, for attaching to an issue.
void SpaHandler::handle_diag_report_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_ANALYTICS
  sungrow_analytics::SungrowAnalytics *an = this->parent_->analytics();
  if (an != nullptr) {
    std::string text = an->build_bundle();
    if (g_reply != nullptr) {
      g_reply->code = 200;
      g_reply->type = "text/plain; charset=utf-8";
      g_reply->no_store = true;
      g_reply->body = std::move(text);
      g_reply->filename = "rewired-s-diagnostics.txt";
    } else {
      send_plain(request, 200, text.c_str());
    }
    return;
  }
#endif
  send_plain(request, 404, "diagnostics upload not configured\n");
}

// ---- Settings page: web login, MQTT broker, cloud version ---------------------------------------
// Pre-built images (docs/prebuilt.md). Bodies are application/x-www-form-urlencoded (request->arg);
// the web_server_idf form cap is 1 KB, plenty for these. Every handler runs on the loop.

// GET /api/web_login -> {"managed":"yaml"|"runtime","username":..,"default":bool}
void SpaHandler::handle_web_login_get_(AsyncWebServerRequest *request) {
  std::string j = "{\"managed\":\"";
  j += this->parent_->runtime_login() ? "runtime" : "yaml";
  j += "\",\"username\":\"";
  if (this->parent_->runtime_login())
    json_escape_into(j, this->parent_->login_username());
  j += "\",\"default\":";
  j += this->parent_->runtime_login() && this->parent_->default_credentials() ? "true" : "false";
  j += '}';
  send_json(request, j);
}

// POST /api/web_login  username, password, confirm, current_password -> WriteResult. 409 with
// verdict MANAGED_BY_YAML when the login is compiled in from secrets.
void SpaHandler::handle_web_login_post_(AsyncWebServerRequest *request) {
  if (!this->parent_->runtime_login()) {
    std::string j;
    j = "{\"ok\":false,\"verdict\":\"MANAGED_BY_YAML\",\"message\":\"the login is set in the configuration\"}";
    send_json_status(request, 409, std::move(j));
    return;
  }
  std::string why;
  const LoginChange v = this->parent_->change_login(request->arg("current_password"), request->arg("username"),
                                                    request->arg("password"), request->arg("confirm"), &why);
  const char *msg = v == LoginChange::OK              ? "login changed"
                    : v == LoginChange::WRONG_CURRENT ? "current password is wrong"
                    : v == LoginChange::MISMATCH      ? "the two passwords differ"
                    : v == LoginChange::INVALID       ? why.c_str()
                    : v == LoginChange::STORE_FAILED  ? "could not store the login"
                                                      : "the login is set in the configuration";
  send_write_result(request, v == LoginChange::OK, login_change_code(v), msg);
}

// GET /api/mqtt_settings -> mqtt_settings_json (never the password).
void SpaHandler::handle_mqtt_settings_get_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_MQTT
  sungrow_mqtt::SungrowMqttComponent *m = this->parent_->mqtt();
  if (m != nullptr) {
    send_json(request, sungrow_mqtt::mqtt_settings_json(m->settings(), m->settings_source(), m->runtime_settings(),
                                                        m->is_connected(), m->last_error()));
    return;
  }
#endif
  send_json(request, "{\"available\":false,\"source\":\"none\",\"managed_by_yaml\":false,\"configured\":false}");
}

// POST /api/mqtt_settings  host, port, username, password (blank = unchanged), clear_password
// (1 = store no password), prefix, tls (1/0) -> WriteResult. 409 MANAGED_BY_YAML when the
// configuration names the broker or runtime settings are not compiled in.
void SpaHandler::handle_mqtt_settings_post_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_BIND_MQTT
  sungrow_mqtt::SungrowMqttComponent *m = this->parent_->mqtt();
  if (m != nullptr) {
    if (!m->runtime_settings() || m->settings_source() == sungrow_mqtt::MqttSettingsSource::YAML) {
      send_json_status(request, 409,
                       "{\"ok\":false,\"verdict\":\"MANAGED_BY_YAML\",\"message\":\"the broker is set in the "
                       "configuration\"}");
      return;
    }
    sungrow_mqtt::MqttSettings s;
    s.host = request->arg("host");
    long port = 1883;
    const std::string port_arg = request->arg("port");
    if (!port_arg.empty() && !arg_int(request, "port", 1, 65535, &port)) {
      send_write_result(request, false, "INVALID", "port must be 1-65535");
      return;
    }
    s.port = static_cast<uint16_t>(port);
    s.user = request->arg("username");
    const std::string pass = request->arg("password");
    const bool clear = request->arg("clear_password") == "1" || request->arg("clear_password") == "true";
    s.pass = clear ? std::string() : (pass.empty() ? m->settings().pass : pass);
    const std::string prefix = request->arg("prefix");
    s.prefix = prefix.empty() ? "homeassistant" : prefix;
    const std::string tls = request->arg("tls");
    s.tls = tls == "1" || tls == "true" || tls == "on";
    std::string why;
    if (!m->apply_settings(s, &why)) {
      send_write_result(request, false, "INVALID", why.c_str());
      return;
    }
    send_write_result(request, true, "OK", s.host.empty() ? "MQTT off" : "settings stored, connecting");
    return;
  }
#endif
  send_write_result(request, false, "UNAVAILABLE", "MQTT is not configured in this build");
}

// GET /api/cloud_version -> {"available","version","source","override","yaml","detected","default"}
void SpaHandler::handle_cloud_version_get_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_HAVE_BOARD
  sungrow_board::SungrowBoard *b = sungrow_board::global_board;
  if (b != nullptr) {
    std::string j = "{\"available\":true,\"version\":\"";
    json_escape_into(j, b->firmware_version());
    j += "\",\"source\":\"";
    j += sungrow_board::cloud_version_source_name(b->cloud_version_source());
    j += "\",\"override\":\"";
    json_escape_into(j, b->override_version());
    j += "\",\"yaml\":\"";
    json_escape_into(j, b->yaml_version());
    j += "\",\"detected\":\"";
    json_escape_into(j, b->detected_version());
    j += "\",\"default\":\"";
    json_escape_into(j, b->default_version());
    j += "\"}";
    send_json(request, j);
    return;
  }
#endif
  send_json(request, "{\"available\":false}");
}

// POST /api/cloud_version  override=<stock version> sets the override, override= (empty) clears
// it (back to YAML / detected / default) -> WriteResult. The cloud identity is re-resolved at once.
void SpaHandler::handle_cloud_version_post_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_HAVE_BOARD
  sungrow_board::SungrowBoard *b = sungrow_board::global_board;
  if (b != nullptr) {
    std::string why;
    if (!b->set_cloud_version_override(request->arg("override"), &why)) {
      send_write_result(request, false, "INVALID", why.c_str());
      return;
    }
#ifdef SUNGROW_WEB_BIND_CLOUD
    if (this->parent_->cloud() != nullptr)
      this->parent_->cloud()->reload_identity("cloud version changed");
#endif
    send_write_result(request, true, "OK", b->firmware_version());
    return;
  }
#endif
  send_write_result(request, false, "UNAVAILABLE", "no board component");
}

// GET /api/boot_slot -> boot_slots_json: the running slot, the other OTA slot (null when there
// is none) with its app descriptor, and whether a switch is possible.
void SpaHandler::handle_boot_slot_get_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_HAVE_BOARD
  sungrow_board::SungrowBoard *b = sungrow_board::global_board;
  if (b != nullptr) {
    send_json(request, sungrow_board::boot_slots_json(sungrow_board::read_running_slot(),
                                                      sungrow_board::read_other_slot(), b->boot_switch_armed()));
    return;
  }
#endif
  send_json(request, "{\"running\":null,\"other\":null,\"armed\":false,\"can_switch\":false}");
}

// POST /api/boot_slot?confirm=XXXX: boot the other slot. The token (POST /api/confirm?device=0&
// address=65531 first) is the web UI's confirm dialog and stands in for the "Allow boot switch"
// switch; the other slot must hold a valid app image. The dongle restarts 3 s after OK.
void SpaHandler::handle_boot_slot_post_(AsyncWebServerRequest *request) {
#ifdef SUNGROW_WEB_HAVE_BOARD
  sungrow_board::SungrowBoard *b = sungrow_board::global_board;
  if (b != nullptr) {
    if (!this->consume_confirm(0, BOOT_SLOT_CONFIRM_ADDR, parse_token(request->arg("confirm")), millis())) {
      send_write_result(request, false, "CONFIRM_REQUIRED",
                        "booting the other slot needs confirmation (POST /api/confirm?address=65531 first)");
      return;
    }
    const sungrow_board::BootSwitchVerdict v = b->boot_other_slot(/*confirmed=*/true);
    send_write_result(request, v == sungrow_board::BootSwitchVerdict::OK, sungrow_board::boot_switch_verdict_code(v),
                      sungrow_board::boot_switch_verdict_text(v));
    return;
  }
#endif
  send_write_result(request, false, "UNAVAILABLE", "no board component");
}

void SpaHandler::handleRequest(AsyncWebServerRequest *request) {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  std::string url = request->url_to(buf).c_str();

  // Static assets: flash-resident and shared with nothing, served straight from httpd.
  if (request->method() != HTTP_POST) {
    if (url == "/" || url == "/index.html") {
      // The shell names the hashed assets, so it must not be cached.
      send_gzip(request, INDEX_HTML_GZ, INDEX_HTML_GZ_LEN, "text/html; charset=utf-8", "no-cache");
      return;
    }
    if (url == APP_JS_PATH) {
      send_gzip(request, APP_JS_GZ, APP_JS_GZ_LEN, "text/javascript; charset=utf-8",
                "public, max-age=31536000, immutable");
      return;
    }
    if (url == APP_CSS_PATH) {
      send_gzip(request, APP_CSS_GZ, APP_CSS_GZ_LEN, "text/css; charset=utf-8",
                "public, max-age=31536000, immutable");
      return;
    }
  }

  if (request->method() != HTTP_POST && url == "/api/capture_file") {
    this->handle_capture_file_(request);
    return;
  }

  // API: runs on the main loop (the store, the master and ControlCore are loop-owned); the reply
  // is captured there and sent from here, so a slow client never holds up the loop. The request
  // object stays valid while this task waits.
  std::string body;
  if (request->method() == HTTP_POST)
    body = this->take_body_(request);
  WebReply reply;
  const bool ran = this->parent_->run_on_loop(
      [this, request, &url, &body](WebReply &r) {
        g_reply = &r;
        this->dispatch_api_(request, url, body);
        g_reply = nullptr;
      },
      &reply, API_LOOP_TIMEOUT_MS);
  if (!ran) {
    reply.code = 503;
    reply.type = "text/plain";
    reply.body = "busy\n";
  }
  send_reply(request, reply);
}

void SpaHandler::dispatch_api_(AsyncWebServerRequest *request, const std::string &url, const std::string &body) {
  if (request->method() == HTTP_POST) {
    if (url == "/api/write")
      this->handle_write_(request);
    else if (url == "/api/confirm")
      this->handle_confirm_(request);
    else if (url == "/api/setup_write")
      this->handle_setup_write_(request, body);
    else if (url == "/api/grid_read_start")
      this->handle_grid_read_start_(request, body);
    else if (url == "/api/setup_apply")
      this->handle_setup_apply_(request);
    else if (url == "/api/setup_start")
      this->handle_setup_start_(request);
    else if (url == "/api/capture_start")
      this->handle_capture_start_(request);
    else if (url == "/api/capture_cancel")
      this->handle_capture_cancel_(request);
    else if (url == "/api/diag_send")
      this->handle_diag_send_(request);
    else if (url == "/api/web_login")
      this->handle_web_login_post_(request);
    else if (url == "/api/mqtt_settings")
      this->handle_mqtt_settings_post_(request);
    else if (url == "/api/cloud_version")
      this->handle_cloud_version_post_(request);
    else if (url == "/api/boot_slot")
      this->handle_boot_slot_post_(request);
    else
      send_plain(request, 404, "not found\n");
    return;
  }
  if (url == "/api/devices") {
    const void *core = nullptr;
#ifdef SUNGROW_WEB_BIND_CONTROL
    if (this->parent_->control() != nullptr)
      core = &this->parent_->control()->core();
#endif
    send_json(request, build_devices(this->parent_->master(), this->parent_->store(), core));
    return;
  }
  if (url == "/api/signals") {
    const std::string arg = request->arg("device");
    if (arg.empty()) {
      send_plain(request, 400, "need ?device=N\n");
      return;
    }
    char *end = nullptr;
    const long dev = std::strtol(arg.c_str(), &end, 10);
    if (end == arg.c_str() || *end != '\0' || dev < 0 || dev > 255) {
      send_plain(request, 400, "bad device\n");
      return;
    }
    SignalFilter filter;
    if (!parse_signal_filter(request->arg("keys"), request->arg("class"), &filter)) {
      send_plain(request, 400, "bad keys/class\n");
      return;
    }
    send_json(request, build_signals(this->parent_->store(), this->parent_->master(), static_cast<int>(dev), filter));
    return;
  }
  if (url == "/api/grid_read") {
    this->handle_grid_read_(request);
    return;
  }
  if (url == "/api/setup_options") {
    this->handle_setup_options_(request);
    return;
  }
  if (url == "/api/setup_preview") {
    this->handle_setup_preview_(request);
    return;
  }
  if (url == "/api/capture") {
#ifdef SUNGROW_WEB_BIND_CAPTURE
    if (this->parent_->capture() != nullptr) {
      send_json(request, this->parent_->capture()->status_json());
      return;
    }
#endif
    send_json(request, "{\"available\":false}");
    return;
  }
  if (url == "/api/capture_meta") {
    this->handle_capture_meta_(request);
    return;
  }
  if (url == "/api/diag_send") {
    this->handle_diag_status_(request);
    return;
  }
  if (url == "/api/diag_report") {
    this->handle_diag_report_(request);
    return;
  }
  if (url == "/api/web_login") {
    this->handle_web_login_get_(request);
    return;
  }
  if (url == "/api/mqtt_settings") {
    this->handle_mqtt_settings_get_(request);
    return;
  }
  if (url == "/api/cloud_version") {
    this->handle_cloud_version_get_(request);
    return;
  }
  if (url == "/api/boot_slot") {
    this->handle_boot_slot_get_(request);
    return;
  }
  // canHandle() guarantees we never reach here for a non-whitelisted path.
  ESP_LOGW(TAG, "unexpected path %s", url.c_str());
  send_plain(request, 404, "not found\n");
}

}  // namespace sungrow_web
}  // namespace esphome
