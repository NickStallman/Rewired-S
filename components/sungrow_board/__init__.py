"""sungrow_board: per-dongle port layer (WiNet-S / WiNet-S2).

- Emits USE_WINET_S / USE_WINET_S2 and product constants for other components.
- Optionally feeds the external GPIO supervisor ("WDOG") of the WiNet-S2 from the
  very first line of the generated setup(), before any other component exists.
- `diagnostics:` keeps a reset journal and log tail in RTC memory, switches the supervisor off on
  a panic, and reports how the previous boot ended (diag.h).
- `coredump: true` writes an IDF core dump to the `coredump` partition on a crash.
- `boot_slot:` the "Allow boot switch" switch (never restored) and the "Boot other slot" button
  that boots the other OTA slot (normally the stock app; docs/back_to_stock.md).
"""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import button, switch, text_sensor
from esphome.components.logger import LOG_LEVEL_SEVERITY, is_log_level, request_log_listener
import esphome.config_validation as cv
from esphome.const import (
    CONF_ENABLE_PIN,
    CONF_ID,
    CONF_INTERVAL,
    CONF_INVERTED,
    CONF_LOG_LEVEL,
    CONF_NUMBER,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.core import CORE
from esphome.coroutine import CoroPriority, coroutine_with_priority

CODEOWNERS = ["@nickstallman"]
AUTO_LOAD = ["text_sensor", "switch", "button"]

CONF_VARIANT = "variant"
CONF_WATCHDOG = "watchdog"
CONF_INIT_TOGGLES = "init_toggles"
CONF_INIT_DELAY = "init_delay"
CONF_KICK_PIN = "kick_pin"
CONF_PRODUCT_NAME = "product_name"
CONF_PRODUCT_TYPE = "product_type"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_VERSION_FLOOR = "version_floor"
CONF_IO = "io"
CONF_LED_RED = "led_red"
CONF_LED_GREEN = "led_green"
CONF_LED_BLUE = "led_blue"
CONF_CLOUD_CONNECTED = "cloud_connected"
CONF_CLOUD_ENABLED = "cloud_enabled"
CONF_CLOUD_ERROR = "cloud_error"
CONF_DEVICES_OFFLINE = "devices_offline"
CONF_FAULT_GRACE = "fault_grace"
CONF_BUTTON = "button"
CONF_HOLD_MARKS = "hold_marks"
CONF_ON_CLICK = "on_click"
CONF_ON_LONG_PRESS = "on_long_press"
CONF_ON_HOLD_REACHED = "on_hold_reached"
CONF_DIAGNOSTICS = "diagnostics"
CONF_LAST_RESET = "last_reset"
CONF_WEB = "web"
CONF_COREDUMP = "coredump"
CONF_BOOT_SLOT = "boot_slot"
CONF_ALLOW = "allow"
CONF_BOOT_OTHER = "boot_other"

VARIANT_WINET_S = "winet_s"
VARIANT_WINET_S2 = "winet_s2"

# cloud identity: must match the stock dongle
# Stock values per variant. The firmware version string reaches the cloud only as logger sig
# 0x1000 (app version) in the MQTT point uploads (isolarcloud.cpp sets 0x1000 from
# firmware_version); the board files pass the version their site's dongle shipped with
# (docs/cloud_identity.md). firmware_version "" = detect it from the stock image left in the
# other OTA slot (cloud_version.h), falling back to the variant default; a web UI override wins.
VARIANT_DEFAULTS = {
    VARIANT_WINET_S: {
        CONF_PRODUCT_NAME: "WiNet-S",
        # 1813: V200 hard-codes sprintf("%d", 0x715) + "WiNet-S" into publicData, and 0x715 is
        # the 0xF7 table default of sig 0x0004.
        CONF_PRODUCT_TYPE: 0x715,
        CONF_FIRMWARE_VERSION: "WINET-SV200.001.00.P046",
        CONF_VERSION_FLOOR: "P026",
    },
    VARIANT_WINET_S2: {
        CONF_PRODUCT_NAME: "WiNet-S2",
        CONF_PRODUCT_TYPE: 0x720,  # 1824
        CONF_FIRMWARE_VERSION: "WINET-SV300.001.03.P032",
        CONF_VERSION_FLOOR: "P012",
    },
}

sungrow_board_ns = cg.esphome_ns.namespace("sungrow_board")
SungrowBoard = sungrow_board_ns.class_("SungrowBoard", cg.Component)
BoardIO = sungrow_board_ns.class_("BoardIO", cg.Component)
BootSwitchArmSwitch = sungrow_board_ns.class_("BootSwitchArmSwitch", switch.Switch, cg.Component)
BootOtherSlotButton = sungrow_board_ns.class_("BootOtherSlotButton", button.Button)
_output_ns = cg.esphome_ns.namespace("output")
_BinaryOutput = _output_ns.class_("BinaryOutput")
_binary_sensor_ns = cg.esphome_ns.namespace("binary_sensor")
_BinarySensor = _binary_sensor_ns.class_("BinarySensor")


def _apply_variant_defaults(config):
    defaults = VARIANT_DEFAULTS[config[CONF_VARIANT]]
    for key, value in defaults.items():
        if key not in config:
            if value is None:
                raise cv.Invalid(
                    f"'{key}' has no known default for variant '{config[CONF_VARIANT]}'; set it explicitly",
                    path=[key],
                )
            config[key] = value
    return config


# Stock sequence (docs/board_pins.md): enable low -> 10x (100 ms, kick=s, s=!s,
# s starts at 1) -> enable high -> esp_timer toggles kick every 500 ms.
WATCHDOG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_KICK_PIN): pins.internal_gpio_output_pin_schema,
        cv.Optional(CONF_ENABLE_PIN): pins.internal_gpio_output_pin_schema,
        cv.Optional(CONF_INTERVAL, default="500ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(milliseconds=10)),
        ),
        cv.Optional(CONF_INIT_TOGGLES, default=10): cv.int_range(min=0, max=100),
        cv.Optional(CONF_INIT_DELAY, default="100ms"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(max=cv.TimePeriod(milliseconds=1000)),
        ),
    }
)

