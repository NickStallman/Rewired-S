# SBR battery stacks: how they work and what each register means

This page explains how Sungrow SBR battery stacks work with an SH-RS hybrid inverter (an SH10RS
here), and what every battery register our firmware reads means. It is written for anyone running this firmware with SBR batteries.

It describes BCU firmware `SBRBCU-S_22011.01.30` behind SH10RS ARM firmware
`SUNSTONE-H_01011.02.69`. Point names and numbers are those of the iSolarCloud app's local-Modbus
configuration, whose register numbers are our doc numbers. Background in SG-Docs:
[registers via the inverter](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sbr192/registers-via-inverter.md),
[balancing and SOC](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sbr192/balancing-and-soc.md),
[protections and limits](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sbr192/protections-and-limits.md),
[CAN bus](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sbr192/can-bus.md) and the
[SH10RS battery interface](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/battery-interface.md).

**Conventions**

- Register numbers are 1-based **doc** numbers, as in Sungrow documents and
  `data/families/battery.yaml`. On the wire the PDU address is **doc − 1**. Our Modbus master
  subtracts the 1, and the inverter firmware's switch statements compare wire addresses.
- u32 values are two registers, **low word first**.
- "Example" values are slave 200 / slave 201 of the system below, discharging at about 1.5 kW
  near 96 % SOC.

**Example system**

| Item | Value |
|---|---|
| Inverter | SH10RS, Modbus address 1, protocol type 3, firmware SUNSTONE-H_01011.02.69 |
| Stack 1 | SBR192, Modbus slave **200**, 6 modules with cell-type code `D`, about 723 cycles |
| Stack 2 | SBR192, Modbus slave **201**, 6 modules with cell-type code `K`, about 71 cycles |
| SBR192 | 6 modules × 20 cells (3.2 V nominal), 120 cells in series, 384 V nominal, 50 Ah, 19.2 kWh |

---

## 1. Topology

```
  module 1 ... module N        each: 20 cells, LTC681x cell monitor, its own EEPROM
      │  isoSPI daisy chain
  ┌───┴──────────────┐
  │ BCU (top box)    │── CAN2: J1939 service bus (29-bit IDs, service tools)
  │ STM32F417        │
  └───┬──────────────┘
      │ CAN1: 11-bit IDs, little-endian payloads; shared with the other stacks' BCUs
      │
  SH10RS inverter (battery protocol 'i')
      │ RS485 Modbus RTU
  WiNet dongle (this firmware)
      slave 1         = the inverter
      slaves 200–204  = the stacks, served by the inverter itself from CAN data
```

### 1.1 Inside a stack

- Each stack has one **BCU** (battery control unit, the top box) and up to 8 modules. An SBR
  module is 20 cells, 64 V nominal.
- The BCU reads every cell through an **LTC681x** cell-monitor chain over isoSPI. Cell voltages
  are kept in 0.1 mV, temperatures in 0.01 °C.
- Each module carries an **EEPROM** with its 18-character S/N, its charge-current derating table
  and a persisted statistics block. Module history (SOC model state, cycle counter) therefore
  travels with the module.
- The BCU stores its parameters (thresholds, counters, the stack address) in FRAM.

### 1.2 CAN1 ID layout

Every CAN1 ID has three fields: **bits 10–8 = message class, bits 7–5 = BCU address n (0–3),
bits 4–0 = sub-index k**.

| Class | IDs | Direction | Content |
|---|---|---|---|
| 0 | 0x000–0x01F | master BCU → inverter | system frames: the per-stack layout aggregated over all stacks |
| 1 | 0x100–0x109 | inverter → BCU | inverter measurements, time, SOC window, acknowledgements (§1.6) |
| 1 | 0x1E0 | inverter → BCU | Modbus-RTU tunnel to the BCU service registers (§1.5) |
| 1 | 0x1D0 | inverter → BCU | Modbus-RTU tunnel for the stored-record export (run, event and fault logs, fault-upload packets), sent by the inverter |
| 1 | 0x1F0 | inverter → BCU | firmware-update protocol |
| 4, 5 | 0x400+0x20n…, 0x500+0x20n… | BCU → BCUs | peer status and the address/master heartbeat |
| 5 | 0x5F0 | tool ↔ BCU | a second Modbus-RTU-over-CAN tunnel |
| 6 | 0x600+r | BCU → BCUs | address claims during the master election |
| 7 | **0x700 + 0x20·n + k** | BCU → inverter | **per-stack data**, k = 0x00–0x1F |

Stack n's frames are 0x700–0x71F for n = 0, 0x720–0x73F for n = 1, and so on. The inverter
derives the stack index from `(ID & 0xFF) >> 5` and has room for five stacks (n = 0–4). The BCU
election hands out addresses 0–3, so at most four stacks run in parallel.

The per-stack frames, and which ones the inverter uses:

| k | Content | Inverter uses it for |
|---|---|---|
| 0x00 | u32 protection word, u32 warning word | 10667–10674, fault list 5448–5472 |
| 0x01 | charge/discharge voltage limit, charge/discharge current limit | **not parsed** per stack. The limits reach the inverter only through the master's system frame 0x001 (5635/5636) |
| 0x02 | SOC (0.01 %), SOH, remaining and rated energy | 10744, 10745 |
| 0x03 | lifetime charge / discharge energy (Wh) | 10746–10749 |
| 0x04 | stack voltage, current (+ = discharge), link voltage, temperature | 10741–10743 |
| 0x05 | operating state, request flags, master flag, model code, … | 10740, 10666, 10710 |
| 0x06 | cell temperature and voltage extremes | **not parsed** |
| 0x07 | BCU firmware version word, nominal energy, module count, BCU address | 10721–10735, 10665 |
| 0x08 | BCU (stack) S/N, multiplexed | 10711–10720, slave registration |
| 0x09 | BCU date | **not parsed** |
| 0x0A / 0x0B | "SUNGROW" / "SBRXXX" | **not parsed** per stack |
| 0x0D / 0x0E | BMS capability word / the app's self-description bitmasks (§3.2) | 10699 / 10701, 10705 |
| 0x0F | multiplexed statistics (8 sub-frames) | 10675–10698 |
| 0x12 | service-mode requests (only while a service mode is active) | the system copy 0x012 feeds the inverter's request channels (§2.12) |
| 0x13 / 0x14 | temperature / cell-voltage extremes with locations | 10757–10764 |
| 0x15–0x18 | per-module min and max cell voltage | 10765–10780 |
| 0x19 | contactor status, faulted-module bitmap, two more bytes | 10789, 10790 |
| 0x1A | per-module cell-type byte | 10781–10788 |
| 0x1B–0x1E | per-module manufacturing date code | 10791–10806 |
| 0x1F | module S/Ns, multiplexed | 10822–10893 |

