#include "write_entities.h"

namespace esphome {
namespace sungrow_control {

using sungrow_datamodel::TYPE_U16;

// ---- hybrid (SH) ------------------------------------------------------------------------------
static const WriteOptionText k_opt_ems_mode[] = {
    {0x0000, "Self-consumption mode (default)"},
    {0x0002, "Forced mode"},
    {0x0003, "External EMS"},
    {0x0004, "VPP"},
};
static const WriteOptionText k_opt_forced_cmd[] = {
    {0x00CC, "Stop (default)"},
    {0x00AA, "Forced charge"},
    {0x00BB, "Forced discharge"},
};
static const WriteOptionText k_opt_enabled_disabled[] = {
    {0x00AA, "Enabled"},
    {0x0055, "Disabled"},
};

static const WriteEntity k_write_hybrid[] = {
    {"ems_mode", "EMS mode", 13050, SIG_EMS_MODE, TYPE_U16, WriteKind::SELECT, 1.0f, "", 1.0f, 0.0f, 8.0f, 0, 8,
     k_opt_ems_mode, 4},
    {"forced_cmd", "Battery forced charge/discharge command", 13051, SIG_FORCED_CMD, TYPE_U16, WriteKind::SELECT,
     1.0f, "", 1.0f, 170.0f, 204.0f, 170, 204, k_opt_forced_cmd, 3},
    {"forced_power", "Battery forced charge/discharge power", 13052, SIG_FORCED_POWER, TYPE_U16, WriteKind::NUMBER,
     1.0f, "W", 1.0f, 0.0f, 10000.0f, 0, 10000, nullptr, 0},
    {"max_soc", "Battery max SOC", 13058, SIG_MAX_SOC, TYPE_U16, WriteKind::NUMBER, 0.1f, "%", 1.0f, 50.0f, 100.0f,
     500, 1000, nullptr, 0},
    {"min_soc", "Battery min SOC", 13059, SIG_MIN_SOC, TYPE_U16, WriteKind::NUMBER, 0.1f, "%", 1.0f, 0.0f, 50.0f, 0,
     500, nullptr, 0},
    {"export_power_limit", "Export power limit", 13074, SIG_EXPORT_POWER_LIMIT, TYPE_U16, WriteKind::NUMBER, 1.0f,
     "W", 1.0f, 0.0f, 30000.0f, 0, 30000, nullptr, 0},
    {"backup_mode", "Backup mode (off-grid option)", 13075, SIG_BACKUP_MODE, TYPE_U16, WriteKind::SWITCH, 1.0f, "",
     1.0f, 85.0f, 170.0f, 85, 170, k_opt_enabled_disabled, 2},
    {"export_power_limit_mode", "Export power limit mode", 13087, SIG_EXPORT_POWER_LIMIT_MODE, TYPE_U16,
     WriteKind::SWITCH, 1.0f, "", 1.0f, 85.0f, 170.0f, 85, 170, k_opt_enabled_disabled, 2},
    {"reserved_soc_backup", "Reserved SOC for backup", 13100, SIG_RESERVED_SOC_BACKUP, TYPE_U16, WriteKind::NUMBER,
     1.0f, "%", 1.0f, 0.0f, 100.0f, 0, 100, nullptr, 0},
    {"battery_max_charge_power", "Battery max charge power", 33047, SIG_BATTERY_MAX_CHARGE_POWER, TYPE_U16,
     WriteKind::NUMBER, 10.0f, "W", 10.0f, 10.0f, 15000.0f, 1, 1500, nullptr, 0},
    {"battery_max_discharge_power", "Battery max discharge power", 33048, SIG_BATTERY_MAX_DISCHARGE_POWER, TYPE_U16,
     WriteKind::NUMBER, 10.0f, "W", 10.0f, 10.0f, 15000.0f, 1, 1500, nullptr, 0},
    {"battery_charging_start_power", "Battery charging start power", 33149, SIG_BATTERY_CHARGING_START_POWER,
     TYPE_U16, WriteKind::NUMBER, 10.0f, "W", 10.0f, 0.0f, 1000.0f, 0, 100, nullptr, 0},
    {"battery_discharging_start_power", "Battery discharging start power", 33150, SIG_BATTERY_DISCHARGING_START_POWER,
     TYPE_U16, WriteKind::NUMBER, 10.0f, "W", 10.0f, 0.0f, 1000.0f, 0, 100, nullptr, 0},
};

// ---- string (SG) ------------------------------------------------------------------------------
// start_stop: a routine day-to-day control (takes the inverter on/off grid; reversible). 0xCF=Start,
// 0xCE=Stop. A 2-option SELECT (not a SWITCH) so the operator makes an explicit choice. MQTT / :502 /
// HA writes are gated by the writes_enabled switch; web writes by the login (no per-write confirm).
static const WriteOptionText k_opt_start_stop[] = {
    {0x00CF, "Start"},
    {0x00CE, "Stop"},
};

static const WriteEntity k_write_string[] = {
    {"power_limit_switch", "Power limitation switch", 5007, SIG_POWER_LIMIT_SWITCH, TYPE_U16, WriteKind::SWITCH, 1.0f,
     "", 1.0f, 85.0f, 170.0f, 85, 170, k_opt_enabled_disabled, 2},
    {"power_limit_pct", "Power limitation setting", 5008, SIG_POWER_LIMIT_PCT, TYPE_U16, WriteKind::NUMBER, 0.1f, "%",
     1.0f, 0.0f, 100.0f, 0, 1000, nullptr, 0},
    {"power_limit_kw", "Power limitation adjustment", 5039, SIG_POWER_LIMIT_KW, TYPE_U16, WriteKind::NUMBER, 0.1f,
     "kW", 0.1f, 0.0f, 100.0f, 0, 1000, nullptr, 0},
    {"start_stop", "Start/stop", 5006, SIG_START_STOP, TYPE_U16, WriteKind::SELECT, 1.0f, "", 1.0f, 206.0f, 207.0f, 206,
     207, k_opt_start_stop, 2},
};

const WriteEntity *writable_entities(Profile p, uint8_t *count) {
  switch (p) {
    case Profile::HYBRID:
      *count = static_cast<uint8_t>(sizeof(k_write_hybrid) / sizeof(k_write_hybrid[0]));
      return k_write_hybrid;
    case Profile::STRING:
      *count = static_cast<uint8_t>(sizeof(k_write_string) / sizeof(k_write_string[0]));
      return k_write_string;
    default:
      *count = 0;
      return nullptr;
  }
}

}  // namespace sungrow_control
}  // namespace esphome
