"""sungrow_modbus_tcp: Modbus-TCP slave re-exporting the signal store like the stock WiNet dongle.

YAML (see common/modbus_tcp.yaml):

    sungrow_modbus_tcp:
      id: modbus_tcp_server
      sungrow_datamodel_id: sungrow_dm
      sungrow_modbus_id: rs485_hub   # optional: auto unit map, dev_type from discovery, fn 3 live reads
      port: 502
      max_clients: 4
      idle_timeout: 120s             # 0s = never
      word_order: low_word_first     # u32/s32 low word in the first register (Sungrow docs)
      unknown_unit: exception        # exception (0x0B) | ignore (stock: no reply)
      passthrough: true              # fn 3 with unmapped registers -> live RS485 read (stock)
      poll_fallback: false           # serve the poll-table sigid at the same address (non-stock)
      allowed_ips: [192.0.2.10, 192.0.2.0/24]   # empty = everyone (stock whitelist)
      sungrow_control_id: sungrow_ctl   # optional: fn 6/16 writes gated by sungrow_control
                                        # (without it, writes answer exception 1)
      write_allowed_ips: [192.0.2.10]   # optional: who may write (empty = any connected client)
      units:                         # optional; default: unit id = stock DM device id (inverter 1, SBR 2, 3 ...)
        - unit: 1
          device: 0                  # SignalStore device index
          dev_type: auto             # auto | 1 | 21 | 35 | 18 | 44 (selects the tcpslave map)
          # address: 1               # RS485 address for live reads (default: the hub device's)
      enabled: {name: Modbus-TCP server}           # restored switch; until the user toggles it,
                                                   # the inverter's country decides (stock: on for
                                                   # 6, 99, 114..117); ALWAYS_ON/OFF pins the state
      clients: {name: Modbus-TCP clients}
      requests: {name: Modbus-TCP requests}
      errors: {name: Modbus-TCP errors}
"""

import ipaddress

import esphome.codegen as cg
from esphome.components import sensor, socket, switch
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    CONF_PORT,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)

from esphome.components.sungrow_datamodel import CONF_SUNGROW_DATAMODEL_ID, SungrowDataModel

CODEOWNERS = []
DEPENDENCIES = ["network", "sungrow_datamodel"]
AUTO_LOAD = ["socket", "sensor", "switch"]

ns = cg.esphome_ns.namespace("sungrow_modbus_tcp")
SungrowModbusTcp = ns.class_("SungrowModbusTcp", cg.Component)
EnableSwitch = ns.class_("EnableSwitch", switch.Switch, cg.Parented.template(SungrowModbusTcp))

sungrow_modbus_ns = cg.esphome_ns.namespace("sungrow_modbus")
SungrowModbus = sungrow_modbus_ns.class_("SungrowModbus", cg.Component)
SungrowControl = cg.esphome_ns.namespace("sungrow_control").class_("SungrowControl", cg.Component)

CONF_SUNGROW_MODBUS_ID = "sungrow_modbus_id"
CONF_MAX_CLIENTS = "max_clients"
CONF_IDLE_TIMEOUT = "idle_timeout"
CONF_STATS_INTERVAL = "stats_interval"
CONF_WORD_ORDER = "word_order"
CONF_UNKNOWN_UNIT = "unknown_unit"
CONF_PASSTHROUGH = "passthrough"
CONF_POLL_FALLBACK = "poll_fallback"
CONF_ALLOWED_IPS = "allowed_ips"
CONF_WRITE_ALLOWED_IPS = "write_allowed_ips"
CONF_SUNGROW_CONTROL_ID = "sungrow_control_id"
CONF_UNITS = "units"
CONF_UNIT = "unit"
CONF_DEVICE = "device"
CONF_DEV_TYPE = "dev_type"
CONF_ENABLED = "enabled"
CONF_CLIENTS = "clients"
CONF_REQUESTS = "requests"
CONF_ERRORS = "errors"

WORD_ORDERS = {"high_word_first": 0, "low_word_first": 2}
DEV_TYPES = [1, 21, 35, 18, 44]


def _dev_type(value):
    if isinstance(value, str) and value.lower() == "auto":
        return -1
    value = cv.int_(value)
    if value not in DEV_TYPES:
        raise cv.Invalid(f"dev_type must be auto or one of {DEV_TYPES}")
    return value


def _ip_rule(value):
    value = cv.string_strict(value)
    try:
        net = ipaddress.IPv4Network(value, strict=False)
    except ValueError as e:
        raise cv.Invalid(f"Invalid IPv4 address or network: {value}") from e
    return str(net)


UNIT_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_UNIT): cv.int_range(min=0, max=255),
        cv.Required(CONF_DEVICE): cv.int_range(min=0, max=255),
        cv.Optional(CONF_DEV_TYPE, default="auto"): _dev_type,
        cv.Optional(CONF_ADDRESS, default=0): cv.int_range(min=0, max=247),
    }
)


def _unique_units(units):
    seen = set()
    for u in units:
        if u[CONF_UNIT] in seen:
            raise cv.Invalid(f"Duplicate unit id {u[CONF_UNIT]}")
        seen.add(u[CONF_UNIT])
    return units