The BCU sends the per-stack frames every second once the inverter has put it in "inverter mode
3". The inverter does that by setting 0x102 byte 7 = 1 when its handshake completes. The BCU
also sends the system frames 0x000–0x01F in that mode, but only from the master.

0x70D is not sent during a firmware-update session. The session starts when a CAN 0x1F0 update
frame arrives and ends with the next inverter frame 0x100–0x109 or after 600 s.

### 1.3 Battery detection

The inverter cycles through battery protocols every 3 s. For Sungrow it switches the CAN
prescaler to 24 (250 kbit/s: 42 MHz APB1 clock, 7 time quanta per bit; the BCU runs prescaler 12
with 14 time quanta, the same rate) and
sends the Sungrow handshake (0x101 time, 0x102 flags, 0x103 inverter S/N, 0x104 "SUNGROW", 0x105
identity, 0x106 SOC window). It locks onto protocol **'i'** when the master's system frames
0x007–0x00B have all arrived and 0x00A carries "SUNGROW". Only protocol 'i' fills the per-stack
records and serves slaves 200–204.

### 1.4 How the inverter serves slaves 200–204

The inverter does **not** forward battery register reads to the BCU. It answers them itself
from the latest CAN frames.

- **Binding a slave to a stack.** When stack n's S/N frame (0x708 + 0x20·n) arrives, the inverter
  looks the S/N up in a 5-slot registry. If the S/N is new, it takes the first free slot k and
  saves the registry. Slave **200 + k** is then that stack. The binding is by S/N and
  **first-seen order**, not by CAN address, and it survives reboots.
- **Going offline.** Every frame from a stack resets that stack's timeout counter. After **80
  ticks** without a frame the inverter marks the stack offline: 10740 = 0x7FFF, its 10665 capacity
  = 0, and in the same pass it clears the stack's slave slot (0xFF). The slave then
  stops answering until the stack registers again, so over RS485 the dongle sees timeouts and
  **never reads 10709 = 0**. The stacks send every second.
- **Reset.** Disabling the battery or changing protocol clears all stacks, the module S/Ns, the
  fault words and the statistics, and unbinds every slave.
- Inside 5200–12000, an address the inverter does not implement reads **0xFFFF**.

### 1.5 Modbus-over-CAN tunnel to the BCU (19100–19999)

One window is forwarded: wire addresses **19100–19999** (doc 19101–20000), addressed to slave
200–204 (or the inverter's own address) while the protocol is 'i'.

- The inverter rewrites the slave ID to the BCU's own Modbus address (stack index + 1), wraps the
  request as Modbus RTU with CRC, and streams it on CAN ID 0x1E0. The loop that retries up to 3
  times (5 ms apart) repeats a failed CAN transmit, not the Modbus request. The BCU
  answers on the same tunnel. A request addressed to the inverter's own address (slave 1) in this
  window is sent to the stack bound to slot 0. If the gate fails or the slot is unbound, there is no
  reply at all.
- This reaches the BCU's **service register map**. Its address decoding uses **wire** addresses,
  with one u32 per even address, low word first. Wire 19200 + 2·i is BCU parameter p[i]. Above
  that sit live values: cell extremes and locations, contactor status, the faulted-module
  bitmap, capacities in 0.01 Ah, lifetime Ah, per-module max/min cell voltage and per-module SOC.
  Wire 19898 is an indirect read selected by a write to wire 19716.
- Our battery table reads one register here: **doc 19939 = wire 19938 = p[0x171]**, the
  **discharge cut-off SOC** (`discharge_cutoff_soc`, 0.01 %, default 500 = 5.00 %).
  The next parameter, doc 19941 = p[0x172], is the **emergency charging SOC** (300 = 3.00 %); our
  firmware does not poll it. §2.12 explains both.
- The app lists both as holding registers (fc 3), writable at its "senior" access level. 19939
  accepts 0–5000 and must be ≥ 19941.

### 1.6 Inverter → BCU frames

| ID | Content |
|---|---|
| 0x100 | inverter-measured battery voltage (5630) and current (5631), inverter temperature (5008), inverter mode 0–3. Sent about 3 times in every 11 transmit slots of ≥ 250 ms |
| 0x101 | Unix time; sets the BCU clock. At most once an hour |
| 0x102 | handshake flags; byte 7 = 1 puts the BCU in inverter mode 3 |
| 0x106 | **SOC window**: min SOC (13059) and max SOC (13058), 0.1 %, plus request acknowledgements in byte 6 (§2.12) |
| 0x108, 0x109 | inverter flags |
| 0x1E0 | the Modbus tunnel (§1.5) |

### 1.7 CAN2 service bus

The BCU's second CAN bus is a J1939-style service bus with 29-bit IDs and source address 0xA1 +
n. It carries a data dictionary (data groups 0x01–0x45 on request), direct parameter read/write
by index, firmware and table-image transfer (PF 0x50–0x53), a bulk export of stored records, module
EEPROM and flash (PF 0x60–0x63), and a CAN self-test that cross-checks both buses. The BCU also
transmits about 40 kinds of periodic frames on it (stack values, per-module values, bulk
cell-voltage and temperature arrays). The inverter does not use it.

Naming note: the bus the BCU calls CAN1 is wired to the inverter peripheral the inverter firmware
calls CAN2. Both names refer to the same physical battery bus.

---

## 2. Battery operation

Everything in this section runs in the BCU unless it says "inverter". The inverter has no
balancing, SOC, contactor or per-module limit logic of its own. It copies the BMS limits through
to its power stage.

### 2.1 Measurements

- The BCU's 50 ms task aggregates the cell data into stack extremes: min/max cell voltage with
  location (module in the high byte, cell in the low byte), min/max temperature with location,
  stack voltage and current.
- Internally the current is positive when charging. The BCU **inverts the sign on transmit**, so
  10742 is **positive when discharging**, the same convention as the inverter's 5631.

### 2.2 SOC

- Each module has its own SOC model: coulomb counting, an OCV calibration at rest (an
  OCV-versus-temperature table) and capacity tracking. Module SOC is 10675–10682 (0.01 %).
- **Stack SOC** is blended from the lowest and highest module SOC (all in 0.01 %):

  | Condition | Stack SOC |
  |---|---|
  | highest module ≥ 95.00 % | the highest module SOC |
  | lowest module ≤ 6.00 % | the lowest module SOC |
  | otherwise | 600 + (min − 600) · 8900 / (8900 − (max − min)) |

  The stack therefore reads 100 % when its first module is full and 0 % when its first module is
  empty. The blend in the middle stretches the lowest module's SOC so that the two ends meet.
  Example: modules between 40.00 % and 50.00 % give 600 + 3400 · 8900 / 7900 = 44.30 %.
- Example: stack 201's modules range 95.64–97.56 %. The highest is ≥ 95 %, so the stack SOC is
  97.56 %, which 10744 shows as 97.5 %. Stack 200's modules are 95.00–95.06 %, giving 95.0 %.
- On CAN the BCU sends SOC in 0.01 % and sends **100.00 % once the blended SOC reaches 99.01 %**.
  The inverter divides by 10, so 10744 is in 0.1 %.
- The master BCU computes the **system SOC** across stacks with the same blend. That is the
  inverter's 5633 (example 97.5 %: stack 201's 97.5 % is ≥ 95 %, so it wins).
