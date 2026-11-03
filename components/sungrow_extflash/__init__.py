"""sungrow_extflash: external SPI NOR + READ-ONLY LittleFS mount of /usrdataout (identity source).

The external flash holds the dongle's irreplaceable cloud identity (SN etc., docs/cloud_identity.md).
Stock firmware formats it when a mount fails; this component never formats, erases or writes it
(see the SAFETY note in sungrow_extflash.h).
"""

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import (
    CONF_CLK_PIN,
    CONF_CS_PIN,
    CONF_FREQUENCY,
    CONF_ID,
    CONF_MISO_PIN,
    CONF_MOSI_PIN,
    CONF_NUMBER,
    Framework,
)
from esphome.core import CORE
import esphome.final_validate as fv

CODEOWNERS = ["@nickstallman"]

# Managed component that provides esp_vfs_littlefs_register. Stock uses esp_littlefs with
# LittleFS 2.9 (disk v2.1); 1.22.3 ships LittleFS 2.11, same disk v2.1 (reads v2.0 too).
LITTLEFS_COMPONENT = "joltwallet/littlefs"
LITTLEFS_VERSION = "1.22.3"

CONF_SPI_HOST = "spi_host"
CONF_IO_MODE = "io_mode"
CONF_DMA_CHANNEL = "dma_channel"
CONF_MOUNT_PATH = "mount_path"
CONF_PARTITION_LABEL = "partition_label"
CONF_PARTITION_SIZE = "partition_size"
CONF_IDENTITY_FILE = "identity_file"
CONF_IDENTITY_FALLBACKS = "identity_fallbacks"
CONF_DEFAULT_PRODUCT_TYPE = "default_product_type"
CONF_ON_IDENTITY = "on_identity"
CONF_SUNGROW_EXTFLASH_ID = "sungrow_extflash_id"

CONF_MAX_RECORD_LEN = "max_record_len"

# Identity file names. The live file is
# `dev_file_247_<productType>_2` (devid 0xF7, devtype = the compile-time
# productType, devcode 2): WiNet-S `dev_file_247_1813_2`, WiNet-S2 `dev_file_247_1824_2`.
# Stock fallback chain (both builds), first file >= 10 B wins:
#   log/sigdata/<name> -> logbak/sigdata/<name> -> log/factory/dev_file_factory
#   -> logbak/factory/dev_file_factory
# We walk the same chain but take the first file with a plausible SN, and use stock's choice only
# when none has one (choose_identity_file() in sigdata.cpp, docs/cloud_identity.md).
# The productType comes from sungrow_board (or default_product_type:), else 0x720 (WiNet-S2).
FALLBACK_PRODUCT_TYPE = 0x720


def sigdata_name(product_type: int) -> str:
    # int(): cv.hex_uint16_t yields a HexInt, whose str() is "0x...". Stock uses "%d".
    return f"dev_file_247_{int(product_type)}_2"


def default_identity_file(mount: str, product_type: int) -> str:
    return f"{mount}/log/sigdata/{sigdata_name(product_type)}"


def default_identity_fallbacks(mount: str, product_type: int) -> list[str]:
    return [
        f"{mount}/logbak/sigdata/{sigdata_name(product_type)}",
        f"{mount}/log/factory/dev_file_factory",
        f"{mount}/logbak/factory/dev_file_factory",
    ]


# Sigdata record length cap per board (as the stock readers).
MAX_RECORD_LEN_BY_VARIANT = {"winet_s": 0x100, "winet_s2": 0x420}
DEFAULT_MAX_RECORD_LEN = 0x420

sungrow_extflash_ns = cg.esphome_ns.namespace("sungrow_extflash")
SungrowExtFlash = sungrow_extflash_ns.class_("SungrowExtFlash", cg.Component)
IdentityTrigger = sungrow_extflash_ns.class_("IdentityTrigger", automation.Trigger.template())
IoMode = sungrow_extflash_ns.enum("IoMode", is_class=True)

# Quad modes are deliberately not offered: esp_flash_init() would then write the status register
# (QE bit). The non-quad modes only read. Stock: S2 = DIO, S = FASTRD.
IO_MODES = {
    "slowrd": IoMode.SLOWRD,
    "fastrd": IoMode.FASTRD,
    "dout": IoMode.DOUT,
    "dio": IoMode.DIO,
}
# spi_host_device_t values (SPI1_HOST = 0 is the main flash and not allowed).
SPI_HOSTS = {"SPI2": 1, "SPI3": 2, "HSPI": 1, "VSPI": 2}

