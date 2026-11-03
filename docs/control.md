# Inverter control (register writes)

`sungrow_control` lets Home Assistant, MQTT, LAN Modbus-TCP clients and the web UI write a
curated set of inverter holding registers: EMS mode, forced charge/discharge, SOC limits, export
limit, SG power limitation and start/stop. Its entities and write semantics match mkaiser's
[Sungrow-SHx-Inverter-Modbus-Home-Assistant], so existing automations and battery controllers
work unchanged. Background on the inverter's control behaviour:
[control and EMS](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/control-and-ems.md),
[writes and meter sync](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/writes-and-meter-sync.md),
[Modbus function codes](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/modbus-function-codes.md).

**Inverter clock.** `clock_sync_time_id: sntp_time` (`common/control.yaml`) writes each inverter's
clock (doc 5000-5005, one fn 16: year, month, day, hour, minute, second in local time including
daylight saving) once SNTP is valid after boot and then every 24 h (`ControlCore::write_clock`).
The clock is a system parameter: the "Inverter writes enabled" switch does not gate it. It is in
no allow-list, so HA, MQTT and :502 cannot write it, and the values must be in the inverter's own
ranges.

**iSolarCloud writes have their own gate.** Cloud `setparam` (cmdType 10303,
[cloud commands](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/cloud-commands.md))
is handled in `components/isolarcloud` and does not use any of this: it is gated only by the
separate **Cloud writes** switch (`common/cloud.yaml` `cloud_writes_restore`, default off), does
not pass `sungrow_control`'s allow-list, and refuses any point that overlaps the inverter's
internal channels on every device: doc 16000-17500, 29500-29900 and 36702-38999 (power-stage
forward windows), 19101-20200 (firmware upload), 21001-21020 and 30456-30460 (temporary and
persistent baud rate and slave address) and 36099 (power-stage raw relay)
(`setparam_protected_range` in `commands.cpp`; the windows are described in
[register windows](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/register-windows.md)).

> [!WARNING]
> The write paths are host-tested against a simulated inverter. Turn writes on only once you can
> watch the inverter, and read the register-write entries in `docs/limitations.md` first.

## Files

| File | Role |
|---|---|
| `components/sungrow_modbus/master.*` | Write core: priority write queue, fn 6 / fn 16, verify, settle gap, rate limit, stats |
| `components/sungrow_modbus/discovery.*` | Device identity: protocol type, dev_type, grid-code XML family and parameter descriptor (country / grid-code / power-switch registers) per (inverter type, protocol) |
| `components/sungrow_control/control_core.*` | Safety gate (enable, allow-list, ranges, online), the fn 3 read-back poller, start/stop follow-up, power-limit re-assert, the Setup / Installer batch path and on-demand reads. Host-tested |
| `components/sungrow_control/setup_profile.*` | Setup profile resolver: (family, descriptor, feature bits, input 5479, option) → the ordered register list. Host-tested |
| `components/sungrow_control/grid_profiles.gen.{h,cpp}` | Generated run dictionary of the stock POWER_COMPANY profiles |
| `components/sungrow_control/installer_allow.gen.h` | Generated protection-register allow-list per XML family |
| `components/sungrow_control/installer_rules.gen.h` | Generated value ranges and register relations per settings family |
| `components/sungrow_control/dsp_ranges.gen.h` | Generated power-stage acceptance ranges per inverter model |
| `components/sungrow_control/sungrow_control.*`, `*.py` | ESPHome glue: switch, `select`/`number`/`sensor`/`text_sensor` platforms, `sungrow_control.write_registers` action, profile auto-attach |
| `components/sungrow_web/spa_handler.*` | Web endpoints: `/api/write`, `/api/confirm`, Installer (`/api/setup_write`, `/api/grid_read_start`, `/api/grid_read`) and Setup (`/api/setup_options`, `/api/setup_preview`, `/api/setup_apply`, `/api/setup_start`) |
| `components/sungrow_web/web/src/views/setup.ts`, `views/installer.ts` | SPA Setup and Installer pages |
| `components/sungrow_web/web/src/setup_logic.ts` | Pure SPA helpers: U32 word order, pending pairs, commit-first order, diff, range and regulation checks, frame groups. Tested by `setup_logic.test.ts` |
| `data/write_registers.yaml` | Curated allow-list (register per protocol type via `address_by_protocol`) |
| `tools/gen_write_registers.py` | Generates `components/sungrow_control/write_registers.{h,cpp}` and `write_meta.py` |
| `tools/gen_grid_profiles.py` | Generates `grid_profiles.gen.{h,cpp}` and `web/src/setup_profiles.gen.ts` from `data/grid_profiles/` |
| `tools/gen_installer_settings.py` | Generates `web/src/installer_settings.gen.ts`, `installer_allow.gen.h` and `installer_rules.gen.h` |
| `tools/gen_dsp_ranges.py`, `data/dsp_params/` | Generates `dsp_ranges.gen.h` from the model manifest `models.yaml` and one range CSV per model |
| `tools/gen_installer_rules.py`, `data/installer_rules.yaml` | Value rules → raw ranges, relations and regulation strings for the Installer catalogue |
| `components/sungrow_modbus_tcp/mbtcp_core.*` | :502 fn 6 / fn 16 parsing, gate hook, echo replies |
| `common/control.yaml` | Hub, `writes_enabled` switch, stats, the HA action, :502 gating |
| `devices/hybrid_control.yaml`, `devices/string_control.yaml` | Writable entities and read-backs per inverter type |
| `tests/modbus/`, `tests/control/` (`test_control.cpp`, `test_setup_profile.cpp`), `tests/modbus_tcp/`, `tests/test_write_registers.py`, `tests/test_gen_*.py` (incl. `test_gen_dsp_ranges.py`), `tests/test_xml_drift.py`, `make web` | Tests |

