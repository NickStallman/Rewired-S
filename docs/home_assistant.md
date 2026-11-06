# Home Assistant

Rewired-S shows up in Home Assistant in two ways: the dongle itself as a native ESPHome device,
and every device on the RS485 bus as its own device through MQTT discovery.

## Why MQTT, and not just ESPHome entities

ESPHome entities are fixed when the firmware is compiled, but the devices on the bus (inverter,
battery stacks, meter, charger) are only known at runtime. MQTT discovery publishes a Home
Assistant device for each device actually found, keyed by model and serial, with the entity scheme
the Winet Extractor add-on used; the cost is an MQTT broker (the Mosquitto add-on).

If you do not want a broker, `common/native_entities.yaml` compiles the inverter (and optionally a
battery stack and a meter) into the dongle's native-API device instead
([ESPHome](esphome.md)). These native-API control packages are static: they keep the
battery controls whatever battery the inverter reports. Devices beyond those are then only visible
in the web UI and over Modbus-TCP.

## The dongle (ESPHome native API)

Added through *Settings → Devices & services*, where the ESPHome integration discovers it.
Pre-built images have no API key compiled in; Home Assistant generates one when it adopts the
dongle (not yet verified, see [compatibility](compatibility.md); [pre-built images](prebuilt.md)).
Entities, with `<dongle>` the node name (`house_inverter`):

| Entity | Purpose |
|---|---|
| `switch.<dongle>_inverter_writes_enabled` | the gate for every inverter write from Home Assistant, MQTT and port 502; off by default, restored across reboots |
| `switch.<dongle>_cloud_uplink` | the iSolarCloud connection |
| `switch.<dongle>_cloud_writes` | whether iSolarCloud may change inverter settings (default off) |
| `binary_sensor.<dongle>_cloud_connected` | uplink state |
| `switch.<dongle>_modbus_tcp_server` | the port 502 server |
| `switch.<dongle>_usage_reporting` | anonymous usage reporting ([usage reporting](analytics.md)) |
| `switch.<dongle>_allow_boot_switch`, `button.<dongle>_boot_other_slot` | the way back to the app in the other OTA slot, or Settings → Boot slots in the web UI ([back to stock](back_to_stock.md)) |
| `binary_sensor.<dongle>_mqtt_connected` | the local MQTT connection (pre-built images) |
| `sensor.<dongle>_last_reset`, write counters, uptime, versions | diagnostics |
| `button.<dongle>_restart`, `_rs485_rescan` | actions (**Send diagnostics** is on the web UI's Settings page) |
| `update.<dongle>_firmware` | new releases (pre-built images) |

The action `esphome.<dongle>_write_registers` writes allow-listed holding registers from
automations ([control](control.md)).

## The devices (MQTT discovery)

Every discovered device is published under the discovery prefix (`homeassistant` by default) as
its own device, linked to the dongle through `via_device`:

| Device | Entities |
|---|---|
| Inverter (`SH10RS`, `SG5.0RS`, …) | power, voltages and currents per phase and per MPPT, grid import and export, load, energy counters, temperatures, operating status, a **Problem** binary sensor with the active fault codes and names as attributes, serial and firmware version, and the control profile's writable entities: `select` (EMS mode, forced charge/discharge command, start/stop), `number` (forced power, max/min SOC, reserved SOC, export limit, battery max charge/discharge power, charging/discharging start power, power limitation % and kW), `switch` (export limit mode, backup mode, power limitation) |
| Battery stack (`SBR192`, …) | SOC, SOH, voltage, current, temperature, cycle count, balancing status, highest and lowest cell voltage with module and cell index, cell voltage spread, warmest and coolest module, charge/discharge prohibited flags, active fault codes, a **Problem** sensor |
| Meter | per-phase voltages, currents and active power, totals, frequency, import and export energy |

Battery controls (forced charge/discharge, SOC limits, reserved SOC, battery power limits and
start powers) are left out while the inverter reports battery type "No battery".

Entity ids follow `<domain>.<model>_<serial>_<name>` with the model in lower case and dots
replaced (`sh10rs`, `sg5_0rs`, `sbr192`), the serial in lower case and the name slugified:
`sensor.sh10rs_a2xxxxxxxxx_battery_soc`, `select.sh10rs_a2xxxxxxxxx_ems_mode`,
`sensor.sbr192_s23xxxxxxxx_cell_voltage_spread`.
Each device has an availability topic, so entities go unavailable when the device drops off the
bus or the dongle goes down. Values are published on change with a per-signal deadband
([update rates](update_rates.md)); the writable entities show the inverter's own read-back, so a
refused write snaps back to the real value within a few seconds.

Writes over MQTT land only while **Inverter writes enabled** is on, pass the same allow-list and
ranges as every other path, and are rate-limited to one write per register per second.

## MQTT settings

- Compiled images: `mqtt_broker`, `mqtt_username`, `mqtt_password` in `secrets.yaml`; port and
  discovery prefix in `common/mqtt_ha.yaml`.
- Pre-built images: the **Home Assistant** card on the Settings page of the web UI; the
  connection state and the last error are shown there and as
  `binary_sensor.<dongle>_mqtt_connected`.

## Dashboards

[dashboards/](../dashboards/) has three views built from these entities: an overview with
built-in cards, an energy-flow diagram (HACS cards), and an EMS control view. Replace the serial
placeholders as described in [dashboards/README.md](../dashboards/README.md).

For the built-in **Energy** dashboard use the `total_increasing` counters: Total PV yield (or
Total yield), Total import energy, Total export energy, Total battery charge energy, Total
battery discharge energy.

## Modbus-TCP instead of, or as well as, MQTT

The port 502 server speaks Sungrow's public register maps, so
[mkaiser's integration](https://github.com/mkaiser/Sungrow-SHx-Inverter-Modbus-Home-Assistant)
and [SunGather](https://github.com/bohdan-s/SunGather) work unchanged with the dongle as the host.
Registers the firmware does not poll are forwarded to the inverter live. Writes (function 6 and
16) pass the same gate and allow-list as MQTT; `write_allowed_ips` under `sungrow_modbus_tcp:` in
`common/control.yaml` (a commented example) restricts who may write. Both paths can run at the
same time.
