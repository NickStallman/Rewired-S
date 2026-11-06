# Device types, Setup and Installer

How the firmware identifies the devices on the RS485 bus, which poll table, control profile and
grid-code family each one gets, and how the web UI's Setup page (whole grid-code profiles) and
Installer page (single protection settings) write to the inverter.

Background on device types and the stock grid-code profiles:
[device types and grid profiles](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/device-types-and-grid-profiles.md),
[SH10RS grid codes and DRM](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/grid-codes-and-drm.md),
[device discovery](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/device-discovery.md).

---

## 1 Device identification

### 1.1 Finding devices

| Aspect | Behaviour |
|---|---|
| Who is probed | Devices whose poll table is `auto`: YAML `devices:` entries without a `table:`, remembered or searched inverter addresses, and range-probe slots. A device with an explicit `table:` is polled straight away and never probed. |
| Probe | fn 4, registers 2515..2580 (66 registers, the devinfo block). |
| Finding the inverter | Remembered inverter addresses (NVS) are polled from boot. A broadcast fn 3 read of 30035 runs at start, after an inverter goes offline and every 60 s while none is online (`inverter_search: true`), as the stock dongle does. A new address replaces remembered addresses that stayed silent. The SH10RS and SG5.0RS do not answer this read, so they are found from the remembered address or the range probe. |
| Range probe | `scan_range` (shipped `"1-8"`): a read-only probe of each address that is not configured, run only when the search gets no answer, no inverter is online and every remembered address stayed silent; once per boot and once per rescan. |
| Reported children | After each poll round of an online inverter, every battery address in input 10451..10458 (values 1..247) and, from register 5621, bit 0 (EV charger at 248) and bit 2 (Energy Bridge at 246) becomes a device (up to 10). Each is probed with its class identity read: SBR fn 4 10710 × 11, charger 21201 × 24, Energy Bridge 4096 × 42. A reply (SBR: non-zero device code) makes it a device of that class (dev_type 44 / 46 / 92, table `battery` / `charger` / `energy_bridge`); a silent one is asked again every 60 s without holding up the others. Batteries, chargers and Energy Bridges are never range-probed. |
| SBR model | Device codes 0x20E6..0x20EC map to SBR064..SBR256 in 3.2 kWh steps (0x20EA = SBR192); any other code is named `SBR000` until polling fills the model. |
| Timeouts | `response_timeout` 2000 ms from the send; `retries` (default 0) applies to discovery only, never to writes. A scan slot is re-probed until `scan_attempts` (default 2) misses, then abandoned until a rescan; a configured device is re-probed every 2 s for 10 failures, then every 15 s. |
| Exception reply | The device is present: the fallback table (`fallback_table`, default `inv_proto1_4`; `scan_fallback_table` for scan slots) is activated, protocol type 1, the device goes online but counts as not discovered. Without a fallback table the device keeps being probed. |
| Short reply (< 64 registers) | Treated as an exception reply. |
| Successful parse | The device is discovered, the poll table is chosen from the protocol type (§3.2), the block is stored as the table's own data and one fn 3 read of 30644 follows (§1.2). |
| Re-identification | None until reboot: dev_type and protocol are fixed once found; the inverter type follows the poll. The "RS485 rescan" button re-arms the scan slots that are not discovered. |

### 1.2 Inverter type (holding 30644)

After a successful devinfo parse, one fn 3 read of 30644 runs before the first poll round. A
value other than 0xFFFF is the inverter type; 0xFFFF, an exception or a timeout leave it unknown
(not counted as a lost frame). `inv_proto1_4` polls 30644 hourly, so a change updates the type,
logs `addr A: inv_type V -> xml family F` and re-publishes the identity. Only protocol 1 and 4
use the inverter type (§3.1).

The identification log line:
`addr A: dev_code 0x%04X model '…' proto '…' tag '…' -> protocol P, dev_type T, inv_type V|unknown, xml family F`.

### 1.3 Fields of the devinfo block

| Register(s) | Field | Parsing |
|---|---|---|
| 2515 | `dev_code` | u16 |
| 2516..2523 | `model` | 16 bytes, up to the first NUL, trailing space / 0xFF trimmed |
| 2534..2535 | `proto_code` (4 chars) | reg 2535 high, reg 2535 low, reg 2534 high, reg 2534 low |
| 2534..2535 | dev_type marker | the low byte of reg 2535 |
| 2578 | `tag` (2 chars) | high byte, low byte |

The other registers of the block are stored through the poll table's rows only.

