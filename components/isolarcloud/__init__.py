"""isolarcloud: iSolarCloud MQTT uplink, acting as the stock WiNet-S/S2 dongle.

Protocol references: docs/cloud_identity.md (identity, credentials, CONNECT, will) and the SG-Docs
pages reference/protocols/cloud-{mqtt,envelope,messages,commands}.md.
"""

import re

from esphome import automation
import esphome.codegen as cg
from esphome.components import binary_sensor, esp32, sensor, switch, text_sensor, time
import esphome.config_validation as cv
from esphome.core import CORE
from esphome.const import (
    CONF_ID,
    CONF_PASSWORD,
    CONF_PORT,
    CONF_TIME_ID,
    CONF_USERNAME,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_TOTAL_INCREASING,
)

CODEOWNERS = ["@nickstallman"]
DEPENDENCIES = ["network", "time"]
AUTO_LOAD = ["json", "binary_sensor", "sensor", "text_sensor", "switch"]

# Compiled-in shared iSolarCloud MQTT credentials: identical on every WiNet-S2 and WiNet-S
# (docs/cloud_identity.md). Only the SN is per device.
DEFAULT_USERNAME = "random@An3Fc9Ba"
DEFAULT_PASSWORD = "OZxy7oHE1W7LqhSXBOUToNrzTTC4zp2s"

CONF_SN = "sn"
CONF_IDENTITY_SOURCE = "identity_source"
CONF_BROKER_HOST = "broker_host"
CONF_SSL_FLAG = "ssl_flag"
CONF_MQTT_KEY = "mqtt_key"
CONF_ENCRYPT = "encrypt"
CONF_KEYNUM = "keynum"
CONF_COMPRESS = "compress"
CONF_TIMEZONE_ID = "timezone_id"
CONF_TIMEZONE = "timezone"
CONF_APPLY_CLOUD_TIMEZONE = "apply_cloud_timezone"
CONF_CLOUD_SETTIME = "cloud_settime"
CONF_RUNDATA_PERIOD = "rundata_period"
CONF_SILENCE = "silence"
CONF_START_HOUR = "start_hour"
CONF_END_HOUR = "end_hour"
CONF_REMOTE_CTL_MASK = "remote_ctl_mask"
CONF_PRODUCT_TYPE = "product_type"
CONF_PRODUCT_NAME = "product_name"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_VERIFY_SERVER = "verify_server"
CONF_CA_CERTIFICATE = "ca_certificate"
CONF_BEAT_INTERVAL = "beat_interval"
CONF_LOGGER_ATTR_INTERVAL = "logger_attr_interval"
CONF_SEND_TIME_UTC = "send_time_utc"
CONF_DEVINFO_ON_CONNECT = "devinfo_on_connect"
CONF_PUBLISH_ACK = "publish_ack"
CONF_RETRIES = "retries"
CONF_DELAY = "delay"
CONF_RECONNECT = "reconnect"
CONF_MIN_DELAY = "min_delay"
CONF_MAX_DELAY = "max_delay"
CONF_SERVER_SWITCH = "server_switch"
CONF_CONFIRM_AFTER = "confirm_after"
CONF_TIMEOUT = "timeout"
CONF_CONNECTED = "connected"
CONF_PUBLISH_COUNT = "publish_count"
CONF_LAST_PUBLISH = "last_publish"
CONF_LAST_ERROR = "last_error"
CONF_LAST_COMMAND = "last_command"
CONF_LAST_CONTROL_COMMAND = "last_control_command"
CONF_UPLINK = "uplink"
CONF_WRITES = "writes"

isolarcloud_ns = cg.esphome_ns.namespace("isolarcloud")
IsolarcloudComponent = isolarcloud_ns.class_("IsolarcloudComponent", cg.Component)
IdentitySource = isolarcloud_ns.class_("IdentitySource")
# Sibling components the uplink binds to (adapters.h). Referenced by class only, so isolarcloud
# does not import their Python modules or require them.
SungrowExtFlash = cg.esphome_ns.namespace("sungrow_extflash").class_("SungrowExtFlash", cg.Component)
SungrowDataModel = cg.esphome_ns.namespace("sungrow_datamodel").class_("SungrowDataModel", cg.Component)
SungrowModbus = cg.esphome_ns.namespace("sungrow_modbus").class_("SungrowModbus", cg.Component)
CONF_SUNGROW_DATAMODEL_ID = "sungrow_datamodel_id"
CONF_SUNGROW_MODBUS_ID = "sungrow_modbus_id"
UplinkSwitch = isolarcloud_ns.class_("UplinkSwitch", switch.Switch, cg.Component)
WritesSwitch = isolarcloud_ns.class_("WritesSwitch", switch.Switch, cg.Component)
ServerSwitchAction = isolarcloud_ns.class_("ServerSwitchAction", automation.Action)