# Stock-like button + status LEDs (common/board_io.yaml, docs/board_pins.md). Logic: board_logic.h.
IO_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BoardIO),
        cv.Optional(CONF_LED_RED): cv.use_id(_BinaryOutput),
        cv.Optional(CONF_LED_GREEN): cv.use_id(_BinaryOutput),
        cv.Optional(CONF_LED_BLUE): cv.use_id(_BinaryOutput),
        # bool() lambdas: cloud link up / uplink wanted / any configured RS485 device offline.
        cv.Optional(CONF_CLOUD_CONNECTED): cv.returning_lambda,
        cv.Optional(CONF_CLOUD_ENABLED): cv.returning_lambda,
        # Preferred over connected/enabled: the cloud component's own error state.
        cv.Optional(CONF_CLOUD_ERROR): cv.returning_lambda,
        cv.Optional(CONF_DEVICES_OFFLINE): cv.returning_lambda,
        # Device-offline faults are ignored this long after boot (devices start offline until
        # their first reply).
        cv.Optional(CONF_FAULT_GRACE, default="60s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_BUTTON): cv.use_id(_BinarySensor),
        # Hold times reported while held (on_hold_reached), for feedback. Max 4.
        cv.Optional(CONF_HOLD_MARKS, default=[]): cv.All(
            cv.ensure_list(cv.positive_time_period_milliseconds), cv.Length(max=4)
        ),
        cv.Optional(CONF_ON_CLICK): automation.validate_automation({}, single=True),
        cv.Optional(CONF_ON_LONG_PRESS): automation.validate_automation({}, single=True),
        cv.Optional(CONF_ON_HOLD_REACHED): automation.validate_automation({}, single=True),
    }
).extend(cv.COMPONENT_SCHEMA)

# Reset journal + log tail in RTC memory, panic hook, "Last reset" report (diag.h).
DIAGNOSTICS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_LAST_RESET): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:restart-alert"
        ),
        # Highest log level copied into the RTC log tail (the last ~6 KB survive a reset).
        cv.Optional(CONF_LOG_LEVEL, default="DEBUG"): is_log_level,
        # GET /api/diag (report) and, with coredump, /api/coredump, behind the web login.
        cv.Optional(CONF_WEB, default=True): cv.boolean,
    }
)

# "Boot other slot" (boot_slot.h): the arming switch is ALWAYS_OFF (never restored), the button
# refuses unless it is on and the other slot holds a valid app image.
BOOT_SLOT_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ALLOW): switch.switch_schema(
            BootSwitchArmSwitch,
            default_restore_mode="ALWAYS_OFF",
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:alert",
        ),
        cv.Optional(CONF_BOOT_OTHER): button.button_schema(
            BootOtherSlotButton,
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:backup-restore",
        ),
    }
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SungrowBoard),
            cv.Required(CONF_VARIANT): cv.one_of(
                VARIANT_WINET_S, VARIANT_WINET_S2, lower=True
            ),
            cv.Optional(CONF_PRODUCT_NAME): cv.string_strict,
            cv.Optional(CONF_PRODUCT_TYPE): cv.hex_uint16_t,
            cv.Optional(CONF_FIRMWARE_VERSION): cv.string_strict,
            cv.Optional(CONF_VERSION_FLOOR): cv.string_strict,
            cv.Optional(CONF_WATCHDOG): WATCHDOG_SCHEMA,
            cv.Optional(CONF_IO): IO_SCHEMA,
            cv.Optional(CONF_DIAGNOSTICS): DIAGNOSTICS_SCHEMA,
            # Needs a `coredump` data partition in the partition table.
            cv.Optional(CONF_COREDUMP, default=False): cv.boolean,
            cv.Optional(CONF_BOOT_SLOT): BOOT_SLOT_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _apply_variant_defaults,
)