def _validate(config):
    if config[CONF_WRITE_ALLOWED_IPS] and CONF_SUNGROW_CONTROL_ID not in config:
        raise cv.Invalid(f"{CONF_WRITE_ALLOWED_IPS} needs {CONF_SUNGROW_CONTROL_ID}")
    if CONF_SUNGROW_MODBUS_ID not in config:
        if not config[CONF_UNITS]:
            raise cv.Invalid(f"Without {CONF_SUNGROW_MODBUS_ID}, list the {CONF_UNITS} explicitly")
        for u in config[CONF_UNITS]:
            if u[CONF_DEV_TYPE] < 0:
                raise cv.Invalid(f"Without {CONF_SUNGROW_MODBUS_ID}, every unit needs an explicit dev_type")
    return config


def _consume_sockets(config):
    socket.consume_sockets(config[CONF_MAX_CLIENTS], "sungrow_modbus_tcp")(config)
    socket.consume_sockets(1, "sungrow_modbus_tcp", socket.SocketType.TCP_LISTEN)(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SungrowModbusTcp),
            cv.GenerateID(CONF_SUNGROW_DATAMODEL_ID): cv.use_id(SungrowDataModel),
            cv.Optional(CONF_SUNGROW_MODBUS_ID): cv.use_id(SungrowModbus),
            cv.Optional(CONF_PORT, default=502): cv.port,
            cv.Optional(CONF_MAX_CLIENTS, default=4): cv.int_range(min=1, max=8),
            cv.Optional(CONF_IDLE_TIMEOUT, default="120s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_STATS_INTERVAL, default="30s"): cv.positive_not_null_time_period,
            cv.Optional(CONF_WORD_ORDER, default="low_word_first"): cv.enum(WORD_ORDERS, lower=True),
            cv.Optional(CONF_UNKNOWN_UNIT, default="exception"): cv.one_of("exception", "ignore", lower=True),
            cv.Optional(CONF_PASSTHROUGH, default=True): cv.boolean,
            cv.Optional(CONF_POLL_FALLBACK, default=False): cv.boolean,
            cv.Optional(CONF_ALLOWED_IPS, default=[]): cv.ensure_list(_ip_rule),
            cv.Optional(CONF_SUNGROW_CONTROL_ID): cv.use_id(SungrowControl),
            cv.Optional(CONF_WRITE_ALLOWED_IPS, default=[]): cv.ensure_list(_ip_rule),
            cv.Optional(CONF_UNITS, default=[]): cv.All(cv.ensure_list(UNIT_SCHEMA), _unique_units),
            cv.Optional(CONF_ENABLED): switch.switch_schema(
                EnableSwitch,
                default_restore_mode="RESTORE_DEFAULT_ON",
                entity_category=ENTITY_CATEGORY_CONFIG,
                icon="mdi:lan",
            ),
            cv.Optional(CONF_CLIENTS): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:lan-connect",
            ),
            cv.Optional(CONF_REQUESTS): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:swap-horizontal",
            ),
            cv.Optional(CONF_ERRORS): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:alert-circle-outline",
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
    _consume_sockets,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    dm = await cg.get_variable(config[CONF_SUNGROW_DATAMODEL_ID])
    cg.add(var.set_datamodel(dm))
    if CONF_SUNGROW_MODBUS_ID in config:
        hub = await cg.get_variable(config[CONF_SUNGROW_MODBUS_ID])
        cg.add_define("USE_SUNGROW_MODBUS_TCP_HUB")
        cg.add(var.set_hub(hub))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_max_clients(config[CONF_MAX_CLIENTS]))
    cg.add(var.set_idle_timeout(int(config[CONF_IDLE_TIMEOUT].total_milliseconds)))
    cg.add(var.set_stats_interval(int(config[CONF_STATS_INTERVAL].total_milliseconds)))
    cg.add(var.set_word_order(config[CONF_WORD_ORDER]))
    cg.add(var.set_unknown_unit_exception(config[CONF_UNKNOWN_UNIT] == "exception"))
    cg.add(var.set_passthrough(config[CONF_PASSTHROUGH]))
    cg.add(var.set_poll_fallback(config[CONF_POLL_FALLBACK]))
    for rule in config[CONF_ALLOWED_IPS]:
        net = ipaddress.IPv4Network(rule)
        cg.add(var.add_allowed_ip(int(net.network_address), int(net.netmask)))
    if CONF_SUNGROW_CONTROL_ID in config:
        ctl = await cg.get_variable(config[CONF_SUNGROW_CONTROL_ID])
        cg.add_define("USE_SUNGROW_MODBUS_TCP_CONTROL")
        cg.add(var.set_control(ctl))
    for rule in config[CONF_WRITE_ALLOWED_IPS]:
        net = ipaddress.IPv4Network(rule)
        cg.add(var.add_write_allowed_ip(int(net.network_address), int(net.netmask)))
    for u in config[CONF_UNITS]:
        cg.add(var.add_unit(u[CONF_UNIT], u[CONF_DEVICE], u[CONF_DEV_TYPE], u[CONF_ADDRESS]))
    if CONF_ENABLED in config:
        sw = await switch.new_switch(config[CONF_ENABLED])
        await cg.register_parented(sw, var)
        cg.add(var.set_enable_switch(sw))
    for key, setter in ((CONF_CLIENTS, "set_clients_sensor"), (CONF_REQUESTS, "set_requests_sensor"),
                        (CONF_ERRORS, "set_errors_sensor")):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(getattr(var, setter)(s))