### 1.4 Where the identity is shown

| Where | What |
|---|---|
| Native text sensors (configured devices) | dev_code, `dev_type` as `"<T> (<label>, <source>), family <xml family>"`, `protocol` as `"<P> (<proto_code>|table)[, inv_type V]"`, the table name |
| `/api/devices` | per device: `index, address, dev_type, dev_code, protocol, model, online, discovered, state, family, xml_family, inv_type, inv_type_known, dev_type_source, proto_code, descriptor {inv_type, protocol, country_reg, grid_reg, power_switch_reg} | null`, link stats |
| Devices page | dev code, protocol (1 residential/string, 2 AA10+, 3 ZS incremental, 4 old LCD, 5 storage), protocol code and, for inverters, inverter type and XML family; an on-demand read of the country / grid-code registers and 30036 / 30037 |

---

## 2 dev_type classification

### 2.1 Protocol type (`classify_protocol`)

1. `tag == "ZS"` → **3**.
2. Otherwise, a protocol code that contains "AA" and sorts at or above "AA10" → **2**.
3. Otherwise by device code:

| dev_code | protocol |
|---|---|
| 0x0027, 0x0074, 0x010F, 0x0116, 0x011B | 4 |
| 0x0131-0x0132, 0x0134-0x0138, 0x013D, 0x0145-0x0146, 0x014D-0x014E | 4 |
| 0x0D03, 0x0D06-0x0D07, 0x0D09, 0x0D0C | 5 |
| 0x0E00-0x0E03, 0x1061-0x1062, 0x2FC0-0x2FFF | 4 |
| any other | 1 |

### 2.2 dev_type (`classify_dev_type`)

| dev_code | dev_type |
|---|---|
| 0x0D00-0x0EFF | 35 (hybrid) |
| 0x0110-0x0115, 0x011C-0x0127, 0x0133, 0x013C, 0x013E-0x013F, 0x0142-0x0143, 0x0147-0x0149, 0x2400-0x27FF | 21 (residential) |
| any other, marker byte `'R'` | 21 |
| any other, marker byte `'B'` | 35 |
| any other | 1 (string) |

The model string is not consulted.

### 2.3 Source of a device's dev_type

| Source (`dev_type_source`) | Value | When |
|---|---|---|
| `yaml` | YAML `dev_type:` (1, 21, 35, 18, 34, 44, 46, 92) | set on the device; wins over the devinfo block |
| `devinfo` | §2.2, or 44 / 46 / 92 from a child identity read | the identity block was parsed |
| `table` | the table family: inverter 1, battery 44, meter 18, logger 34, charger 46, Energy Bridge 92 | fixed or fallback table, no YAML value |
| `none` | 0 | no table yet |

Without a devinfo block an inverter table also sets the protocol it stands for (`inv_proto1_4` → 1,
`inv_proto2` → 2, `inv_proto3` → 3, `inv_proto5` → 5).

A device is *identified* when it is discovered or has a non-zero dev_type. Identified devices are
shown in the web UI, reported to iSolarCloud and served on :502. The control profile is
auto-attached and the device is published over MQTT only when it is *discovered* (devinfo or child
identity read): dev_type 35 → hybrid profile, 21 or 1 → string profile, any other → none (every
write to it is NOT_ALLOWED). A device with a YAML control package keeps that profile, bound to the
device's protocol.

---

## 3 Families and tables

### 3.1 Grid-code XML family and parameter descriptor

The grid-code family is chosen from **(inverter type, protocol type)**, as the stock dongle does;
the inverter type counts only on protocol 1 and 4 and is 0 otherwise.

| inverter type | protocol | family |
|---|---|---|
| 0 | 1 | `str_pv` |
| 1 | 1 | `res_new_pv` |
| 0 | 5 | `res_es` |
| 0 | 4 | `none` |
| any other pair with a descriptor | | `unit` |
| any other pair without a descriptor | | `unit_nodesc` |

The parameter descriptor, matched exactly on (inverter type, protocol):

| (inv, P) | country register | grid-code register | power switch |
|---|---|---|---|
| (0, 1) | 30500 | 30502 | 5006 |
| (1, 1) | 30643 | — | 5006 |
| (0, 5) | 30643 | — | 13000 |
| (0, 4) | 30643 | 30644 | 30045 |
| (0, 2), (0, 3), (1, 2), (1, 3) | 32310 | 32311 | 5006 |