## Safety model

Every write passes the gate in `ControlCore`, in this order, whatever its source: native
`select`/`number`, the `write_registers` action, MQTT, a :502 client, or the web interface.
Cloud `setparam` is the one write path outside it (its own switch and refused ranges, above).

1. **Writes policy.** The `Inverter writes enabled` switch is restored across reboots and
   defaults to **OFF** (`RESTORE_DEFAULT_OFF`). Its id is `sungrow_writes_enabled`; the debug
   build forces it off. It gates the **Modbus-TCP :502 server, MQTT, the HA action and the
   native entities**: while it is off they are refused (the native path logs "writes disabled",
   :502 answers exception 1). **Web-interface writes are not gated by it:** `/api/write`
   (Devices controls) is authorised by the digest login, and the Installer and Setup endpoints
   by the login plus a single-use confirm token; they call `ControlCore::write(..., authorized =
   true)` and every check below still applies.
2. **Register allow-list per device.** The `profile:` gives the curated set (`hybrid` or
   `string`), with each register at its address for the device's protocol type. You can add
   entries in YAML as `{address, count, min, max, signed}`, where address is the 1-based document
   number and values are raw. Every register in a request must be covered. Otherwise the native
   path refuses it and :502 answers exception 2. A device without a `sungrow_control` entry has an
   empty list. Setup and Installer batches may also write the protection registers of the
   device's own grid-code XML family (`installer_allow.gen.h`); family `none` or a unit pair
   without a descriptor allows none.
3. **Value checks.** Each raw value must be within the entry's `min`/`max`. Enumerations must
   also be in their value set: for example, EMS accepts only {0, 2, 3, 4}, so MicroGrid (8) is
   shown but never written, as in mkaiser. Otherwise :502 answers exception 3. Installer batches
   also check the stock range and the register relations of every protection register
   (`installer_rules.gen.h`), and the power-stage range on the models that have one.
4. **Device online.** The device must be known to the master, polling (table active) and
   online. This is checked when the write is accepted and again just before it is sent.
   Otherwise :502 answers 0x0B.
5. **Rate limit and queue.** A register cannot be written again within `write_rate_limit`
   (default 1 s). The write queue holds 8 entries. Either limit gives :502 exception 6.
