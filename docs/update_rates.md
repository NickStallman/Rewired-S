# Live-data update rates

How fresh the values are, end to end (inverter → RS485 → SignalStore → MQTT / web UI /
Modbus-TCP): the tiered poll scheduler, what it achieves on real installations, and what the
inverter and the battery controller can deliver at most.

The numbers below are from two installations:

- **Hybrid**: an SH10RS with two SBR192 battery stacks on a WiNet-S2. The inverter is Modbus
  slave 1, protocol 3 "AA10", table `inv_proto3`; the stacks are slaves 200/201, table `battery`,
  which the inverter serves from the battery controller's CAN frames.
- **String**: an SG5.0RS on a WiNet-S. Slave 1, protocol 3 "AA10", table `inv_proto3`.

Both buses run at 9600 8N1 (`rs485_baud_rate` in `boards/winet_s*.yaml`, `common/rs485.yaml`).
One character takes 1.042 ms, a read request is 8 B (8.3 ms) and a reply of n registers is
5 + 2n B (a 120-register reply is 245 B, 255 ms).

## Summary

| Signal class | Period (both installations) | Inverter-side floor |
|---|---|---|
| **Fast**: PV W, AC W/V/Hz, grid/meter W, load W, battery W, operating state; per stack V/A/T/SOC/SOH | **1.0 s** | AC/battery/load/energy ≈ 0.6–0.8 s on the SH10RS, ≈ 0.4–0.5 s on the SG5.0RS (power-stage block rotation); meter ≈ 0.1–0.2 s; SBR stack 1 s (battery controller CAN pass). On the SH10RS in EMS mode 0/8 the power-flow registers hold ≈ 40 s means refreshed every 0.6–0.8 s; the meter block (5601+) is the responsive grid-power source (§7) |
| **Live**: MPPT V/A, the inverter's battery V/A/T/SOC/SOH rows, states, faults, flags, limits, energies, backup port, meter detail, power flow | **5.0 s** | MPPT V/I and battery V/I are 4.9 s means inside the inverter (§7) |
| **Slow**: settings, battery status/module SOC/balancing, SBR cell block, cloud-only blocks | **30 s** | some SBR fields 10 s |
| **Static**: identity, versions, ratings, serials, capability bitmap, and every read that returns no values | **5 min** | – |
| MQTT state publish | on change beyond the signal's deadband (`components/sungrow_mqtt/deadband.py`: 0.5 V (cells 0.002 V), 0.2 A, 10 W, 0.01 kW, 0.03 Hz, 0.3 °C/%, 0.02 PF; counters/enums/strings on every change), ≤ 1/s per signal, 60 s forced refresh | – |
| Freshness marker | `/api/devices` `poll.fresh_age_ms` (shown as "data age" on each device card) | – |

Bus load at 9600 baud (§4): hybrid ≈ 58 % (fast set ≈ 21 % inverter + 2 × 4.7 % stacks), string
≈ 30 %. The rest stays free for ad-hoc reads (Modbus-TCP, web UI, `sungrow_control` read-backs) and
writes.

**Compared with the stock dongle**, which in steady state reads the inverter's deadbanded fn 0x65
change stream, Rewired-S reads every value with plain fn 3/4 reads on fixed tiers, so a fast value
is never more than 1 s old.

## 1. Refresh tiers

Source: `components/sungrow_modbus/poll_plan.{h,cpp}`.

Every table row gets one of four tiers (YAML `poll_periods`, defaults below):

| Tier | Period | What |
|---|---|---|
| fast | 1 s | power flow: PV, grid/meter, house load, battery, AC output, operating state; SBR stack V/I/T/SOC/SOH |
| live | 5 s | the rest of the live measurements: MPPT V/I, the inverter's battery V/I/T/SOC/SOH rows, states, faults, flags, limits, energies, backup port, meter detail, power flow |
| slow | 30 s | settings, slow or cloud-only blocks, SBR status/module SOC/balancing/cycles, SBR cell block |
| static | 5 min | identity, versions, ratings, serials, capability bitmap; any read that returns no values |

The tier comes from curated ranges per table and address; a row no range covers takes its `cycle`:
≤ 5 s live, ≤ 60 s slow, longer static (meter rows at 100 ms are fast: a standalone meter is the
grid reading).

Ranges of the private map (`inv_proto2` / `inv_proto3`):

