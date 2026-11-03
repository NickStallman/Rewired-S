#pragma once
// Runtime web login for sungrow_web (`login: runtime`, pre-built images; docs/prebuilt.md).
//
// Header-only, no ESPHome / IDF includes (tests/settings builds it on the host). The component
// keeps the digest-auth pair in NVS (pref key fnv1_hash("sungrow_web_login")) and hands
// WebServerBase pointers into a LoginBuffers pair: the server reads the pointers on every request
// from the httpd task, so a change writes the inactive buffer pair and then swaps the pointers.
// The old pair stays valid (never freed), so a request already comparing against it is safe.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace esphome {
namespace sungrow_web {

struct WebLoginPref {
  uint32_t magic;
  char user[32];
  char pass[64];
};
static constexpr uint32_t WEB_LOGIN_MAGIC = 0x31474F4Cu;  // "LOG1"
static constexpr size_t LOGIN_USER_MAX = sizeof(WebLoginPref::user) - 1;  // 31
static constexpr size_t LOGIN_PASS_MAX = sizeof(WebLoginPref::pass) - 1;  // 63
static constexpr size_t LOGIN_PASS_MIN = 8;

namespace login_detail {
/// Printable ASCII (0x20-0x7e) without ':' (the digest A1 hash is "user:realm:pass").
inline bool login_chars_ok(const std::string &s) {
  for (unsigned char c : s)
    if (c < 0x20 || c > 0x7e || c == ':')
      return false;
  return true;
}
}  // namespace login_detail

/// Username: 1-31 printable ASCII characters, no ':' and no leading/trailing space.
inline bool validate_login_username(const std::string &u, std::string *why) {
  auto fail = [&](const char *m) {
    if (why != nullptr)
      *why = m;
    return false;
  };
  if (u.empty() || u.size() > LOGIN_USER_MAX)
    return fail("username must be 1-31 characters");
  if (!login_detail::login_chars_ok(u))
    return fail("username: printable ASCII without ':'");
  if (u.front() == ' ' || u.back() == ' ')
    return fail("username must not start or end with a space");
  return true;
}

/// Password: 8-63 printable ASCII characters, no ':'.
inline bool validate_login_password(const std::string &p, std::string *why) {
  auto fail = [&](const char *m) {
    if (why != nullptr)
      *why = m;
    return false;
  };
  if (p.size() < LOGIN_PASS_MIN || p.size() > LOGIN_PASS_MAX)
    return fail("password must be 8-63 characters");
  if (!login_detail::login_chars_ok(p))
    return fail("password: printable ASCII without ':'");
  return true;
}

inline void pack_login(const std::string &user, const std::string &pass, WebLoginPref *p) {
  std::memset(p, 0, sizeof(*p));
  p->magic = WEB_LOGIN_MAGIC;
  std::memcpy(p->user, user.data(), user.size() < LOGIN_USER_MAX ? user.size() : LOGIN_USER_MAX);
  std::memcpy(p->pass, pass.data(), pass.size() < LOGIN_PASS_MAX ? pass.size() : LOGIN_PASS_MAX);
}

/// False when the record is not ours or does not pass validation (then the defaults apply).
inline bool unpack_login(const WebLoginPref &p, std::string *user, std::string *pass) {
  if (p.magic != WEB_LOGIN_MAGIC)
    return false;
  if (std::memchr(p.user, '\0', sizeof(p.user)) == nullptr || std::memchr(p.pass, '\0', sizeof(p.pass)) == nullptr)
    return false;
  std::string u = p.user, pw = p.pass;
  if (!validate_login_username(u, nullptr) || !validate_login_password(pw, nullptr))
    return false;
  *user = std::move(u);
  *pass = std::move(pw);
  return true;
}

/// Constant-time string compare (the current-password check).
inline bool login_equal(const std::string &a, const std::string &b) {
  const size_t n = a.size() > b.size() ? a.size() : b.size();
  unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
  for (size_t i = 0; i < n; i++) {
    const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    diff |= static_cast<unsigned>(ca ^ cb);
  }
  return diff == 0;
}

/// Two (user, pass) buffer pairs; set() fills the inactive pair and then flips `active`, so the
/// pointers handed out before the flip keep pointing at a complete, unchanged pair.
class LoginBuffers {
 public:
  LoginBuffers() {
    std::memset(this->user_, 0, sizeof(this->user_));
    std::memset(this->pass_, 0, sizeof(this->pass_));
  }
  /// Write the pair into the inactive buffers and make them the active ones. Values longer than
  /// the buffers are truncated (callers validate first).
  void set(const std::string &user, const std::string &pass) {
    const int next = this->active_ ^ 1;
    copy_(this->user_[next], sizeof(this->user_[next]), user);
    copy_(this->pass_[next], sizeof(this->pass_[next]), pass);
    this->active_ = next;
  }
  const char *user() const { return this->user_[this->active_]; }
  const char *pass() const { return this->pass_[this->active_]; }
  int active_index() const { return this->active_; }

 private:
  static void copy_(char *dst, size_t cap, const std::string &src) {
    const size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
  }
  char user_[2][sizeof(WebLoginPref::user)];
  char pass_[2][sizeof(WebLoginPref::pass)];
  volatile int active_{0};
};

/// Outcome of a login change request (web UI verdict codes).
enum class LoginChange : uint8_t { OK, MANAGED_BY_YAML, WRONG_CURRENT, INVALID, MISMATCH, STORE_FAILED };

inline const char *login_change_code(LoginChange c) {
  switch (c) {
    case LoginChange::OK:
      return "OK";
    case LoginChange::MANAGED_BY_YAML:
      return "MANAGED_BY_YAML";
    case LoginChange::WRONG_CURRENT:
      return "WRONG_CURRENT";
    case LoginChange::INVALID:
      return "INVALID";
    case LoginChange::MISMATCH:
      return "MISMATCH";
    default:
      return "STORE_FAILED";
  }
}

/// Pure part of a change request: check the current password, the new pair and the confirmation.
/// `why` carries the detail on INVALID.
inline LoginChange check_login_change(const std::string &current_pass, const std::string &given_current,
                                      const std::string &new_user, const std::string &new_pass,
                                      const std::string &confirm, std::string *why) {
  if (!login_equal(current_pass, given_current))
    return LoginChange::WRONG_CURRENT;
  if (!validate_login_username(new_user, why) || !validate_login_password(new_pass, why))
    return LoginChange::INVALID;
  if (new_pass != confirm)
    return LoginChange::MISMATCH;
  return LoginChange::OK;
}

}  // namespace sungrow_web
}  // namespace esphome
