// Host tests for the runtime-settings headers: login_prefs.h (web login validation, NVS record,
// double-buffered swap), mqtt_settings.h (pack/unpack/validate/JSON/precedence) and
// cloud_version.h (stock version regex, normalisation, precedence, persist on first sight).
#include <string>

#include "../test_harness.h"
#include "cloud_version.h"
#include "login_prefs.h"
#include "mqtt_settings.h"

using namespace esphome::sungrow_web;
using namespace esphome::sungrow_mqtt;
using namespace esphome::sungrow_board;

static bool contains(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }

// ---- login_prefs.h -----------------------------------------------------------------------------

TEST(login_username_rules) {
  std::string why;
  CHECK(validate_login_username("admin", &why));
  CHECK(validate_login_username("a", &why));
  CHECK(validate_login_username(std::string(31, 'x'), &why));
  CHECK(!validate_login_username("", &why));
  CHECK(!validate_login_username(std::string(32, 'x'), &why));
  CHECK(!validate_login_username("ad:min", &why));  // ':' breaks the digest A1 string
  CHECK(contains(why, "':'"));
  CHECK(!validate_login_username("ad\tmin", &why));
  CHECK(!validate_login_username(" admin", &why));
  CHECK(!validate_login_username("admin ", &why));
  CHECK(!validate_login_username("\xc3\xa9", &why));  // non-ASCII
}

TEST(login_password_rules) {
  std::string why;
  CHECK(validate_login_password("rewired-s", &why));
  CHECK(validate_login_password("12345678", &why));
  CHECK(validate_login_password(std::string(63, 'p'), &why));
  CHECK(validate_login_password("with space ok", &why));
  CHECK(!validate_login_password("1234567", &why));
  CHECK(contains(why, "8-63"));
  CHECK(!validate_login_password(std::string(64, 'p'), &why));
  CHECK(!validate_login_password("pass:word1", &why));
  CHECK(!validate_login_password("pass\nword1", &why));
}

TEST(login_pack_unpack_roundtrip) {
  WebLoginPref p;
  pack_login("nick", "s3cret-pass", &p);
  CHECK_EQ(p.magic, WEB_LOGIN_MAGIC);
  std::string u, pw;
  CHECK(unpack_login(p, &u, &pw));
  CHECK(u == "nick");
  CHECK(pw == "s3cret-pass");
  // Wrong magic, unterminated field and an invalid stored pair are all rejected.
  WebLoginPref bad = p;
  bad.magic = 0;
  CHECK(!unpack_login(bad, &u, &pw));
  bad = p;
  std::memset(bad.user, 'x', sizeof(bad.user));
  CHECK(!unpack_login(bad, &u, &pw));
  pack_login("nick", "short", &p);  // too short to have been accepted; a corrupt record
  CHECK(!unpack_login(p, &u, &pw));
  // Truncation on pack never overruns the arrays.
  pack_login(std::string(100, 'u'), std::string(100, 'p'), &p);
  CHECK_EQ(std::strlen(p.user), LOGIN_USER_MAX);
  CHECK_EQ(std::strlen(p.pass), LOGIN_PASS_MAX);
}

TEST(login_equal_constant_time_compare) {
  CHECK(login_equal("abc", "abc"));
  CHECK(!login_equal("abc", "abd"));
  CHECK(!login_equal("abc", "abcd"));
  CHECK(!login_equal("", "a"));
  CHECK(login_equal("", ""));
}

TEST(login_buffers_double_buffered_swap) {
  LoginBuffers b;
  b.set("admin", "rewired-s");
  const char *u0 = b.user();
  const char *p0 = b.pass();
  CHECK(std::string(u0) == "admin");
  CHECK(std::string(p0) == "rewired-s");
  const int first = b.active_index();
  b.set("nick", "new-password");
  CHECK(b.active_index() != first);
  // The previously handed-out pointers still read the OLD complete pair (never overwritten in
  // place), the new pointers the new pair.
  CHECK(std::string(u0) == "admin");
  CHECK(std::string(p0) == "rewired-s");
  CHECK(std::string(b.user()) == "nick");
  CHECK(std::string(b.pass()) == "new-password");
  CHECK(b.user() != u0);
  // A third set() reuses the first pair's buffers.
  b.set("third", "third-password");
  CHECK(b.user() == u0);
  CHECK(std::string(u0) == "third");
}

