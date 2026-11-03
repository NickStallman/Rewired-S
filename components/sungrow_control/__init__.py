"""sungrow_control: gated inverter register WRITES (EMS mode, forced charge/discharge, SOC
limits, export limit, SG power limitation) for Home Assistant and the Modbus-TCP :502 server.

Safety model (docs/control.md): a per-device register allow-list (curated profile from
data/write_registers.yaml with the register per protocol type, + YAML extras), value range checks,
device online check, per-register rate limit (sungrow_modbus write_rate_limit), INFO log of every
write. The restored `writes_enabled` switch (default OFF) gates writes from MQTT, the Modbus-TCP
:502 server, the HA action and the native entities. Writes from the web interface (Devices
controls, Installer, Setup) are authorised by the web login and confirm token and do not use the
switch; the allow-list, range and online checks apply to them too.

YAML (common/control.yaml + devices/hybrid_control.yaml / devices/string_control.yaml):

    sungrow_control:
      id: sungrow_ctl
      sungrow_modbus_id: rs485_hub
      sungrow_datamodel_id: sungrow_dm
      verify: true                  # default for entities and the API action: read back after write
      writes_enabled:               # switch for MQTT / :502 / HA action / entities; restored,
                                    # default OFF; keep the id stable
        id: sungrow_writes_enabled
        name: Inverter writes enabled
      last_write: {name: Inverter last write}
      writes_ok: {name: Inverter writes ok}
      writes_failed: {name: Inverter writes failed}
      devices:
        - device: 0                 # sungrow_modbus device index
          profile: hybrid           # hybrid | string | none (curated allow-list)
          readback: true            # poll the profile's registers (fn 3)
          reassert_power_limit: true  # string profile: re-write the last power limit after a
                                      # power-stage restart (docs/control.md)
          registers:                # extra allow-list entries (raw values)
            - {address: 13100, count: 1, min: 0, max: 100}   # 1-based (doc) address

    select:
      - platform: sungrow_control
        key: ems_mode               # data/write_registers.yaml key (options from there)
        device: 0
        name: EMS mode
    number:
      - platform: sungrow_control
        key: forced_power
        device: 0
        name: Battery forced charge/discharge power
    sensor / text_sensor:           # read-back only
      - platform: sungrow_control
        key: ems_mode
        device: 0

Action (for `api: actions:` and automations):
    - sungrow_control.write_registers:
        id: sungrow_ctl
        device: 0
        address: 13050              # 1-based (doc) address, wire = address - 1
        values: [2]
        verify: true
"""

from esphome import automation
import esphome.codegen as cg
# Aliased: the platform submodules sensor.py / text_sensor.py of this package would shadow them.
from esphome.components import sensor as es_sensor, switch, text_sensor as es_text_sensor, time as es_time
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_COUNT,
    CONF_DEVICE,
    CONF_ID,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_TOTAL_INCREASING,
)

from esphome.components.sungrow_datamodel import CONF_SUNGROW_DATAMODEL_ID, SungrowDataModel

from .write_meta import KEYS, PROFILES

CODEOWNERS = []
DEPENDENCIES = ["sungrow_modbus", "sungrow_datamodel"]
AUTO_LOAD = ["switch", "sensor", "text_sensor"]

ns = cg.esphome_ns.namespace("sungrow_control")
SungrowControl = ns.class_("SungrowControl", cg.Component)
WritesEnabledSwitch = ns.class_("WritesEnabledSwitch", switch.Switch, cg.Parented.template(SungrowControl))
WriteRegistersAction = ns.class_("WriteRegistersAction", automation.Action, cg.Parented.template(SungrowControl))

sungrow_modbus_ns = cg.esphome_ns.namespace("sungrow_modbus")
SungrowModbus = sungrow_modbus_ns.class_("SungrowModbus", cg.Component)

