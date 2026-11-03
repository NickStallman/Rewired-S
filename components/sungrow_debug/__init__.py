"""sungrow_debug: bring-up / debug tooling for the WiNet debug build (common/debug.yaml).

Never part of the normal builds. Everything goes through public APIs of the other components
(see sungrow_debug.h); the only hook it enables elsewhere is SungrowModbusTcp::set_frame_observer(),
compiled in by the USE_SUNGROW_DEBUG define set here.

    sungrow_debug:
      id: dbg
      sungrow_modbus_id: rs485_hub
      sungrow_datamodel_id: sungrow_dm
      sungrow_modbus_tcp_id: modbus_tcp_server   # optional: :502 frame trace
      sungrow_extflash_id: extflash              # optional: dump_file / list_files
      block_rtu_writes: true       # SAFETY: only fn 3/4 frames reach the RS485 bus
      rtu_trace: true              # initial states of the runtime switches
      decode_trace: true
      only_changed: true
      tcp_trace: true
      trace_addresses: ""          # "" = all, e.g. "1,3-5"
      trace_registers: ""          # "" = all, e.g. "5000-5100,13000+50" (1-based)
      trace_min_interval: 0ms      # per (address, fn, start) node
      lines_per_loop: 6
      stats_interval: 10s
      log_server_port: 0           # >0: plain TCP log stream (nc <ip> <port>), LAN only
      flash_dump: true             # /dump/<region> on the web server (READ-ONLY, behind its auth)
      last_result: {name: ...}     # text sensors / switches / per-device sensors, all optional
      devices:
        - address: 1
          requests: {name: ...}
"""

import esphome.codegen as cg
from esphome.components import logger, sensor, socket, switch, text_sensor
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_MILLISECOND,
    UNIT_PERCENT,
)

from esphome.components.sungrow_datamodel import CONF_SUNGROW_DATAMODEL_ID, SungrowDataModel
from esphome.components.sungrow_modbus import SungrowModbus

CODEOWNERS = []
DEPENDENCIES = ["sungrow_modbus", "sungrow_datamodel"]
AUTO_LOAD = ["sensor", "text_sensor", "switch"]

sungrow_debug_ns = cg.esphome_ns.namespace("sungrow_debug")
SungrowDebug = sungrow_debug_ns.class_("SungrowDebug", cg.Component)
DebugSwitch = sungrow_debug_ns.class_("DebugSwitch", switch.Switch)
SwitchKind = sungrow_debug_ns.enum("SwitchKind", is_class=True)

CONF_SUNGROW_MODBUS_ID = "sungrow_modbus_id"
CONF_SUNGROW_MODBUS_TCP_ID = "sungrow_modbus_tcp_id"
CONF_SUNGROW_EXTFLASH_ID = "sungrow_extflash_id"
CONF_BLOCK_RTU_WRITES = "block_rtu_writes"
CONF_RTU_TRACE = "rtu_trace"
CONF_DECODE_TRACE = "decode_trace"
CONF_ONLY_CHANGED = "only_changed"
CONF_TCP_TRACE = "tcp_trace"
CONF_TRACE_ADDRESSES = "trace_addresses"
CONF_TRACE_REGISTERS = "trace_registers"
CONF_TRACE_MIN_INTERVAL = "trace_min_interval"
CONF_LINES_PER_LOOP = "lines_per_loop"
CONF_STATS_INTERVAL = "stats_interval"
CONF_LOG_SERVER_PORT = "log_server_port"
CONF_FLASH_DUMP = "flash_dump"
CONF_LAST_RESULT = "last_result"
CONF_SCAN_RESULT = "scan_result"
CONF_PARTITION = "partition"
CONF_IDF_VERSION = "idf_version"
CONF_TRACE_FILTER = "trace_filter"
CONF_SWITCHES = "switches"
CONF_ALLOW_BOOT_SWITCH = "allow_boot_switch"
CONF_DEVICES = "devices"

SWITCH_KINDS = {
    CONF_RTU_TRACE: ("RTU_TRACE", "mdi:swap-horizontal"),
    CONF_DECODE_TRACE: ("DECODE_TRACE", "mdi:table-search"),
    CONF_ONLY_CHANGED: ("ONLY_CHANGED", "mdi:filter-variant"),
    CONF_TCP_TRACE: ("TCP_TRACE", "mdi:lan"),
    # Arms the one flash write of the debug build (otadata boot switch). Never restored.
    CONF_ALLOW_BOOT_SWITCH: ("ALLOW_BOOT_SWITCH", "mdi:alert"),
}