The *commit registers* of a device are its descriptor's country and grid-code registers plus
30036 and 30037. Every protocol-2/3 inverter is therefore `unit` with the 32310 descriptor, and a
non-inverter device is `unit_nodesc`. Setup and Installer are offered only for an inverter whose
family is `str_pv`, `res_new_pv`, `res_es` or `unit`.

### 3.2 Mappings

| Mapping | Rule |
|---|---|
| Poll table | protocol 1 and 4 → `inv_proto1_4`, 2 → `inv_proto2`, 3 → `inv_proto3`, 5 → `inv_proto5` |
| :502 register map | dev_type 1 / 21 → `tcpslave_dev1_21`, 35 → `tcpslave_dev35`, 18 → `tcpslave_dev18`, 44 → `tcpslave_dev44`; any other → the device's poll table |
| Start/stop register | string `start_stop` 5006, 30045 on protocol 4; hybrid `hybrid_start_stop` 5006, 13000 on protocol 5 (`data/write_registers.yaml` `address_by_protocol`) |
| Cloud devType | from the table type: 0x01 / 0x15 → 1, 0x23 → 14, 0x12 → 7, 0x2C → 43, 0x22 → 46, 0x2E → 51, 0x5C → 92; devType 0 is not reported |
| Capability gating (bitmap 0x2181) | on for `inv_proto2` / `inv_proto3`, YAML `gate_capabilities` overrides |
| Frame gap | 1 ms for battery, charger and Energy Bridge tables, 18 ms otherwise |
| SBR cell block | `table: battery`: 10757 + 24 every `sbr_interval` (300 s) |

### 3.3 Matrix

| dev_type (source) | protocol | inv type | poll table | :502 map | control profile, start/stop | cloud devType | XML family | country register |
|---|---|---|---|---|---|---|---|---|
| 21 or 1 (devinfo) | 1 | 0 / unknown | `inv_proto1_4` | `dev1_21` | string, 5006 | 1 | `str_pv` | 30500 |
| 21 or 1 (devinfo) | 1 | 1 | `inv_proto1_4` | `dev1_21` | string, 5006 | 1 | `res_new_pv` | 30643 |
| 21 or 1 (devinfo) | 1 | ≥ 2 | `inv_proto1_4` | `dev1_21` | string, 5006 | 1 | `unit_nodesc` | — |
| 21 or 1 (devinfo) | 2 / 3 | any | `inv_proto2` / `inv_proto3` | `dev1_21` | string, 5006 | 1 | `unit` | 32310 |
| 21 or 1 (devinfo) | 4 | 0 / unknown | `inv_proto1_4` | `dev1_21` | string, 30045 | 1 | `none` | — |
| 35 (devinfo) | 5 | any | `inv_proto5` | `dev35` | hybrid, 13000 | 14 | `res_es` | 30643 |
| 35 (devinfo) | 2 / 3 | any | `inv_proto2` / `inv_proto3` | `dev35` | hybrid, 5006 | 14 | `unit` | 32310 |
| 35 (devinfo) | 1 | 0 / 1 | `inv_proto1_4` | `dev35` | hybrid, 5006 | 14 | `str_pv` / `res_new_pv` | 30500 / 30643 |
| 1 (table), devinfo refused | 1 | from the poll | `inv_proto1_4` | `dev1_21` | none (not discovered) | 1 | `str_pv` / `res_new_pv` / `unit_nodesc` | 30500 / 30643 / — |
| 44 (SBR identity) | 0 | — | `battery` | `dev44` | none | 43 | `unit_nodesc` | — |
| 18 (meter table) | 0 | — | `other_dev18_*` | `dev18` | none | 7 | `unit_nodesc` | — |
| 46 / 92 (charger / Energy Bridge) | 0 | — | `charger` / `energy_bridge` | poll table | none | 51 / 92 | `unit_nodesc` | — |

The two tested inverters: the **SG5.0RS** reports dev_code 0x2606, protocol code "AA10", tag "ZS"
(protocol 3, dev_type 21, family `unit`, country 32310); the **SH10RS** reports dev_code 0x0D1B,
"AA10", "ZS" (protocol 3, dev_type 35, family `unit`, country 32310). Both answer 0xFFFF at 30644.
SBR192 stacks report device code 0x20EA.

### 3.4 Modbus-TCP server and the country

The `Modbus-TCP server` switch defaults on. Until the user toggles it (a persisted flag; on a boot
without the flag, a stored "off" counts as a user choice) and unless its restore mode pins it,
the server follows the country of the first inverter, as the stock dongle does: on for country
codes 6, 99 and 114..117, off otherwise. A Setup or Installer frame that starts at the device's
country register and is written OK applies the same rule.

