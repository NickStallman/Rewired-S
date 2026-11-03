# Hardware compatibility

Rewired-S discovers what is on the RS485 bus and classifies each device by the type and protocol
codes it reports, not by its model name
([device types and grid profiles](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/device-types-and-grid-profiles.md)).
Models of one family share a controller design and a firmware line, so a family that has been
tested on one model is expected to work on its siblings. Everything else is polled from the
tables the stock dongle uses for that protocol type and is unverified until someone runs it.

**Status legend**

| | |
|---|---|
| **Tested** | runs in daily use on this model, controls verified |
| **Expected** | same family, same firmware line and register map as a tested model; not run on hardware |
| **Untested** | discovered and polled from the shared tables; values and controls unverified |
| **No** | not supported |

The fastest way to move a model up this table is the **Send diagnostics** button on the Settings
page: the report carries the device's identification reply, the registers it answers and its
static values, with the serial number removed. Open a
[new device report](https://github.com/NickStallman/Rewired-S/issues/new?template=new_device.yml)
with the reference it shows.

## Dongles

| Dongle | Network | Status | Notes |
|---|---|---|---|
| WiNet-S (V200, ESP32) | Wi-Fi | Tested | |
| WiNet-S (V200, ESP32) | Ethernet (DP83848) | Tested | pre-built image verified on hardware (runtime login, MQTT settings, boot-slot switch) |
| WiNet-S2 (SV300, ESP32-S3) | Wi-Fi | Tested | compiled image; the pre-built image's captive-portal provisioning is untested on hardware |
| WiNet-S2 (SV300, ESP32-S3) | Ethernet (W5500) | Untested | compiles; the W5500 reset pin is unverified |
| WiNet (first generation), WiFi-S | | No | different hardware |

Hardware details: [WiNet-S](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s.md),
[WiNet-S2](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s2.md),
[S versus S2](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s-vs-s2.md).

## Inverters

| Family | Models | Type | Status | Notes |
|---|---|---|---|---|
| **SH-RS** single-phase hybrid | SH3.0RS, SH3.6RS, SH4.0RS, SH5.0RS, SH6.0RS, SH8.0RS, **SH10RS** | hybrid (device type 35) | **SH10RS tested**, others Expected | one firmware line (`SUNSTONE-H`); power-stage acceptance ranges known for the SH10RS |
| **SG-RS** single-phase string | SG2.0RS-S, SG2.5RS-S, SG3.0RS-S, SG3.0RS, SG3.6RS, SG4.0RS, **SG5.0RS**, SG6.0RS, SG8.0RS, SG10RS | string (device type 21) | **SG5.0RS tested**, others Expected | one firmware line (`SUNSTONE-S`); power-stage ranges known for the SG5.0RS |
| **SH-RT** three-phase hybrid | SH5.0RT, SH6.0RT, SH8.0RT, SH10RT and the -20 / -V112 variants | hybrid | Untested | classified by protocol type; forced-power unit and start/stop register need checking |
| **SH-T** three-phase hybrid | SH5T, SH6T, SH8T, SH10T, SH12T, SH15T, SH20T, SH25T | hybrid | Untested | |
| **SG-RT** three-phase string | SG5.0RT, SG6.0RT, SG8.0RT, SG10RT, SG12RT, SG15RT, SG17RT, SG20RT | string | Untested | |
| Other Sungrow residential and light-commercial inverters the WiNet supports | | by protocol type | Untested | discovered with the fallback table when the identification block is not served; no control profile then |

What "Expected" means for the controls: the hybrid profile (EMS mode, forced charge/discharge,
SOC limits, export limit, backup) and the string profile (power limitation, start/stop) are
attached by device type. The register allow-list and ranges were written against the SH10RS and
SG5.0RS; a sibling model usually shares them, but check the first write against the inverter's
own display. `docs/limitations.md` lists the registers whose unit or scale is unconfirmed.

## Batteries

| Family | Models | Status | Notes |
|---|---|---|---|
| **SBR** high-voltage LFP stacks | SBR064, SBR096, SBR128, SBR160, **SBR192**, SBR224, SBR256 | **SBR192 tested** (two stacks), others Expected | one battery control unit family; cell voltages, module temperatures, balancing and protection flags ([battery](battery_sbr.md)) |
| **SBH** high-voltage stacks | SBH100 … SBH400 | Untested | different control unit; polled with the battery table |
| Third-party batteries the inverter supports | | Untested | the inverter's own battery registers work; per-stack data depends on the battery answering the stack table |

## Meters and accessories

| Device | Status | Notes |
|---|---|---|
| Energy meter through the inverter (DTSU666, DTSD1352, S100 and the inverter's meter block) | Untested | the inverter-side meter block (grid power, import/export) is tested; a meter as its own device is not |
| EV charger (AC) | Untested | |
| Energy Bridge | Untested | |
| SP600 optimizers, WiMeter, WiProxy | No | not polled |

## Operating environments

| | Status |
|---|---|
| Home Assistant with an MQTT broker (Mosquitto add-on) | Tested |
| Home Assistant without MQTT (native entities compiled in) | compiles; see `docs/configuration.md` |
| mkaiser's Modbus integration and SunGather over port 502 | Tested on the SH10RS (read); writes gated |
| iSolarCloud app and portal | Tested; the installer app's local connection is not supported |
| Grid-code setup profiles | Australia only; other regions need their profile added (`docs/data_format.md`) |
