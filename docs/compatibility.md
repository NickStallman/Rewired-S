# Hardware compatibility

**Every inverter the WiNet-S itself supports works with Rewired-S.** The firmware does not pick
registers by model name: it reads the device-information block, classifies the device by the
type and protocol codes it reports
([device types and grid profiles](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/device-types-and-grid-profiles.md)),
polls it from the register table of that protocol type (the same tables the stock dongle uses),
drops the blocks the device does not serve, and attaches a control profile by device type. So an
inverter nobody has tried yet still shows up with its readings, its Home Assistant devices and
its controls.

What "works" does not mean is "verified": a value with a wrong unit, a control that writes the
right register with the wrong scale, or a sibling model whose power stage accepts a narrower
range than the table knows are the things only a real unit shows. The tables below say, per
class, what the firmware will do and what has actually been checked on hardware.

**Status legend**

| | |
|---|---|
| **Tested** | runs in daily use on this model, values and controls checked against the inverter |
| **Verified** | run on this model by a user and reported (reads checked; controls not necessarily) |
| **Expected** | same protocol table and firmware line as a tested or verified model; not run on hardware |
| **Untested** | discovered and polled from the shared tables; values and controls unverified |
| **No** | not supported |

The fastest way to move a model up a table is the **Send diagnostics** button on the Settings
page: the report carries the device's identification reply, the registers it answers and its
static values, with the serial number removed. Open a
[new device report](https://github.com/NickStallman/Rewired-S/issues/new?template=new_device.yml)
with the reference it shows. One report per model is enough to list it as verified for reads;
a note on which controls you tried against the inverter's own display moves it further.

## Dongles

| Dongle | Network | Status | Notes |
|---|---|---|---|
| WiNet-S (V200, ESP32) | Wi-Fi | Tested | |
| WiNet-S (V200, ESP32) | Ethernet (DP83848) | Tested | pre-built image verified on hardware (runtime login, MQTT settings, boot-slot switch) |
| WiNet-S2 (SV300, ESP32-S3) | Wi-Fi | Tested | compiled image in daily use; the pre-built image's captive-portal provisioning and the app-only package through the stock updater (firmware `P033`) verified by a user ([#10](https://github.com/NickStallman/Rewired-S/issues/10)) |
| WiNet-S2 (SV300, ESP32-S3) | Ethernet (W5500) | Verified | link, DHCP, web UI, MQTT and the cloud uplink over the cable, hours of uptime; reset pin GPIO5 confirmed; the Ethernet MAC is the base MAC + 3 ([#11](https://github.com/NickStallman/Rewired-S/issues/11)) |
| WiNet (first generation), WiFi-S | | No | different hardware |

Hardware details: [WiNet-S](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s.md),
[WiNet-S2](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s2.md),
[S versus S2](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s-vs-s2.md).

## Inverters

Columns: **Firmware line** is the name the inverter's own firmware version starts with (shown
in iSolarCloud and on the Devices page), and every model of a line runs the same control-board
and power-stage images. **Reads** and **Controls** say what the firmware does with the class.
**Power-stage ranges**: the inverter's power stage silently ignores a parameter write outside
its own acceptance range while the control board acknowledges it, so the value reads back as
written and does nothing. For the lines marked **yes** the firmware carries those ranges
(`data/dsp_params/`) and refuses or skips such a write for every model of the line; for the
others the installer catalogue ranges are the only check and the read-back can lie.

| Class | Models | Firmware line | Type / profile | Reads | Controls | Power-stage ranges | Verified on hardware |
|---|---|---|---|---|---|---|---|
| **SH-RS** single-phase hybrid | SH3.0RS, SH3.6RS, SH4.0RS, SH5.0RS, SH6.0RS, SH8.0RS, **SH10RS** (and the `-20` variants) | `SUNSTONE-H` | hybrid (device type 35) | Expected | hybrid profile, Expected | **yes** (read on the SH10RS) | **SH10RS Tested**: daily use, reads and controls, with SBR batteries |
| **SH-T** three-phase hybrid | SH5T, SH6T, SH8T, SH10T, SH12T, **SH15T**, SH20T, SH25T (and the `_V12` variants) | `PEARL-H` | hybrid (device type 35, protocol type 3) | **SH15T Verified**: identification, live values, 3 MPPTs, phases, meter, backup port and energy counters checked against iSolarCloud and an independent meter | hybrid profile attached; not tried yet (unit without a battery) | **yes** (read from the SH-T power-stage firmware) | SH15T, no battery, Sungrow three-phase meter ([#9](https://github.com/NickStallman/Rewired-S/issues/9)) |
| **SH-RT** three-phase hybrid | SH5.0RT, SH6.0RT, SH8.0RT, SH10RT and the `-20` / `-V112` / `-V122` variants | `SAPPHIRE-H` | hybrid | Expected | hybrid profile, Expected; the forced-power unit and the start/stop register need checking | no | none yet |
| **SG-RT** three-phase string | SG5.0RT, SG6.0RT, SG8.0RT, SG10RT, SG12RT, SG15RT, SG17RT, SG20RT (and the `-P2` variants) | `BERYL-S` | string | Expected | string profile, Expected | no | none yet |
| **SG-RS** single-phase string | SG2.0RS-S, SG2.5RS-S, SG3.0RS, SG3.6RS, SG4.0RS, **SG5.0RS**, SG6.0RS, SG8.0RS, SG10RS (and the `-L` variants) | `SUNSTONE-S` | string (device type 21) | Expected | string profile, Expected | **yes** (read on the SG5.0RS) | **SG5.0RS Tested**: daily use, reads and controls |
| **SG-RS-L2** single-phase string | SG3.0RS-L2 … SG10RS-L2 | `SULFUR-S` | string | Expected | string profile, Expected | no | none yet |
| **SH-RL / MG-RL** single-phase hybrid | SH3RL, SH3.6RL, SH4RL, SH5RL, SH6.0RL, SH8.0RL, SH10RL; MG5RL … MG12RL | `BARITE-H` | hybrid | Expected | hybrid profile, Expected | no | none yet |
| **SH-CX** commercial hybrid | SH50CX, SH80CX, SH110CX, SH125CX | `GARNET-H` | hybrid | Expected | hybrid profile, Expected | no | none yet |
| **SG-CX / SG-HX / SG-T** commercial string | SG25CX … SG125CX (`-P2`, `-P3`), SG225HX … SG350HX, SG-T (CN) | `AGATE-S`, `EMERALD-S`, `GARNET`, `MARBLE-S`, `DIAMOND-S`, `MALACHITE-S`, `SAPPHIRE-S`, `BARITE-S`, `TOPAZ-S` | string | Expected (protocol table), Untested | string profile when the device type maps to it; otherwise none | no | none yet |
| Older residential (SH5K-20, SH5K-30, SG-K, SG-KTL-M, …) and anything else the WiNet lists | | various | by device type | Untested: polled with the fallback table when the identification block is not served | none with the fallback table | no | none yet |

What "Expected" means for the controls: the hybrid profile (EMS mode, forced charge/discharge,
SOC limits, export limit, backup) and the string profile (power limitation, start/stop) are
attached by device type. The register allow-list was written against the SH10RS and SG5.0RS; a
sibling model usually shares it, but check the first write against the inverter's own display.
On a hybrid that reports "No battery" the battery controls are left out. `docs/limitations.md`
lists the registers whose unit or scale is unconfirmed. The model name in the tables is the
string the inverter reports in its identification block (`SH5.0RS`, `SH15T`, `SH10RT-20`); the
firmware matches on it for the power-stage ranges.

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