CONF_MIN = "min"
CONF_MAX = "max"
CONF_SUNGROW_MODBUS_ID = "sungrow_modbus_id"
CONF_SUNGROW_CONTROL_ID = "sungrow_control_id"
CONF_VERIFY = "verify"
CONF_CLOCK_SYNC_TIME_ID = "clock_sync_time_id"
CONF_WRITES_ENABLED = "writes_enabled"
CONF_LAST_WRITE = "last_write"
CONF_WRITES_OK = "writes_ok"
CONF_WRITES_FAILED = "writes_failed"
CONF_DEVICES = "devices"
CONF_PROFILE = "profile"
CONF_READBACK = "readback"
CONF_REASSERT_POWER_LIMIT = "reassert_power_limit"
CONF_REGISTERS = "registers"
CONF_SIGNED = "signed"
CONF_KEY = "key"
CONF_VALUES = "values"

def _check_rule(c):
    if c[CONF_MIN] > c[CONF_MAX] or c[CONF_ADDRESS] + c[CONF_COUNT] > 65536:
        raise cv.Invalid("need min <= max and address + count <= 65536")
    return c


RULE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_ADDRESS): cv.int_range(min=1, max=65535),
            cv.Optional(CONF_COUNT, default=1): cv.int_range(min=1, max=123),
            cv.Required(CONF_MIN): cv.int_range(min=-32768, max=65535),
            cv.Required(CONF_MAX): cv.int_range(min=-32768, max=65535),
            cv.Optional(CONF_SIGNED, default=False): cv.boolean,
        }
    ),
    lambda c: _check_rule(c),
)

DEVICE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_DEVICE): cv.int_range(min=0, max=255),
        cv.Optional(CONF_PROFILE, default="none"): cv.one_of("none", *PROFILES, lower=True),
        cv.Optional(CONF_READBACK, default=True): cv.boolean,
        cv.Optional(CONF_REASSERT_POWER_LIMIT, default=True): cv.boolean,
        cv.Optional(CONF_REGISTERS, default=[]): cv.ensure_list(RULE_SCHEMA),
    }
)


def _unique_devices(devs):
    seen = set()
    for d in devs:
        if d[CONF_DEVICE] in seen:
            raise cv.Invalid(f"Duplicate sungrow_control device {d[CONF_DEVICE]}")
        seen.add(d[CONF_DEVICE])
    return devs


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SungrowControl),
        cv.GenerateID(CONF_SUNGROW_MODBUS_ID): cv.use_id(SungrowModbus),
        cv.GenerateID(CONF_SUNGROW_DATAMODEL_ID): cv.use_id(SungrowDataModel),
        cv.Optional(CONF_VERIFY, default=True): cv.boolean,
        # Keep the inverters' clocks set from this time source (not gated by writes_enabled).
        cv.Optional(CONF_CLOCK_SYNC_TIME_ID): cv.use_id(es_time.RealTimeClock),
        cv.Required(CONF_WRITES_ENABLED): switch.switch_schema(
            WritesEnabledSwitch,
            default_restore_mode="RESTORE_DEFAULT_OFF",
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:pencil-lock",
        ),
        cv.Optional(CONF_LAST_WRITE): es_text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:pencil-circle-outline"
        ),
        cv.Optional(CONF_WRITES_OK): es_sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:pencil-plus",
        ),
        cv.Optional(CONF_WRITES_FAILED): es_sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:pencil-remove",
        ),
        cv.Optional(CONF_DEVICES, default=[]): cv.All(cv.ensure_list(DEVICE_SCHEMA), _unique_devices),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    hub = await cg.get_variable(config[CONF_SUNGROW_MODBUS_ID])
    cg.add(var.set_hub(hub))
    dm = await cg.get_variable(config[CONF_SUNGROW_DATAMODEL_ID])
    cg.add(var.set_datamodel(dm))
    cg.add(var.set_default_verify(config[CONF_VERIFY]))
    if CONF_CLOCK_SYNC_TIME_ID in config:
        clk = await cg.get_variable(config[CONF_CLOCK_SYNC_TIME_ID])
        cg.add(var.set_clock_source(clk))
    sw = await switch.new_switch(config[CONF_WRITES_ENABLED])
    await cg.register_parented(sw, var)
    cg.add(var.set_enable_switch(sw))
    if CONF_LAST_WRITE in config:
        s = await es_text_sensor.new_text_sensor(config[CONF_LAST_WRITE])
        cg.add(var.set_last_write_sensor(s))
    if CONF_WRITES_OK in config:
        s = await es_sensor.new_sensor(config[CONF_WRITES_OK])
        cg.add(var.set_writes_ok_sensor(s))
    if CONF_WRITES_FAILED in config:
        s = await es_sensor.new_sensor(config[CONF_WRITES_FAILED])
        cg.add(var.set_writes_failed_sensor(s))
    for d in config[CONF_DEVICES]:
        cg.add(var.add_device(d[CONF_DEVICE], d[CONF_PROFILE], d[CONF_READBACK], d[CONF_REASSERT_POWER_LIMIT]))
        for r in d[CONF_REGISTERS]:
            cg.add(var.add_rule(r[CONF_ADDRESS], r[CONF_COUNT], r[CONF_MIN], r[CONF_MAX], r[CONF_SIGNED]))