MQTT never exposes installer or grid-code settings.

---

## 4 Setup data

### 4.1 Inputs

| Input | Content |
|---|---|
| `data/grid_profiles/index.yaml` | the settings families, the descriptor table and the Setup countries |
| `data/grid_profiles/<family>.yaml` | per family `str_pv`, `res_new_pv`, `res_es`, `unit`: the setup profiles with their register sets, the register width table and the installer catalogue (English labels) |
| `data/installer_rules.yaml` | value rules per family (ranges in display units, regulation rules `@<address>`, scale) |
| `data/installer_help.yaml` | help text per (family, address) |
| `data/dsp_params/models.yaml`, `data/dsp_params/*.csv` | power-stage acceptance ranges per inverter model (§5.4) |

The descriptor table of §3.1 exists in `components/sungrow_modbus/discovery.cpp` and in
`data/grid_profiles/index.yaml`, from which `tools/gen_grid_profiles.py` derives each family's
commit registers. No component fetches or parses anything at runtime; everything is generated
(`make gen`, checked by `make check-gen`). `tools/fetch_grid_profiles.py` downloads the current
grid-code XML set from iSolarCloud into `data/fetched/grid_profiles/<region>/` (not tracked) and
converts it to the project format (`--to-project-format`, `tools/grid_profiles_xml.py`) for
`tests/test_xml_drift.py` to compare. The file formats are described in `docs/data_format.md`.

### 4.2 Setup profiles (`tools/gen_grid_profiles.py`)

One option per profile of `data/grid_profiles/<family>.yaml` (for `unit` the picker bound to
register 30036); a profile's variants are told apart by input 5479 when `variant_by_input_5479` is
set. Each variant holds its register sets in file order, verbatim (U32 low word first). Output:
`components/sungrow_control/grid_profiles.gen.{h,cpp}` and `web/src/setup_profiles.gen.ts`. The
bundled set:

| Family | Option code | Standard | Variants |
|---|---|---|---|
| str_pv | 1 | AS/NZS 4777.2:2015 | one, 20 sets |
| res_es | 1 | AS/NZS 4777.2:2015 | one, 16 sets |
| res_new_pv | 1 | AS/NZS 4777.2:2015 | one, 14 sets |
| unit | 12 | AS/NZS 4777.2:2015 | one, 19 sets |
| unit | 13 / 14 / 15 | AS/NZS 4777.2:2020 Australia A / B / C | 5479 ∈ {1, 2} → 111 registers (PV); 5479 ∈ {3..6} → 121 registers (ES) |

The country picker offers Australia (code 6).

### 4.3 Installer catalogue (`tools/gen_installer_settings.py`)

Every number / enum / switch item of the family's `catalogue` (one item per holding register,
grouped by page breadcrumb). The unit family's country register 32310 is a catalogue item with the
country enum. The risk tier comes from the group's `write_priority` (developer → expert, senior →
advanced, else basic) and is presentation only; there is one role.

Ranges per (family, address), recorded as `rangeSource`:
- `stock`: the `data/installer_rules.yaml` range, widened to cover the family's profile set values
  and enum options;
- `envelope`: min / max over the family's profile set values and enum options;
- `dtype`: the full U16 / S16 / U32 range.

Relations: the `@a op @b` rules (`<`, `<=`, `>`, `>=`, `=`) of `data/installer_rules.yaml` whose
registers are both in the family's allow-list (`tools/gen_installer_rules.py`). The allow-list of
a family is its catalogue addresses (U32: both registers), every register of its profile sets and
its commit registers; `none` and `unit_nodesc` have none.

Outputs: `web/src/installer_settings.gen.ts` (`INSTALLER_SETTINGS`), and
`components/sungrow_control/installer_allow.gen.h` / `installer_rules.gen.h` for the server-side
allow-lists, ranges and relations.

Catalogue fields: `family`, `address` (1-based holding register), `label`, `category`, `control`
(number / enum / switch), `dtype` (U16 / S16 / U32), `wordOrder` (`lo_first` on U32 rows), `scale`,
`accuracy`, `unit`, `risk`, `min`, `max`, `rangeSource`, `regulation` (the rule string with every
row reference written `@<address>`, e.g. `"@32322<@32319;@32322>=@32326"`), `help`, `options`.

### 4.4 Protection panel