def _validate_variant_vs_chip(config):
    from esphome.components.esp32 import get_esp32_variant
    from esphome.components.esp32.const import VARIANT_ESP32, VARIANT_ESP32S3

    expected = {VARIANT_WINET_S: VARIANT_ESP32, VARIANT_WINET_S2: VARIANT_ESP32S3}[
        config[CONF_VARIANT]
    ]
    if get_esp32_variant() != expected:
        raise cv.Invalid(
            f"variant {config[CONF_VARIANT]} requires esp32 variant {expected}, got {get_esp32_variant()}"
        )
    return config


def _validate_coredump_partition(config):
    if not config[CONF_COREDUMP]:
        return config
    import esphome.final_validate as fv

    esp32_conf = fv.full_config.get().get("esp32", {})
    table = esp32_conf.get("partitions")
    if table is None:
        raise cv.Invalid("coredump: true needs a custom partition table with a coredump partition")
    path = CORE.relative_config_path(table)
    with open(path, encoding="utf-8") as f:
        rows = [line.split("#", 1)[0].split(",") for line in f]
    if not any(len(r) > 2 and r[1].strip() == "data" and r[2].strip() == "coredump" for r in rows):
        raise cv.Invalid(f"coredump: true but {table} has no 'data, coredump' partition", [CONF_COREDUMP])
    return config


def _validate_web(config):
    diag = config.get(CONF_DIAGNOSTICS)
    if diag and diag[CONF_WEB] and "web_server_base" not in CORE.loaded_integrations:
        diag[CONF_WEB] = False
    return config


def _final_validate(config):
    _validate_variant_vs_chip(config)
    _validate_coredump_partition(config)
    return _validate_web(config)


FINAL_VALIDATE_SCHEMA = _final_validate


async def _register_component(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_product_name(config[CONF_PRODUCT_NAME]))
    cg.add(var.set_product_type(config[CONF_PRODUCT_TYPE]))
    cg.add(var.set_firmware_version(config[CONF_FIRMWARE_VERSION]))
    cg.add(var.set_default_version(VARIANT_DEFAULTS[config[CONF_VARIANT]][CONF_FIRMWARE_VERSION]))
    cg.add(var.set_version_floor(config[CONF_VERSION_FLOOR]))
    if io := config.get(CONF_IO):
        await _register_io(io)
    if (bs := config.get(CONF_BOOT_SLOT)) is not None:
        if conf := bs.get(CONF_ALLOW):
            sw = await switch.new_switch(conf)
            await cg.register_component(sw, conf)
            await cg.register_parented(sw, var)
            cg.add(var.set_boot_switch(sw))
        if conf := bs.get(CONF_BOOT_OTHER):
            btn = await button.new_button(conf)
            await cg.register_parented(btn, var)
    if (diag := config.get(CONF_DIAGNOSTICS)) is not None:
        cfg = cg.StructInitializer(
            sungrow_board_ns.struct("DiagConfig"),
            ("log_level", LOG_LEVEL_SEVERITY.index(diag[CONF_LOG_LEVEL])),
        )
        cg.add(var.set_diag_config(cfg))
        if conf := diag.get(CONF_LAST_RESET):
            sens = await text_sensor.new_text_sensor(conf)
            cg.add(var.set_last_reset_sensor(sens))


async def _register_io(config):
    cg.add_define("USE_SUNGROW_BOARD_IO")
    io = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(io, config)
    leds = []
    for key in (CONF_LED_RED, CONF_LED_GREEN, CONF_LED_BLUE):
        leds.append(await cg.get_variable(config[key]) if key in config else cg.nullptr)
    cg.add(io.set_leds(*leds))
    for key, setter in (
        (CONF_CLOUD_CONNECTED, io.set_cloud_connected),
        (CONF_CLOUD_ENABLED, io.set_cloud_enabled),
        (CONF_CLOUD_ERROR, io.set_cloud_error),
        (CONF_DEVICES_OFFLINE, io.set_devices_offline),
    ):
        if key in config:
            lam = await cg.process_lambda(config[key], [], return_type=cg.bool_)
            cg.add(setter(lam))
    cg.add(io.set_fault_grace(config[CONF_FAULT_GRACE]))
    if CONF_BUTTON in config:
        cg.add(io.set_button(await cg.get_variable(config[CONF_BUTTON])))
    for mark in sorted(config[CONF_HOLD_MARKS]):
        cg.add(io.add_hold_mark(mark))
    # Restore value for the fallback-AP flag: ESPHome's configured wifi ap_timeout.
    ap_timeout = 0
    if (wifi := CORE.config.get("wifi")) and (ap := wifi.get("ap")):
        ap_timeout = ap["ap_timeout"].total_milliseconds
    cg.add(io.set_fallback_ap_timeout(ap_timeout))
    # One automation per trigger (x = click count, or ms held / mark reached).
    for key, trig, arg in (
        (CONF_ON_CLICK, io.get_click_trigger(), cg.uint8),
        (CONF_ON_LONG_PRESS, io.get_long_press_trigger(), cg.uint32),
        (CONF_ON_HOLD_REACHED, io.get_hold_reached_trigger(), cg.uint32),
    ):
        if conf := config.get(key):
            await automation.build_automation(trig, [(arg, "x")], conf)