- When an internal BCU flag is set, the reported SOC is floored at (min SOC + 1 %).

### 2.3 SOC calibration

A module's SOC is recalibrated from its open-circuit voltage after a long rest:

- **When.** After at least **60 min** at rest (BCU parameter p[0x214]) with the module voltage stable within
  **5 mV**.
- **Threshold.** The low end of the OCV table, about **3.28 V** per cell at 25 °C.
- **Per-module state.** It takes only these values:

  | State | Meaning |
  |---|---|
  | 0 | no rest calibration since the current last exceeded 0.5 A |
  | 1 | only the module's lowest cells are at or below the threshold |
  | 3 | the module's highest cell is at or below the threshold: the whole module is at the low end. This also sets the capacity-learning anchor |
  | 6 | recalibrated above the low end |

- **Aggregate.** The BCU folds the module states into one byte, served as **10684
  (`soc_calibration_state`, "Low-SOC rest calibration")**:

  | Value | Condition | Published label |
  |---|---|---|
  | 5 | every module is in state 3 | All modules calibrated |
  | 1 | otherwise, any module is in state 1 or 3 | Some modules calibrated |
  | 2 | otherwise, any module is in state 2 or 4. Modules never enter those states, so this does not occur | Reserved |
  | 0 | otherwise | Inactive |

- Aggregates 1 and 5 start **capacity-mode balancing** (§2.6). Expect non-zero values rarely and
  briefly: only after a long rest near empty, and only until the current next exceeds 0.5 A.
- The per-module states are readable through the BCU service map (indirect code 0x1004, as
  nibbles).

### 2.4 SOH

Stack SOH is the **lowest module SOH**. 10745 is in whole percent (the inverter divides the BCU's
0.01 % by 100). The inverter's 5634 is the system SOH in 0.1 %. Example: 96 % / 99 %.

### 2.5 Cycles and lifetime counters

- **Throughput.** Every 50 ms the BCU adds |I|·dt to a charge or a discharge accumulator (A·ms),
  and V·|I|·dt to a charge or a discharge energy accumulator (W·s).
- **Lifetime Ah**: charge Ah → **10691/10692 (`lifetime_charge_ah`)**, discharge Ah →
  **10688/10689 (`lifetime_discharge_ah`)**. Example: 35328 / 3581 Ah charged and 35492 / 3476 Ah
  discharged.
- **Lifetime energy**: 10746/10747 (`total_charge_energy`) and 10748/10749
  (`total_discharge_energy`), 0.1 kWh. Example: 14306.9 / 1444.8 kWh charged and 13844.9 / 1362.4 kWh
  discharged, a round-trip energy ratio of about 96.8 % (older stack) and 94.3 % (newer stack).
- **Discharge/charge ratio** → **10687 (`discharge_charge_ratio`)** = discharge Ah / charge Ah ×
  10000, so 0.01 % per count. A ratio of 1.0007 or more is sent as **9990**, which marks "out of
  range" rather than 99.90 %. Example: 9990 on the older stack (35492 > 35328) and 9706 on the
  newer one (3476 / 3581 = 0.97068).
- **Cycles.** Each module counts its own throughput (charge + discharge) in its EEPROM. Every
  learned module capacity / 50 of throughput adds one count, so a full cycle (2 × capacity) is
  100 counts and the counter is in 0.01 cycle. The learned capacity is nominal × SOH.
  The stack counter is **overwritten on every pass with module 1's counter** and saved to FRAM.
  **10685/10686 (`cycle_count`)** = cycles × 10. Example: 723.5 and 70.6 cycles.
  - The older stack's Ah totals give (35492 + 35328) / 100 = 708.2 cycles at nominal capacity.
    723.5 / 708.2 = 1.0216, consistent with counting against a learned capacity of about 97.9 % of
    nominal. The newer stack's counter matches its Ah totals, (3476 + 3581) / 100 = 70.6,
    so its module 1 has learned close to nominal capacity.
- **Internal resistance → 10690 (`internal_resistance`, "Internal resistance", mΩ)**, 0.1 mΩ.
  The app calls it "Internal resistance of battery system" (point 58654). It is **a model, not a
  measurement**: the BCU takes 4.000 mΩ per cell
  plus 0.0005 mΩ per cycle, multiplies by the cell count (modules × 20) and sends the result
  / 100. That is cells · (40.00 + 0.005 · cycles) in 0.1 mΩ. Example: 523.4 mΩ (older stack) and
  484.2 mΩ (newer stack). Its only consumer is the CAN frame:
  neither the BCU nor the inverter uses it, and the SH10RS only serves it.

### 2.6 Balancing

Balancing bleeds high cells through resistors in the cell monitors. The BCU runs a balancing pass
at most every **240 s**, as long as balancing is enabled (parameter p[0x13F] ≠ 0, default 1).
**10683 (`balancing_status`)** shows the result: **0 idle, 1 capacity mode, 2 voltage mode**.

**Voltage mode** (value 2) is used when all modules in the stack have the same cell-type code
and voltage-mode balancing is enabled (p[0x24A] = 1, the default). A stack of six modules of one
cell type qualifies.

