"""Read-back sensor of an allow-listed register (the polled inverter value, display units)."""
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import CONF_UNIT_OF_MEASUREMENT, STATE_CLASS_MEASUREMENT

from esphome.components.sungrow_datamodel.sensor import DmSensor

from . import CONF_KEY, ENTITY_SCHEMA, KEYS, setup_readback

DEPENDENCIES = ["sungrow_control"]


def _fill(config):
    meta = KEYS[config[CONF_KEY]]
    if meta["options"]:
        raise cv.Invalid(f"'{config[CONF_KEY]}' is an enumeration; use the text_sensor platform")
    if CONF_UNIT_OF_MEASUREMENT not in config and meta["unit"]:
        config[CONF_UNIT_OF_MEASUREMENT] = meta["unit"]
    return config


CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(DmSensor, state_class=STATE_CLASS_MEASUREMENT, accuracy_decimals=0).extend(ENTITY_SCHEMA),
    _fill,
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await setup_readback(var, config)
