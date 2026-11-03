import esphome.codegen as cg
from esphome.components import select
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_CONFIG

from . import CONF_KEY, CONF_SUNGROW_CONTROL_ID, CONF_VERIFY, ENTITY_SCHEMA, KEYS, ns, setup_readback

DEPENDENCIES = ["sungrow_control"]

ControlEntity = ns.class_("ControlEntity")
ControlSelect = ns.class_("ControlSelect", select.Select, ControlEntity)


def _is_select(config):
    meta = KEYS[config[CONF_KEY]]
    if not meta["options"]:
        raise cv.Invalid(f"'{config[CONF_KEY]}' is a numeric register; use the number platform")
    return config


CONFIG_SCHEMA = cv.All(
    select.select_schema(ControlSelect, entity_category=ENTITY_CATEGORY_CONFIG)
    .extend(ENTITY_SCHEMA)
    .extend({cv.Optional(CONF_VERIFY): cv.boolean}),
    _is_select,
)


async def to_code(config):
    meta = KEYS[config[CONF_KEY]]
    # Only writable values are options (e.g. EMS "MicroGrid" is shown by the text sensor only).
    opts = [(raw, text) for raw, text in meta["options"] if raw in meta["values"]]
    var = await select.new_select(config, options=[t for _, t in opts])
    ctl = await cg.get_variable(config[CONF_SUNGROW_CONTROL_ID])
    cg.add(var.set_parent(ctl))
    t = 3 if meta["type"] == "s16" else 4
    cg.add(var.set_register(meta["address"], t, meta["min"], meta["max"]))
    cg.add(var.set_verify(config.get(CONF_VERIFY, True)))
    for raw, text in opts:
        cg.add(var.add_option(raw, text))
    await setup_readback(var, config)