| Rule | Value |
|---|---|
| Start: no BMS fault, valid cell readings | required |
| Start: stack at rest | \|I\| < 0.51 A, and no current for **more than 29 min** |
| Stop: temperature spread | max − min module temperature ≥ 10 °C |
| Stop: too hot | max temperature > 44.99 °C |
| Stop: too cold | min temperature ≤ 0 °C |
| Which cells (min cell ≥ 3.1001 V) | bleed cells ≥ **min cell + 35.0 mV**; stop a cell below **min + 15.0 mV** |
| Which cells (min cell < 3.1001 V) | bleed cells ≥ **3.135 V**; stop a cell below **3.115 V** |
| Any bled cell | must be ≥ 3.100 V (p[0x238]) |
| Thermal guard | when a module's max temperature reaches 70 °C (p[0x134]), no new cells are added to its mask |

**Capacity mode** (value 1) works on charge rather than voltage. The BCU estimates each cell's
surplus charge against a reference cell (the lowest or highest, depending on the mode setting)
and bleeds the surplus. The threshold is p[0x141] = 300 in 0.01 % of capacity (capacity ×
300 / 10000 = 3.00 %). Capacity mode starts when the low-SOC rest calibration aggregate (10684,
§2.3) is 1 or 5, so it runs rarely and briefly: only after a long rest
near empty.

**Why balancing starts about 40 minutes after the battery reaches 100 %.** Charging stops at
100 %. If PV then covers the house load, the stack current stays below 0.51 A. After 30 minutes
at rest the next balancing pass (at most 4 minutes later) finds cells more than 35 mV above the
lowest cell and starts bleeding them. It stops each cell once it is within 15 mV of the lowest.
On SBR192 stacks that takes 30–70 minutes. LFP cells sit well above 3.1 V at full charge, so
the relative rule (min + 35 mV) applies. Any charge or discharge current above 0.51 A restarts
the 30-minute rest timer.

### 2.7 Current and voltage limits

**Per module.** The BCU looks up each module's charge limit in a 2-D **temperature × SOC derating
table** stored in that module's EEPROM (flash defaults if the EEPROM read fails). The discharge
limit comes from a similar per-module calculation.

**Per stack.** The stack limit is the **lowest module limit**. The BCU then forces it to 0 in
these cases:

| Limit | Forced to 0 when |
|---|---|
| Charge | a module is full; stack SOC = 100.00 %; a cell or pack over-voltage, over-temperature, cell-spread or similar alarm is at level ≥ 1; or any protection bit is set |
| Discharge | a module is empty; stack SOC = 0; a cell or pack under-voltage, over-temperature or similar alarm is at level ≥ 1; or any protection bit is set |

The stack limits go out per stack in frame 0x701 (0.1 A), which the inverter ignores. The master
sends the system limits in frame 0x001, which the inverter serves as:

- **5635 (`bms_max_charge_current`)**, A. The inverter forces it to **0 when the system SOC is
  100.0 %**.
- **5636 (`bms_max_discharge_current`)**, A. The inverter forces it to **0 when the system SOC is
  0**.

Example: 19 A charge (tapering near full) and 30 A discharge with two stacks.

**How the master combines the stacks:** with N stacks in the set and L the smallest per-stack
limit, the system limit is N × L, capped at 30.0 A (50.0 A with a BCU configuration flag set and
N > 1). Per-stack limits of 0.1–2.9 A
are floored to 3.0 A per stack. While a parallel connect is in progress the limit is at most
N × p[0x243] (6.0 A per stack); in rotation mode the charge limit is at most N × 3.0 A. The
example's 30 A discharge is the 30.0 A cap, and its 19 A charge is 2 × 9.5 A. The system SOC is
the highest stack SOC if that is ≥ 95.00 %, the lowest if that is ≤ 6.00 %, and
otherwise the blend of §2.2; only stacks in the best operating state count.

**Voltage limits.** The BCU's charge and discharge voltage limits are cells × 3.80 V and cells ×
2.00 V, the level-3 cell over- and under-voltage set points. The inverter receives the system
copy (0x001 bytes 0–3) but does not serve it.

### 2.8 Rated power and max discharge power

- **10696 (`rated_power`)** = modules × 64 V × 30 A. For an SBR192 that is 11520 W.
- **10697 (`max_discharge_power`)** = modules × 64 V × the module-limited discharge current
  (rounded to whole amps). It equals the rated power when no module derates, and **drops below
  10696 when the modules derate** (cold or hot cells, low SOC). Example: 11520 W on both.
- **10698 (`module_rated_capacity`)** = 5000 = 50.00 Ah, or 4000 = 40 Ah for the 40 Ah cell
  variant. Modules are in series, so this is also the stack's Ah capacity.
- **10665** (not polled) is the stack nominal energy in 0.01 kWh (1920 = 19.20 kWh). The inverter
  sums it over the stacks into **5639 (`battery_capacity`)**, example 38.40 kWh.

### 2.9 Protections

Each protection has three levels. Each level has its own set threshold, clear threshold and
debounce time, all BCU parameters. Defaults:

| Protection | L1 | L2 | L3 | Clears at |
|---|---|---|---|---|
| Cell over-voltage | 3.65 V | 3.70 V | 3.80 V | 3.50 / 3.62 / 3.62 V |
| Cell under-voltage | 2.70 V | 2.50 V | 2.00 V | 2.90 / 2.65 / 2.65 V (separate thresholds apply at low temperature) |
| Pack over-voltage (8-module defaults) | 584 V | 592 V | 608 V | — |
| Pack under-voltage (8-module defaults) | 432 V | 400 V | 320 V | — |
| Over-temperature (max cell temp) | 56 °C | 58 °C | 60 °C | 52 °C |
| Charge under-temperature (min temp while charging > 0.5 A) | −1 °C | −3 °C | −5 °C | +3 °C |
| Discharge under-temperature (min temp while discharging) | −21 °C | −23 °C | −25 °C | −17 °C |
| Temperature spread (max − min) | 16 °C | 18 °C | — | 13 °C |
| Cell voltage spread (max − min) | 500 mV | 600 mV | — | 400 mV |
| Over-current | 35 A | 40 A | 50 A | 25 A (also checked against the module-limited limit + 5 A) |