TEST(login_change_checks) {
  std::string why;
  CHECK(check_login_change("rewired-s", "wrong", "nick", "new-password", "new-password", &why) ==
        LoginChange::WRONG_CURRENT);
  CHECK(check_login_change("rewired-s", "rewired-s", "", "new-password", "new-password", &why) ==
        LoginChange::INVALID);
  CHECK(check_login_change("rewired-s", "rewired-s", "nick", "short", "short", &why) == LoginChange::INVALID);
  CHECK(check_login_change("rewired-s", "rewired-s", "nick", "new-password", "other-password", &why) ==
        LoginChange::MISMATCH);
  CHECK(check_login_change("rewired-s", "rewired-s", "nick", "new-password", "new-password", &why) ==
        LoginChange::OK);
  CHECK(std::string(login_change_code(LoginChange::MANAGED_BY_YAML)) == "MANAGED_BY_YAML");
  CHECK(std::string(login_change_code(LoginChange::OK)) == "OK");
}

// ---- mqtt_settings.h ---------------------------------------------------------------------------

static MqttSettings sample_mqtt() {
  MqttSettings s;
  s.host = "192.0.2.20";
  s.port = 8883;
  s.user = "mqtt";
  s.pass = "p@ss word";
  s.prefix = "homeassistant";
  s.tls = true;
  return s;
}

TEST(mqtt_validate) {
  std::string why;
  CHECK(validate_mqtt_settings(sample_mqtt(), &why));
  MqttSettings s = sample_mqtt();
  s.host = "";  // MQTT off
  CHECK(validate_mqtt_settings(s, &why));
  s = sample_mqtt();
  s.host = std::string(127, 'h');
  CHECK(validate_mqtt_settings(s, &why));
  s.host = std::string(128, 'h');
  CHECK(!validate_mqtt_settings(s, &why));
  CHECK(contains(why, "host"));
  s = sample_mqtt();
  s.host = "my broker";
  CHECK(!validate_mqtt_settings(s, &why));
  s = sample_mqtt();
  s.host = "mqtt://broker";
  CHECK(!validate_mqtt_settings(s, &why));
  CHECK(contains(why, "URL"));
  s = sample_mqtt();
  s.port = 0;
  CHECK(!validate_mqtt_settings(s, &why));
  s = sample_mqtt();
  s.user = std::string(63, 'u');
  CHECK(validate_mqtt_settings(s, &why));
  s.user = std::string(64, 'u');
  CHECK(!validate_mqtt_settings(s, &why));
  s = sample_mqtt();
  s.pass = std::string(64, 'p');  // a 64-character broker password (Mosquitto add-on style) must fit
  CHECK(validate_mqtt_settings(s, &why));
  s.pass = std::string(127, 'p');
  CHECK(validate_mqtt_settings(s, &why));
  s.pass = std::string(128, 'p');
  CHECK(!validate_mqtt_settings(s, &why));
  s = sample_mqtt();
  s.pass = "tab\there";
  CHECK(!validate_mqtt_settings(s, &why));
  s = sample_mqtt();
  s.prefix = "";
  CHECK(!validate_mqtt_settings(s, &why));
  s.prefix = "home assistant";
  CHECK(!validate_mqtt_settings(s, &why));
  s.prefix = "ha/+";
  CHECK(!validate_mqtt_settings(s, &why));
  s.prefix = "/ha";
  CHECK(!validate_mqtt_settings(s, &why));
  s.prefix = "site-1/ha_disc";
  CHECK(validate_mqtt_settings(s, &why));
}

