"""Diagnostic binary sensors for sungrow_extflash."""

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_SUNGROW_EXTFLASH_ID, SungrowExtFlash

DEPENDENCIES = ["sungrow_extflash"]

CONF_MOUNTED = "mounted"
CONF_IDENTITY_VALID = "identity_valid"

TYPES = {
    CONF_MOUNTED: ("set_mounted_binary_sensor", "mdi:harddisk"),
    CONF_IDENTITY_VALID: ("set_identity_valid_binary_sensor", "mdi:card-account-details-outline"),
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_SUNGROW_EXTFLASH_ID): cv.use_id(SungrowExtFlash),
        **{
            cv.Optional(key): binary_sensor.binary_sensor_schema(
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
            sens = await binary_sensor.new_binary_sensor(conf)
            cg.add(getattr(parent, setter)(sens))
