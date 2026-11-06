# Home Assistant dashboards

Three dashboards built from the MQTT discovery entities, plus the dongle's own switches in
`ems-control.yaml`. Each is a Lovelace **view** you paste into the raw configuration editor of a
dashboard (⋮ → *Edit dashboard* → ⋮ → *Raw configuration editor*), or save as a dashboard of its
own.

| File | What it shows | Needs |
|---|---|---|
| `overview.yaml` | Solar, battery, grid and load at a glance, daily energy, inverter status | built-in cards only |
| `energy-flow.yaml` | A live flow diagram from the MPPTs through PV, battery and grid to the load, with short-history graphs | [ha-sankey-chart](https://github.com/MindFreeze/ha-sankey-chart) and [mini-graph-card](https://github.com/kalkih/mini-graph-card) from HACS |
| `ems-control.yaml` | Inverter state, EMS mode, forced charge/discharge, SOC limits, export limit, string-inverter power limit, battery stack cells and temperatures. Battery controls are absent without a battery; delete those rows. | built-in cards only |

## Entity ids

The entity-id scheme is in [Home Assistant](../docs/home_assistant.md#the-devices-mqtt-discovery).
The files use the placeholders `SH_SERIAL` (hybrid inverter), `SG_SERIAL` (string inverter),
`BAT_SERIAL` (battery stack), each the device serial number in lower case, and `<dongle>` (the
dongle's ESPHome node name). Replace them before pasting, for example:

```sh
sed -e 's/SH_SERIAL/a2xxxxxxxxx/g' -e 's/BAT_SERIAL/s23xxxxxxxx/g' \
    -e 's/<dongle>/house_inverter/g' dashboards/ems-control.yaml
```

Your serials are on the Devices page of the web UI and in Home Assistant under
*Settings → Devices & services → MQTT*. A card for a device you do not have (a battery stack, a
string inverter) can be deleted.

## Battery charge and discharge as separate flows

The hybrid inverter reports one signed **Battery power** entity (positive while discharging,
negative while charging). The flow diagram wants two positive numbers, so `energy-flow.yaml`
expects these two template sensors. Add them to `configuration.yaml` (or a package) with the
serial filled in:

```yaml
template:
  - sensor:
      - name: Battery charging power
        unique_id: sh10rs_SH_SERIAL_battery_charging_power
        unit_of_measurement: W
        device_class: power
        state_class: measurement
        availability: "{{ states('sensor.sh10rs_SH_SERIAL_battery_power') | is_number }}"
        state: "{{ [0, -(states('sensor.sh10rs_SH_SERIAL_battery_power') | float)] | max | round(0) }}"
      - name: Battery discharging power
        unique_id: sh10rs_SH_SERIAL_battery_discharging_power
        unit_of_measurement: W
        device_class: power
        state_class: measurement
        availability: "{{ states('sensor.sh10rs_SH_SERIAL_battery_power') | is_number }}"
        state: "{{ [0, states('sensor.sh10rs_SH_SERIAL_battery_power') | float] | max | round(0) }}"
```

## Energy dashboard and Modbus-TCP

The entities for Home Assistant's built-in Energy dashboard are in
[Home Assistant](../docs/home_assistant.md#dashboards); running mkaiser's integration against the
dongle's Modbus-TCP server instead (its own dashboards, other entity ids) is in
[Modbus-TCP](../docs/home_assistant.md#modbus-tcp-instead-of-or-as-well-as-mqtt).