`data/families/inverter.yaml` has a `reads` list of inverter grid-code registers that are
not polled: country 30500 / 32310, standard 30036, the 32318..32329 block (recovery and stage-1 /
stage-2 OV / UV / OF / UF), 30092, 32536 / 32537 / 32549 / 32550, Volt-Watt 31230..31238 and
Q(U) 31865..31872. `gen_web_meta.py` emits them as `PROTECTION_READS`; the Devices page's
read-only Protection panel reads them on demand.

---

## 5 Writing

Both pages end in `ControlCore::setup_apply()`, which takes one batch at a time and sends it
frame by frame from the main loop. Web writes are authorised by the web login and a confirm
token, not by the "Inverter writes enabled" switch; :502, MQTT and the HA action can reach only
the routine profile registers (plus YAML `registers:` extras), never the installer allow-list.

### 5.1 Installer batch (`POST /api/setup_write`)

| Step | Behaviour |
|---|---|
| Pending edits | The page keeps a map setting → new raw value; entries equal to the current value are dropped; U32 = low word at `address`, high word at `address + 1`. |
| Inline checks | Range (display units) and the row's regulation rule; a range error or a broken rule disables Apply; a rule whose peer could not be read shows "not checked here" and does not block. |
| Request | "Apply N change(s)" → `POST /api/confirm?device=D&address=65535` → `POST /api/setup_write?device=D&confirm=T`, body `"addr:val,addr:val,..."` (≤ 128 pairs, ≤ 8192 bytes). |
| Batch checks | BUSY while a batch runs; 1..128 registers; unknown or offline device → OFFLINE; duplicate address → INVALID. |
| Per register | routine-profile register → its write rule (BAD_VALUE `range:A`); otherwise it must be in the family's allow-list (NOT_ALLOWED `register A`), then inside the model's power-stage range (BAD_VALUE `dsp_range:A:MIN-MAX`, §5.4), then inside the generated range (BAD_VALUE `range:A`), then every relation over the batch values and the last read values: unknown peer → BAD_VALUE `relation:<addr>`, violated → BAD_VALUE `relation:<lhs><op><rhs>`. |
| Order | Commit registers first (stable), as the stock web UI orders them. |
| Frames | Adjacent contiguous registers of the same class (routine / protection) merge, ≤ 123 per frame. |

### 5.2 Setup profile (`/api/setup_options` → `/api/setup_preview` → `/api/setup_apply`)

| Step | Behaviour |
|---|---|
| Device | `#/setup/<index>`, else the first identified inverter with a supported family; `none` / `unit_nodesc` → "Grid-code setup is not supported for this inverter type". |
| Options | `GET /api/setup_options?device=D` (start-or-poll): reads input 5479 and the country / grid-code registers (+ 30036 / 30037 for `unit`) when the last read is older than 30 s. Reply: family, descriptor, feature bits W32B5 / W29B12 of bitmap 0x2181, 5479, the current country / grid code / company / grid type, the countries and the family's options. |
| Prepare | `GET /api/setup_preview?device=D&option=O` (start-or-poll): reads 5479 when the option needs it, resolves the profile, then reads every register it will write. A prepare is valid for 60 s. |
| Resolve | 1. the descriptor country register := 6; 2. on `unit` with W32B5, 30036 := option code; 3. the variant's `<set>` runs in XML order. Errors: NO_FAMILY, NO_DESCRIPTOR, UNKNOWN_OPTION, NEED_CAPABILITY (the capability bitmap is not polled yet), NEED_5479, NO_VARIANT_FOR_5479, GRID_TYPE_UNSUPPORTED. |
| Preview | Items `{a, new, cur, valid, kind, changed}` (kind country / company / grid_type / set; an unknown current value counts as changed), the frames, `n_changed`, and `note`: the items the apply will skip because they are outside the model's power-stage range. The page shows the diff (U32 rows joined low word first) and the note as a warning. |
| Apply | "Apply profile…" → dialog (standard, variant, totals) → token (device, 65534) → `POST /api/setup_apply?device=D&option=O&confirm=T`. Needs a complete prepare of the same (D, O) younger than 60 s, else PREVIEW_REQUIRED; the prepare is used once. Family allow-list for protection registers, routine registers value-checked, no range or relation check, **order kept** (country, 30036, then the sets), the XML values written verbatim. Items outside the power-stage range are skipped with a WARN log and returned as `note`. |
| Progress | The page polls `/api/grid_read` every 800 ms and shows a row per frame. |
| Start | "Start inverter…" (when the descriptor has a power switch) → dialog → token (device, 65533) → `POST /api/setup_start`: writes 0xCF to the descriptor's power-switch register (fn 16) through the profile's start/stop rule, which starts the start/stop follow-up (`docs/control.md`). |