| Read | Tier | Contents |
|---|---|---|
| 5214+39 | fast | battery/export/load power, device state, yields, active/reactive/DC/apparent power, PF, Hz, bus V, temperature, insulation, phase V/I |
| 5601+8 | fast | meter power (total + phases) |
| 5333+8 | live | MPPT 1–4 V/I (its own read: the string blocks on either side are slow) |
| 5253–5332, 5341–5372 | slow | strings 1–40 and MPPT 5–20 (unserved on the SH10RS and SG5.0RS, so they run at the static period) |
| 5448+72 | live | fault list, status words, function flags, DRM, limits, power-limit reason, AFCI count |
| 5609+73, 13041+2 | live | import/export energies, battery V/I/T/SOC/SOH, BMS limits, PV/battery/AC energies, self-consumption |
| 5719+81 | live | backup port, meter frequency/reactive/phase V/I |
| 6099+1 | live | power-flow flags |
| 4950–5044, 5374–5447, 5520+60, 5800–5967, 7100–7815, 8801+120 | static | versions, ratings, serial, capability bitmap, version strings, per-cluster arrays |
| 5968–6098, 6200–6203, 6303–6501, 6950, 7816–8215, 10451+8, 10752+5, fn 3 31225, 31452–31461 (settings) | slow | status words, fans, cloud-only source block, ground voltages, battery address list |

The public-map tables have their own fast ranges: `inv_proto1_4` 5003–5038 and 5083–5092 (meter and
load power), `inv_proto5` 5003–5038 and 13000–13034.

SBR stack (`battery`): fast 10740+6 (operation status, V, I, T, SOC, SOH); slow 5448+25 (fault
list), 10628–10699 and 10709–10710 (status, flags, module SOC, balancing, cycles, stack online),
10746+4 (lifetime energy), 10789+2 (contactor, faulted modules) and the cell block 10757+24
(`sbr_interval`, 30 s); static the identity, 10700–10708, module dates/serials and 19939+2 (a
setting the inverter tunnels to the battery controller).

`build_poll_plan()` groups the rows of one tier into reads in address order: at most 120 registers,
at most 16 unlisted registers bridged (one register costs ~2 ms of wire, a separate read ~37 ms of
overhead), an end-of-request row ends its read, and rows with sigid 0 (gap fillers) are skipped.
Read boundaries follow the tiers, not the stock table order.

## 2. Scheduler

Source: `components/sungrow_modbus/master.{h,cpp}`.

- **Slots.** Each read has fixed slots `offset + k × period` on the ms clock. The fast reads of every
  device share offset 0, so each period yields one coherent snapshot (PV, grid, load and battery
  from the same second). The reads of every other tier are spread evenly over their period, offset
  per device: the bus carries a steady trickle (on the hybrid one live read every ~0.8 s, one slow
  read every ~1.2 s, one static read every ~8 s) instead of bursts. A read sent late keeps its grid;
  one more than half a period late skips to the next slot.
- **Priority.** Writes, then due fast reads (earliest slot first), then ad-hoc reads (Modbus-TCP
  forwards, web UI, cloud getparam, `sungrow_control` read-backs) in arrival order, the inverter
  search, discovery / offline probes / the inverter-type read, the initial pass, and the remaining
  due reads (lowest tier, then earliest slot).
- **Fast reservation.** Anything but a fast read or a write starts only if its expected bus time
  (gap + request + reply + 25 ms) ends before the next fast slot; a read that does not fit lets a
  smaller one go first. After 2 s of holding back the check is waived, so nothing starves. Jitter of
  the fast set: < 30 ms.
- **Initial pass.** After start and after every reconnect each read is sent once: the capability
  bitmap first (gated rows decode against it), then the fast reads, then the rest.
  `first_round_done()` turns true when all have answered or timed out; the MQTT publisher waits for
  it. Hybrid ≈ 20 s, string ≈ 10 s.