6. **:502 only.** The optional `write_allowed_ips` list on `sungrow_modbus_tcp` restricts which
   clients may write. Default: any client that the connection allow-list `allowed_ips` admits.
7. **Logging.** Every accepted write and its outcome are logged at INFO, e.g.
   `write dev 0 13050=[2] (select): ok`. Rejections are logged at INFO or WARN. The last
   outcome is also published as the `Inverter last write` text sensor. Counters:
   `Inverter writes ok` / `Inverter writes failed`. "Failed" includes rejected writes.
8. **Follow-ups.** A start/stop command is tracked against the inverter's running state until it
   takes effect or times out ("Start/stop follow-up" below). On a string inverter the last
   accepted power limit is written again after a power-stage restart, under the same gate as the
   original write ("Power limit re-assert" below).

Defaults:

| Setting | Default |
|---|---|
| `writes_enabled` | off (gates :502, MQTT, the HA action and the native entities) |
| Verify (fn 3 read-back after native and API writes) | on |
| `write_timeout` | 2000 ms |
| `write_settle` | 100 ms |
| `write_rate_limit` | 1 s per register |
| Read-back poll | 10 s (some registers 30 / 60 s), plus right after each write |

The `write_*` settings are on `sungrow_modbus`.

## Write core (sungrow_modbus)

`ModbusMaster::submit_write(addr, start, values, count, {fn, verify}, cb)`:

- **Queue.** A separate write queue, served before ad-hoc reads and cyclic polls. One
  transaction is in flight at a time. A frame already picked still goes first, which costs at
  most one 18 ms gap plus that frame. The stock dongle orders its queues the same way.
- **Function code.** fn 6 for one register, fn 16 for more, or the caller's choice. :502 keeps
  the client's function code. Internally generated writes use fn 16; native entities use fn 6,
  the same as mkaiser.
- **No retransmit.** A write is sent once, even with `retries > 0`.
- **Timeout.** `write_timeout` is measured from the send. A timeout counts as a lost frame for
  the offline logic.
- **Verify.** After the echo and the settle gap, a fn 3 read of the same range follows
  immediately. The outcome is `OK` or `VERIFY_MISMATCH`, with the values read back. The
  read-back also updates the entities at once.
- **Settle.** After every write, whatever the outcome, the bus stays silent for `write_settle`,
  so the verify read sees the applied value.
- **Results and stats.** Results are `OK`, `EXCEPTION` (with the code), `TIMEOUT`,
  `VERIFY_MISMATCH`, `VERIFY_FAILED`, `OFFLINE`, `RATE_LIMITED`, `QUEUE_FULL` and `INVALID`.
  `WriteStats` counts ok / failed / rejected / verify_mismatch.

## Path A: Modbus-TCP :502

The reply to a successful write is rebuilt from the request: fn 6 echoes the request byte for
byte, fn 16 answers `unit 10 addr count`. Writes use the same 5 s TCP context timeout and 2 s
RS485 timeout as reads. Background:
[Modbus-TCP](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/modbus-tcp.md).

| Case | Reply | Stock dongle |
|---|---|---|
| Allowed, inverter echo | fn 6: the request byte for byte; fn 16: `addr count` | same |
| Inverter exception / timeout / verify problem | exception **4** | same |
| `writes_enabled` off, or no `sungrow_control_id` | exception **1** | writes anything |
| Client not in `write_allowed_ips` | exception **1** | has no write list |
| Register not allow-listed | exception **2** | writes anything |
| Value out of range or not in its set | exception **3** | writes anything |
| Malformed (byte count ≠ 2×count, count 0 or > 123, short fn 6) | exception **3** | exception 4 after 5 s |
| Device offline or undiscovered | exception **0x0B** | exception 4 |
| Rate limited / write queue full | exception **6** | exception 4 |
| fn 5 / 0x0F / 0x60 | exception **1** | forwards fn 5, and fn 0x60 for unit 0 as an unchecked RS485 broadcast |
| Unknown unit | 0x0B (or no reply with `unknown_unit: ignore`) | exception 2 or no reply, by unit |

