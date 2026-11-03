"""sungrow_analytics: anonymous usage reporting and the diagnostics report upload.

Once a day the dongle posts a usage report (firmware, board and feature facts, the models and
firmware versions of the connected devices, uptime, memory, reset reason) and short error events
to the collector, identified by a hash of its serial number. The diagnostics report (build,
detected hardware with its register map, feature state, memory, reset journal, log tail; serials,
the Wi-Fi name and addresses replaced) is uploaded from the web UI on request, and automatically
on a crash-class reset, a runtime fault or an unrecognised device when `enabled` is on, at most
three times a day. docs/analytics.md has the details.

    sungrow_analytics:
      enabled: true                                  # false: manual upload from the web UI only
      time_id: sntp_time                             # optional: timestamps on the reports
      reporting: {name: Usage reporting}             # restored switch; `enabled` is its default
      # endpoint: https://posthog.nickstallman.net   # collector
      # api_key: ...                                 # selects the data source at the collector

Siblings (sungrow_modbus, sungrow_datamodel, sungrow_extflash, isolarcloud, sungrow_mqtt,
sungrow_modbus_tcp, sungrow_control, sungrow_capture) bind to the single configured instance;
set the *_id option when there is more than one.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, switch, time
from esphome.const import CONF_ID, ENTITY_CATEGORY_CONFIG

CODEOWNERS = ["@nickstallman"]
DEPENDENCIES = ["network", "sungrow_modbus", "sungrow_datamodel", "sungrow_board"]
AUTO_LOAD = ["switch"]

sungrow_analytics_ns = cg.esphome_ns.namespace("sungrow_analytics")
SungrowAnalytics = sungrow_analytics_ns.class_("SungrowAnalytics", cg.Component)
ReportingSwitch = sungrow_analytics_ns.class_("ReportingSwitch", switch.Switch, cg.Component)

# Siblings by class only (no Python import), as sungrow_mqtt binds its siblings.
SungrowDataModel = cg.esphome_ns.namespace("sungrow_datamodel").class_("SungrowDataModel", cg.Component)
SungrowModbus = cg.esphome_ns.namespace("sungrow_modbus").class_("SungrowModbus", cg.Component)
SungrowExtFlash = cg.esphome_ns.namespace("sungrow_extflash").class_("SungrowExtFlash", cg.Component)
IsolarcloudComponent = cg.esphome_ns.namespace("isolarcloud").class_("IsolarcloudComponent", cg.Component)
SungrowMqttComponent = cg.esphome_ns.namespace("sungrow_mqtt").class_("SungrowMqttComponent", cg.Component)
SungrowModbusTcp = cg.esphome_ns.namespace("sungrow_modbus_tcp").class_("SungrowModbusTcp", cg.Component)
SungrowControl = cg.esphome_ns.namespace("sungrow_control").class_("SungrowControl", cg.Component)
SungrowCapture = cg.esphome_ns.namespace("sungrow_capture").class_("SungrowCapture", cg.Component)

CONF_ENABLED = "enabled"
CONF_REPORTING = "reporting"
CONF_ENDPOINT = "endpoint"
CONF_API_KEY = "api_key"
CONF_TIME_ID = "time_id"
CONF_SUNGROW_MODBUS_ID = "sungrow_modbus_id"
CONF_SUNGROW_DATAMODEL_ID = "sungrow_datamodel_id"
CONF_SUNGROW_EXTFLASH_ID = "sungrow_extflash_id"
CONF_ISOLARCLOUD_ID = "isolarcloud_id"
CONF_SUNGROW_MQTT_ID = "sungrow_mqtt_id"
CONF_SUNGROW_MODBUS_TCP_ID = "sungrow_modbus_tcp_id"
CONF_SUNGROW_CONTROL_ID = "sungrow_control_id"
CONF_SUNGROW_CAPTURE_ID = "sungrow_capture_id"

DEFAULT_ENDPOINT = "https://posthog.nickstallman.net"
# Public by nature (it ships in every build); it only selects the data source at the collector.
DEFAULT_API_KEY = "phc_fw_csxkWf-F1ZAJ2B2Dqi2ra6frlwKt"


def _https_url(value):
    value = cv.string_strict(value).rstrip("/")
    if not value.startswith("https://"):
        raise cv.Invalid("endpoint must be an https:// URL")
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SungrowAnalytics),
            cv.Optional(CONF_ENABLED, default=True): cv.boolean,
            cv.Optional(CONF_ENDPOINT, default=DEFAULT_ENDPOINT): _https_url,
            cv.Optional(CONF_API_KEY, default=DEFAULT_API_KEY): cv.string_strict,
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            # Runtime on/off (restored). Without a restored state it starts as `enabled`.
            cv.Optional(CONF_REPORTING): switch.switch_schema(
                ReportingSwitch,
                default_restore_mode="RESTORE_DEFAULT_ON",
                entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:chart-box-outline",
            ),
            cv.Optional(CONF_SUNGROW_MODBUS_ID): cv.use_id(SungrowModbus),
            cv.Optional(CONF_SUNGROW_DATAMODEL_ID): cv.use_id(SungrowDataModel),
            cv.Optional(CONF_SUNGROW_EXTFLASH_ID): cv.use_id(SungrowExtFlash),
            cv.Optional(CONF_ISOLARCLOUD_ID): cv.use_id(IsolarcloudComponent),
            cv.Optional(CONF_SUNGROW_MQTT_ID): cv.use_id(SungrowMqttComponent),
            cv.Optional(CONF_SUNGROW_MODBUS_TCP_ID): cv.use_id(SungrowModbusTcp),
            cv.Optional(CONF_SUNGROW_CONTROL_ID): cv.use_id(SungrowControl),
            cv.Optional(CONF_SUNGROW_CAPTURE_ID): cv.use_id(SungrowCapture),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


def _sibling_id(config, key, domain):
    """Explicit id, else the id of the single configured `domain` instance, else None."""
    from esphome.core import CORE

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

    # ESP-IDF HTTP client over esp-tls with the certificate bundle (the collector sits behind a
    # public CA). Both IDF components are excluded from the build until something asks for them.
    esp32.include_builtin_idf_component("esp_http_client")
    esp32.include_builtin_idf_component("esp-tls")
    esp32.require_certificate_bundle()

    cg.add(var.set_enabled(config[CONF_ENABLED]))
    cg.add(var.set_endpoint(config[CONF_ENDPOINT]))
    cg.add(var.set_api_key(config[CONF_API_KEY]))
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
    if conf := config.get(CONF_REPORTING):
        sw = await switch.new_switch(conf)
        await cg.register_component(sw, conf)
        await cg.register_parented(sw, var)

    mb_id = _sibling_id(config, CONF_SUNGROW_MODBUS_ID, "sungrow_modbus")
    dm_id = _sibling_id(config, CONF_SUNGROW_DATAMODEL_ID, "sungrow_datamodel")
    if mb_id is None or dm_id is None:
        raise cv.Invalid(
            "sungrow_analytics needs a sungrow_modbus and a sungrow_datamodel; set "
            "sungrow_modbus_id / sungrow_datamodel_id when more than one is configured"
        )
    cg.add(var.set_modbus(await cg.get_variable(mb_id)))
    cg.add(var.set_datamodel(await cg.get_variable(dm_id)))

    for key, domain, define, setter in (
        (CONF_SUNGROW_EXTFLASH_ID, "sungrow_extflash", "SUNGROW_ANALYTICS_BIND_EXTFLASH", var.set_extflash),
        (CONF_ISOLARCLOUD_ID, "isolarcloud", "SUNGROW_ANALYTICS_BIND_CLOUD", var.set_cloud),
        (CONF_SUNGROW_MQTT_ID, "sungrow_mqtt", "SUNGROW_ANALYTICS_BIND_MQTT", var.set_mqtt),
        (CONF_SUNGROW_MODBUS_TCP_ID, "sungrow_modbus_tcp", "SUNGROW_ANALYTICS_BIND_MODBUS_TCP", var.set_modbus_tcp),
        (CONF_SUNGROW_CONTROL_ID, "sungrow_control", "SUNGROW_ANALYTICS_BIND_CONTROL", var.set_control),
        (CONF_SUNGROW_CAPTURE_ID, "sungrow_capture", "SUNGROW_ANALYTICS_BIND_CAPTURE", var.set_capture),
    ):
        sib_id = _sibling_id(config, key, domain)
        if sib_id is not None:
            cg.add_define(define)
            cg.add(setter(await cg.get_variable(sib_id)))

    cg.add_define("USE_SUNGROW_ANALYTICS")