def _sn(value):
    # "" is allowed and means "not set yet": the uplink stays idle (or waits for identity_source).
    value = cv.string(value)
    if len(value) > 20:
        raise cv.Invalid("SN must be at most 20 characters (sig 0x0002 is 20 bytes)")
    if any(c in value for c in "/#+ ") or not value.isprintable():
        raise cv.Invalid("SN must be printable and must not contain '/', '#' or '+'")
    return value


def _key16(value):
    value = cv.string_strict(value)
    if len(value.encode()) > 16:
        raise cv.Invalid("mqtt_key is at most 16 bytes (AES-128, sig 0x1013)")
    return value


_TZ_RE = re.compile(r"^(UTC|GMT)([+-]\d{1,2}(:\d{2})?)?$")


def _timezone(value):
    # Cloud-style zone (sig 0x1089): "UTC+10:00" = 10 h AHEAD of UTC (the opposite of POSIX TZ).
    value = cv.string_strict(value)
    m = _TZ_RE.match(value)
    if not m:
        raise cv.Invalid("timezone must look like 'UTC+10:00', 'UTC-3:30' or 'UTC' (cloud style, + = east)")
    if m.group(2):
        hh, _, mm = m.group(2)[1:].partition(":")
        if int(hh) * 60 + int(mm or 0) > 14 * 60 or int(mm or 0) > 59:
            raise cv.Invalid("timezone offset must be within +-14:00")
    if len(value.encode()) > 16:
        raise cv.Invalid("timezone is at most 16 bytes (sig 0x1089)")
    return value


def _validate(config):
    if CONF_SN not in config and CONF_IDENTITY_SOURCE not in config:
        raise cv.Invalid(f"Set '{CONF_SN}' (the dongle serial) or '{CONF_IDENTITY_SOURCE}'")
    if CONF_CA_CERTIFICATE in config and not config[CONF_VERIFY_SERVER]:
        raise cv.Invalid(f"'{CONF_CA_CERTIFICATE}' needs '{CONF_VERIFY_SERVER}: true'")
    rc = config[CONF_RECONNECT]
    if rc[CONF_MIN_DELAY] > rc[CONF_MAX_DELAY]:
        raise cv.Invalid("reconnect min_delay must be <= max_delay")
    return config