# The watchdog start sequence is emitted as a file-scope object whose constructor runs during
# IDF's global constructors, before the scheduler and app_main: the supervisor is fed before any
# ESPHome or IDF application code runs, as stock does. The periodic kick is armed right after by
# an IDF SECONDARY-stage init function in sungrow_board.cpp (esp_timer is not usable in the
# constructor). It uses raw GPIO numbers on purpose: no ESPHome pin objects or Application state
# exist yet. The Component itself is registered at normal priority.
@coroutine_with_priority(CoroPriority.EARLY_INIT + 100)
async def to_code(config):
    if config[CONF_VARIANT] == VARIANT_WINET_S2:
        cg.add_define("USE_WINET_S2")
    else:
        cg.add_define("USE_WINET_S")
    # Compile-time copies for code that wants constexpr access.
    cg.add_define("SUNGROW_PRODUCT_NAME", config[CONF_PRODUCT_NAME])
    cg.add_define("SUNGROW_PRODUCT_TYPE", config[CONF_PRODUCT_TYPE])
    # The YAML value ("" = detect at runtime); code that wants the live value reads
    # global_board->firmware_version().
    cg.add_define("SUNGROW_FIRMWARE_VERSION", config[CONF_FIRMWARE_VERSION])

    if wd := config.get(CONF_WATCHDOG):
        cg.add_define("USE_SUNGROW_WDOG")
        kick = wd[CONF_KICK_PIN]
        en = wd.get(CONF_ENABLE_PIN)
        call = (
            "esphome::sungrow_board::watchdog_early_start("
            f"{kick[CONF_NUMBER]}, {str(kick[CONF_INVERTED]).lower()}, "
            f"{en[CONF_NUMBER] if en else -1}, {str(en[CONF_INVERTED]).lower() if en else 'false'}, "
            f"{wd[CONF_INTERVAL].total_milliseconds}u, {wd[CONF_INIT_TOGGLES]}u, "
            f"{wd[CONF_INIT_DELAY].total_milliseconds}u)"
        )
        cg.add_global(
            cg.RawStatement(
                "// External supervisor: start sequence from a global constructor, before the scheduler starts.\n"
                f"static const struct SungrowWdogEarly {{ SungrowWdogEarly() {{ {call}; }} }} sungrow_wdog_early;"
            )
        )

    if (diag := config.get(CONF_DIAGNOSTICS)) is not None:
        from esphome.components.esp32 import add_idf_sdkconfig_option

        cg.add_define("USE_SUNGROW_DIAG")
        if diag[CONF_WEB]:
            cg.add_define("USE_SUNGROW_DIAG_WEB")
        # Panic entry hook (diag.cpp): supervisor off + crash record before IDF's panic handler.
        cg.add_build_flag("-Wl,--wrap=panicHandler")
        cg.add_build_flag("-Wl,--wrap=xt_unhandled_exception")
        request_log_listener()
        if CONF_WATCHDOG in config:
            # Kick-stall detector runs from the esp_timer ISR (independent of task scheduling).
            add_idf_sdkconfig_option("CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD", True)

    if config[CONF_COREDUMP]:
        from esphome.components.esp32 import add_idf_sdkconfig_option, include_builtin_idf_component

        cg.add_define("USE_SUNGROW_COREDUMP")
        include_builtin_idf_component("espcoredump")
        add_idf_sdkconfig_option("CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH", True)
        add_idf_sdkconfig_option("CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF", True)
        add_idf_sdkconfig_option("CONFIG_ESP_COREDUMP_CHECKSUM_CRC32", True)
        # Own stack for writing the dump: the panicking task's stack may be nearly full.
        add_idf_sdkconfig_option("CONFIG_ESP_COREDUMP_STACK_SIZE", 1792)

    CORE.add_job(_register_component, config)
