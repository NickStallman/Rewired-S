# Build inputs

The device data the generators in `tools/` read: register tables, signal definitions, grid-code
profiles, installer catalogue, value rules and cloud point tables, all in the project's own
schema ([docs/data_format.md](../docs/data_format.md)). Every generated table in the firmware is
reproduced from these files (`make gen`) and checked for drift (`make check-gen`; the register
tables by `tests/test_gen_deterministic.py`). The generators need PyYAML
(`pip install -r tools/requirements.txt`).

| Path | Used by | Produces |
|---|---|---|
| `families/<family>.yaml` | `tools/gen_tables.py`, `tests/test_request_nodes.py` | `components/sungrow_datamodel/register_tables.{h,cpp}`, `register_meta.py` and `devices/*.yaml`: the poll tables of every device family (inverter, battery, charger, energy_bridge, meter, logger), their signal definitions (keys, names, units, HA classes, evidence), address fixes and the non-polled protection reads |
| `modbus_tcp/<map>.yaml` | `tools/gen_tables.py` | the Modbus-TCP (:502) export maps in `register_tables.{h,cpp}` |
| `cloud/point_tables.yaml` | `tools/gen_mqtt_table.py` | `components/isolarcloud/mqtt_point_table.{h,cpp}`: the iSolarCloud point ids, formats and scales of the uplink |
| `grid_profiles/index.yaml`, `grid_profiles/<family>.yaml` | `tools/gen_grid_profiles.py`, `tools/gen_installer_settings.py` | `components/sungrow_control/grid_profiles.gen.{h,cpp}`, `installer_allow.gen.h`, `installer_rules.gen.h`, `components/sungrow_web/web/src/setup_profiles.gen.ts` and `components/sungrow_web/web/src/installer_settings.gen.ts`: the country / grid-code setup profiles and the installer catalogue |
| `installer_rules.yaml` | `tools/gen_installer_rules.py` (imported by `tools/gen_installer_settings.py`) | the value ranges, register relations and regulation rules in `installer_rules.gen.h` and `installer_settings.gen.ts` |
| `installer_help.yaml` | `tools/gen_installer_settings.py` | the `help` tooltips of `installer_settings.gen.ts` |
| `write_registers.yaml` | `tools/gen_write_registers.py` | `components/sungrow_control/write_registers.{h,cpp}`, `write_meta.py`: the write allow-list and its read-back table |
| `dsp_params/models.yaml`, `dsp_params/*.csv` | `tools/gen_dsp_ranges.py` | `components/sungrow_control/dsp_ranges.gen.h`: the parameter acceptance ranges of each power-stage firmware line (SH-RS, SG-RS, SH-T), matched by device code or model string |

`tools/fetch_grid_profiles.py` downloads the current grid-code XML set from iSolarCloud into
`fetched/grid_profiles/<region>/` (not tracked) and converts it to `grid_profiles/` format
(`--to-project-format`, `tools/grid_profiles_xml.py`); `tools/extract_init_param_xml.py` does the
same from an external-flash dump. `tests/test_xml_drift.py` compares such a download with the
committed `grid_profiles/` when present.

Adding a device family or a settings family means adding one file here;
[docs/data_format.md](../docs/data_format.md) walks through it.