# start == end = silenced all day.
SILENCE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_START_HOUR): cv.int_range(min=0, max=23),
        cv.Required(CONF_END_HOUR): cv.int_range(min=0, max=24),
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(IsolarcloudComponent),
            cv.GenerateID(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            # --- identity (unset fields: stock defaults < persisted < identity source) ---
            cv.Optional(CONF_SN): _sn,
            # The sungrow_extflash id (boards/ name it `extflash`). Only values found in the
            # dongle's sigdata file are used; YAML values above still win.
            cv.Optional(CONF_IDENTITY_SOURCE): cv.use_id(SungrowExtFlash),
            # Telemetry (rundata/devinfo) and getparam. Default: the configured
            # sungrow_datamodel / sungrow_modbus instance, if any.
            cv.Optional(CONF_SUNGROW_DATAMODEL_ID): cv.use_id(SungrowDataModel),
            cv.Optional(CONF_SUNGROW_MODBUS_ID): cv.use_id(SungrowModbus),
            cv.Optional(CONF_BROKER_HOST): cv.All(cv.string_strict, cv.Length(min=1, max=64)),  # iot.isolarcloud.com
            cv.Optional(CONF_PORT): cv.port,  # 16668
            # 0 = plain TCP, 2 = TLS (default), anything else = no connection.
            # Port 19999 forces TCP.
            cv.Optional(CONF_SSL_FLAG): cv.uint16_t,
            cv.Optional(CONF_MQTT_KEY): _key16,
            cv.Optional(CONF_ENCRYPT): cv.uint16_t,  # 0
            cv.Optional(CONF_KEYNUM): cv.uint16_t,  # 0
            cv.Optional(CONF_COMPRESS): cv.uint16_t,  # 1 = zlib
            # 0x107a timezoneId (default 8): `cmd/<id>/enum_0_0_0` broadcast; the cloud updates it.
            cv.Optional(CONF_TIMEZONE_ID): cv.uint16_t,
            # 0x1089 (default "UTC+8:00", the stock factory zone). sendTime/dataTime are local time
            # = UTC + this offset. Set it to your plant's zone unless the extflash copy has it.
            cv.Optional(CONF_TIMEZONE): _timezone,
            # Apply the cloud's settimezone to the local time (default on).
            cv.Optional(CONF_APPLY_CLOUD_TIMEZONE, default=True): cv.boolean,
            # Honour the cloud's settime broadcast (10307) by setting the system clock.
            cv.Optional(CONF_CLOUD_SETTIME, default=True): cv.boolean,
            # 0x1015, minutes; 0 = 5. Forced to 5 for the first 30 min after boot (stock).
            cv.Optional(CONF_RUNDATA_PERIOD): cv.int_range(min=0, max=1440),
            cv.Optional(CONF_SILENCE): SILENCE_SCHEMA,
            cv.Optional(CONF_REMOTE_CTL_MASK): cv.hex_uint32_t,  # 0x4FF
            # --- product constants (default: sungrow_board) ---
            cv.Optional(CONF_PRODUCT_TYPE): cv.hex_uint16_t,
            cv.Optional(CONF_PRODUCT_NAME): cv.string_strict,
            cv.Optional(CONF_FIRMWARE_VERSION): cv.string_strict,
            # --- connection ---
            cv.Optional(CONF_USERNAME, default=DEFAULT_USERNAME): cv.string_strict,
            cv.Optional(CONF_PASSWORD, default=DEFAULT_PASSWORD): cv.string_strict,
            # The chain is verified against /usrdataout/tlsca/cafile; the host name is not checked.
            # Default trust store: that file from the ext flash; ca_certificate overrides it; the IDF
            # bundle is the last resort. false = no verification at all (escape hatch).
            cv.Optional(CONF_VERIFY_SERVER, default=True): cv.boolean,
            cv.Optional(CONF_CA_CERTIFICATE): cv.string_strict,
            cv.Optional(CONF_RECONNECT, default={}): cv.Schema(
                {
                    cv.Optional(CONF_MIN_DELAY, default="5s"): cv.positive_time_period_milliseconds,
                    cv.Optional(CONF_MAX_DELAY, default="5min"): cv.positive_time_period_milliseconds,
                }
            ),
            cv.Optional(CONF_SERVER_SWITCH, default={}): cv.Schema(
                {
                    cv.Optional(CONF_CONFIRM_AFTER, default="5s"): cv.positive_time_period_milliseconds,
                    cv.Optional(CONF_TIMEOUT, default="10min"): cv.positive_time_period_milliseconds,
                    cv.Optional(CONF_DELAY, default="10s"): cv.positive_time_period_milliseconds,
                }
            ),
            # PUBACK gating: the next chunk / message is sent after the previous PUBACK; without one
            # within `timeout` the chunk is re-sent up to `retries` times, then the message dropped.
            cv.Optional(CONF_PUBLISH_ACK, default={}): cv.Schema(
                {
                    cv.Optional(CONF_TIMEOUT, default="20s"): cv.All(
                        cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(seconds=1))
                    ),
                    cv.Optional(CONF_RETRIES, default=3): cv.int_range(min=0, max=10),
                }
            ),
            # --- publishing ---
            # beatdata every 1800 s (and at every CONNACK), logger attrs every 24 h.
            cv.Optional(CONF_BEAT_INTERVAL, default="1800s"): cv.All(
                cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(seconds=5))
            ),
            cv.Optional(CONF_LOGGER_ATTR_INTERVAL, default="24h"): cv.All(
                cv.positive_time_period_milliseconds, cv.Range(min=cv.TimePeriod(minutes=1))
            ),
            # devinfo (logger: server endpoints, 38781 = zeros) after the first-connect rundata of a
            # boot. auto = per sungrow_board variant: S2 yes, WiNet-S no (it sends devinfo at every
            # CONNACK instead); yes without a board. A server-switch confirm always sends it.
            cv.Optional(CONF_DEVINFO_ON_CONNECT, default="auto"): cv.Any(cv.one_of("auto", lower=True), cv.boolean),
            # Escape hatch: sendTime/dataTime in UTC instead of the timezone's local time.
            cv.Optional(CONF_SEND_TIME_UTC, default=False): cv.boolean,
            # --- entities ---
            cv.Optional(CONF_CONNECTED): binary_sensor.binary_sensor_schema(
                device_class="connectivity", entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            ),
            cv.Optional(CONF_PUBLISH_COUNT): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:cloud-upload",
            ),
            cv.Optional(CONF_LAST_PUBLISH): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:cloud-clock"
            ),
            cv.Optional(CONF_LAST_ERROR): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:cloud-alert"
            ),
            # Last cloud->dongle command ("<cmdType> <label> [batchId=<id>]"); every command.
            cv.Optional(CONF_LAST_COMMAND): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:cloud-download"
            ),
            # Same, but only write/control commands (not getparam / sp600-read / propertydata polls).
            cv.Optional(CONF_LAST_CONTROL_COMMAND): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:cloud-cog"
            ),
            cv.Optional(CONF_UPLINK): switch.switch_schema(
                UplinkSwitch,
                default_restore_mode="RESTORE_DEFAULT_ON",
                entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:cloud",
            ),
            # Lets iSolarCloud change device settings (setparam). Separate from sungrow_control's
            # writes_enabled, so the cloud can stay read-only while local control is on, or the
            # reverse. Without this switch the cloud is read-only.
            cv.Optional(CONF_WRITES): switch.switch_schema(
                WritesSwitch,
                default_restore_mode="RESTORE_DEFAULT_OFF",
                entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:cloud-lock-open",
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _validate,
)