- The pack limits are stored for 8 modules (they equal the cell levels × 160 cells).
- Level 1 sets a **warning** bit and, for the alarms listed in §2.7, zeroes the matching current
  limit. Levels 2 and 3 set **protection** bits; any protection bit zeroes **both** limits.
- In cold weather the derating tables cut charge current as the cells cool. Charge
  under-temperature is checked only while charging: −1 °C raises a warning, and −3 °C sets
  protection bit 3, which zeroes both limits. Discharge
  under-temperature warns at −21 °C and protects at −23 °C. Voltage-mode balancing stops at 0 °C.
- `cell_voltage_spread` (derived from 10757 − 10759) is the quantity behind the 500 mV spread
  alarm. Example: about 3 mV.
- **Protection event counters.** Every time a protection `id` enters a non-zero `level`, the BCU
  increments p[0x18E + 3·(id − 1) + (level − 1)].
- **Deep discharge count → 10693 (`deep_discharge_count`)**, the app's "Deep discharge count"
  (point 58656). The BCU sends p[0x1A7] + p[0x1AA]:
  - p[0x1AA] counts entries of protection 10 (cell under-voltage) into level 2, i.e. min cell ≤
    2.50 V. A drop back from level 3 to level 2 also counts.
  - p[0x1A7] is protection 9 level 2, the other under-voltage slot. Nothing raises protection 9,
    so it stays 0.

### 2.10 State machine, contactors and precharge

The operating state is **10740 (`operation_status`)**:

| Value | State | What happens |
|---|---|---|
| 0 | Idle | outputs enabled, moves to Standby |
| 1 | Standby | limits 0, contactors open; checks precharge and faults, then moves to Running, Fault or Shutdown |
| 2 | Running | closes the contactors on command and runs the checks |
| 3 | Fault | limits 0, contactors open; back to Standby after 10 s without a level-3 fault, or to Shutdown when certain faults persist (3 s to 15 min depending on the fault) |
| 4 | Shutdown | contactors open |
| 5 | Update | firmware update |
| 6 | Correction | calibration |
| 7 | Test | test mode |
| 0x7FFF | offline | set by the inverter after the stack's timeout (§1.4) |

- **Closing** runs four relay outputs in order: relay 3 on, 50 ms, relay 0 on, 500 ms, relay 1
  on, 500 ms, relay 2 off. **Opening**: relay 0 off, 500 ms, relay 1 off, 50 ms, relay 3 off, then
  relay 2 off.
- **Precharge acceptance** compares the stack voltage with the link (inverter-side) voltage. The
  difference must be within **5.0 V** (p[0x212]), or **25.0 V** (p[0x274]) for the parallel-stack
  window, with the current within ±3 A.
- **10789 (`dc_contactor_status`)**: 0 open, 1 contactor fault, 2 closed, 3 contactor fault (two
  different fault sources).

### 2.11 Parallel stacks

- **Master election.** At start-up every BCU draws a random number 400–699 and sends address
  claims on CAN1. The lowest number, or a BCU that is alone, becomes **master with address 0**;
  the others take the lowest free address 1–3. The address is saved in the BCU.
- Every BCU keeps a copy of its peers' data from their 0x5xx/0x7xx frames.
- **One stack at a time.** The master compares the stacks' voltages and SOCs and lets **one stack
  at a time** close its contactors, using target/command bytes in its heartbeat. The voltage
  windows are the same 5.0 V and 25.0 V parameters as precharge.
- Only the master sends the system frames (0x000–0x01F) that carry the system SOC, SOH, limits,
  energy totals and the system fault word.
- The Modbus slave number (200, 201, …) comes from the inverter's S/N registry (§1.4). It does
  not follow the BCU address or the master role.

### 2.12 SOC window, low-SOC warning and request channels

- **SOC window.** The inverter sends its min SOC (**13059 `min_soc`**, default 5.0 %) and max SOC
  (**13058 `max_soc`**, default 100.0 %) in frame 0x106. The BCU accepts a lower limit up to 50.0 %
  and an upper limit of 50.0–100.0 %, and stores both.
- The BCU has two SOC thresholds of its own:
  - **Discharge cut-off SOC**, p[0x171] = **5.00 %** (19939, `discharge_cutoff_soc`). Below it the
    BCU sets "discharging not allowed" (10666 bit 1).
  - **Emergency charging SOC**, p[0x172] = **3.00 %** (19941, not polled). At or below it the BCU
    sets "discharging not allowed" and requests a forced charge (bits 1 and 2, flags 6).
- **24 h low-SOC warning** (inverter). While the system SOC is at or below the min SOC (plus some
  inverter state conditions), the inverter counts seconds. At **86 400 s (24 h)** it raises an
  inverter warning. Any break in the condition resets the count and clears the warning.
- **Request channels** (inverter). The master's system frame 0x012 carries two request channels,
  each with an enable bit and a state 0/1/2, plus two values. The inverter latches the requests,
  marks them done, and acknowledges them in 0x106 byte 6. 0x012 is the system copy of the BCU's
  service-mode frame 0x712 (service mode 2 drives channel A, modes 1/3 channel B). Neither value
  is served on a register our firmware reads.

### 2.13 BMS request flags (10666)

**10666 (`bms_request_flags`)** is the second byte of the stack status frame 0x705:

| Bit | Meaning | Our derived entity |
|---|---|---|
| 0 (0x01) | charging not allowed (charge limit 0) | `charge_prohibited` ("Charging not allowed") |
| 1 (0x02) | discharging not allowed (discharge limit 0, SOC below the discharge cut-off SOC, link down, …) | `discharge_prohibited` |
| 2 (0x04) | forced-charge request (also set at or below the emergency charging SOC, §2.12) | `forced_charge_request` |
| 4 (0x10) | mode-dependent | — |

While the stack is running with a low cell (1.8 V ≤ min cell < 2.70 V, or an under-voltage alarm),
the BCU sets bits 1 and 2 together: discharge stops and it asks for a forced charge. Both clear
when the min cell reaches 3.0 V or the max cell exceeds 3.65 V.

### 2.14 Fault and alarm words

Each stack sends a 32-bit **protection word** and a 32-bit **warning word** in frame 0x700.

**As raw text (10667–10674).** The inverter serves the eight bytes as hex text, one byte per
register: 10667–10670 are the protection word, 10671–10674 the warning word, least significant
byte first. Within each register the **high byte is the byte's low nibble**, so byte 0x54 reads as
"45". To decode, swap the two characters of each register, parse them as hex, and assemble the
bytes little-endian. With no fault it reads "0000000000000000".

