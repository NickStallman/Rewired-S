"""sungrow_modbus: RS485 Modbus-RTU master hub that polls Sungrow devices into sungrow_datamodel.

YAML (see common/rs485.yaml):

    uart:
      id: rs485_uart
      tx_pin: ...
      rx_pin: ...
      flow_control_pin: ...     # DE on RTS: ESPHome puts the IDF UART into RS485 half-duplex
      baud_rate: 9600
      rx_buffer_size: 512       # a 120-register reply is 245 B

    sungrow_modbus:
      id: rs485_hub
      uart_id: rs485_uart
      sungrow_datamodel_id: sungrow_dm
      # de_pin: GPIOx           # software DE fallback when the uart has no flow_control_pin
      frame_gap: 18ms           # before every frame (>= 3.5 chars); stock nSendFrameSpace 0 -> 18 ms
      frame_gap_fast: 1ms       # battery / charger / energy bridge devices (stock default 1)
      poll_periods: {fast: 1s, live: 5s, slow: 30s, static: 5min}   # refresh tiers (poll_plan.h)
      poll_timeout_margin: 200ms  # cyclic reads time out after their wire time + this
      response_timeout: 1000ms  # discovery, inverter type, ad-hoc and verify reads
      tunnel_timeout: 2000ms    # reads the inverter forwards to its DSP / the BCU
      retries: 0                # stock sends a Modbus request once
      offline_after: 10         # consecutive lost frames ...
      offline_silence: 10s      # ... spanning at least this long -> offline
      write_timeout: 2000ms     # writes (sungrow_control): reply timeout
      write_settle: 100ms       # bus quiet after a write (stock: none; ours, before the read-back)
      write_rate_limit: 1s      # min time between writes to the same register (0s = off)
      devices:
        - index: 0              # SignalStore device index = `device:` of the entities
          address: 1            # Modbus slave address
          table: auto           # discovery (devinfo 2515+66) or a table name
          fallback_table: inv_proto1_4   # default: devinfo rejected -> protocol 1 (as stock)
          # dev_type: 35        # optional stock dev_type; default: devinfo, else the table family
          online: {name: Inverter RS485 online}
          packet_loss: {name: Inverter RS485 packet loss}
          dev_code: {name: Inverter dev code}

Device identity: a discovered device takes dev_type and protocol type from the devinfo block and
then reads holding 30644 (inverter type). A fixed-table or fallback device takes dev_type from
`dev_type:` or the table family (inverter 1, battery 44, meter 18, logger 34, charger 46,
energy bridge 92) and, for an inverter table, the protocol type the table stands for.

Refresh tiers (poll_plan.h, docs/update_rates.md): every register is read at one of four
periods: fast (power flow: PV, grid, load, battery), live (states, faults, energies), slow
(settings, battery status, cloud-only values) and static (identity, versions, ratings). Reads of
one tier are spread over its period; reads that return no values drop to the static period.
"""

import esphome.codegen as cg
from esphome import pins
from esphome.components import binary_sensor, sensor, text_sensor, uart
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_ID,
    CONF_INDEX,
    DEVICE_CLASS_CONNECTIVITY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    ICON_CHIP,
    STATE_CLASS_MEASUREMENT,
    UNIT_PERCENT,
)

from esphome.components.sungrow_datamodel import CONF_SUNGROW_DATAMODEL_ID, SungrowDataModel

