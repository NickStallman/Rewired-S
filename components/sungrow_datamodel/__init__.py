"""sungrow_datamodel: register tables, decoder and signal store for Sungrow devices.

YAML:
    sungrow_datamodel:
      id: sungrow_dm

    sensor:
      - platform: sungrow_datamodel
        family: inverter          # inverter|battery|charger|energy_bridge|meter|logger
        key: curr_power           # canonical key (register_meta.py) ... or: sigid: 0x2080
        device: 0                 # device index the Modbus master stores this device under
        name: Total active power

`key:` also takes a fallback list (`key: [load_power, load_power_sg]`): one entity bound to
every listed signal, publishing the first one that is valid (protocol-suffixed duplicates
_sg/_sh/_pub: only the sigid of the inverter's own protocol is populated). The first key
gives the metadata; the others must have the same device/state class and a compatible unit
(W/kW and Wh/kWh are converted).

Bitfield children (`<parent>_bit<n>` keys, data/families/*.yaml `bits`) bind to their parent signal and
bit, so they work whether or not `expand_bitfields` stores the children.

    sungrow_datamodel:
      id: sungrow_dm
      float_scale: false       # true = multiply type-9 float32 values by the point-table scale
      expand_bitfields: false  # store bitfield children in the store, like stock's DM

Unit / multiplier / device_class / state_class / accuracy_decimals come from the generated
register_meta.py and can be overridden in YAML (when overriding the unit, also set
`multiplier`, which is applied on top of the table scale).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import (
    CONF_ACCURACY_DECIMALS,
    CONF_DEVICE_CLASS,
    CONF_ID,
    CONF_KEY,
    CONF_STATE_CLASS,
    CONF_UNIT_OF_MEASUREMENT,
)

from .register_meta import FAMILIES

CODEOWNERS = []
MULTI_CONF = True

sungrow_datamodel_ns = cg.esphome_ns.namespace("sungrow_datamodel")
SungrowDataModel = sungrow_datamodel_ns.class_("SungrowDataModel", cg.Component)
DmBinding = sungrow_datamodel_ns.class_("DmBinding")

CONF_SUNGROW_DATAMODEL_ID = "sungrow_datamodel_id"
CONF_DEVICE = "device"
CONF_FAMILY = "family"
CONF_SIGID = "sigid"
CONF_THROTTLE = "throttle"
CONF_FORCE_PUBLISH_INTERVAL = "force_publish_interval"
CONF_MULTIPLIER = "multiplier"

# Resolved metadata is stashed in the validated config under this private key.
CONF_META = "_dm_meta"

CONF_FLOAT_SCALE = "float_scale"
CONF_EXPAND_BITFIELDS = "expand_bitfields"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SungrowDataModel),
        # Type-9 meter floats are in engineering units; as in stock the point-table
        # scale only sets their display decimals. true multiplies them by the scale as well, for
        # a meter that sends floats in point-table units.
        cv.Optional(CONF_FLOAT_SCALE, default=False): cv.boolean,
        cv.Optional(CONF_EXPAND_BITFIELDS, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.store().set_float_scale(config[CONF_FLOAT_SCALE]))
    cg.add(var.store().set_expand_bitfields(config[CONF_EXPAND_BITFIELDS]))


# ---------------------------------------------------------------------------------------
# Shared entity-platform helpers

ENTITY_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_SUNGROW_DATAMODEL_ID): cv.use_id(SungrowDataModel),
        cv.Optional(CONF_DEVICE, default=0): cv.int_range(min=0, max=255),
        cv.Optional(CONF_FAMILY, default="inverter"): cv.one_of(*sorted(FAMILIES), lower=True),
        # one key, or a fallback list (first valid signal is published)
        cv.Optional(CONF_KEY): cv.All(cv.ensure_list(cv.string_strict), cv.Length(min=1)),
        cv.Optional(CONF_SIGID): cv.hex_uint16_t,
        cv.Optional(CONF_THROTTLE, default="1s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_FORCE_PUBLISH_INTERVAL): cv.positive_time_period_milliseconds,
    }
)

_FIELDS = ("sigid", "name", "unit", "multiplier", "device_class", "state_class",
           "accuracy", "is_string", "type", "nreg", "bit_parent", "bit")

# Unit conversions allowed between the alternatives of one key fallback list.
_UNIT_FACTORS = {("W", "kW"): 0.001, ("kW", "W"): 1000.0, ("Wh", "kWh"): 0.001,
                 ("kWh", "Wh"): 1000.0, ("var", "kvar"): 0.001, ("VA", "kVA"): 0.001}


def _lookup(family, key=None, sigid=None):
    fam = FAMILIES.get(family, {})
    if key is not None:
        hit = fam.get(key)
        if hit is None:
            low = {k.lower(): k for k in fam}
            if key.lower() in low:
                hit = fam[low[key.lower()]]
        if hit is None:
            import difflib

            close = difflib.get_close_matches(key, list(fam), n=5)
            raise cv.Invalid(
                f"Unknown {family} key '{key}'"
                + (f"; did you mean: {', '.join(close)}?" if close else "")
                + " (or use sigid:)"
            )
        return dict(zip(_FIELDS, hit))
    for vals in fam.values():
        if vals[0] == sigid:
            return dict(zip(_FIELDS, vals))
    return None  # unnamed sigid: no metadata, raw value


def _alt_multiplier(first, alt, key):
    """Multiplier turning `alt`'s published value into `first`'s unit."""
    for f in ("device_class", "state_class", "is_string"):
        if first[f] != alt[f]:
            raise cv.Invalid(f"key fallback '{key}': {f} {alt[f]!r} differs from {first[f]!r}")
    if first["unit"] == alt["unit"]:
        return alt["multiplier"]
    factor = _UNIT_FACTORS.get((alt["unit"], first["unit"]))
    if factor is None:
        raise cv.Invalid(f"key fallback '{key}': unit {alt['unit']!r} cannot be converted "
                         f"to {first['unit']!r}")
    return alt["multiplier"] * factor


def resolve_signal(config):
    """Validator: resolve key(s)/sigid -> metadata (stored under CONF_META). A bitfield child
    binds to its parent sigid + bit; a key list adds fallback signals (meta["fallbacks"] =
    [(sigid, multiplier)])."""
    if (CONF_KEY in config) == (CONF_SIGID in config):
        raise cv.Invalid("Specify exactly one of 'key' or 'sigid'")
    keys = config.get(CONF_KEY) or [None]
    meta = _lookup(config[CONF_FAMILY], keys[0], config.get(CONF_SIGID))
    if meta is None:
        meta = {"sigid": config[CONF_SIGID], "unit": "", "multiplier": 1.0,
                "device_class": "", "state_class": "", "accuracy": None,
                "is_string": False, "name": None, "bit_parent": 0, "bit": -1}
    meta = dict(meta)
    meta["fallbacks"] = []
    for k in keys[1:]:
        alt = _lookup(config[CONF_FAMILY], k)
        if alt["bit"] >= 0 or meta["bit"] >= 0:
            raise cv.Invalid(f"key fallback '{k}': bitfield children cannot be combined")
        meta["fallbacks"].append((alt["sigid"], _alt_multiplier(meta, alt, k)))
    config = config.copy()
    config[CONF_META] = meta
    return config


def fill_sensor_defaults(config):
    meta = config[CONF_META]
    if meta["is_string"]:
        raise cv.Invalid(
            f"'{config.get(CONF_KEY, [hex(meta['sigid'])])[0]}' is a string signal; "
            "use the text_sensor platform"
        )
    if meta.get("bit", -1) >= 0:
        raise cv.Invalid("bitfield children are binary signals; use the binary_sensor platform")
    from esphome.components import sensor

    # Run ESPHome's own validators so defaults look exactly like YAML-provided values.
    if CONF_UNIT_OF_MEASUREMENT not in config and meta["unit"]:
        config[CONF_UNIT_OF_MEASUREMENT] = sensor.validate_unit_of_measurement(meta["unit"])
    if CONF_DEVICE_CLASS not in config and meta["device_class"]:
        config[CONF_DEVICE_CLASS] = sensor.validate_device_class(meta["device_class"])
    if CONF_STATE_CLASS not in config and meta["state_class"]:
        config[CONF_STATE_CLASS] = sensor.validate_state_class(meta["state_class"])
    if CONF_ACCURACY_DECIMALS not in config and meta["accuracy"] is not None:
        config[CONF_ACCURACY_DECIMALS] = sensor.validate_accuracy_decimals(meta["accuracy"])
    config.setdefault(CONF_MULTIPLIER, meta["multiplier"])
    return config


async def setup_binding(var, config):
    parent = await cg.get_variable(config[CONF_SUNGROW_DATAMODEL_ID])
    meta = config[CONF_META]
    sigid = meta["bit_parent"] if meta.get("bit", -1) >= 0 else meta["sigid"]
    cg.add(var.set_signal(config[CONF_DEVICE], sigid))
    for alt_sigid, mult in meta.get("fallbacks", []):
        cg.add(var.add_fallback(alt_sigid, mult))
    cg.add(var.set_throttle(config[CONF_THROTTLE]))
    if CONF_FORCE_PUBLISH_INTERVAL in config:
        cg.add(var.set_force_interval(config[CONF_FORCE_PUBLISH_INTERVAL]))
    cg.add(var.attach(parent))
