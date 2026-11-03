"""Diagnostic text sensors for sungrow_extflash."""

import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_SUNGROW_EXTFLASH_ID, SungrowExtFlash

DEPENDENCIES = ["sungrow_extflash"]

CONF_SERIAL_NUMBER = "serial_number"
CONF_BROKER = "broker"
CONF_IDENTITY_SOURCE = "identity_source"
CONF_CHIP = "chip"

TYPES = {
    CONF_SERIAL_NUMBER: ("set_sn_text_sensor", "mdi:identifier"),
    CONF_BROKER: ("set_broker_text_sensor", "mdi:cloud-outline"),
    CONF_IDENTITY_SOURCE: ("set_identity_source_text_sensor", "mdi:file-key-outline"),
    CONF_CHIP: ("set_chip_text_sensor", "mdi:memory"),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_SUNGROW_EXTFLASH_ID): cv.use_id(SungrowExtFlash),
        **{
            cv.Optional(key): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon=icon
            )
            for key, (_, icon) in TYPES.items()
        },
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_SUNGROW_EXTFLASH_ID])
    for key, (setter, _) in TYPES.items():
        if conf := config.get(key):
            sens = await text_sensor.new_text_sensor(conf)
            cg.add(getattr(parent, setter)(sens))