CODEOWNERS = []
DEPENDENCIES = ["uart", "sungrow_datamodel"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor"]

sungrow_modbus_ns = cg.esphome_ns.namespace("sungrow_modbus")
SungrowModbus = sungrow_modbus_ns.class_("SungrowModbus", cg.Component, uart.UARTDevice)

CONF_DE_PIN = "de_pin"
CONF_FRAME_GAP = "frame_gap"
CONF_FRAME_GAP_FAST = "frame_gap_fast"
CONF_POLL_PERIODS = "poll_periods"
CONF_POLL_TIMEOUT_MARGIN = "poll_timeout_margin"
CONF_TUNNEL_TIMEOUT = "tunnel_timeout"
CONF_OFFLINE_SILENCE = "offline_silence"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_RETRIES = "retries"
CONF_OFFLINE_AFTER = "offline_after"
CONF_OFFLINE_PROBE_INTERVAL = "offline_probe_interval"
CONF_STATS_INTERVAL = "stats_interval"
CONF_WRITE_TIMEOUT = "write_timeout"
CONF_WRITE_SETTLE = "write_settle"
CONF_WRITE_RATE_LIMIT = "write_rate_limit"
CONF_SCAN_RANGE = "scan_range"
CONF_INVERTER_SEARCH = "inverter_search"
CONF_SCAN_ATTEMPTS = "scan_attempts"
CONF_CHILD_DISCOVERY = "child_discovery"
CONF_FAST_LOOP = "fast_loop"
CONF_SCAN_FALLBACK_TABLE = "scan_fallback_table"
CONF_DEVICES = "devices"
CONF_TABLE = "table"
CONF_FALLBACK_TABLE = "fallback_table"
CONF_WORD_ORDER = "word_order"
CONF_GATE_CAPABILITIES = "gate_capabilities"
CONF_SBR_CELLS = "sbr_cells"
CONF_SBR_INTERVAL = "sbr_interval"
CONF_ONLINE = "online"
CONF_PACKET_LOSS = "packet_loss"
CONF_DEV_CODE = "dev_code"
CONF_DEV_TYPE = "dev_type"
CONF_DEV_TYPE_NAME = "dev_type_name"
CONF_PROTOCOL = "protocol"
CONF_TABLE_SENSOR = "table_name"

# Poll tables (register_tables.h names; tcpslave_* are Modbus-TCP slave maps, not poll tables).
POLL_TABLES = [
    "inv_proto1_4",
    "inv_proto2",
    "inv_proto3",
    "inv_proto5",
    "battery",
    "charger",
    "energy_bridge",
    "other_dev18_st6100",
    "other_dev18_st6200",
    "other_dev18_st6300",
    "other_dev34_st1819",
    "other_dev34_st1820",
]

# Stock dev_types a device can have (from the devinfo classification and the child probes).
DEV_TYPES = [1, 21, 35, 18, 34, 44, 46, 92]

WORD_ORDERS = {"high_word_first": 0, "raw": 1, "low_word_first": 2, "unswapped": 3}

POLL_TIERS = ("fast", "live", "slow", "static")
POLL_PERIODS_SCHEMA = cv.Schema(
    {
        cv.Optional("fast", default="1s"): cv.positive_time_period_milliseconds,
        cv.Optional("live", default="5s"): cv.positive_time_period_milliseconds,
        cv.Optional("slow", default="30s"): cv.positive_time_period_milliseconds,
        cv.Optional("static", default="5min"): cv.positive_time_period_milliseconds,
    }
)
PollTier = sungrow_modbus_ns.enum("PollTier", is_class=True)


def _gate(value):
    if isinstance(value, str) and value.lower() == "auto":
        return -1
    return 1 if cv.boolean(value) else 0


def _device_defaults(config):
    config = dict(config)
    if CONF_SBR_CELLS not in config:
        # Only SBR stacks (dev_type 44) carry the cell block; a `battery` table device is
        # treated as one.
        config[CONF_SBR_CELLS] = config[CONF_TABLE] == "battery"
    # Stock runs a device that rejects the devinfo read as protocol 1 (inv_proto1_4).
    if config[CONF_TABLE] == "auto" and CONF_FALLBACK_TABLE not in config:
        config[CONF_FALLBACK_TABLE] = "inv_proto1_4"
    return config


DEVICE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_INDEX): cv.int_range(min=0, max=255),
            cv.Optional(CONF_ADDRESS, default=1): cv.int_range(min=1, max=247),
            cv.Optional(CONF_TABLE, default="auto"): cv.one_of("auto", *POLL_TABLES, lower=True),
            # Table when the devinfo read is rejected (table: auto); default inv_proto1_4.
            cv.Optional(CONF_FALLBACK_TABLE): cv.one_of(*POLL_TABLES, lower=True),
            # Stock dev_type override; default: devinfo block, else the table family.
            cv.Optional(CONF_DEV_TYPE): cv.one_of(*DEV_TYPES, int=True),
            cv.Optional(CONF_WORD_ORDER, default="low_word_first"): cv.enum(WORD_ORDERS, lower=True),
            cv.Optional(CONF_GATE_CAPABILITIES, default="auto"): _gate,
            cv.Optional(CONF_SBR_CELLS): cv.boolean,
            # SBR cell block (whole-block read, slow tier).
            cv.Optional(CONF_SBR_INTERVAL, default="30s"): cv.positive_time_period_milliseconds,
            # default: frame_gap / frame_gap_fast by the table family
            cv.Optional(CONF_FRAME_GAP): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ONLINE): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PACKET_LOSS): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:lan-disconnect",
            ),
            cv.Optional(CONF_DEV_CODE): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon=ICON_CHIP
            ),
            # "<dev_type> (<label>, <source>), family <xml family>"
            cv.Optional(CONF_DEV_TYPE_NAME): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon=ICON_CHIP
            ),
            cv.Optional(CONF_PROTOCOL): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon=ICON_CHIP
            ),
            cv.Optional(CONF_TABLE_SENSOR): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon=ICON_CHIP
            ),
        }
    ),
    _device_defaults,
)


