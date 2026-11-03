"""sungrow_mqtt: per-device Home Assistant MQTT discovery for the connected Sungrow devices.

Each attached Modbus device (inverter / battery / meter / charger / ...) is published to a LOCAL
MQTT broker as a SEPARATE Home Assistant device via MQTT discovery, modelled on the winet-extractor
project. The dongle itself stays a native-API ESPHome device (isolarcloud / web_server); only the
connected devices go over this second esp_mqtt client.

YAML:
    sungrow_mqtt:
      broker: !secret mqtt_broker          # 192.0.2.10; omit it with runtime_settings (below)
      username: !secret mqtt_username
      password: !secret mqtt_password
      # port: 1883
      # discovery_prefix: homeassistant
      # runtime_settings: true             # broker/credentials from the web UI, stored in NVS
      #                                    # (pre-built images); a YAML broker always wins
      # connected: {name: MQTT connected}  # binary sensor: the broker session is up
      # sungrow_datamodel_id / sungrow_modbus_id default to the single configured instance
      # sungrow_extflash_id: extflash       # dongle SN (publisher root / via_device)
      # sungrow_control_id: control         # writable controls (number/select/switch), gated

CODEGEN: at import this module bakes a C++ discovery-metadata table (ha_meta_gen.h) from
components/sungrow_datamodel/register_meta.py (names/units/classes/precision) plus a per-signal
publish deadband (deadband.py) through ha_meta_gen.py, so the publisher reads SignalStore values
alongside a flash-resident presentation table rather than hundreds of hidden entity objects.
"""

import esphome.codegen as cg
from esphome.components import binary_sensor, esp32
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_PASSWORD,
    CONF_PORT,
    CONF_USERNAME,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from .ha_meta_gen import write_if_changed as _write_ha_meta

CODEOWNERS = ["@nickstallman"]
DEPENDENCIES = ["network"]
AUTO_LOAD = ["binary_sensor"]

sungrow_mqtt_ns = cg.esphome_ns.namespace("sungrow_mqtt")
SungrowMqttComponent = sungrow_mqtt_ns.class_("SungrowMqttComponent", cg.Component)

# Sibling components referenced by class only (no Python import / hard dependency), matching how
# isolarcloud binds its siblings.
SungrowDataModel = cg.esphome_ns.namespace("sungrow_datamodel").class_("SungrowDataModel", cg.Component)
SungrowModbus = cg.esphome_ns.namespace("sungrow_modbus").class_("SungrowModbus", cg.Component)
SungrowExtFlash = cg.esphome_ns.namespace("sungrow_extflash").class_("SungrowExtFlash", cg.Component)
SungrowControl = cg.esphome_ns.namespace("sungrow_control").class_("SungrowControl", cg.Component)

CONF_BROKER = "broker"
CONF_RUNTIME_SETTINGS = "runtime_settings"
CONF_CONNECTED = "connected"
CONF_DISCOVERY_PREFIX = "discovery_prefix"
CONF_TLS = "tls"
CONF_CA_CERTIFICATE = "ca_certificate"
CONF_SW_VERSION = "sw_version"
CONF_STATE_THROTTLE = "state_throttle"
CONF_SETTLE_GRACE = "settle_grace"
CONF_DEADBAND_SCALE = "deadband_scale"
CONF_SUNGROW_DATAMODEL_ID = "sungrow_datamodel_id"
CONF_SUNGROW_MODBUS_ID = "sungrow_modbus_id"
CONF_SUNGROW_EXTFLASH_ID = "sungrow_extflash_id"
CONF_SUNGROW_CONTROL_ID = "sungrow_control_id"


# Bake the discovery-metadata table at import (config-load time), before the component
# directory is copied to the build.
_write_ha_meta()


