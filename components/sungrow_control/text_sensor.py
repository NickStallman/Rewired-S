"""Read-back text sensor of an enumerated register, with mkaiser's state strings."""
import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv

from esphome.components.sungrow_datamodel.text_sensor import DmTextSensor

from . import CONF_KEY, ENTITY_SCHEMA, KEYS, ns, setup_readback

DEPENDENCIES = ["sungrow_control"]

ControlTextSensor = ns.class_("ControlTextSensor", DmTextSensor)

CONFIG_SCHEMA = text_sensor.text_sensor_schema(ControlTextSensor).extend(ENTITY_SCHEMA)


async def to_code(config):
    meta = KEYS[config[CONF_KEY]]
    var = await text_sensor.new_text_sensor(config)
    if not meta["options"]:
        cg.add(var.set_decimals(1 if meta["scale"] < 1 else 0))
    for raw, text in meta["options"]:
        cg.add(var.add_mapping(raw, text))
    await setup_readback(var, config)