Distinct codes make the reason for a refusal visible to the client without changing mkaiser's
happy path. `:502` writes are not verified by the dongle; the reply follows the inverter's echo,
and mkaiser refreshes its own read-backs.

**mkaiser compatibility.** mkaiser sends only fn 6 (`modbus.write_register` with a scalar
`value:`, and `modbus.switch` with `write_type: holding`), always to unit 1 with the wire
address. `tests/control` `mbtcp_write_end_to_end` replays its exact frames:
- `00 01 00 00 00 06 01 06 32 F9 00 02` (EMS 13050 = Forced mode)
- `… 06 33 11 27 10` (export limit 13074 = 10000 W)

It also covers a fn 16 range write. mkaiser's fn 3 read-backs of these holding registers
(13049…) are not in the slave maps, so they are read live from the inverter (passthrough).

Config: `common/control.yaml` adds `sungrow_control_id: sungrow_ctl` to `sungrow_modbus_tcp`.
Optionally restrict writers:

```yaml
sungrow_modbus_tcp:
  write_allowed_ips: [192.0.2.10]    # e.g. the HA host
```

## Path B: native ESPHome entities, MQTT and the action

- `select` entities use mkaiser's option strings and raw values. The options list only the
  writable values.
- `number` entities have units, ranges and steps from `data/write_registers.yaml`. The value
  is converted as `raw = round(value / scale)` and checked against the allow-list.
- **State is never optimistic.** Every entity shows the read-back signal, i.e. what the
  inverter reports. None of these registers is in a regular poll table, so `sungrow_control`
  polls them itself with fn 3 (blocks per profile, every 10 s, and right after each write).
  The values go into the SignalStore under synthetic sigids 0xF100+.
  - A rejected or failed write re-publishes the real value, so HA snaps back.
  - When the device goes offline, the values are cleared: numbers and sensors show
    unknown, and text sensors show `"unknown"`.
- **Read-back sensors with mkaiser state strings.**
  - Text sensors give `Self-consumption mode (default)` / `Forced mode` / `External EMS` /
    `VPP` / `MicroGrid`, `Stop (default)` / `Forced charge` / `Forced discharge`, and
    `Enabled` / `Disabled`. An unknown code shows as `0x00NN`.
  - Numeric sensors are in display units: W, %.
- **MQTT.** `sungrow_mqtt` publishes the profile's writable registers as HA `number` / `select`
  controls on each inverter's MQTT device; each control's `/state` follows its read-back within
  about 1 s of a write. The result of a write goes to the control's result topic as JSON
  `{"ok", "status", "state", "final"}`.
- **Action.** `sungrow_control.write_registers` takes `device`, `address` (1-based document
  number), `values[]` and `verify`. It completes when the write is done, so a following
  `api.respond` reports the result.
  - `common/control.yaml` exposes it to HA as `esphome.<node>_write_registers` (device, address,
    values, verify), with a response (success / error message).
  - The same gate applies (enable switch, allow-list, ranges).

Example (HA):

```yaml
action: esphome.winet_s2_write_registers
data: {device: 0, address: 13050, values: [2], verify: true}
```

## Allow-list

The address is the 1-based **document** register number (= the table address). The wire / PDU
address, and mkaiser's YAML `address:`, is one less. The register meanings are those of the
public Sungrow register documents, mkaiser's integration and SunGather
([registers](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/registers.md),
[SG5.0RS registers](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sg5rs/registers.md)).