TEST(mqtt_pack_unpack) {
  MqttSettingsPref p;
  pack_mqtt_settings(sample_mqtt(), &p);
  CHECK_EQ(p.magic, MQTT_SETTINGS_MAGIC);
  CHECK_EQ(p.port, 8883);
  CHECK_EQ(p.tls, 1);
  MqttSettings s;
  CHECK(unpack_mqtt_settings(p, &s));
  CHECK(s.host == "192.0.2.20");
  CHECK(s.user == "mqtt");
  CHECK(s.pass == "p@ss word");
  CHECK(s.prefix == "homeassistant");
  CHECK(s.tls);
  // Defaults fill a record with port 0 / empty prefix.
  p.port = 0;
  p.prefix[0] = '\0';
  CHECK(unpack_mqtt_settings(p, &s));
  CHECK_EQ(s.port, 1883);
  CHECK(s.prefix == "homeassistant");
  // Wrong magic or an unterminated field: ignored.
  p.magic = 1;
  CHECK(!unpack_mqtt_settings(p, &s));
  pack_mqtt_settings(sample_mqtt(), &p);
  std::memset(p.pass, 'x', sizeof(p.pass));
  CHECK(!unpack_mqtt_settings(p, &s));
  // Over-long inputs are truncated into the record, never past it.
  MqttSettings big = sample_mqtt();
  big.host = std::string(200, 'h');
  pack_mqtt_settings(big, &p);
  CHECK_EQ(std::strlen(p.host), sizeof(p.host) - 1);
}

TEST(mqtt_resolve_precedence) {
  MqttSettings yaml;  // no broker in YAML
  MqttSettings stored = sample_mqtt();
  MqttSettings out;
  CHECK(resolve_mqtt_settings(yaml, &stored, &out) == MqttSettingsSource::RUNTIME);
  CHECK(out.host == "192.0.2.20");
  CHECK(resolve_mqtt_settings(yaml, nullptr, &out) == MqttSettingsSource::NONE);
  CHECK(out.host.empty());
  CHECK_EQ(out.port, 1883);
  MqttSettings empty_stored;  // record present but cleared from the UI
  CHECK(resolve_mqtt_settings(yaml, &empty_stored, &out) == MqttSettingsSource::NONE);
  yaml.host = "broker.lan";
  yaml.port = 1883;
  CHECK(resolve_mqtt_settings(yaml, &stored, &out) == MqttSettingsSource::YAML);  // YAML wins
  CHECK(out.host == "broker.lan");
  CHECK_EQ(out.port, 1883);
  CHECK(std::string(mqtt_settings_source_name(MqttSettingsSource::YAML)) == "yaml");
  CHECK(std::string(mqtt_settings_source_name(MqttSettingsSource::RUNTIME)) == "runtime");
  CHECK(std::string(mqtt_settings_source_name(MqttSettingsSource::NONE)) == "none");
}

TEST(mqtt_json_never_carries_the_password) {
  const std::string j = mqtt_settings_json(sample_mqtt(), MqttSettingsSource::RUNTIME, true, false, "refused");
  CHECK(!contains(j, "p@ss"));
  CHECK(contains(j, "\"has_password\":true"));
  CHECK(contains(j, "\"host\":\"192.0.2.20\""));
  CHECK(contains(j, "\"port\":8883"));
  CHECK(contains(j, "\"username\":\"mqtt\""));
  CHECK(contains(j, "\"tls\":true"));
  CHECK(contains(j, "\"source\":\"runtime\""));
  CHECK(contains(j, "\"managed_by_yaml\":false"));
  CHECK(contains(j, "\"configured\":true"));
  CHECK(contains(j, "\"connected\":false"));
  CHECK(contains(j, "\"last_error\":\"refused\""));
  CHECK(contains(j, "\"available\":true"));
  MqttSettings s;
  const std::string none = mqtt_settings_json(s, MqttSettingsSource::NONE, true, false, "");
  CHECK(contains(none, "\"configured\":false"));
  CHECK(contains(none, "\"has_password\":false"));
  MqttSettings q = sample_mqtt();
  q.user = "quo\"te\\";
  const std::string esc = mqtt_settings_json(q, MqttSettingsSource::YAML, false, true, "");
  CHECK(contains(esc, "\"username\":\"quo\\\"te\\\\\""));
  CHECK(contains(esc, "\"managed_by_yaml\":true"));
}

// ---- cloud_version.h ---------------------------------------------------------------------------

