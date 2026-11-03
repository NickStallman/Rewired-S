import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv

from . import CONF_META, DmBinding, ENTITY_SCHEMA, resolve_signal, setup_binding, sungrow_datamodel_ns

DEPENDENCIES = ["sungrow_datamodel"]

CONF_MAP = "map"
CONF_DECIMALS = "decimals"

DmTextSensor = sungrow_datamodel_ns.class_("DmTextSensor", text_sensor.TextSensor, DmBinding)


def _map_schema(value):
    value = cv.Schema({cv.hex_uint32_t: cv.string_strict})(value)
    return value


CONFIG_SCHEMA = cv.All(
    text_sensor.text_sensor_schema(DmTextSensor)
    .extend(ENTITY_SCHEMA)
    .extend(
        {
            # numeric code -> text (status words); unknown codes show as 0xNNNN
            cv.Optional(CONF_MAP): _map_schema,
            # decimals for numeric signals without a map
            cv.Optional(CONF_DECIMALS): cv.int_range(min=0, max=6),
        }
    ),
    resolve_signal,
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    meta = config[CONF_META]
    decimals = config.get(CONF_DECIMALS, meta.get("accuracy") or 0)
    cg.add(var.set_decimals(decimals))
    for code, text in sorted(config.get(CONF_MAP, {}).items()):
        cg.add(var.add_mapping(code, text))
    await setup_binding(var, config)
