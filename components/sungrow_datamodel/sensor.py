import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv

from . import (
    CONF_MULTIPLIER,
    DmBinding,
    ENTITY_SCHEMA,
    fill_sensor_defaults,
    resolve_signal,
    setup_binding,
    sungrow_datamodel_ns,
)

DEPENDENCIES = ["sungrow_datamodel"]

DmSensor = sungrow_datamodel_ns.class_("DmSensor", sensor.Sensor, DmBinding)

CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(DmSensor)
    .extend(ENTITY_SCHEMA)
    .extend({cv.Optional(CONF_MULTIPLIER): cv.float_}),
    resolve_signal,
    fill_sensor_defaults,
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    cg.add(var.set_multiplier(config[CONF_MULTIPLIER]))
    await setup_binding(var, config)