def _unique_devices(devices):
    seen_idx, seen_addr = set(), set()
    for d in devices:
        if d[CONF_INDEX] in seen_idx:
            raise cv.Invalid(f"Duplicate device index {d[CONF_INDEX]}")
        if d[CONF_ADDRESS] in seen_addr:
            raise cv.Invalid(f"Duplicate Modbus address {d[CONF_ADDRESS]}")
        seen_idx.add(d[CONF_INDEX])
        seen_addr.add(d[CONF_ADDRESS])
    return devices


def _scan_range(value):
    """Parse "A-B" (inclusive 1-based Modbus addresses) into a (first, last) tuple."""
    value = cv.string_strict(value)
    parts = value.split("-")
    if len(parts) != 2:
        raise cv.Invalid('scan_range must be "A-B", e.g. "1-8"')
    try:
        first, last = int(parts[0]), int(parts[1])
    except ValueError as err:
        raise cv.Invalid('scan_range bounds must be integers, e.g. "1-8"') from err
    if first < 1:
        raise cv.Invalid("scan_range start must be >= 1")
    if last < first:
        raise cv.Invalid("scan_range end must be >= start")
    if last > 247:
        raise cv.Invalid("scan_range end must be <= 247")
    return (first, last)


def _require_devices_or_scan(config):
    if not config.get(CONF_DEVICES) and CONF_SCAN_RANGE not in config and not config[CONF_INVERTER_SEARCH]:
        raise cv.Invalid("At least one of `devices`, `inverter_search` or `scan_range` is required")
    return config


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SungrowModbus),
            cv.GenerateID(CONF_SUNGROW_DATAMODEL_ID): cv.use_id(SungrowDataModel),
            cv.Optional(CONF_DE_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_FRAME_GAP, default="18ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_FRAME_GAP_FAST, default="1ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_POLL_PERIODS, default={}): POLL_PERIODS_SCHEMA,
            cv.Optional(CONF_POLL_TIMEOUT_MARGIN, default="200ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_RESPONSE_TIMEOUT, default="1000ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_TUNNEL_TIMEOUT, default="2000ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_OFFLINE_SILENCE, default="10s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_RETRIES, default=0): cv.int_range(min=0, max=5),
            cv.Optional(CONF_OFFLINE_AFTER, default=10): cv.int_range(min=1, max=100),
            cv.Optional(CONF_OFFLINE_PROBE_INTERVAL, default="15s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_STATS_INTERVAL, default="30s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_WRITE_TIMEOUT, default="2000ms"): cv.positive_time_period_milliseconds,
            # Bus silence after every write. Differs from stock, which pauses 100 ms only after a
            # broadcast write.
            cv.Optional(CONF_WRITE_SETTLE, default="100ms"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_WRITE_RATE_LIMIT, default="1s"): cv.positive_time_period_milliseconds,
            # Stock inverter search: remembered addresses polled directly + one broadcast read of
            # 30035 at start / after an inverter goes offline (master.h).
            cv.Optional(CONF_INVERTER_SEARCH, default=True): cv.boolean,
            # Read-only range probe: the fallback, once per boot / rescan, when the search gets no
            # answer, no inverter is online and the remembered addresses are silent (at start
            # when inverter_search is off).
            cv.Optional(CONF_SCAN_RANGE): _scan_range,
            cv.Optional(CONF_SCAN_ATTEMPTS, default=2): cv.int_range(min=1, max=10),
            # Add the batteries / charger / Energy Bridge an inverter reports (10451.., 5621), as
            # stock does.
            cv.Optional(CONF_CHILD_DISCOVERY, default=True): cv.boolean,
            # Hold ESPHome's high-frequency loop while a Modbus transaction is pending (reply
            # awaited, retry, queued write / ad-hoc read, settle gap): the frame gap and the reply
            # detection stop being quantised to the ~16 ms main loop, at the price of CPU time.
            cv.Optional(CONF_FAST_LOOP, default=False): cv.boolean,
            # Table for a scanned device whose devinfo read is rejected (applies with scan_range);
            # default inv_proto1_4 (protocol 1, as stock).
            cv.Optional(CONF_SCAN_FALLBACK_TABLE): cv.one_of(*POLL_TABLES, lower=True),
            cv.Optional(CONF_DEVICES, default=[]): cv.All(cv.ensure_list(DEVICE_SCHEMA), _unique_devices),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _require_devices_or_scan)