def _devinfo_on_connect(value):
    """Resolve `auto`: false only for a sungrow_board with variant winet_s."""
    if value != "auto":
        return value
    board = CORE.config.get("sungrow_board")
    if isinstance(board, list):
        board = board[0] if len(board) == 1 else None
    return not (board and board.get("variant") == "winet_s")


def _sibling_id(config, key, domain):
    """Explicit id, else the id of the single configured `domain` instance, else None."""
    if key in config:
        return config[key]
    conf = CORE.config.get(domain)
    if isinstance(conf, list):
        if len(conf) != 1:
            return None
        conf = conf[0]
    return conf[CONF_ID] if conf else None


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # IDF esp-mqtt (excluded by default because ESPHome's own `mqtt:` normally brings it) and
    # esp-tls (mqtt_client.h exposes its types). Same as esphome/components/mqtt on IDF < 6.
    if esp32.idf_version() >= cv.Version(6, 0, 0):
        esp32.add_idf_component(name="espressif/mqtt", ref="1.0.0")
    else:
        esp32.include_builtin_idf_component("mqtt")
    esp32.include_builtin_idf_component("esp-tls")

    cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))

    if config.get(CONF_SN):
        cg.add(var.set_sn(config[CONF_SN]))
    if CONF_IDENTITY_SOURCE in config:
        cg.add_define("ISOLARCLOUD_BIND_EXTFLASH")
        cg.add(var.bind_extflash(await cg.get_variable(config[CONF_IDENTITY_SOURCE])))
    if (dm_id := _sibling_id(config, CONF_SUNGROW_DATAMODEL_ID, "sungrow_datamodel")) is not None:
        cg.add_define("ISOLARCLOUD_BIND_DATAMODEL")
        cg.add(var.bind_datamodel(await cg.get_variable(dm_id)))
    if (mb_id := _sibling_id(config, CONF_SUNGROW_MODBUS_ID, "sungrow_modbus")) is not None:
        cg.add_define("ISOLARCLOUD_BIND_MODBUS")
        cg.add(var.bind_modbus(await cg.get_variable(mb_id)))
    for key, setter in (
        (CONF_BROKER_HOST, "set_broker_host"),
        (CONF_PORT, "set_broker_port"),
        (CONF_SSL_FLAG, "set_ssl_flag"),
        (CONF_MQTT_KEY, "set_mqtt_key"),
        (CONF_ENCRYPT, "set_encrypt"),
        (CONF_KEYNUM, "set_keynum"),
        (CONF_COMPRESS, "set_compress"),
        (CONF_TIMEZONE_ID, "set_timezone_id"),
        (CONF_TIMEZONE, "set_timezone"),
        (CONF_RUNDATA_PERIOD, "set_rundata_period_min"),
        (CONF_REMOTE_CTL_MASK, "set_remote_ctl_mask"),
        (CONF_PRODUCT_TYPE, "set_product_type"),
        (CONF_PRODUCT_NAME, "set_product_name"),
        (CONF_FIRMWARE_VERSION, "set_firmware_version"),
    ):
        if key in config:
            cg.add(getattr(var, setter)(config[key]))
    if silence := config.get(CONF_SILENCE):
        cg.add(var.set_silence(True, silence[CONF_START_HOUR], silence[CONF_END_HOUR]))

    cg.add(var.set_credentials(config[CONF_USERNAME], config[CONF_PASSWORD]))
    cg.add(var.set_verify_server(config[CONF_VERIFY_SERVER]))
    if config[CONF_VERIFY_SERVER]:
        if CONF_CA_CERTIFICATE in config:
            cg.add(var.set_ca_certificate(config[CONF_CA_CERTIFICATE]))
        else:
            # Fallback trust store only: normally the CA comes from the ext flash
            # /usrdataout/tlsca/cafile at connect time (C10/C25; contents need HW, N4).
            esp32.require_certificate_bundle()
    else:
        # esp-tls refuses TLS without a verification method unless these are set (global; the
        # http_request component sets the same options from its own verify_ssl).
        esp32.add_idf_sdkconfig_option("CONFIG_ESP_TLS_INSECURE", True)
        esp32.add_idf_sdkconfig_option("CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY", True)

    rc = config[CONF_RECONNECT]
    cg.add(var.set_reconnect_backoff(rc[CONF_MIN_DELAY].total_milliseconds, rc[CONF_MAX_DELAY].total_milliseconds))
    ss = config[CONF_SERVER_SWITCH]
    cg.add(
        var.set_server_switch_timing(
            ss[CONF_CONFIRM_AFTER].total_milliseconds,
            ss[CONF_TIMEOUT].total_milliseconds,
            ss[CONF_DELAY].total_milliseconds,
        )
    )
    pa = config[CONF_PUBLISH_ACK]
    cg.add(var.set_ack_timing(pa[CONF_TIMEOUT].total_milliseconds, pa[CONF_RETRIES]))
    cg.add(var.set_beat_interval(config[CONF_BEAT_INTERVAL].total_milliseconds))
    cg.add(var.set_logger_attr_interval(config[CONF_LOGGER_ATTR_INTERVAL].total_milliseconds))
    cg.add(var.set_devinfo_on_connect(_devinfo_on_connect(config[CONF_DEVINFO_ON_CONNECT])))
    cg.add(var.set_send_time_utc(config[CONF_SEND_TIME_UTC]))
    cg.add(var.set_apply_cloud_timezone(config[CONF_APPLY_CLOUD_TIMEZONE]))
    cg.add(var.set_cloud_settime(config[CONF_CLOUD_SETTIME]))

    if conf := config.get(CONF_CONNECTED):
        cg.add(var.set_connected_binary_sensor(await binary_sensor.new_binary_sensor(conf)))
    if conf := config.get(CONF_PUBLISH_COUNT):
        cg.add(var.set_publish_count_sensor(await sensor.new_sensor(conf)))
    if conf := config.get(CONF_LAST_PUBLISH):
        cg.add(var.set_last_publish_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_LAST_ERROR):
        cg.add(var.set_last_error_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_LAST_COMMAND):
        cg.add(var.set_last_command_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_LAST_CONTROL_COMMAND):
        cg.add(var.set_last_control_command_text_sensor(await text_sensor.new_text_sensor(conf)))
    if conf := config.get(CONF_UPLINK):
        sw = await switch.new_switch(conf)
        await cg.register_component(sw, conf)
        await cg.register_parented(sw, var)
    if conf := config.get(CONF_WRITES):
        sw = await switch.new_switch(conf)
        await cg.register_component(sw, conf)
        await cg.register_parented(sw, var)

    cg.add_define("USE_ISOLARCLOUD")


SERVER_SWITCH_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(IsolarcloudComponent),
        cv.Required(CONF_BROKER_HOST): cv.templatable(cv.string_strict),
        cv.Optional(CONF_PORT): cv.templatable(cv.port),
        cv.Optional(CONF_SSL_FLAG): cv.templatable(cv.uint16_t),
        cv.Optional(CONF_MQTT_KEY): cv.templatable(_key16),
    }
)


@automation.register_action(
    "isolarcloud.server_switch", ServerSwitchAction, SERVER_SWITCH_ACTION_SCHEMA, synchronous=True
)
async def server_switch_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    cg.add(var.set_host(await cg.templatable(config[CONF_BROKER_HOST], args, cg.std_string)))
    if CONF_PORT in config:
        cg.add(var.set_port(await cg.templatable(config[CONF_PORT], args, cg.uint16)))
    if CONF_SSL_FLAG in config:
        cg.add(var.set_ssl_flag(await cg.templatable(config[CONF_SSL_FLAG], args, cg.uint16)))
    if CONF_MQTT_KEY in config:
        cg.add(var.set_mqtt_key(await cg.templatable(config[CONF_MQTT_KEY], args, cg.std_string)))
    return var
