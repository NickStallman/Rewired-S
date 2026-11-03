import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv

from . import CONF_META, DmBinding, ENTITY_SCHEMA, resolve_signal, setup_binding, sungrow_datamodel_ns

DEPENDENCIES = ["sungrow_datamodel"]

CONF_BIT = "bit"

DmBinarySensor = sungrow_datamodel_ns.class_(
    "DmBinarySensor", binary_sensor.BinarySensor, DmBinding
)


def _no_string(config):
    if config[CONF_META]["is_string"]:
        raise cv.Invalid("binary_sensor needs a numeric signal")
    if CONF_BIT in config and config[CONF_META].get("bit", -1) >= 0:
        raise cv.Invalid("a bitfield child key already names one bit; drop 'bit:'")
    return config


CONFIG_SCHEMA = cv.All(
    binary_sensor.binary_sensor_schema(DmBinarySensor)
    .extend(ENTITY_SCHEMA)
    # bit of a status / bitfield word (rows with nbits); omitted = "value != 0"
    .extend({cv.Optional(CONF_BIT): cv.int_range(min=0, max=63)}),
    resolve_signal,
    _no_string,
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    child_bit = config[CONF_META].get("bit", -1)
    if CONF_BIT in config:
        cg.add(var.set_bit(config[CONF_BIT]))
    elif child_bit >= 0:
        cg.add(var.set_bit(child_bit))
    await setup_binding(var, config)