TEST(cloud_version_regex_and_normalise) {
  CHECK(is_stock_version_string("WINET-SV300.001.03.P032B001-dir"));
  CHECK(is_stock_version_string("WINET-SV200.001.00.P046B001-dir"));
  CHECK(is_stock_version_string("WINET-SV200.001.00.P046"));
  CHECK(!is_stock_version_string("WINET-SV200.001.00.P04"));
  CHECK(!is_stock_version_string("WINET-SV20.001.00.P046"));
  CHECK(!is_stock_version_string("WINET-SV200.001.0.P046"));
  CHECK(!is_stock_version_string("WINET-SV200.001.00.X046"));
  CHECK(!is_stock_version_string("2026.9.1"));
  CHECK(!is_stock_version_string(""));
  CHECK(!is_stock_version_string(nullptr));
  CHECK(normalize_stock_version("WINET-SV300.001.03.P032B001-dir") == "WINET-SV300.001.03.P032");
  CHECK(normalize_stock_version("WINET-SV200.001.00.P046B001-dir") == "WINET-SV200.001.00.P046");
  CHECK(normalize_stock_version("WINET-SV200.001.00.P046") == "WINET-SV200.001.00.P046");
  CHECK(normalize_stock_version("rewired-s 0.9.0").empty());
  std::string why;
  CHECK(validate_cloud_version_override("WINET-SV200.001.00.P044", &why));
  CHECK(!validate_cloud_version_override("WINET-SV200.001.00.P044B001-dir", &why));  // not normalised
  CHECK(!validate_cloud_version_override("P044", &why));
  CHECK(contains(why, "WINET-SV"));
}

TEST(cloud_version_precedence) {
  CloudVersionPref p;
  cloud_version_pref_init(&p);
  CHECK(cloud_version_pref_valid(p));
  CloudVersionChoice c = choose_cloud_version(&p, "", "WINET-SV200.001.00.P046");
  CHECK(c.source == CloudVersionSource::DEFAULT);
  CHECK(c.value == "WINET-SV200.001.00.P046");
  c = choose_cloud_version(nullptr, "", "WINET-SV200.001.00.P046");
  CHECK(c.source == CloudVersionSource::DEFAULT);
  CHECK(note_detected_version(&p, "WINET-SV200.001.00.P044B001-dir"));
  c = choose_cloud_version(&p, "", "WINET-SV200.001.00.P046");
  CHECK(c.source == CloudVersionSource::DETECTED);
  CHECK(c.value == "WINET-SV200.001.00.P044");
  c = choose_cloud_version(&p, "WINET-SV200.001.00.P040", "WINET-SV200.001.00.P046");
  CHECK(c.source == CloudVersionSource::YAML);  // YAML beats detected
  CHECK(c.value == "WINET-SV200.001.00.P040");
  CHECK(set_cloud_version_override(&p, "WINET-SV200.001.00.P038"));
  CHECK(!set_cloud_version_override(&p, "WINET-SV200.001.00.P038"));  // unchanged: no save needed
  c = choose_cloud_version(&p, "WINET-SV200.001.00.P040", "WINET-SV200.001.00.P046");
  CHECK(c.source == CloudVersionSource::OVERRIDE);  // override beats everything
  CHECK(c.value == "WINET-SV200.001.00.P038");
  CHECK(set_cloud_version_override(&p, ""));
  c = choose_cloud_version(&p, "", "WINET-SV200.001.00.P046");
  CHECK(c.source == CloudVersionSource::DETECTED);
  CHECK(std::string(cloud_version_source_name(CloudVersionSource::OVERRIDE)) == "override");
  CHECK(std::string(cloud_version_source_name(CloudVersionSource::DEFAULT)) == "default");
}

TEST(cloud_version_persist_on_first_sight) {
  CloudVersionPref p;
  cloud_version_pref_init(&p);
  CHECK(!note_detected_version(&p, "2026.9.1"));  // a Rewired-S image in the other slot: nothing
  CHECK(p.detected[0] == '\0');
  CHECK(note_detected_version(&p, "WINET-SV300.001.03.P032B001-dir"));
  CHECK(std::string(p.detected) == "WINET-SV300.001.03.P032");
  // The slot is overwritten by the next OTA (or shows another stock build later): the first
  // sighting is kept and no further save is requested.
  CHECK(!note_detected_version(&p, "WINET-SV300.001.03.P024B001-dir"));
  CHECK(std::string(p.detected) == "WINET-SV300.001.03.P032");
  CHECK(!note_detected_version(&p, "2026.9.1"));
  // A record from another build (wrong magic / unterminated) is replaced by an empty one.
  CloudVersionPref bad = p;
  bad.magic = 0;
  CHECK(!cloud_version_pref_valid(bad));
  bad = p;
  std::memset(bad.detected, 'x', sizeof(bad.detected));
  CHECK(!cloud_version_pref_valid(bad));
}

int main() { return th::run_all(); }