| Profile | Key | Doc (wire) | Meaning | Raw range / values | Display |
|---|---|---|---|---|---|
| hybrid | `ems_mode` | 13050 (13049) | EMS mode | {0, 2, 3, 4} (8 shown only) | select |
| hybrid | `forced_cmd` | 13051 (13050) | Forced charge/discharge cmd | {0xAA, 0xBB, 0xCC} | select |
| hybrid | `forced_power` | 13052 (13051) | Forced charge/discharge power | 0–10000 | W |
| hybrid | `max_soc` | 13058 (13057) | Max SOC | 500–1000 | ×0.1 % |
| hybrid | `min_soc` | 13059 (13058) | Min SOC | 0–500 | ×0.1 % |
| hybrid | `export_power_limit` | 13074 (13073) | Export power limit | 0–30000 | W |
| hybrid | `backup_mode` | 13075 (13074) | Off-grid option | {0x55, 0xAA} | select |
| hybrid | `export_power_limit_mode` | 13087 (13086) | Export (feed-in) limitation on/off | {0x55, 0xAA} | select |
| hybrid | `reserved_soc_backup` | 13100 (13099) | Reserved SOC for backup | 0–100 | % |
| hybrid | `battery_max_charge_power` | 33047 (33046) | Battery max charge power | 1–1500 | ×10 W |
| hybrid | `battery_max_discharge_power` | 33048 (33047) | Battery max discharge power | 1–1500 | ×10 W |
| hybrid | `battery_charging_start_power` | 33149 (33148) | Charging start power | 0–100 | ×10 W |
| hybrid | `battery_discharging_start_power` | 33150 (33149) | Discharging start power | 0–100 | ×10 W |
| hybrid | `hybrid_start_stop` | 5006 (5005); protocol 5: 13000 (12999) | Start / stop | {0xCE stop, 0xCF start} | select |
| string | `power_limit_switch` | 5007 (5006) | Power limitation switch | {0x55, 0xAA} | select |
| string | `power_limit_pct` | 5008 (5007) | Power limitation setting | 0–1000 | ×0.1 % |
| string | `power_limit_kw` | 5039 (5038) | Power limitation adjustment | 0–1000 | ×0.1 kW |
| string | `start_stop` | 5006 (5005); protocol 4: 30045 (30044) | Start / stop (on/off grid) | {0xCE stop, 0xCF start} | select |

**Register per protocol type.** A rule with `address_by_protocol` in `data/write_registers.yaml`
uses another register on devices of that protocol type, following the stock parameter
descriptor's power-switch register: string start/stop 30045 on protocol 4, hybrid start/stop
13000 on protocol 5, 5006 otherwise. `gen_write_registers.py` emits these as `RuleDef.alt`;
`profile_rules(profile, protocol)` and `ControlCore::bind_protocol()` bind the allow-list, the
read-back block and the entity / MQTT write address to the device's protocol
(`address_for_sigid`). The SPA Devices controls post the rule key
(`/api/write?device=D&key=K&value=V`), which `address_for_key` resolves to the same bound
register; `/api/devices` reports the attached profile (`control.profile`), so the SPA offers the
controls the firmware will accept.

**Start/stop is a routine control.** It takes the inverter on or off grid (reversible: send Start
again). The power stage does not open at once on 0xCE: it limits active power to 1 %, waits until
the output is below 6.25 % of rated (or a ramp timer runs out), then disconnects and reports
0x1300; 0xBB is an emergency stop (0x1500). A start waits for the configured reconnection delay
(30091) with the grid within limits
([states and control](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/control-and-ems.md)).
It has no per-write confirm token: MQTT, :502, the HA action and the native entities need the
`writes_enabled` switch, `/api/write` needs the web login. Setup's "Start inverter" writes 0xCF
to the descriptor's power-switch register through `/api/setup_start` (login + confirm token).
There is no fault-clear control: the inverter's fault clear and self-check are not holding
registers.

**Deliberately not allowed by default**:
- factory reset 30043
- global MPPT scan 30230
- load adjustment 13002 / 13011
- forced startup under low SoC 13017
- active power limitation 13089 / 13090
- SG export limitation 5010 / 5011

Add any of them yourself under `registers:` if you need them.