**As fault codes (5448–5472).** The inverter turns the bits into Sungrow codes (at most 10) and
serves them per stack as a word block: 5448 = 0, 5449 = flags (0x100 = a fault is present, 0x80 = an
alarm is present), 5450–5451 = 0, 5452 = count, then 10 pairs of (code, class) at 5453 + 2i /
5454 + 2i for i = 0–9, ending at 5472. Class 25 is a fault, 24 an alarm, and 0 a code in neither list (840/841). Our
firmware decodes this block into `active_fault_count`, `active_fault_codes` and `first_fault`, for
example "732 fault, 933 alarm", or "No active faults", and each stack's Home Assistant device
gets the Problem binary sensor ([`inverter_registers.md`](inverter_registers.md) §3).

Protection word → code:

| Bit | BCU meaning | Code |
|---|---|---|
| 0 / 31 | cell or pack over-voltage, level 2 / level 3 | 732 |
| 1 / 30 | under-voltage, level 2 / 3 | 739 |
| 2 / 29 | over-temperature, level 2 / 3 | 733 |
| 3 | under-temperature, level ≥ 2 | 734 |
| 4 / 23 | over-current, level 2 / 3 | 735 |
| 5 | a second current protection, level 2 | 735 |
| 6 | internal BMS check, level 3 | 741 |
| 17 / 18 | — | 840 / 841 |
| 19 | internal BMS check, level 3 | not mapped by the inverter |
| 20 / 21 / 22 | internal BMS fault bytes, level 3 | 743 / 744 / 745 |
| 24 | temperature-spread (or related), level 3 | 746 |
| 27 | contactor fault, level 3 | 747 |
| 28 | internal BMS check, level 3 | 833 |

Warning word → code:

| Bit | BCU meaning | Code |
|---|---|---|
| 0 / 1 | over-/under-voltage level 1 (by analogy with the protection word) | 932 / 939 |
| 2 | over-temperature, level 1 | 933 |
| 3 | under-temperature, level 1 | 934 |
| 4 / 5 | over-current, level 1 | 935 |
| 6 | temperature-spread (or related), level 1–2 | 964 |
| 8, 15, 16, 17 | — | 937, 940, 941, 942 |
| 12 / 13 / 19 | — | 741 / 740 / 742 |
| 18 | a module flag | not mapped by the inverter |

The master's system word (frame 0x000) is mapped separately into four inverter flag words,
served at 13070–13079 (§4).

**10790 (`fault_module_id`, "Faulted modules (bitmap)")** is a **bitmap**, not an index: bit
m−1 is set for every module named by an active over-/under-voltage or temperature alarm. 0 means
no module is flagged.

### 2.15 The one-off write to BCU register 19759

The inverter sends one Modbus write per stack through the CAN tunnel when all of these hold:

- the protocol is 'i';
- byte 6 of the master's system frame 0x007 equals 7. In the per-stack layout that byte is the
  module count, so this may restrict the write to 7-module systems;
- the system SOC is not 100.0 %, the battery voltage is at least 502.0 V and the battery is charging
  at 0.3 A or more.

The write is fc 0x10 to wire 0x4D2E (doc 19759), 2 registers, value u32 **100000**. A per-stack flag
stops it from repeating until the condition drops.

The BCU decodes this range by wire address, so wire 19758 lands on **p[0x117]**, a capacity
parameter in A·ms (default 180 000 000 = 50 Ah, range 108 000 000–216 000 000). 100000 is outside
that range. On 6-module stacks the condition does not hold. Our firmware never sends this write.

---

## 3. Register reference: battery slaves 200–204

All registers are fc 4 (input). The stacks are dev_type 44 in our firmware. "CAN" names the
per-stack frame for stack 0 (add 0x20·n for stack n) and the payload bytes dN. "—" in the key
column means the row is polled for iSolarCloud but has no entity.

### 3.1 Not served by SH10RS firmware SUNSTONE-H_01011.02.69

| doc | Notes |
|---|---|
| 10601–10664 | The inverter serves **0xFFFF** for the whole range. The stock poll table has rows at 10628–10633, 10635, 10650–10663; u16 rows read absent, s16/s32 rows read −1, the string reads empty. `status_word_10632` and `status_word_10633` are kept as hidden entities only because stock uploads them to iSolarCloud. No BCU source exists |
| 10752–10756 | 0xFFFF |
| 10807–10820 | 0xFFFF (10821 is a constant 0). The stock rows at 10811–10819 therefore never have a value |
| ≥ 10894 | 0xFFFF |

### 3.2 Served registers