# ---------------------------------------------------------------------------------------------
# config schema
# ---------------------------------------------------------------------------------------------

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SungrowMqttComponent),
            # All credential/host values are supplied via !secret at the YAML level; never logged.
            # Optional only with runtime_settings (validated below).
            cv.Optional(CONF_BROKER): cv.string_strict,
            # Broker, port, credentials, discovery prefix and TLS editable on the web UI's Settings
            # page and stored in NVS (mqtt_settings.h). A YAML broker takes precedence.
            cv.Optional(CONF_RUNTIME_SETTINGS, default=False): cv.boolean,
            cv.Optional(CONF_CONNECTED): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PORT, default=1883): cv.port,
            cv.Optional(CONF_USERNAME): cv.string_strict,
            cv.Optional(CONF_PASSWORD): cv.string_strict,
            cv.Optional(CONF_DISCOVERY_PREFIX, default="homeassistant"): cv.string_strict,
            cv.Optional(CONF_TLS, default=False): cv.boolean,
            cv.Optional(CONF_CA_CERTIFICATE): cv.string_strict,
            cv.Optional(CONF_SW_VERSION): cv.string_strict,
            cv.Optional(CONF_STATE_THROTTLE, default="1s"): cv.positive_time_period_milliseconds,
            # Boot: hold the first settled device at least this long (longer, up to 30 s, while another
            # device is still settling) so the whole bus publishes in one pass.
            cv.Optional(CONF_SETTLE_GRACE, default="5s"): cv.positive_time_period_milliseconds,
            # Multiplies every signal's publish deadband (deadband.py); 0 publishes every change.
            cv.Optional(CONF_DEADBAND_SCALE, default=1.0): cv.float_range(min=0.0),
            # Siblings default to the single configured instance (resolved in to_code).
            cv.Optional(CONF_SUNGROW_DATAMODEL_ID): cv.use_id(SungrowDataModel),
            cv.Optional(CONF_SUNGROW_MODBUS_ID): cv.use_id(SungrowModbus),
            cv.Optional(CONF_SUNGROW_EXTFLASH_ID): cv.use_id(SungrowExtFlash),
            cv.Optional(CONF_SUNGROW_CONTROL_ID): cv.use_id(SungrowControl),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


def _validate_broker(config):
    if CONF_BROKER not in config and not config[CONF_RUNTIME_SETTINGS]:
        raise cv.Invalid("sungrow_mqtt needs a broker, or runtime_settings: true to take it from the web UI")
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _validate_broker)


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

    # IDF esp-mqtt + esp-tls (same as isolarcloud; a second esp_mqtt client is safe).
    if esp32.idf_version() >= cv.Version(6, 0, 0):
        esp32.add_idf_component(name="espressif/mqtt", ref="1.0.0")
    else:
        esp32.include_builtin_idf_component("mqtt")
    esp32.include_builtin_idf_component("esp-tls")

    cg.add(var.set_broker(config.get(CONF_BROKER, "")))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_credentials(config.get(CONF_USERNAME, ""), config.get(CONF_PASSWORD, "")))
    cg.add(var.set_discovery_prefix(config[CONF_DISCOVERY_PREFIX]))
    cg.add(var.set_tls(config[CONF_TLS]))
    if config[CONF_TLS] and CONF_CA_CERTIFICATE in config:
        cg.add(var.set_ca_certificate(config[CONF_CA_CERTIFICATE]))
    elif config[CONF_TLS]:
        esp32.require_certificate_bundle()
    if config[CONF_RUNTIME_SETTINGS]:
        # TLS can be switched on from the web UI, so the CA bundle must be in the build.
        cg.add_define("SUNGROW_MQTT_RUNTIME_SETTINGS")
        cg.add(var.set_runtime_settings(True))
        esp32.require_certificate_bundle()
    if conf := config.get(CONF_CONNECTED):
        cg.add(var.set_connected_binary_sensor(await binary_sensor.new_binary_sensor(conf)))
    if CONF_SW_VERSION in config:
        cg.add(var.set_sw_version(config[CONF_SW_VERSION]))
    cg.add(var.set_state_throttle(config[CONF_STATE_THROTTLE].total_milliseconds))
    cg.add(var.set_settle_grace(config[CONF_SETTLE_GRACE].total_milliseconds))
    cg.add(var.set_deadband_scale(config[CONF_DEADBAND_SCALE]))

    # Required siblings (data + inventory).
    dm_id = _sibling_id(config, CONF_SUNGROW_DATAMODEL_ID, "sungrow_datamodel")
    mb_id = _sibling_id(config, CONF_SUNGROW_MODBUS_ID, "sungrow_modbus")
    if dm_id is None or mb_id is None:
        raise cv.Invalid(
            "sungrow_mqtt needs a sungrow_datamodel and a sungrow_modbus; set "
            "sungrow_datamodel_id / sungrow_modbus_id when more than one is configured"
        )
    cg.add(var.set_datamodel(await cg.get_variable(dm_id)))
    cg.add(var.set_modbus(await cg.get_variable(mb_id)))

    # Optional siblings.
    if (ef_id := _sibling_id(config, CONF_SUNGROW_EXTFLASH_ID, "sungrow_extflash")) is not None:
        cg.add_define("SUNGROW_MQTT_BIND_EXTFLASH")
        cg.add(var.set_extflash(await cg.get_variable(ef_id)))
    if (ctl_id := _sibling_id(config, CONF_SUNGROW_CONTROL_ID, "sungrow_control")) is not None:
        cg.add_define("SUNGROW_MQTT_BIND_CONTROL")
        cg.add(var.set_control(await cg.get_variable(ctl_id)))

    cg.add_define("USE_SUNGROW_MQTT")