# Name of each host as ESPHome's spi/ethernet components spell it, for clash checks.
_HOST_NAMES = {1: "SPI2_HOST", 2: "SPI3_HOST"}


def _validate_host(value):
    value = cv.string_strict(value).upper()
    if value.endswith("_HOST"):
        value = value[: -len("_HOST")]
    if value not in SPI_HOSTS:
        raise cv.Invalid(f"spi_host must be one of {', '.join(SPI_HOSTS)}")
    return value


def _validate_dma(value):
    if isinstance(value, str) and value.lower() == "auto":
        return 0
    return cv.int_range(min=1, max=2)(value)


def _abs_path(value):
    value = cv.string_strict(value)
    if not value.startswith("/"):
        raise cv.Invalid("path must be absolute")
    return value.rstrip("/") or "/"


def _check_chip(config):
    from esphome.components.esp32 import get_esp32_variant
    from esphome.components.esp32.const import VARIANT_ESP32, VARIANT_ESP32S2, VARIANT_ESP32S3

    variant = get_esp32_variant()
    if config[CONF_DMA_CHANNEL] != 0 and variant != VARIANT_ESP32:
        raise cv.Invalid("dma_channel 1/2 is only valid on the original ESP32; use 'auto'", [CONF_DMA_CHANNEL])
    if SPI_HOSTS[config[CONF_SPI_HOST]] == 2 and variant not in (VARIANT_ESP32, VARIANT_ESP32S2, VARIANT_ESP32S3):
        raise cv.Invalid("SPI3 is not available on this variant", [CONF_SPI_HOST])
    for key in (CONF_IDENTITY_FILE,):
        if key in config and not config[key].startswith(config[CONF_MOUNT_PATH] + "/"):
            raise cv.Invalid(f"{key} must be under mount_path {config[CONF_MOUNT_PATH]}", [key])
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SungrowExtFlash),
            cv.Optional(CONF_SPI_HOST, default="SPI2"): _validate_host,
            # Full pin schema so `ignore_strapping_warning: true` can be set (WiNet-S uses
            # strapping pins GPIO2/GPIO15 for MOSI/CS, as stock does). Only the number is used.
            cv.Required(CONF_MOSI_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_MISO_PIN): pins.internal_gpio_input_pin_schema,
            cv.Required(CONF_CLK_PIN): pins.internal_gpio_output_pin_schema,
            cv.Required(CONF_CS_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_FREQUENCY, default="40MHz"): cv.All(
                cv.frequency, cv.Range(min=1e6, max=80e6)
            ),
            cv.Optional(CONF_IO_MODE, default="dio"): cv.enum(IO_MODES, lower=True),
            cv.Optional(CONF_DMA_CHANNEL, default="auto"): _validate_dma,
            cv.Optional(CONF_MOUNT_PATH, default="/usrdataout"): _abs_path,
            cv.Optional(CONF_PARTITION_LABEL, default="usrdataout"): cv.All(
                cv.string_strict, cv.Length(min=1, max=16)
            ),
            # Default: the whole chip, as stock (esp_flash_get_size). LittleFS takes block_count
            # from the superblock, so this only has to be >= the filesystem.
            cv.Optional(CONF_PARTITION_SIZE): cv.All(cv.int_, cv.Range(min=65536)),
            # Default: <mount>/log/sigdata/dev_file_247_<productType>_2 and the stock fallbacks
            # (see default_identity_*); productType as for default_product_type below.
            cv.Optional(CONF_IDENTITY_FILE): _abs_path,
            cv.Optional(CONF_IDENTITY_FALLBACKS): cv.ensure_list(_abs_path),
            # productType used when 0x0004 is not in the file, and in the default file names.
            # Default: sungrow_board's product_type, else 0x720 (WiNet-S2).
            cv.Optional(CONF_DEFAULT_PRODUCT_TYPE): cv.hex_uint16_t,
            # Sigdata record length cap. Default: per sungrow_board variant (S 0x100, S2 0x420),
            # else 0x420.
            cv.Optional(CONF_MAX_RECORD_LEN): cv.int_range(min=0x40, max=0xFFFF),
            cv.Optional(CONF_ON_IDENTITY): automation.validate_automation(
                {cv.GenerateID(): cv.declare_id(IdentityTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    cv.only_with_framework(Framework.ESP_IDF),
    _check_chip,
)


def _final_validate(config):
    """Refuse configs where ESPHome's own spi:/ethernet: would grab our SPI host."""
    full = fv.full_config.get()
    ours = _HOST_NAMES[SPI_HOSTS[config[CONF_SPI_HOST]]]
    if spi_configs := full.get("spi"):
        from esphome.components.spi import CONF_INTERFACE_INDEX, get_spi_interface

        for spi_conf in spi_configs:
            if (index := spi_conf.get(CONF_INTERFACE_INDEX)) is not None and get_spi_interface(index) == ours:
                raise cv.Invalid(
                    f"spi: bus '{spi_conf[CONF_ID]}' uses {ours}, which sungrow_extflash needs for the "
                    "external NOR. Set `interface:` on the spi bus to the other host."
                )
    if eth := full.get("ethernet"):
        iface = eth.get("interface")
        if iface is not None and f"{str(iface).upper()}_HOST" == ours and "spi_id" not in eth:
            raise cv.Invalid(
                f"ethernet: uses interface {iface} ({ours}), which sungrow_extflash needs for the external "
                "NOR. Set `ethernet: interface:` to the other host (stock W5500 on the S2 is SPI3)."
            )
    used = {config[k][CONF_NUMBER]: k for k in (CONF_MOSI_PIN, CONF_MISO_PIN, CONF_CLK_PIN, CONF_CS_PIN)}
    if len(used) != 4:
        raise cv.Invalid("sungrow_extflash pins must be distinct")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_spi_host(SPI_HOSTS[config[CONF_SPI_HOST]]))
    cg.add(
        var.set_pins(*(config[k][CONF_NUMBER] for k in (CONF_MOSI_PIN, CONF_MISO_PIN, CONF_CLK_PIN, CONF_CS_PIN)))
    )
    cg.add(var.set_frequency_mhz(int(config[CONF_FREQUENCY] // 1e6)))
    cg.add(var.set_io_mode(config[CONF_IO_MODE]))
    cg.add(var.set_dma_channel(config[CONF_DMA_CHANNEL]))
    cg.add(var.set_mount_path(config[CONF_MOUNT_PATH]))
    cg.add(var.set_partition_label(config[CONF_PARTITION_LABEL]))
    if CONF_PARTITION_SIZE in config:
        cg.add(var.set_partition_size(config[CONF_PARTITION_SIZE]))
    board = CORE.config.get("sungrow_board") or {}
    product_type = config.get(CONF_DEFAULT_PRODUCT_TYPE, board.get("product_type", FALLBACK_PRODUCT_TYPE))
    cg.add(var.set_default_product_type(product_type))
    mount = config[CONF_MOUNT_PATH]
    identity_file = config.get(CONF_IDENTITY_FILE, default_identity_file(mount, product_type))
    fallbacks = config.get(CONF_IDENTITY_FALLBACKS, default_identity_fallbacks(mount, product_type))
    for path in [identity_file, *fallbacks]:
        cg.add(var.add_identity_file(path))
    max_len = config.get(CONF_MAX_RECORD_LEN, MAX_RECORD_LEN_BY_VARIANT.get(board.get("variant"), DEFAULT_MAX_RECORD_LEN))
    cg.add(var.set_max_record_len(max_len))

    for conf in config.get(CONF_ON_IDENTITY, []):
        trigger = cg.new_Pvariable(conf[CONF_ID], var)
        await automation.build_automation(trigger, [], conf)

    cg.add_define("USE_SUNGROW_EXTFLASH")

    esp32.add_idf_component(name=LITTLEFS_COMPONENT, ref=LITTLEFS_VERSION)
    # Directory listing (list_dir / "list files" button) needs opendir/readdir.
    esp32.require_vfs_dir()
    # esp_littlefs defaults match stock: block size is fixed at 4096 in esp_littlefs.c, and lfs
    # name_max is LFS_NAME_MAX (255), so any on-disk name_max <= 255 mounts.