- **Unserved reads.** A read whose answer holds no valid value (every row absent, gated off by the
  capability bitmap, or not kept by the store's row filter) or that the slave rejects with an
  exception runs at the static period until it carries a value again (an empty fault list counts as
  a value). This is model-agnostic: on the hybrid 15 of 71 inverter reads drop out (strings, unfitted
  MPPTs, absent arrays and settings), on the string inverter 35 of 71 (no battery, meter or backup
  port).
- **Timeouts.** Cyclic reads, offline probes and discovery: wire time + `poll_timeout_margin`
  (200 ms; ≈ 460 ms for a 120-register read). Reads of ranges the inverter forwards to its power
  stage or tunnels to the battery controller: `tunnel_timeout` (2 s); the inverter ranges are doc
  16000–17500, 29500–29899 and 36450–38999 for every inverter, the battery range is 19101–20000
  ([SG-Docs: SH10RS register windows](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/register-windows.md)).
  Inverter type, ad-hoc and verify reads: `response_timeout` (1 s). Writes: `write_timeout` (2 s).
  No retransmit.
- **Offline.** `offline_after` (10) consecutive failures that also span `offline_silence` (10 s); with
  short timeouts a brief silence does not drop a device. While offline, one probe every 15 s.
- **Freshness.** `Device::fresh_ms()` is the last answer of the device's fastest tier that carries
  values; `/api/devices` reports it as `poll.fresh_age_ms` together with the reads per tier and the
  unserved count. The web UI shows it as "data age" on each device card. A device that stops
  answering is marked offline (above), which flips its MQTT availability.

## 3. Pacing and `fast_loop`

- Frame gap (`frame_gap`, `frame_gap_fast`): 18 ms before a frame to an inverter, 1 ms (raised to the
  3.5-character floor, 4 ms) for battery, charger and Energy Bridge devices.
- `tick()` runs from `SungrowModbus::loop()`. With the default `fast_loop: false` the gap and the
  reply detection are quantised to the ~16 ms main loop: ≈ 37 ms of overhead per inverter
  transaction, ≈ 21 ms per battery transaction.
- `fast_loop: true` (`sungrow_modbus` YAML option, default off) holds ESPHome's high-frequency loop
  request while a transaction is pending (`ModbusMaster::wants_fast_loop()`: a reply is awaited, a
  retry waits for its gap, a write or an ad-hoc read is queued, or the post-write settle gap runs)
  and releases it between transactions, so the reply is seen and the next frame goes out within
  about a millisecond instead of a loop period (estimated ≈ 37 → ~24 ms per inverter transaction, ≈ 60 ms per
  hybrid fast cycle). The capture component uses the same mechanism during a YMODEM transfer; each
  holds its own request. Both boards are dual-core, so the cost is CPU time and Wi-Fi contention on
  the main-loop core rather than lost loops; compare the per-transaction time in the link stats
  before and after enabling it.

```yaml
sungrow_modbus:
  fast_loop: true
```

## 4. Bus budget

Each transaction = wire time + 37 ms (inverter) or 21 ms (battery):

| Device | fast | live | slow | static |
|---|---|---|---|---|
| `inv_proto3`, hybrid (string and MPPT 5–20 blocks unserved) | 2 reads, 21 % | 6 reads, 16 % | 6.6 % | 1.7 % |
| `battery`, each stack | 1 read, 4.7 % | – | 1.3 % | 0.2 % |
| `inv_proto3`, string inverter (meter/battery reads unserved) | 1 read, 14 % | ≈ 12 % | ≈ 3 % | ≈ 2 % |

Transaction rates: hybrid inverter ≈ 5.6/s, each stack ≈ 1.2/s; string inverter ≈ 4.3/s.

## 5. Measured cadence

Read-only samples of `/api/signals` (`updated_ms` is stamped on every store write) every ~0.7 s for
90 s, after the initial pass:

| Signal | Hybrid | String |
|---|---|---|
| battery/export/load/active/DC power, device state, meter power | 1.00 s | 1.00 s |
| SBR stack V/I/SOC | 1.00 s | – |
| MPPT 1–4 V/I, the inverter's battery V/I/T/SOC/SOH rows (live tier) | 5.0 s | 5.0 s |
| power flow, backup power, meter phase V, total battery charge, active power-limit setpoint, AFCI count | 5.0 s | 5.0 s |
| hybrid cloud block 0x2308, 0x2343; SBR module SOC, balancing, cycle count, cell max | 30 s | – |

On the hybrid, MQTT `battery_power` publishes about once a second; unchanged values (e.g. MPPT at
night) are deduplicated and refreshed every 60 s. Inverter link: 0 timeouts, 0 lost
frames; heap steady (≈ 168 KB internal free on the WiNet-S2).

## 6. Publish path

- **SignalStore.** `SignalStore::set` always stamps `updated_ms`; listeners fire only on a change.
- **MQTT** (`components/sungrow_mqtt/sungrow_mqtt.cpp`): dirty queue, up to 8 per loop, each signal
  throttled to `state_throttle` = 1 s (re-queued, not dropped), identical payloads deduplicated,
  every state force-republished every 60 s, QoS 1 retained. The 1 s throttle matches the fast tier.
  Every bulk publisher (discovery back-fill, dirty queue, 60 s refresh) pauses for the loop once the
  esp-mqtt outbox holds 12 KB and resumes next loop, so nothing is dropped. At boot the first settled
  device is held for `settle_grace` (5 s), and longer (up to 30 s) while another discovered device is
  still finishing its first poll round; then every settled device is published in one pass, device
  by device, each config followed by its state; the 60 s refresh waits until that back-fill is
  complete.
- **Home Assistant recorder.** A value that changes every second is recorded every second: on the
  hybrid that is ~25 fast entities. Exclude entities in the HA recorder config if the database grows
  too fast.
- **Web UI.** `/api/signals?device=N` takes two optional filters: `class=fast|live|slow|static` keeps
  the signals of one poll tier of the device's register table (derived signals, sigid 0xF000 and up,
  always pass), and `keys=0x2080,0x2086,...` keeps up to 64 listed sigids (hex or decimal); with both
  given, a signal must match both. A malformed list, an unknown class or more than 64 keys answers
  400. The dashboard polls `class=fast` every second (a few hundred bytes) and the full set every
  10 s, and merges the two; the device views poll the full set every 2 s. One full hybrid inverter
  body is about 21 KB.