**Power-limit read-backs and the setpoint (0x217E).** The writable registers 5006/5007/5008 are
re-read every 10 s (and immediately after a write) into the derived sigids 0xF143/0xF140/0xF141,
and each MQTT write entity's `/state` follows its read-back within about 1 s via the post-write
settle watch in `sungrow_mqtt`. The **active power-limit setpoint 5077 (0x217E)** is a
read-only, live-polled signal (fn 4, published as a sensor). It reflects whatever the inverter
reports: writing only the percent (5008) while the switch (5007) is Disabled leaves the limit
inactive, so 0x217E stays at rated power; enable the switch for the setpoint to track the
percent.

**SG power limitation in one frame.** The string profile allows 5007 and 5008 in one fn 16, so
switch and setting change together with no transient: `address: 5007, values: [170, 500]`
(0xAA = enabled, 50.0 %). The SG limit lives in the power stage's RAM and is lost when the power
stage restarts; the re-assert below restores it.

## Setup and Installer (grid code and protection registers)

Both pages are reached from an inverter's card on the Devices tab and are offered only when the
device's grid-code XML family is `str_pv`, `res_es`, `res_new_pv` or `unit`. The family comes from
the pair (inverter type = holding 30644 read at boot, protocol type), as in stock; dev_type plays
no part. Full detail: `docs/device_types_setup_installer.md`; background:
[grid codes and DRM](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/grid-codes-and-drm.md).

**Setup** applies a stock country / standard profile, the way the stock dongle does:
1. `GET /api/setup_options?device=D` reads input 5479 (machine type) and the country / standard
   registers and returns the family's options (AS/NZS 4777.2:2015, and for `unit` also
   4777.2:2020 Region A / B / C). Australia (country 6) is the only country.
2. `GET /api/setup_preview?device=D&option=O` resolves the profile on the dongle: the descriptor's
   country register (30500, 30643 or 32310) := 6, then on `unit` with feature bit W32B5 30036 :=
   the option, then the option's `<set>` registers in XML order (the PV or ES variant chosen by
   5479). It reads every target register and returns current vs new values.
3. After a confirmation dialog, `POST /api/confirm?device=D&address=65534` and
   `POST /api/setup_apply?device=D&option=O&confirm=T` write every register of the profile, in that
   order, with the XML values as the stock dongle writes them. It needs a complete preview of the
   same device and option from the last 60 s (`PREVIEW_REQUIRED` otherwise).
4. Optional "Start inverter": `POST /api/confirm?address=65533` + `POST /api/setup_start` writes
   0xCF to the descriptor's power-switch register (5006, 13000 or 30045).

**Installer** edits single protection settings. It lists every editable register of the family's
XML (`installer_settings.gen.ts`) with its current value (`/api/grid_read_start` +
`/api/grid_read`), checks pending edits against the stock range and regulation rules, drops edits
equal to the current value, and writes them as one batch: `POST /api/confirm?address=65535` +
`POST /api/setup_write` (body `addr:val,…`; a U32 setting is two registers, low word first).

**Server checks** (`ControlCore::setup_apply`): one batch at a time (BUSY); ≤ 128 registers; an
unknown or offline device is rejected at once (OFFLINE); each protection register must be in the
family's allow-list (NOT_ALLOWED); Installer batches also need each value inside its stock range
and every relation with the other registers (batch values over the last read values) to hold
(BAD_VALUE `range:A` / `relation:…`). Installer batches write the commit registers (country,
grid code, 30036, 30037) first. Frames are fn 16 with a read-back verify; the batch has a 60 s
deadline after which unsent frames are dropped as failed. Progress and a result per frame are in
`GET /api/grid_read` (`setup.frames`).

**Power-stage acceptance ranges.** The inverter's power stage accepts each protection parameter
only inside its own range and silently keeps the old value otherwise, while the control board
acknowledges the write
([parameters](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/architecture.md)).
The ranges are known for the SH10RS and the SG5.0RS: `data/dsp_params/models.yaml` lists each
model with the device code it reports in its devinfo block, the power-stage firmware the ranges
belong to and a CSV of ranges, and `dsp_ranges.gen.h` holds the generated tables. On a listed
model the server applies those ranges on top of the checks above:
- an Installer value outside its range is refused (`BAD_VALUE`, detail `dsp_range:ADDR:MIN-MAX`);
- a Setup profile item outside its range is left out of the batch with a WARN log and listed in
  the `note` of `/api/setup_preview` and `/api/setup_apply`; the rest of the profile is still
  applied, so a stock country profile stays applicable.

