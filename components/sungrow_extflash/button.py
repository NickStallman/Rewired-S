"""Buttons for sungrow_extflash: hex-dump the identity file / list files to the log (backup aid)."""

import esphome.codegen as cg
from esphome.components import button
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_SUNGROW_EXTFLASH_ID, SungrowExtFlash, sungrow_extflash_ns

DEPENDENCIES = ["sungrow_extflash"]

DumpIdentityButton = sungrow_extflash_ns.class_("DumpIdentityButton", button.Button)
ListFilesButton = sungrow_extflash_ns.class_("ListFilesButton", button.Button)

CONF_DUMP_IDENTITY = "dump_identity"
CONF_LIST_FILES = "list_files"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_SUNGROW_EXTFLASH_ID): cv.use_id(SungrowExtFlash),
        cv.Optional(CONF_DUMP_IDENTITY): button.button_schema(
            DumpIdentityButton, entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:file-download-outline"
        ),
        cv.Optional(CONF_LIST_FILES): button.button_schema(
            ListFilesButton, entity_category=ENTITY_CATEGORY_DIAGNOSTIC, icon="mdi:folder-search-outline"
        ),
    }
)


async def to_code(config):
    for key in (CONF_DUMP_IDENTITY, CONF_LIST_FILES):
        if conf := config.get(key):
            btn = await button.new_button(conf)
            await cg.register_parented(btn, config[CONF_SUNGROW_EXTFLASH_ID])