Nothing stops the inverter for a setup, and nothing decides whether a device needs one: the Setup
page shows the country, standard and grid type read from the inverter.

### 5.3 Sending the batch

| Aspect | Behaviour |
|---|---|
| One frame in flight | the next frame is submitted when the previous one finished |
| Offline device | the frame waits, up to the deadline |
| Headroom | waits while the master holds ≥ 2 queued writes |
| Protection frame | allow-list re-check, fn 16 always, read-back verify; the written values update the read cache |
| Routine frame | the profile rules; fn 6 for one register, fn 16 for more; verified |
| Deadline | 60 s from acceptance: frames still queued are dropped as `timeout` (device online) or `offline`, WARN log |
| Result | per frame: status and exception; the batch continues after a failed frame |

Frame statuses: `pending`, `ok`, `exception`, `timeout`, `expired`, `verify_mismatch`,
`verify_failed`, `offline`, `rate_limited`, `queue_full`, `invalid`.

### 5.4 Power-stage acceptance ranges

The inverter's power stage ignores a parameter write outside its own range while the control board
acknowledges it, so a value can read back as written and still have no effect. The firmware carries
these ranges for the firmware lines listed in `data/dsp_params/models.yaml` (SH-RS, SG-RS, SH-T),
one CSV per line, generated into `components/sungrow_control/dsp_ranges.gen.h` and matched on the
device code of the devinfo block, else on its model string. A U32 setting is compared as one
32-bit value.

- Installer: a value outside the range is refused with BAD_VALUE `dsp_range:ADDR:MIN-MAX`.
- Setup profile: the item is skipped (with the second word of a U32), logged at WARN and listed in
  the preview and apply `note`; the rest of the profile is written, so a stock country profile stays
  applicable.
- Any other model has no power-stage range check; the installer ranges still apply.

Adding a line is a row in `models.yaml` and its CSV, adding a model is a name under `models`, then `make gen`. Background:
[SH10RS register windows](https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/sh10rs/register-windows.md).

### 5.5 On-demand reads (`/api/grid_read_start`, `/api/grid_read`)

`POST /api/grid_read_start?device=D` with a body `"addr,addr,..."` (up to 256 addresses) replaces
the single read cache. Readable are the family's allow-list, the routine profile registers and
{30644, 30051, 32310, 32311, 30036, 30037, 30500, 30502, 30643}; other addresses stay invalid with
reason `off-list`. Reads run as contiguous fn 3 runs of ≤ 64 registers; an exception on a longer run
splits it in halves, a single register reports `exception`, a timeout marks the run `timeout`.
`GET /api/grid_read?device=D` answers `{device, complete, values: [{a, v, valid, reason?}], setup:
{device, busy, total, done, ok, failed, deadline_ms, frames: [{start, n, status, ex}]}}`. Setup's
option and preview reads use their own read sets.

### 5.6 Results, tokens and logs

Every write endpoint answers `{ok, verdict, message}`: `verdict` is `OK`, `DISABLED`, `NOT_ALLOWED`,
`BAD_VALUE`, `OFFLINE`, `BUSY`, `INVALID`, `PREVIEW_REQUIRED`, `CONFIRM_REQUIRED` or `UNAVAILABLE`;
`message` is `"queued"` or the detail (`range:32323`, `dsp_range:32550:1000-1200`,
`relation:32323<=32327`, `register 30646`, `duplicate register N`, `device D`, …).

Confirm tokens (`POST /api/confirm?device&address`) are random, bound to (device, address: 65535
Installer, 65534 Setup apply, 65533 Setup start), valid 20 s and used once; four can be pending.

| Event | Log |
|---|---|
| Rejected batch / prepare / start | WARN `setup rejected: <verdict> (<detail>)` |
| Batch accepted | INFO `setup dev D (<family>): N register(s) in F frame(s) queued (installer|profile, <source>)` |
| Profile items skipped | WARN `setup dev D (<model>): skipped <note>` |
| Protection frame | INFO `write grid-code dev D A=[…] (src): queued`, then the result; the "Inverter last write" text sensor |
| Deadline | WARN `setup dev D: deadline (60 s) passed, F frame(s) / N register(s) dropped unsent` |
| Prepare | INFO `setup prepare dev D (<family>) option O`, then the status and item count |

Source tags: `api:web`, `api:installer`, `api:setup`. Both frame kinds count in the "Inverter
writes ok / failed" sensors.