# ---- shared helpers for the entity platforms ------------------------------------------------

def key_meta(value):
    value = cv.string_strict(value)
    if value not in KEYS:
        raise cv.Invalid(f"Unknown sungrow_control key '{value}'; one of: {', '.join(sorted(KEYS))}")
    return value


ENTITY_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_SUNGROW_CONTROL_ID): cv.use_id(SungrowControl),
        cv.GenerateID(CONF_SUNGROW_DATAMODEL_ID): cv.use_id(SungrowDataModel),
        cv.Required(CONF_KEY): key_meta,
        cv.Optional(CONF_DEVICE, default=0): cv.int_range(min=0, max=255),
    }
)


async def setup_readback(var, config):
    """Bind a DmBinding-based entity to the key's read-back signal."""
    dm = await cg.get_variable(config[CONF_SUNGROW_DATAMODEL_ID])
    meta = KEYS[config[CONF_KEY]]
    cg.add(var.set_signal(config[CONF_DEVICE], meta["sigid"]))
    cg.add(var.set_throttle(0))
    cg.add(var.attach(dm))


# ---- action -----------------------------------------------------------------------------------

WRITE_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(SungrowControl),
        cv.Optional(CONF_DEVICE, default=0): cv.templatable(cv.int_range(min=0, max=255)),
        cv.Required(CONF_ADDRESS): cv.templatable(cv.int_range(min=1, max=65535)),
        cv.Required(CONF_VALUES): cv.templatable(
            cv.All(cv.ensure_list(cv.int_range(min=-32768, max=65535)), cv.Length(min=1, max=123))
        ),
        cv.Optional(CONF_VERIFY): cv.templatable(cv.boolean),
    }
)


@automation.register_action(
    "sungrow_control.write_registers", WriteRegistersAction, WRITE_ACTION_SCHEMA, synchronous=False
)
async def write_registers_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    tmpl = await cg.templatable(config[CONF_DEVICE], args, cg.int_)
    cg.add(var.set_device(tmpl))
    tmpl = await cg.templatable(config[CONF_ADDRESS], args, cg.int_)
    cg.add(var.set_address(tmpl))
    vals = config[CONF_VALUES]
    if isinstance(vals, list):
        vals = cg.RawExpression("std::vector<int32_t>{%s}" % ", ".join(str(int(v)) for v in vals))
        tmpl = await cg.templatable(vals, args, cg.std_vector.template(cg.int32))
    else:
        tmpl = await cg.templatable(vals, args, cg.std_vector.template(cg.int32))
    cg.add(var.set_values(tmpl))
    if CONF_VERIFY in config:
        tmpl = await cg.templatable(config[CONF_VERIFY], args, cg.bool_)
        cg.add(var.set_verify(tmpl))
    return var