| doc | Meaning | Type, scale | CAN | Key | Example 200 / 201 |
|---|---|---|---|---|---|
| 5448–5472 | per-stack fault list (§2.14) | words | 0x700 | `active_fault_count`, `active_fault_codes`, `first_fault` (derived) | none / none |
| 10665 | stack nominal energy | u16, 0.01 kWh | 0x707 d4d5 / 10 | not polled | (1920; Σ = 5639) |
| 10666 | BMS request flags (§2.13) | u8 | 0x705 d1 | `bms_request_flags` (+ `charge_prohibited`, `discharge_prohibited`, `forced_charge_request`) | 0 / 0 |
| 10667–10674 | protection word (10667–10670) and warning word (10671–10674) as hex text (§2.14) | 2 ASCII chars per byte | 0x700 | — | "0000000000000000" |
| 10675–10682 | module 1–8 SOC | u16, 0.01 % | 0x70F mux 5–7 | `module_N_soc` | 95.06–95.00 / 97.56–95.64 %; 7–8 absent |
| 10683 | balancing: 0 idle, 1 capacity mode, 2 voltage mode (§2.6) | u8 enum | 0x70F mux 4 d7 | `balancing_status` | 0 / 0 |
| 10684 | Low-SOC rest calibration: 0 Inactive, 1 Some modules calibrated, 2 Reserved, 5 All modules calibrated (§2.3) | u8 enum | 0x70F mux 4 d6 | `soc_calibration_state` | 0 / 0 |
| 10685/10686 | cycle count: module 1's counter, one count per learned capacity / 50 (§2.5) | u32 LWF, 0.1 | 0x70F mux 4 d2–5 | `cycle_count` | 723.5 / 70.6 |
| 10687 | lifetime discharge Ah / charge Ah; 9990 = out of range (§2.5) | u16, 0.01 % | 0x70F mux 3 d6d7 | `discharge_charge_ratio` | 99.90 (sentinel) / 97.06 % |
| 10688/10689 | lifetime discharge throughput | u32 LWF, Ah | 0x70F mux 3 d2–5 | `lifetime_discharge_ah` | 35492 / 3476 |
| 10690 | internal resistance of the battery system, a cycle-aged model: cells · (4.000 + 0.0005 · cycles) mΩ (§2.5) | u16, 0.1 mΩ | 0x70F mux 2 d6d7 | `internal_resistance` | 523.4 / 484.2 mΩ |
| 10691/10692 | lifetime charge throughput | u32 LWF, Ah | 0x70F mux 2 d2–5 | `lifetime_charge_ah` | 35328 / 3581 |
| 10693 | deep discharge count: entries into cell under-voltage level 2 (min cell ≤ 2.50 V), p[0x1A7] + p[0x1AA] (§2.9) | u16 | 0x70F mux 1 d6d7 | `deep_discharge_count` | 0 / 0 |
| 10694 | hours spent charging (> 0.5 A) under a charge under-temperature alarm (level ≥ 2) | u16, h | 0x70F mux 1 d4d5 | `charge_undertemp_alarm_hours` | 0 / 0 |
| 10695 | hours spent under an under-temperature alarm (level ≥ 2) | u16, h | 0x70F mux 1 d2d3 | `undertemp_alarm_hours` | 0 / 0 |
| 10696 | rated power = modules × 64 V × 30 A | u16, W | 0x70F mux 0 d6d7 | `rated_power` | 11520 / 11520 |
| 10697 | max discharge power, falls with module derating | u16, W | 0x70F mux 0 d4d5 | `max_discharge_power` | 11520 / 11520 |
| 10698 | rated capacity | u16, 0.01 Ah | 0x70F mux 0 d2d3 | `module_rated_capacity` | 50.00 Ah |
| 10699/10700 | BMS capability word, BCU constant 0x0F. 0x70D carries 0F 00 00 00 0F 00 00 00; the inverter keeps the first u32. Read aligned: a read starting at 10700 returns 0xFFFF | u32 LWF | 0x70D d0–3 | `bms_capability_word` (hidden) | 15 / 15 |
| 10701 | self-description bitmask for the app's read-write groups, BCU constant 15; mask 1 = 19939/19941 shown. The stock row types 10701–10704 as a string, so it reads empty | u16 | 0x70E d2d3 | — | absent |
| 10702–10704 | constant 0 | | | | |
| 10705 | self-description bitmask for the app's read-only groups, BCU constant 7: mask 1 basic info, 2 contactor/module data, 4 life/chemistry. Stock types 10705–10708 as a string | u16 | 0x70E d0d1 | — | absent |
| 10706–10708 | constant 0 | | | | |
| 10709 | stack online: 1 after the S/N frame; the slave goes silent on the timeout, so 0 is never read over RS485 | u8 | — | `stack_online` | 1 / 1 |
| 10710 | model code 0x20E6 + (modules − 2); 0x20EA = 6 modules = SBR192 | u16 | 0x705 d3d4 | — (feeds the device model) | 8426 / 8426 |
| 10711–10720 | stack (BCU) S/N | string | 0x708 | `serial_number` | `S` + 10 digits |
| 10721–10735 | BCU firmware version, "SBRBCU-S_22011.%02x.%02x" from the 0x707 version word | string | 0x707 d0–3 | `firmware_version` | SBRBCU-S_22011.01.30 |
| 10736/10737 | battery protocol code "10AA" (an inverter constant, equal to the inverter's 2534/2535) | u32 LWF | inverter | `protocol_code` (hidden) | "10AA" (ASCII) |
| 10738/10739 | battery protocol version 0x01000800 = V1.0.8 (inverter constant) | u32 LWF | inverter | `protocol_version` (hidden) | 0x01000800 |
| 10740 | operating state (§2.10) | u16 enum | 0x705 d0 | `operation_status` | 2 / 2 (Running) |
| 10741 | stack voltage | u16, 0.1 V | 0x704 d0d1 | `battery_voltage` | 401.6 V |
| 10742 | stack current, **+ = discharge** | s16, 0.1 A | 0x704 d2d3 | `battery_current` | +2.5 / +3.3 A |
| 10743 | stack temperature | s16, 0.1 °C | 0x704 d6d7 | `battery_temperature` | 27.2 / 26.8 °C |
| 10744 | stack SOC (§2.2) | u16, 0.1 % | 0x702 d0d1 / 10 | `battery_soc` | 95.0 / 97.5 % |
| 10745 | stack SOH = lowest module SOH | u8, 1 % | 0x702 d2d3 / 100 | `battery_soh` | 96 / 99 % |
| 10746/10747 | lifetime charge energy | u32 LWF, 0.1 kWh | 0x703 d0–3 / 100 | `total_charge_energy` | 14306.9 / 1444.8 kWh |
| 10748/10749 | lifetime discharge energy | u32 LWF, 0.1 kWh | 0x703 d4–7 / 100 | `total_discharge_energy` | 13844.9 / 1362.4 kWh |
| 10750 | the **inverter's** own Modbus address | u8 | inverter | `inverter_address` (hidden) | 1 / 1 |
| 10751 | the **inverter's** model class (3 or 4, from its device type) | u16 | inverter | `inverter_model_class` (hidden) | 3 / 3 |
| 10757 | highest cell voltage | u16, 0.1 mV | 0x714 d2d3 | `cell_voltage_max` | 3.3503 / 3.3505 V |
| 10758 | its location: module (high byte), cell (low byte) | u16 | 0x714 d0d1 | `cell_voltage_max_position` (+ derived `_module`, `_cell`) | 784 = M3 C16 / 518 = M2 C6 |
| 10759 | lowest cell voltage | u16, 0.1 mV | 0x714 d6d7 | `cell_voltage_min` | 3.3476 / 3.3481 V |
| 10760 | its location | u16 | 0x714 d4d5 | `cell_voltage_min_position` | 1546 = M6 C10 |
| 10761 | highest temperature | s16, 0.1 °C | 0x713 d6d7 | `module_temperature_max` | 29.6 / 29.0 °C |
| 10762 | its location: module (high byte), sensor (low byte) | u16 | 0x713 d4d5 | `module_temperature_max_position` | 1538 = M6 S2 |
| 10763 | lowest temperature | s16, 0.1 °C | 0x713 d2d3 | `module_temperature_min` | 26.3 / 25.8 °C |
| 10764 | its location | u16 | 0x713 d0d1 | `module_temperature_min_position` | 513 = M2 S1 / 257 = M1 S1 |
| 10765–10772 | module 1–8 highest cell voltage | u16, 0.1 mV | 0x715–0x718, 2nd u16 per module | `module_N_cell_voltage_max` | 3.3497–3.3503 V; 7–8 absent |
| 10773–10780 | module 1–8 lowest cell voltage (0xFFFF without a valid cell) | u16, 0.1 mV | 0x715–0x718, 1st u16 per module | `module_N_cell_voltage_min` | 3.3476–3.3489 V |
| 10781–10788 | module 1–8 cell-type code = character 5 of the module S/N (1 if the EEPROM read failed, 0 if no module; a 0 slot has its type code, cell voltages and SOC cleared, so it gets no entities) | u8 (ASCII) | 0x71A byte m | `module_N_cell_type_code` | 68 'D' ×6 / 75 'K' ×6; 7–8 = 0 |
| 10789 | DC contactor: 0 open, 1 fault, 2 closed, 3 fault | u8 enum | 0x719 d0 | `dc_contactor_status` | 2 / 2 |
| 10790 | faulted-module bitmap (bit m−1 = module m) | u8 | 0x719 d1 | `fault_module_id` | 0 / 0 |
| 10791–10806 | module 1–8 manufacturing date code (below). Read aligned: the second register of each pair returns nothing | u32 LWF | 0x71B–0x71E | `module_N_date_code` (hidden) + derived `module_N_manufacturing_date` | 2023-09-23 / 2025-11-11 |
| 10822–10893 | module 1–8 S/N, 18 characters (9 registers each) | string | 0x71F | `packN_serial_number` | EM032D6239230112DF… / EM032K725B110548KC… |
| 19939/19940 | discharge cut-off SOC, BCU p[0x171] via the CAN tunnel (§1.5, §2.12); fc 3 holding in the app | u32 LWF, 0.01 % | 0x1E0 tunnel | `discharge_cutoff_soc` | 5.00 / 5.00 % |

**Module manufacturing date.** The app names these registers "Pack N manufacturing date" (points
58823–58830). The BCU builds the value from the module S/N characters (0-based) as
10⁶·(c7c8) + 10⁴·m + 100·(c10c11) + (c6 − '0'), where m is c9 read as a hex digit (A–C = months
10–12). The value is 0xFFFFFFFF if any of those characters is below '0'. Our firmware publishes
`module_N_manufacturing_date` as "YYYY-MM-DD" and treats 0, 0xFFFFFFFF, a month outside 1–12 or a
day outside 1–31 as invalid; the raw `module_N_date_code` entities are hidden.

- "EM032D6**239230**…" gives 23092306 → **2023-09-23**, and "EM032K7**25B110**…" gives 25111107 →
  **2025-11-11**.
- The stack S/N encodes the BCU's date (`S` YYMMDD + 4 digits), which can differ from its
  modules' dates.

---

## 4. Inverter-side battery registers (slave 1)

| doc | Key | Meaning | Example |
|---|---|---|---|
| 5214/5215 | `battery_power` | battery power, s32 W, + = discharge | 1459 W |
| 5630 | `battery_voltage` | battery voltage measured by the inverter, 0.1 V (also sent to the BMS in 0x100) | 401.5 V |
| 5631 | `battery_current` | battery current, s16 0.1 A, + = discharge | 5.2 A |
| 5632 | `battery_temperature` | 0.1 °C | 27.0 °C |
| 5633 | `battery_soc` | **system** SOC (the master's blend over stacks), 0.1 % | 97.5 % |
| 5634 | `battery_soh` | system SOH, 0.1 % | 96 % |
| 5635 | `bms_max_charge_current` | system charge limit from 0x001, A; **forced to 0 at 100.0 % SOC** | 19 A |
| 5636 | `bms_max_discharge_current` | system discharge limit from 0x001, A; **forced to 0 at 0 % SOC** | 30 A |
| 5637 | `bms_total_discharge_energy` | not served by SUNSTONE-H_01011.02.69 | absent |
| 5639 | `battery_capacity` | 0.01 kWh: holding 33049 when non-zero, else Σ stacks' 10665, else 0xFFFF | 38.40 kWh |
| 5669 | `daily_battery_discharge_energy` | 0.1 kWh | 13.3 kWh |
| 5670/5671 | `total_battery_discharge_energy` | with SBR: the BMS system total (0x003), full u32, 0.1 kWh | 15207.4 kWh (= 13844.9 + 1362.4) |
| 5672 | `daily_battery_charge_energy` | 0.1 kWh | 22.2 kWh |
| 5673/5674 | — | **inverter bug**: 5673 serves only the **low 16 bits** of the total charge, and 5674 is never written | 2644.6 kWh (low word of 15751.7) |
| 13041/13042 | `total_battery_charge_energy` | the **full** u32 total charge, 0.1 kWh, from the same field. Our firmware reads the total from here on protocol-3 inverters | 15751.7 kWh (= 14306.9 + 1444.8) |
| 13020–13025 | (:502 export) | the same fields as 5630–5634 | |
| 13039 | `battery_capacity_sh` | **system nominal energy, 0.1 kWh** (0x007 d4d5 / 100, no further scaling). Our firmware polls it only for protocol type 5, so it has no value on the SH10RS | 384 = 38.4 kWh |
| 13058 | `max_soc` (holding) | max SOC sent to the BMS in 0x106, 0.1 % | |
| 13059 | `min_soc` (holding) | min SOC sent to the BMS in 0x106, 0.1 %; also the 24 h low-SOC warning threshold | |
| 13070–13079 | — | four inverter flag words built from the master's system fault/alarm word: 13070/71, 13072/73, 13074/75, 13078/79 (13076/77 = 0). Not polled; the stock :502 export maps them to battery rows 10811–10819, which SUNSTONE-H_01011.02.69 does not serve | |
| 33047 / 33048 | `battery_max_charge_power` / `battery_max_discharge_power` (holding) | the inverter's own charge/discharge power caps, ×10 W | |
| 33049 | — (holding) | battery capacity override that 5639 serves when non-zero | |