- **Modbus-TCP** serves from the store (same freshness); unmapped fn 3 reads are forwarded as ad-hoc
  reads.
- **Cloud** rundata every 5 min from the store; no bus traffic of its own.

## 7. Inverter and battery-controller limits

What the inverter and the battery controller can deliver bounds what faster polling can gain. The
details are in SG-Docs:
[SH10RS architecture](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/architecture.md),
[SH10RS control and EMS](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/control-and-ems.md),
[SG5.0RS architecture](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sg5rs/architecture.md),
[SBR192 CAN bus](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sbr192/can-bus.md),
[Modbus function codes](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/modbus-function-codes.md),
[RS485 master](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/rs485-master.md).

| Values | Refresh inside the inverter |
|---|---|
| Power-stage values (AC V/I/Hz/PF, battery V/I/W, load, energy counters) | every ≈ 0.6–0.8 s on the SH10RS, ≈ 0.4–0.5 s on the SG5.0RS (a rotating block read, one step per 100 ms) |
| MPPT V/I (5011–5016), battery current | 4.9 s means, stepping every ~5 s |
| SH10RS power flow in EMS modes 0 and 8: 5031-5032 / 13034-13035, 13008-13011, 5017-5018, 13022 / 5214-5215 | ≈ 40 s means refreshed every 0.6–0.8 s (50 % of a step after 16–22 s); not averaged in modes 2, 3, 4 |
| Meter block 5601+ | every ≈ 0.1–0.2 s (the inverter polls the meter every 100–200 ms): the responsive grid-power source |
| SBR stack records (slaves 200/201) | at most once per second in battery mode (60 s otherwise); some fields 10 s, module serial numbers 61 s |

The cloud-parity derived values (`update_hybrid_cloud` in
`components/sungrow_datamodel/derived.cpp`) are built from the averaged registers and inherit their
lag.

Modbus-slave limits that shape the poll plan:

- **Function codes.** SH10RS 3, 4, 6, 0x10, 0x17, 0x20, 0x60, 0x65; SG5.0RS 3, 4, 6, 0x10, 0x60,
  0x65. Every other code is dropped silently.
- **At most 125 registers per read**; a longer request is dropped silently. Rewired-S caps reads at
  120.
- **Turnaround** 4–10 ms (SH10RS) or 6–13 ms (SG5.0RS) after an end-of-request timeout of 3–4 ms;
  no rate limit and no pipelining.
- **Slaves 200/201** (SH10RS only) serve doc 5200–12000 from RAM; 19101–20000 is tunnelled to the
  battery controller (about 1.5 s internal timeout).
- **Ranges forwarded to the power stage** (SH10RS doc 16000–17500, 29500–29899, 36702–38999;
  SG5.0RS 16000–17500, 29500–29900) block the inverter's other ports for a while, so they are not
  in the cyclic plan.
- **Not used:** 115200 baud (a temporary change reverts after 600 s without frames), a frame gap
  below the stock 18 ms, and the deadbanded fn 0x65 change stream.

A control write delays the fast set by ≈ 100–150 ms (writes keep priority, then a 100 ms settle
gap).

## 8. Method

- **Poll plans.** `build_poll_plan()` on the generated tables (host build of `poll_plan.cpp`); wire
  time = (8 + 5 + 2n) characters × 1.042 ms.
- **Refresh intervals.** Read-only samples of `/api/signals` ≥ 0.5 s apart; the interval of a signal
  is the delta between its distinct `updated_ms` values (a sample loop slower than 1 s shows
  occasional 2 s deltas for the fast set: two refreshes between samples).
- **Bus model.** Each transaction = wire time + 37 ms (inverter) or 21 ms (battery) of measured
  overhead.
- **Host tests.** `tests/modbus/test_modbus.cpp`: `poll_plan_tiers`, `scheduler_tiers_periods_and_smear`,
  `scheduler_fast_slots_stay_on_time`, `scheduler_unserved_reads_drop_to_static`,
  `scheduler_reconnect_restarts_initial_pass`, `master_wants_fast_loop`.