def _ms(v):
    return int(v.total_milliseconds)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    dm = await cg.get_variable(config[CONF_SUNGROW_DATAMODEL_ID])
    cg.add(var.set_datamodel(dm))
    if CONF_DE_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_DE_PIN])
        cg.add(var.set_de_pin(pin))
    cg.add(var.set_frame_gap(_ms(config[CONF_FRAME_GAP])))
    cg.add(var.set_frame_gap_fast(_ms(config[CONF_FRAME_GAP_FAST])))
    for tier in POLL_TIERS:
        cg.add(var.set_tier_period(getattr(PollTier, tier.upper()), _ms(config[CONF_POLL_PERIODS][tier])))
    cg.add(var.set_poll_timeout_margin(_ms(config[CONF_POLL_TIMEOUT_MARGIN])))
    cg.add(var.set_response_timeout(_ms(config[CONF_RESPONSE_TIMEOUT])))
    cg.add(var.set_tunnel_timeout(_ms(config[CONF_TUNNEL_TIMEOUT])))
    cg.add(var.set_offline_silence(_ms(config[CONF_OFFLINE_SILENCE])))
    cg.add(var.set_retries(config[CONF_RETRIES]))
    cg.add(var.set_offline_after(config[CONF_OFFLINE_AFTER]))
    cg.add(var.set_offline_probe_interval(_ms(config[CONF_OFFLINE_PROBE_INTERVAL])))
    cg.add(var.set_stats_interval(_ms(config[CONF_STATS_INTERVAL])))
    cg.add(var.set_write_timeout(_ms(config[CONF_WRITE_TIMEOUT])))
    cg.add(var.set_write_settle(_ms(config[CONF_WRITE_SETTLE])))
    cg.add(var.set_write_rate_limit(_ms(config[CONF_WRITE_RATE_LIMIT])))

    cg.add(var.set_child_discovery(config[CONF_CHILD_DISCOVERY]))
    cg.add(var.set_fast_loop(config[CONF_FAST_LOOP]))
    cg.add(var.set_inverter_search(config[CONF_INVERTER_SEARCH]))
    if CONF_SCAN_RANGE in config:
        first, last = config[CONF_SCAN_RANGE]
        cg.add(var.set_scan(first, last, config[CONF_SCAN_ATTEMPTS]))
        if CONF_SCAN_FALLBACK_TABLE in config:
            cg.add(var.set_scan_fallback(config[CONF_SCAN_FALLBACK_TABLE]))

    for dev in config[CONF_DEVICES]:
        fallback = dev.get(CONF_FALLBACK_TABLE)
        cg.add(
            var.add_device(
                dev[CONF_INDEX],
                dev[CONF_ADDRESS],
                dev[CONF_TABLE],
                cg.RawExpression(f'"{fallback}"') if fallback else cg.nullptr,
                dev[CONF_WORD_ORDER],
                dev[CONF_GATE_CAPABILITIES],
                dev[CONF_SBR_CELLS],
                _ms(dev[CONF_SBR_INTERVAL]),
                _ms(dev[CONF_FRAME_GAP]) if CONF_FRAME_GAP in dev else -1,
                dev.get(CONF_DEV_TYPE, 0),
            )
        )
        if CONF_ONLINE in dev:
            s = await binary_sensor.new_binary_sensor(dev[CONF_ONLINE])
            cg.add(var.set_online_sensor(s))
        if CONF_PACKET_LOSS in dev:
            s = await sensor.new_sensor(dev[CONF_PACKET_LOSS])
            cg.add(var.set_packet_loss_sensor(s))
        for key, setter in (
            (CONF_DEV_CODE, var.set_dev_code_sensor),
            (CONF_DEV_TYPE_NAME, var.set_dev_type_sensor),
            (CONF_PROTOCOL, var.set_protocol_sensor),
            (CONF_TABLE_SENSOR, var.set_table_sensor),
        ):
            if key in dev:
                s = await text_sensor.new_text_sensor(dev[key])
                cg.add(setter(s))