A U32 setting is compared as one 32-bit value. Other models are checked against the installer
catalogue only. Adding a model is a manifest row and a CSV (`make gen`).

**Writes policy.** These endpoints are authorised by the web login and a confirm token bound to the
device and the endpoint (65535 / 65534 / 65533, 20 s, single use); the `writes_enabled` switch does
not apply to them. Protection registers cannot be written over MQTT, :502 or the HA action unless a
YAML `registers:` extra lists them.

**SG5.0RS values.** On an SG5.0RS (protocol 3, "AA10", tag "ZS") the family is `unit`, the
country register is 32310 (Australia = 6), the standard is 30036, input 5479 = 1 selects the PV
variant, feature bits W32B5 and W29B12 are set, and U32 protection pairs are low word first.

## Start/stop follow-up

A start (0xCF) or stop (0xCE) written to the device's start/stop register (the profile's
`start_stop` / `hybrid_start_stop` rule at the address of the device's protocol type, or the
descriptor's power switch through `/api/setup_start`) is not confirmed by its echo: the inverter
acknowledges the command and acts on it later. `ControlCore` therefore follows the running state
(`SIG_DEVICE_STATE`, classified by the generated `state_class()`) after every accepted command
and reports the progress as text:

| Command | While pending | Outcome |
|---|---|---|
| Stop | `stop pending: output ramping` (the power stage ramps the output down before it opens) | `stopped` once the state is of the stopped class; `stop pending: time-out (state 0x....)` after 15 s |
| Start | `start pending: reconnection delay (N s)`, then `start pending: standby` / `start pending: starting` as the state moves | `started` on a running-class state; `start accepted: standby (low PV)` when the inverter is still in standby after the delay; `start failed: fault (state ...)` on a fault state |

N is the inverter's reconnection delay, read once from holding register 30091 when the start is
accepted (clamped to 10-900 s; 60 s until it answers; it is not added to any poll tier); the
deadline is that delay plus 15 s. The power stage re-runs the delay after each grid trip, so
while the state stays in the starting or a transient class at the deadline, the deadline extends
by 10 min up to three times (`start pending: reconnection delay extended (grid trip k/3)`),
never past 1 h; then `start time-out (state ...)`. A start that leaves the inverter in a stopped
state ends with `start failed: still stopped (state 0x1300)`; on a string-family model the text
names the two causes the inverter does not distinguish: `start failed: still stopped (key stop
or rapid shutdown)`. The device going offline ends the follow-up with `<start|stop> cancelled:
device offline`.

Every text change is reported once: to the `Inverter last write` text sensor as `dev N <text>`,
on MQTT to the control's result topic as `{"ok","status","state","final"}` (`state` = the
running-state code seen last, `final` = the outcome is decided), and in `/api/devices` as
`control.expect` (`kind`, `key`, `text`, `final`, `ok`, `age_ms`, `state`). The Devices page shows
the text on the start/stop control row while the command is pending and for 60 s after the
outcome. The status stays readable until the next start/stop command.

## Power limit re-assert (string inverters)

The string profile's `power_limit_switch` (5007) and `power_limit_pct` (5008) live in the power
stage's RAM: after a power-stage restart the inverter runs with its boot value of 110 % (no
limit) while the control board still reports the earlier write as accepted. `ControlCore`
remembers the last accepted write to these two rules (switch, setting, source, whether it was
web-authorised) and re-asserts it:

- **Trigger.** The device comes back online, or its running state drops from a running class to
  0x1200 (the power stage's initial state after a restart; the nightly standby 0x1400 keeps the
  RAM and does not trigger).
- **Check.** The re-assert waits for a read-back of 5008 newer than the trigger (the read-back
  block is requested at once). Equal to the remembered setting: nothing (debug log `power limit
  intact`).
