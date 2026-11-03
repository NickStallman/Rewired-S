import esphome.codegen as cg
from esphome.components import number
import esphome.config_validation as cv
from esphome.const import (
    CONF_MAX_VALUE,
    CONF_MIN_VALUE,
    CONF_MODE,
    CONF_STEP,
    CONF_UNIT_OF_MEASUREMENT,
    ENTITY_CATEGORY_CONFIG,
)

from . import CONF_KEY, CONF_SUNGROW_CONTROL_ID, CONF_VERIFY, ENTITY_SCHEMA, KEYS, ns, setup_readback

DEPENDENCIES = ["sungrow_control"]

ControlEntity = ns.class_("ControlEntity")
ControlNumber = ns.class_("ControlNumber", number.Number, ControlEntity)


def _fill(config):
    meta = KEYS[config[CONF_KEY]]
    if meta["options"]:
        raise cv.Invalid(f"'{config[CONF_KEY]}' is an enumeration; use the select platform")
    if CONF_UNIT_OF_MEASUREMENT not in config and meta["unit"]:
        config[CONF_UNIT_OF_MEASUREMENT] = meta["unit"]
    lo = config.get(CONF_MIN_VALUE, meta["number_min"])
    hi = config.get(CONF_MAX_VALUE, meta["number_max"])
    if lo < meta["min"] * meta["scale"] - 1e-9 or hi > meta["max"] * meta["scale"] + 1e-9 or lo > hi:
        raise cv.Invalid(
            f"'{config[CONF_KEY]}': range must lie inside the allow-list "
            f"{meta['min'] * meta['scale']}..{meta['max'] * meta['scale']}"
        )
    config[CONF_MIN_VALUE] = lo
    config[CONF_MAX_VALUE] = hi
    config.setdefault(CONF_STEP, meta["step"])
    return config


CONFIG_SCHEMA = cv.All(
    number.number_schema(ControlNumber, entity_category=ENTITY_CATEGORY_CONFIG)
    .extend(ENTITY_SCHEMA)
    .extend(
        {
            cv.Optional(CONF_VERIFY): cv.boolean,
            cv.Optional(CONF_MIN_VALUE): cv.float_,
            cv.Optional(CONF_MAX_VALUE): cv.float_,
            cv.Optional(CONF_STEP): cv.positive_float,
            cv.Optional(CONF_MODE, default="BOX"): cv.enum(number.NUMBER_MODES, upper=True),
        }
    ),
    _fill,
)


async def to_code(config):
    meta = KEYS[config[CONF_KEY]]
    var = await number.new_number(
        config, min_value=config[CONF_MIN_VALUE], max_value=config[CONF_MAX_VALUE], step=config[CONF_STEP]
    )
    ctl = await cg.get_variable(config[CONF_SUNGROW_CONTROL_ID])
    cg.add(var.set_parent(ctl))
    t = 3 if meta["type"] == "s16" else 4
    cg.add(var.set_register(meta["address"], t, meta["min"], meta["max"]))
    cg.add(var.set_scale(meta["scale"]))
    cg.add(var.set_verify(config.get(CONF_VERIFY, True)))
    await setup_readback(var, config)