# (key, StatKind index, unit, decimals, state class, icon)
STATS = [
    ("requests", 0, None, 0, STATE_CLASS_TOTAL_INCREASING, "mdi:upload"),
    ("responses", 1, None, 0, STATE_CLASS_TOTAL_INCREASING, "mdi:download"),
    ("timeouts", 2, None, 0, STATE_CLASS_TOTAL_INCREASING, "mdi:timer-alert-outline"),
    ("crc_errors", 3, None, 0, STATE_CLASS_TOTAL_INCREASING, "mdi:alert-octagon-outline"),
    ("exceptions", 4, None, 0, STATE_CLASS_TOTAL_INCREASING, "mdi:alert-circle-outline"),
    ("latency_avg", 5, UNIT_MILLISECOND, 0, STATE_CLASS_MEASUREMENT, "mdi:timer-outline"),
    ("latency_max", 6, UNIT_MILLISECOND, 0, STATE_CLASS_MEASUREMENT, "mdi:timer-sand"),
    # ms since the device's most frequently polled read last answered (published every stats period)
    ("data_age", 7, UNIT_MILLISECOND, 0, STATE_CLASS_MEASUREMENT, "mdi:sync"),
    ("packet_loss", 8, UNIT_PERCENT, 1, STATE_CLASS_MEASUREMENT, "mdi:lan-disconnect"),
]

DEVICE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ADDRESS): cv.int_range(min=1, max=247),
        **{
            cv.Optional(key): sensor.sensor_schema(
                **({"unit_of_measurement": unit} if unit else {}),
                accuracy_decimals=dec,
                state_class=sc,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon=icon,
            )
            for key, _, unit, dec, sc, icon in STATS
        },
    }
)


def _ranges(lo, hi):
    def validate(value):
        value = cv.string_strict(value)
        s = value.strip()
        if s in ("", "*", "all"):
            return value
        for part in s.split(","):
            part = part.strip()
            try:
                if "+" in part:
                    a, n = part.split("+")
                    a, b = int(a, 0), int(a, 0) + int(n, 0) - 1
                elif "-" in part:
                    a, b = (int(x, 0) for x in part.split("-"))
                else:
                    a = b = int(part, 0)
            except ValueError as e:
                raise cv.Invalid(f"bad range '{part}'") from e
            if a > b or a < lo or b > hi:
                raise cv.Invalid(f"range '{part}' outside {lo}..{hi}")
        return value

    return validate


def _text(icon):
    return text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon=icon)


def _consume_sockets(config):
    if config[CONF_LOG_SERVER_PORT]:
        socket.consume_sockets(2, "sungrow_debug")(config)
        socket.consume_sockets(1, "sungrow_debug", socket.SocketType.TCP_LISTEN)(config)
    return config


def _validate(config):
    if config[CONF_FLASH_DUMP]:
        from esphome.core import CORE

        if "web_server_base" not in CORE.loaded_integrations and "web_server" not in CORE.loaded_integrations:
            raise cv.Invalid("flash_dump needs web_server (the /dump handler lives on it, behind its auth)")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SungrowDebug),
            cv.GenerateID(CONF_SUNGROW_MODBUS_ID): cv.use_id(SungrowModbus),
            cv.GenerateID(CONF_SUNGROW_DATAMODEL_ID): cv.use_id(SungrowDataModel),
            cv.Optional(CONF_SUNGROW_MODBUS_TCP_ID): cv.use_id(cg.esphome_ns.namespace("sungrow_modbus_tcp").class_("SungrowModbusTcp")),
            cv.Optional(CONF_SUNGROW_EXTFLASH_ID): cv.use_id(cg.esphome_ns.namespace("sungrow_extflash").class_("SungrowExtFlash")),
            cv.Optional(CONF_BLOCK_RTU_WRITES, default=True): cv.boolean,
            cv.Optional(CONF_RTU_TRACE, default=True): cv.boolean,
            cv.Optional(CONF_DECODE_TRACE, default=True): cv.boolean,
            cv.Optional(CONF_ONLY_CHANGED, default=True): cv.boolean,
            cv.Optional(CONF_TCP_TRACE, default=True): cv.boolean,
            cv.Optional(CONF_TRACE_ADDRESSES, default=""): _ranges(1, 247),
            cv.Optional(CONF_TRACE_REGISTERS, default=""): _ranges(1, 65535),
            cv.Optional(CONF_TRACE_MIN_INTERVAL, default="0ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_LINES_PER_LOOP, default=6): cv.int_range(min=1, max=50),
            cv.Optional(CONF_STATS_INTERVAL, default="10s"): cv.positive_not_null_time_period,
            cv.Optional(CONF_LOG_SERVER_PORT, default=0): cv.Any(cv.one_of(0, int=True), cv.port),
            cv.Optional(CONF_FLASH_DUMP, default=False): cv.boolean,
            cv.Optional(CONF_LAST_RESULT): _text("mdi:console"),
            cv.Optional(CONF_SCAN_RESULT): _text("mdi:radar"),
            cv.Optional(CONF_PARTITION): _text("mdi:harddisk"),
            cv.Optional(CONF_IDF_VERSION): _text("mdi:information-outline"),
            cv.Optional(CONF_TRACE_FILTER): _text("mdi:filter"),
            cv.Optional(CONF_SWITCHES, default={}): cv.Schema(
                {
                    cv.Optional(key): switch.switch_schema(
                        DebugSwitch,
                        # The boot switch must never come back on by itself.
                        default_restore_mode="ALWAYS_OFF" if key == CONF_ALLOW_BOOT_SWITCH else "DISABLED",
                        entity_category=ENTITY_CATEGORY_CONFIG,
                        icon=icon,
                    )
                    for key, (_, icon) in SWITCH_KINDS.items()
                }
            ),
            cv.Optional(CONF_DEVICES, default=[]): cv.ensure_list(DEVICE_SCHEMA),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _consume_sockets,
)

