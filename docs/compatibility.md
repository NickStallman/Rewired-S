# Hardware compatibility

Every inverter the WiNet-S itself supports works with Rewired-S: the firmware reads the device
identification block, picks the register table of the protocol type the device reports (the
same tables the stock dongle uses), drops the blocks the device does not serve and attaches a
control profile by device type
([device types and grid profiles](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/device-types-and-grid-profiles.md)).
"Works" is not "verified": a wrong unit, a control with the wrong scale or a narrower power-stage
range only show on a real unit. The tables say what has been checked on hardware.

**Legend:** ✅ tested by the maintainer in daily use · 🟢 verified by a user report · 🟡 expected:
polled from a protocol table a tested model uses, same profile; not run on hardware · ⚪ untested:
fallback table only, or no report · ❌ not supported

To move a model up: **Send diagnostics** on the Settings page uploads the device's identification
reply, the registers it answers and its static values (serial removed); open a
[new device report](https://github.com/NickStallman/Rewired-S/issues/new?template=new_device.yml)
with the reference it shows. One report makes a model 🟢 for reads; a note on which controls you
checked against the inverter's display makes it 🟢 for controls.

## Dongles

| Dongle | Network | Status | Evidence |
|---|---|---|---|
| WiNet-S (V200, ESP32) | Wi-Fi | ✅ | |
| WiNet-S (V200, ESP32) | Ethernet (DP83848) | ✅ | daily use; pre-built image, runtime web login, MQTT settings, MQTT discovery and **Boot other slot** checked on it |
| WiNet-S2 (SV300, ESP32-S3) | Wi-Fi | ✅ | daily use; pre-built captive portal and app-only package through the stock updater (`P033`, P016 path variant) 🟢 [#10](https://github.com/NickStallman/Rewired-S/issues/10) |
| WiNet-S2 (SV300, ESP32-S3) | Ethernet (W5500) | 🟢 | link, DHCP, MQTT and cloud over the cable; reset GPIO5; MAC = base MAC + 3 [#11](https://github.com/NickStallman/Rewired-S/issues/11) |
| WiNet (first generation), WiFi-S | | ❌ | different hardware |

Hardware details: [WiNet-S](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s.md),
[WiNet-S2](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s2.md),
[S versus S2](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s-vs-s2.md).

## Inverters

**Firmware line** is what the inverter's own version string starts with (iSolarCloud, Devices
page); every model of a line runs the same images. **Ranges**: whether `data/dsp_params/` holds
the line's power-stage acceptance ranges. The power stage silently ignores a parameter write
outside its range while the control board acknowledges it; with the table the firmware refuses
or skips such a write, without it only the installer catalogue ranges apply.

| Line | Models | Firmware | Profile | Reads | Controls | Ranges | Evidence |
|---|---|---|---|---|---|---|---|
| **SH-RS** 1-phase hybrid | SH3.0RS, SH3.6RS, SH4.0RS, SH5.0RS, SH6.0RS, SH8.0RS, **SH10RS** (`-20` variants) | `SUNSTONE-H` | hybrid | ✅ | ✅ | yes | SH10RS with two SBR192 and a DTSU666, daily use |
| **SH-T** 3-phase hybrid | SH5T, SH6T, SH8T, SH10T, SH12T, **SH15T**, SH20T, SH25T (`_V12` variants) | `PEARL-H` | hybrid | 🟢 | 🟡 | yes | SH15T, no battery, 3-phase meter; values checked against iSolarCloud [#9](https://github.com/NickStallman/Rewired-S/issues/9) |
| **SH-RT** 3-phase hybrid | SH5.0RT, SH6.0RT, SH8.0RT, SH10RT (`-20`, `-V112`, `-V122`) | `SAPPHIRE-H` | hybrid | 🟡 | 🟡 | — | forced-power unit and start/stop register unchecked |
| **SG-RT** 3-phase string | SG5.0RT, SG6.0RT, SG8.0RT, SG10RT, SG12RT, SG15RT, SG17RT, SG20RT (`-P2`) | `BERYL-S` | string | 🟡 | 🟡 | — | |
| **SG-RS** 1-phase string | SG2.0RS-S, SG2.5RS-S, SG3.0RS, SG3.6RS, SG4.0RS, **SG5.0RS**, SG6.0RS, SG8.0RS, SG10RS (`-L`) | `SUNSTONE-S` | string | ✅ | ✅ | yes | SG5.0RS, daily use |
| **SG-RS-L2** 1-phase string | SG3.0RS-L2 … SG10RS-L2 | `SULFUR-S` | string | 🟡 | 🟡 | — | |
| **SH-RL / MG-RL** 1-phase hybrid | SH3RL … SH10RL, MG5RL … MG12RL | `BARITE-H` | hybrid | 🟡 | 🟡 | — | |
| **SH-CX** commercial hybrid | SH50CX, SH80CX, SH110CX, SH125CX | `GARNET-H` | hybrid | 🟡 | 🟡 | — | |
| **SG-CX / SG-HX** commercial string | SG25CX … SG125CX, SG225HX … SG350HX | `AGATE-S`, `EMERALD-S`, `GARNET`, `DIAMOND-S`, others | string | ⚪ | ⚪ | — | profile only where the device type maps |
| Older residential (SH5K, SG-K, SG-KTL-M) and anything else the WiNet lists | | various | by type | ⚪ | ❌ | — | fallback table when the identification block is not served |

Controls: the hybrid profile (EMS mode, forced charge/discharge, SOC limits, reserved SOC,
battery power limits, export limit and mode, backup, start/stop) and the string profile (power
limitation, start/stop) attach by device type. The allow-list was written against the SH10RS and
SG5.0RS; check a sibling's first write against the inverter's display. On a hybrid that reports
"No battery" the battery controls are left out of MQTT discovery and the web Devices page (the
native-API packages keep them). Model names are the strings the inverter reports (`SH5.0RS`,
`SH15T`, `SH10RT-20`); ranges match by device code, else by this model string.
[Limitations](limitations.md) lists registers whose unit or scale is unconfirmed.

## Batteries

| Family | Models | Status | Evidence |
|---|---|---|---|
| **SBR** high-voltage LFP | SBR064, SBR096, SBR128, SBR160, **SBR192**, SBR224, SBR256 | ✅ | two SBR192 stacks, daily use: cell voltages, module temperatures, balancing, protection flags ([battery](battery_sbr.md)); siblings 🟡 |
| **SBH** high-voltage | SBH100 … SBH400 | ⚪ | different control unit; polled with the battery table |
| Third-party batteries the inverter supports | | ⚪ | the inverter's battery registers work; per-stack data needs the battery to answer the stack table |

## Meters and accessories

| Device | Status | Evidence |
|---|---|---|
| DTSU666 on the inverter's meter port (grid power, import, export, phases through the inverter's meter block) | ✅ | the maintainer's SH10RS, daily use; the SH15T's Sungrow 3-phase meter 🟢 [#9](https://github.com/NickStallman/Rewired-S/issues/9) |
| A meter answering as its own device on the dongle's RS485 (DTSU666, DTSD1352, S100) | ⚪ | |
| EV charger (AC) | ⚪ | |
| Energy Bridge | ⚪ | |
| SP600 optimizers, WiMeter, WiProxy | ❌ | not polled |

## Operating environments

| | Status |
|---|---|
| Home Assistant with an MQTT broker (Mosquitto add-on) | ✅ |
| Home Assistant without MQTT (native entities compiled in) | 🟡 compiles; see [ESPHome](esphome.md) |
| mkaiser's Modbus integration and SunGather over port 502 | ✅ reads on the SH10RS; writes gated |
| iSolarCloud app and portal | ✅ the installer app's local connection is not supported |
| Grid-code setup profiles | ✅ Australia (country code 6) only; other regions need their profile added (`docs/data_format.md`) |
