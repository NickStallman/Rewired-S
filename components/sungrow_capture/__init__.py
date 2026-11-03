"""sungrow_capture: diagnostic captures from the inverter, started from the firmware web UI.

- I-V string scan (SG5.0RS, SH10RS): the inverter sweeps its PV inputs and sends VI_ALL.
- Grid waveform (SH10RS only): the inverter records 1..8 cycles and sends GridWaveRecord.

Each file is fetched with the inverter's YMODEM sender, raw on the RS485 line, while the bus is
held (polling pauses for the transfer only). Results stay in RAM (PSRAM) until reboot and are
downloaded raw from the web UI (sungrow_web /api/capture*). Nothing goes to MQTT / Home Assistant.
The trigger writes go through ControlCore::write_diag (a fixed allow-list of the trigger
registers; the web confirm token is the authorisation, not the writes-enabled switch).

    sungrow_capture:
      id: sungrow_cap
      sungrow_modbus_id: rs485_hub
      sungrow_control_id: sungrow_ctl
      sungrow_datamodel_id: sungrow_dm
      time_id: sntp_time        # optional: wall-clock start time in the metadata
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import time
from esphome.const import CONF_ID

from esphome.components.sungrow_control import SungrowControl
from esphome.components.sungrow_datamodel import CONF_SUNGROW_DATAMODEL_ID, SungrowDataModel
from esphome.components.sungrow_modbus import SungrowModbus

CODEOWNERS = []
DEPENDENCIES = ["sungrow_modbus", "sungrow_control", "sungrow_datamodel"]

sungrow_capture_ns = cg.esphome_ns.namespace("sungrow_capture")
SungrowCapture = sungrow_capture_ns.class_("SungrowCapture", cg.Component)

CONF_SUNGROW_MODBUS_ID = "sungrow_modbus_id"
CONF_SUNGROW_CONTROL_ID = "sungrow_control_id"
CONF_TIME_ID = "time_id"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SungrowCapture),
        cv.GenerateID(CONF_SUNGROW_MODBUS_ID): cv.use_id(SungrowModbus),
        cv.GenerateID(CONF_SUNGROW_CONTROL_ID): cv.use_id(SungrowControl),
        cv.GenerateID(CONF_SUNGROW_DATAMODEL_ID): cv.use_id(SungrowDataModel),
        cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    cg.add_define("USE_SUNGROW_CAPTURE")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_modbus(await cg.get_variable(config[CONF_SUNGROW_MODBUS_ID])))
    cg.add(var.set_control(await cg.get_variable(config[CONF_SUNGROW_CONTROL_ID])))
    cg.add(var.set_datamodel(await cg.get_variable(config[CONF_SUNGROW_DATAMODEL_ID])))
    if CONF_TIME_ID in config:
        cg.add(var.set_time(await cg.get_variable(config[CONF_TIME_ID])))
