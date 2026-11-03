# Inverter registers: what each entity means

This page explains every inverter value our firmware publishes, for the SH10RS hybrid and the SG5.0RS string inverter. It also covers what the values mean on other Sungrow models that use the same tables. The battery stacks behind the SH10RS have their own page, [`battery_sbr.md`](battery_sbr.md).

The example values are readings from an SH10RS and an SG5.0RS. Background on the registers, states and control behaviour of these models: SG-Docs [SH10RS registers](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/registers.md), [SH10RS architecture](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/architecture.md), [SH10RS control and EMS](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/control-and-ems.md), [SG5.0RS registers](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sg5rs/registers.md), [SG5.0RS architecture](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sg5rs/architecture.md) and the [error codes](https://github.com/NickStallman/SG-Docs/blob/main/reference/appendix/error-codes.md).

**Conventions**

- Register numbers are 1-based **doc** numbers. The PDU address on the wire is doc − 1.
- u32/s32 values are two registers, **low word first**.
- Unless a row says otherwise, every register here is an input register (fc 4).
- “SH10RS” = an SH10RS hybrid, 10 kW, single-phase, 4 MPPT, ARM firmware `SUNSTONE-H_01011.02.69`.
- “SG5.0RS” = an SG5.0RS string inverter, 5 kW, single-phase, 2 MPPT, ARM firmware `SUNSTONE-S_01011.01.49`.
- Both inverters report protocol code “AA10” with protocol type 3, so the firmware polls both with the **private 5xxx map** (`inv_proto3`). Entities with a `_sg` or `_sh` key suffix come from the public maps that other models use; they never have a value on these two.

## 1. How values reach Home Assistant

Each inverter is its own Home Assistant device over MQTT discovery (`<model>_<serial>`, e.g. `SG50RS_A2291234567`).

**Which signals become entities.** Every named signal that currently has a value becomes an entity, unless its category is `hidden`. A register the inverter does not serve never creates an entity.

**Entity ids.** The entity id is built from the entity **name**.

**Placeholder values.** Some registers are served with a placeholder rather than left unserved. The firmware stores those as “no value”, so they do not appear:

| Rows | Absent when | Why |
|---|---|---|
| Phase B/C voltage and current | the inverter reports **output type 0** (single-phase) | the SG5.0RS serves 0 there, the SH10RS 0xFFFF |
| `meter_*` rows (meter phase values, meter frequency, meter reactive power) | no meter active power is valid | a system without a smart meter serves −0.01 A and 0 Hz there |
| Phase B/C current | the raw value is 0xFFFF | 0xFFFF is not the s16 “no data” value (0x7FFF), so it would read −0.1 A |

Stock “no data” values are always absent: u16 0xFFFF, s16 0x7FFF, u32 0xFFFFFFFF, s32 0x7FFFFFFF.

**Signs**

| Entity | + means | − means |
|---|---|---|
| Export power | exporting to the grid | importing |
| Meter active power | importing | exporting |
| Battery power, battery current | discharging | charging |

## 2. Identity and firmware (diagnostic)

| Entity (key) | Register | Meaning | SH10RS | SG5.0RS |
|---|---|---|---|---|
| Serial number (`serial_number`) | 5378, 10 regs | inverter S/N | A2101234567 | A2291234567 |
| Device model (`device_model`) | 2516, 8 regs | model name | SH10RS | SG5.0RS |
| Device type code (`device_type_code`) | 2515 | Sungrow model code (decimal) | 3355 (0x0D1B) | 9734 (0x2606) |
| **Firmware version** (`firmware_version`) | 2582 | the installed ARM firmware build (first of eight MCU/PLD version slots) | SUNSTONE-H_01011.02.69 | SUNSTONE-S_01011.01.49 |
| ARM software version (`arm_software_version`) | 4954 | ARM software identifier (stays the same across builds) | ARM_SUNSTONE-H_V11_V01_A | ARM_SUNSTONE-S_V11_V01_A |
| DSP software version (`dsp_software_version`) | 4969 | DSP software identifier | MDSP_SUNSTONE-H_V11_V01_A | MDSP_SUNSTONE-S_V11_V01_A |
| Inverter firmware version (`inverter_firmware_version`) | 8801 | name of the last firmware **package** installed | SUNSTONE-H_B000.V002.P067 | not served |
| Communication module firmware version (`comm_module_firmware_version`) | 8816 | what the communication module last reported to the inverter. Our firmware does not report itself, so this still shows the stock dongle version | WINET-SV300.001.03.P024 | not served |
| Battery firmware version (`battery_firmware_version`) | 8831 | BCU firmware of the battery | SBRBCU-S_22011.01.30 | not served |
| Rated active power (`rated_active_power`) | 5374 | nameplate output, kW (0.1) | 10 kW | 5 kW |
| Rated reactive power (`rated_reactive_power`) | 5376 | kvar (0.1) | 6 kvar | 3 kvar |
| Maximum apparent power (`max_apparent_power`) | 5659 | kVA (0.1), a rating | 10 kVA | 5 kVA |
| Output type (`output_type`) | 5375 | see §8 | Single-phase | Single-phase |
| Battery type (`battery_type`, hybrids) | 5629 | the battery type the hybrid is set up for, see §8 | Sungrow high-voltage Li-ion | — |

Hidden but still polled for iSolarCloud:

- 5800, the certification software version. It repeats 4954 on both inverters.
- 2534/2535, the protocol code “AA10”. The dongle's Devices page shows it as text.
- 34209, a second model field. It is empty on the SH10RS.
- 5487/5489, function flags. They are build-time constants, and only bits 4 (115200 gate), 22 and 25 have a known use.
- 33144, the remote-server connection status.
- 5622/5623, the export-limit setting range.

## 3. Status

| Entity (key) | Register | Meaning | SH10RS (example) | SG5.0RS (example) |
|---|---|---|---|---|
| Operating status (`device_state`) | 5221 (SG public map 5038, SH public map 13000) | the inverter's running state, see §8 | Dispatched running | Running |
| Power flow (`power_flow`, derived) | 6099 | the hybrid's power-flow flags as text, e.g. “PV generating, Battery charging, Load consuming, Importing from grid”; “Idle” when nothing flows | see example | not served |
| Power limitation reason (`power_limit_reason`) | 5506 | why output is being limited, see §8 | No power limitation | No power limitation |
| DRM state (`drm_state`) | 5503 | AS/NZS 4777 demand-response mode, see §8 | not polled: the inverter does serve it (255, DRM disabled), but its capability word 5521 has bit 2 clear, so the dongle drops the row and no entity appears | No DRM dispatching |
| Reactive power mode (`reactive_power_mode`) | 5507 | the active reactive-power regulation mode, see §8 | not served | Q(U) (volt-var) |
| AFCI fault count (`afci_fault_count`) | 5519 | number of arc-fault (AFCI) trips | 0 | **45** |
| Active faults / Active fault codes (`active_fault_count`, `active_fault_codes`, derived) | 5448–5472 | count and codes of the inverter's active faults and alarms, e.g. “No active faults”, “12 Excess leakage current” or “732 fault, 933 alarm”. Codes with a known protection condition carry a name: the SH10RS conditions (grid over/under-voltage and -frequency, grid outage, DC injection, leakage current, grid abnormal, 10-minute overvoltage, bus over-voltage and unbalance, insulation, relay, self-check, PV configuration, temperature, sensor, arc fault, frequency derating) and the SG5.0RS codes for the same bit layout (PV over-current and reverse polarity, surge-protection alarms, hardware trips, measurement-channel faults); 50 codes in all, the rest stay numeric | 0 / No active faults | — |
| First active fault (`first_fault`, derived) | 5448–5472 | the first entry of the active-fault list as “NN Name” (e.g. “39 Low insulation resistance”), “No active faults” when the list is empty; the battery stacks carry the same entity from their own fault list | No active faults | — |
| Transient state (`state_transient`, derived) | 5221 | Yes while the operating status is one the inverter leaves by itself: Initial standby (0x1200), Starting (0x1600), Intelligent station building (0x1800), Safe mode (0x1900), Restarting (0x2501), Recovering from fault (0x6600), Standby (low PV) (0xA100); on string inverters also Key stop (0x1300), which the SG5.0RS reports for a rapid shutdown. Cleared with the status | No | No |

**Dispatched running** (0x8200) means the inverter is running under a power dispatch or limit rather than freely. An SH10RS shows it in normal operation, both while exporting and while charging.

**Derived fault and state signals.** The fault list (5448–5472) is decoded into three synthetic signals per device (inverter and battery stacks alike): 0xF060 active-fault count, 0xF061 active-fault text (“39 Low insulation resistance, 933 alarm”) and 0xF065 first fault (the first entry of 0xF061). The running state (5221, signal 0x2086) sets 0xF066 transient state. Both come from tables generated by `tools/gen_tables.py`: every running-state code has a state class (running, stopped, standby, starting, transient, fault, offline or unknown, `state_class()`), and `state_transient(code, string_family)` holds the transient set of the inverter family plus the per-model-family extras (0x1300 for string inverters). The same state classes drive the start/stop follow-up and the power-limit re-assert ([`control.md`](control.md)). Fault names come from one generated table (`fault_code_text()`), shared by the firmware and the web UI.

**Problem (Home Assistant, MQTT).** Each inverter and battery device also gets a `binary_sensor` with device class `problem`: ON while the active-fault count is above zero, OFF otherwise. Its attributes carry `count`, `codes` (the numeric fault codes), `names` (their names, in the same order) and `first` (the First active fault text), so an automation can key on the sensor and read the detail from the attributes. It is published with the device's discovery set, whenever the fault list changes and in the periodic full refresh.

**Power flow flags.** The raw 6099 word is hidden; the text entity carries the same information.

| Bit | Meaning |
|---|---|
| 0 | PV generating |
| 1 | Battery charging |
| 2 | Battery discharging |
| 3 | Load consuming |
| 4 | Exporting to grid |
| 5 | Importing from grid |
| 7 | Load feeding back |

## 4. AC side

| Entity (key) | Register | Unit, scale | Meaning / notes |
|---|---|---|---|
| Total active power (`active_power`) | 5232 s32 | kW, 0.001 (HA: W) | AC output of the inverter |
| Total reactive power (`reactive_power`) | 5234 s32 | kvar, 0.001 (HA: var) | |
| Total apparent power (`apparent_power`) | 5238 u32 | kVA, 0.001 (HA: VA) | |
| Total power factor (`power_factor`) | 5241 s16 | 0.001 | signed |
| Grid frequency (`grid_frequency`) | 5242 | Hz, 0.01 | |
| Phase A voltage (`phase_a_voltage`) | 5247 | V, 0.1 | the AC voltage on single-phase models |
| Phase A current (`phase_a_current`) | 5250 s16 | A, 0.1 | |
| Phase B/C voltage, current | 5248/5249, 5251/5252 | V 0.1, A 0.1 | three-phase models only; absent on both inverters here (§1) |
| Active power limit setpoint (`active_power_limit_setpoint`) | 5509 u32 | W | the power limit in force; equals the rating when limiting is off (SG5.0RS: 5000 W) |

## 5. DC / PV side

| Entity (key) | Register | Unit, scale | Notes |
|---|---|---|---|
| Total DC power (`total_dc_power`) | 5236 u32 | kW, 0.001 | |
| MPPT1–4 voltage / current (`mpptN_voltage`, `mpptN_current`) | 5333–5340 | V 0.1, A 0.1 | SH10RS 4 MPPT, SG5.0RS 2 |
| MPPT N power (`mpptN_power`, derived) | | W | V × I per tracker |
| MPPT total power (`mppt_total_power`, derived) | | W | sum of the trackers |
| Bus voltage (`bus_voltage`) | 5244 | V, 0.1 | internal DC bus |
| Insulation impedance to ground (`insulation_resistance`) | 5246 | kΩ | PV array insulation; SH10RS 3258, SG5.0RS 2199 |
| Negative voltage to ground (`negative_voltage_to_ground`) | 5243 s16 | V, 0.1 | SG5.0RS only |
| Internal air temperature (`internal_temperature`) | 5245 s16 | °C, 0.1 | |

String voltages/currents (5253–5332) and MPPT 5–20 are defined for larger models; neither inverter here serves them.

## 6. Energy counters (kWh, 0.1 unless noted)

| Entity (key) | Register | SH10RS | SG5.0RS | Notes |
|---|---|---|---|---|
| Daily yield (`daily_yield`) | 5223 | not served | ✓ | AC yield today (string inverters) |
| Monthly / Yearly yield (`monthly_yield`, `yearly_yield`) | 5224 / 5226 u32 | not served | ✓ | |
| Total yield (`total_yield`, derived) | 5228 u32 | not served | ✓ | lifetime AC yield |
| Daily / Total PV yield (`daily_pv_yield`, `total_pv_yield`) | 5660 / 5661 | ✓ | not served | hybrids: PV energy |
| Daily / Total AC output energy (`daily_ac_output_energy`, `total_ac_output_energy`) | 5676 / 5677 | ✓ | not served | energy out of the AC port |
| Daily / Total AC input energy (`daily_ac_input_energy`, `total_ac_input_energy`) | 5679 / 5680 | ✓ | not served | energy taken in through the AC port (charging from the grid) |
| Daily / Total PV export energy (`daily_pv_export_energy`, `total_pv_export_energy`) | 5663 / 5664 | ✓ | not served | |
| Daily / Total direct energy consumption (`daily_direct_consumption_energy`, `total_direct_consumption_energy`) | 5617 / 5619 | ✓ | not served | PV energy used directly by the loads |
| Total export energy / Total import energy (`total_export_energy`, `total_import_energy`) | 5611 / 5615 | ✓ | not served (no meter) | measured by the meter |
| Daily export / import energy | 5609 / 5613 | not served by SH10RS firmware SUNSTONE-H_01011.02.69 | not served | |
| Battery charge/discharge counters | 5666–5672, 13041 | ✓ | — | see [`battery_sbr.md`](battery_sbr.md) §4 |
| Total on-grid operating time (`total_running_time`) | 5230 u32 | not served | ✓ | h |
| Daily running time (`daily_running_time`) | 5240 | not served | ✓ | min, resets daily |

Daily counters reset at midnight (HA state class `total_increasing`).

## 7. Grid meter, load and backup (hybrids)

| Entity (key) | Register | Unit | Notes |
|---|---|---|---|
| Export power (`export_power`) | 5216 s32 | kW, 0.001 | **+ export / − import** |
| Grid import / export power (`grid_import_power`, `grid_export_power`, derived) | | kW | the two halves of export power, each ≥ 0 |
| Load power (`load_power`) | 5218 s32 | kW, 0.001 | house load |
| Meter active power (`meter_active_power`) | 5601 s32 | W | **+ import / − export**, measured at the meter |
| Meter phase A/B/C active power | 5603/5605/5607 s32 | W | |
| Meter phase A/B/C voltage, current | 5741–5743, 5744–5746 s16 | V 0.1, A 0.01 | a three-phase meter on a single-phase supply reads all three phases, so phases B/C show a few tenths of a volt and 0 A |
| Meter grid frequency, reactive power | 5737, 5738 | Hz 0.01, var | |
| Backup power (`backup_power`) | 5726 s32 | W | load on the backup (EPS) port |
| Backup phase A voltage, current, power; backup frequency | 5731, 5720, 5723, 5734 | V 0.1, A 0.1, W, Hz 0.01 | phases B/C are not served on the single-phase SH10RS |
| Battery voltage, current, power, SOC, SOH, temperature, capacity, BMS limits | 5214, 5630–5639 | | see [`battery_sbr.md`](battery_sbr.md) §4 |

All meter rows except Meter active power are absent when no meter is connected (§1).

## 8. Value lists

**Operating status** (5221; also SG 5038 and SH 13000):

| Code | Text | Code | Text |
|---|---|---|---|
| 0x0000 | Running | 0x1300 | Awaiting startup |
| 0x0400 | Running (maintain mode) | 0x1400 | Standby |
| 0x0800 | Running (forced mode) | 0x1500 | Emergency stop |
| 0x0801 | Running (backup mode) | 0x1600 | Starting |
| 0x1000 | Running (off-grid) | 0x1700 | AFCI self-test shutdown |
| 0x1202 | Running | 0x1800 | Intelligent station building |
| 0x2000 | Open loop | 0x1900 | Safe mode |
| 0x4000 | Running (external EMS) | 0x2500 | Communication fault |
| 0x4001 | Emergency charging | 0x2501 | Restarting |
| 0x0041 | Off-grid DC charging | 0x5500 | Fault |
| 0x1111 | Uninitialized | 0x6600 | Recovering from fault |
| 0x1200 | Initial standby | 0x8000 | Stop |
| 0x8100 | Derating running | 0x8200 | Dispatched running |
| 0x9100 | Running with alarm | 0xA100 | Standby (low PV) |

The SH10RS power stage itself produces only 0x0000, 0x0400, 0x0800, 0x1000, 0x1200, 0x1300,
0x1400, 0x1500, 0x1600, 0x1800, 0x1900, 0x2000, 0x4000, 0x4001, 0x5500, 0x8100, 0x8200 and
0x9100; its controller adds 0x2500 (communication fault) and reports 0x1400 while the power stage
does not answer. 0x1300 is the state after a stop command (the inverter waits for a start),
0x1800 the string-detection sweep, 0x1900 rapid shutdown engaged, 0x8200 a run in which the
active-power limit is the binding derating source and 0x8100 any other derating source. The SG5.0RS power stage produces 0x0000, 0x1200,
0x1300, 0x1400, 0x1500, 0x1600, 0x5500, 0x8100, 0x8200, 0x9100 and 0xA100 (waiting for PV power);
it reports a rapid shutdown as 0x1300, not 0x1900.

The SH public map 13000 uses one bit per state:

| Code | Text | Code | Text |
|---|---|---|---|
| 0x0001 | Stop | 0x0080 | Derating running |
| 0x0002 | Awaiting startup | 0x0100 | Fault |
| 0x0004 | Emergency stop | 0x0200 | Update failed |
| 0x0008 | Standby | 0x0400 | Running (maintain mode) |
| 0x0010 | Initial standby | 0x0800 | Running (forced mode) |
| 0x0020 | Starting | 0x1000 | Running (off-grid) |
| 0x0040 | Running | 0x4000 | Running (external EMS) |

**Output type** (5375):

| Code | Text |
|---|---|
| 0 | Single-phase |
| 1 | Three-phase (3P4L) |
| 2 | Three-phase (3P3L) |

**Power limitation reason** (5506):

| Code | Text | Code | Text |
|---|---|---|---|
| 0 | No power limitation | 256 | Grid overvoltage |
| 1 | Temperature or microgrid derating (one code for the temperature derating and the EMS microgrid mode's output cap) | 257 | Ripple control |
| 2 | Active power limitation | 512 | DRM5-DRM8 |
| 4 | Grid undervoltage | 1024 | DRM1-DRM4 |
| 5 | Power factor below 1 | 2048 | Grid underfrequency |
| 8 | Grid overfrequency | 4096 | Charging derating at low grid voltage |
| 11 | Optimizer overtemperature | 8192 | Reactive power regulation |
| 16 | Feed-in limitation (without meter) | 16384 | High PV voltage |
| 32 | Soft start | 32768 | High altitude |
| 64 | Feed-in limitation | 32770 | Undertemperature |
| 128 | PV input power limit | 32771 | Alarm |
| 36864 | Generation limit control | 32772 | Module temperature abnormal |

The SH10RS power stage writes this register as a single value for the most restrictive active
source, never a bit combination, and produces only 0, 1, 2, 4, 8, 32, 256, 512 and 2048 (a DRM
limit is always 512; 1 also covers the microgrid-mode cap). The SG5.0RS power stage never emits 4
or 512 (its DRM logic lives in the controller) and can emit 36864 under some country codes.

The web UI dashboard's "Active-power limit" card shows the 5509 setpoint when it is valid, else the
string inverter's power-limit read-backs ("Limit 50 %" / "No limit"), with the 5506 text as its
caption and "Limited by the active-power setting" while the operating status is 0x8200.

**DRM state** (5503):

| Code | Text |
|---|---|
| 1–9 | DRM0–DRM8 |
| 255 | No DRM dispatching |

255 is forced when the DRM function is disabled on both inverters. The SH10RS reports 254 when DRM is
enabled and no command is asserted. On the SG5.0RS, 255 is also forced when the country byte is not
Australia (6) or New Zealand (99), and it is returned for an unrecognised code too, so 255 there does
not tell "DRM disabled" from "enabled, DRED connected and idle". The SG5.0RS serves the register
directly; on the SH10RS the capability word 5521 bit 2 is set only while a state of 1–9 is asserted,
so the dongle sees the SH10RS state only then.

**Reactive power mode** (5507). The power stage itself implements five modes, selected by the
installer setting the controller pushes down (off, fixed power factor, Q(U), fixed reactive power,
Q(P)); LVRT and HVRT are ride-through states of its protection, not reactive-power modes, and the
power stage caps reactive power by its apparent-power limit (14.4 kVA / 63.5 A on the SH10RS) and
the installer Q limits. The controller maps that into this enum:

| Code | Text |
|---|---|
| 0 | Off |
| 1 | LVRT |
| 2 | Fixed Q(T) |
| 3 | Fixed power factor |
| 4 | Q(P) |
| 5 | Q(U) (volt-var) |
| 6 | HVRT |
| 7 | SVG mode |

**Battery type** (5629), main values:

| Code | Text |
|---|---|
| 2 | No battery |
| 9 | Sungrow Li-ion |
| 105 | Sungrow high-voltage Li-ion (SBR stacks) |
| 109 | Sungrow LFP |
| 110 | Sungrow modular battery |

The other 24 codes name third-party batteries; see `tools/gen_tables.py` `BATTERY_TYPE_MAP`.

**Country/region** (protection read 32310, web UI only): 150 Sungrow country codes, 6 = Australia (`COUNTRY_MAP`).

## 9. Model differences at a glance

| | SH10RS | SG5.0RS |
|---|---|---|
| Yield | PV yield (5660/5661), AC output/input energy | daily/monthly/yearly/total yield (5223–5228) |
| Operating time | not served | total (5230) and daily (5240) |
| Battery, backup port, power-flow flags | yes | no |
| Meter | yes (import/export totals, phase values) | none: meter rows absent |
| DRM state | served by the inverter, but gated off for the dongle while no DRM mode is asserted | served (255 when DRM is disabled, in an unsupported country, or idle) |
| Reactive power mode | not served | served |
| Active power limit setpoint | not served | served |
| Phase B/C, string currents, MPPT 5–20 | absent | absent |
| Daily import/export energy | not served (5609/5613) | no meter |