FINAL_VALIDATE_SCHEMA = _validate


async def to_code(config):
    cg.add_define("USE_SUNGROW_DEBUG")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    hub = await cg.get_variable(config[CONF_SUNGROW_MODBUS_ID])
    cg.add(var.set_hub(hub))
    dm = await cg.get_variable(config[CONF_SUNGROW_DATAMODEL_ID])
    cg.add(var.set_datamodel(dm))
    if CONF_SUNGROW_MODBUS_TCP_ID in config:
        cg.add_define("USE_SUNGROW_DEBUG_TCP")
        tcp = await cg.get_variable(config[CONF_SUNGROW_MODBUS_TCP_ID])
        cg.add(var.set_tcp(tcp))
    if CONF_SUNGROW_EXTFLASH_ID in config:
        cg.add_define("USE_SUNGROW_DEBUG_EXTFLASH")
        ef = await cg.get_variable(config[CONF_SUNGROW_EXTFLASH_ID])
        cg.add(var.set_extflash(ef))
    cg.add(var.set_block_rtu_writes(config[CONF_BLOCK_RTU_WRITES]))
    cg.add(var.set_rtu_trace(config[CONF_RTU_TRACE]))
    cg.add(var.set_decode_trace(config[CONF_DECODE_TRACE]))
    cg.add(var.set_only_changed(config[CONF_ONLY_CHANGED]))
    cg.add(var.set_tcp_trace(config[CONF_TCP_TRACE]))
    cg.add(var.set_initial_filter(config[CONF_TRACE_ADDRESSES], config[CONF_TRACE_REGISTERS]))
    cg.add(var.set_trace_min_interval(int(config[CONF_TRACE_MIN_INTERVAL].total_milliseconds)))
    cg.add(var.set_lines_per_loop(config[CONF_LINES_PER_LOOP]))
    cg.add(var.set_stats_interval(int(config[CONF_STATS_INTERVAL].total_milliseconds)))
    if config[CONF_LOG_SERVER_PORT]:
        cg.add_define("USE_SUNGROW_DEBUG_LOG_SERVER")
        logger.request_log_listener()
        cg.add(var.set_log_server_port(config[CONF_LOG_SERVER_PORT]))
    if config[CONF_FLASH_DUMP]:
        cg.add_define("USE_SUNGROW_DEBUG_DUMP")

    for key, setter in (
        (CONF_LAST_RESULT, var.set_last_result_sensor),
        (CONF_SCAN_RESULT, var.set_scan_result_sensor),
        (CONF_PARTITION, var.set_partition_sensor),
        (CONF_IDF_VERSION, var.set_idf_version_sensor),
        (CONF_TRACE_FILTER, var.set_filter_sensor),
    ):
        if key in config:
            s = await text_sensor.new_text_sensor(config[key])
            cg.add(setter(s))

    for key, conf in config[CONF_SWITCHES].items():
        sw = await switch.new_switch(conf)
        await cg.register_parented(sw, var)
        cg.add(sw.set_kind(getattr(SwitchKind, SWITCH_KINDS[key][0])))
        cg.add(var.add_switch(sw))

    for dev in config[CONF_DEVICES]:
        cg.add(var.add_stats_device(dev[CONF_ADDRESS]))
        for key, kind, *_ in STATS:
            if key in dev:
                s = await sensor.new_sensor(dev[key])
                cg.add(var.set_stat_sensor(kind, s))