- **Write.** Otherwise one fn 16 at 5007 with `[switch, setting]` and a read-back verify, source
  `reassert(<original source>)`, INFO log `power limit re-asserted: 1100 -> 500`, under the same
  gate as the original write: it goes out when `Inverter writes enabled` is on or the original
  write came from the web interface, and at most once per 10 min. Otherwise one WARN and the
  trigger is dropped until the next one.
- **Scope.** Per device, `reassert_power_limit: true` by default in the `sungrow_control`
  `devices:` list (`false` turns it off); only profiles with both power-limit rules remember
  anything, so a hybrid is unaffected; a device without read-back (`readback: false`) never has a
  fresh value and never re-asserts.

## Entity mapping for mkaiser-style consumers

Battery controllers and automations written for mkaiser's integration find the same controls
under these names. Over MQTT each control is an entity of the inverter's own HA device; with
`common/native_entities.yaml` they are native entities of the dongle, where `<n>` is the node's
slug (`winet_s2` with the default friendly name "WiNet-S2"):

| mkaiser entity | Native entity | Values / units |
|---|---|---|
| `select.ems_mode` | `select.<n>_inverter_ems_mode` | `Self-consumption mode (default)`, `Forced mode`, `External EMS`, `VPP` |
| `select.battery_forced_charge_discharge` | `select.<n>_inverter_battery_forced_charge_discharge` | `Stop (default)`, `Forced charge`, `Forced discharge` |
| forced charge/discharge power | `number.<n>_inverter_battery_forced_charge_discharge_power` | W, 0–10000, step 1 |
| `sensor.ems_mode_selection` | `sensor.<n>_inverter_ems_mode_selection` (text) | as above, plus `MicroGrid`; `unknown` when offline |
| `sensor.battery_forced_charge_discharge_cmd` | `sensor.<n>_inverter_battery_forced_charge_discharge_cmd` (text) | as above |
| `sensor.battery_forced_charge_discharge_power` | `sensor.<n>_inverter_battery_forced_charge_discharge_power_readback` | W |
| reserved SOC for backup | `number.<n>_inverter_battery_reserved_soc_for_backup` | %, 0–100 |
| `sensor.reserved_soc_for_backup` | `sensor.<n>_inverter_reserved_soc_for_backup` | % |
| export power limit | `number.<n>_inverter_export_power_limit` | W, 0–30000 |
| export power limit mode | `select.<n>_inverter_export_power_limit_mode` | `Enabled` / `Disabled` |
| `sensor.export_power_limit` | `sensor.<n>_inverter_export_power_limit_readback` | W |
| `sensor.export_power_limit_mode` | `sensor.<n>_inverter_export_power_limit_mode_readback` (text) | `Enabled` / `Disabled` |
| min / max SOC | `number.<n>_inverter_battery_min_soc` / `…_max_soc` | %, step 1 |
| SG power limitation setting | `number.<n>_inverter_power_limitation_setting` (`string_control.yaml`) | %, 0–100, step 1 |
| SG power limitation switch | `select.<n>_inverter_power_limitation_switch` | `Enabled` / `Disabled` |
| SG power limitation read-back | `sensor.<n>_inverter_power_limitation_setting_readback` | % |
| SG power limitation mode | `sensor.<n>_inverter_power_limitation_mode` (text) | `Enabled` / `Disabled` |

The entities write on set, so no push scripts or `modbus.write_register` calls are needed; a
consumer that used them calls `select.select_option` / `number.set_value` on the entity instead.
ESPHome selects write on every set, including an unchanged option.

**Timing for consumers.**
- **Read-backs** refresh every 10 s and right after each write (the verify read plus a block
  re-read).
- **Rate limit.** A burst to the same register within 1 s is refused (busy); writes to different
  registers are not affected.
- **Offline.** When the inverter is offline, read-backs go `unknown`.

[Sungrow-SHx-Inverter-Modbus-Home-Assistant]: https://github.com/mkaiser/Sungrow-SHx-Inverter-Modbus-Home-Assistant
