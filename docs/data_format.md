# Data files (`data/`)

Everything the generators read lives under `data/` in the project's own YAML schema. The files
are plain data: every number is a Modbus register number, a scale, a code value or a poll
parameter of the device interface, and every row carries its `evidence` (SG-Docs page, public
Sungrow Modbus document, mkaiser / SunGather project or hardware observation). `tools/datalib.py`
loads and validates them; the generators (`make gen`) turn them into the C++ / Python /
TypeScript tables, and `make check-gen` fails when a generated file is stale.

Conventions shared by every file:

- register numbers are the 1-based numbers of the Sungrow documents (wire address = number − 1);
- 32-bit values span two registers, low word first;
- signal ids are written as hex (`0x2080`), everything else as decimal;
- `scale` is the multiplier applied to the raw value (`display = raw × scale`); omitted = raw;
- lists keep their order where order matters (poll order, entity order, option order).

## 1 Register tables and signals: `data/families/<family>.yaml`

One file per polled device family; the file name is the family name (`inverter`, `battery`,
`charger`, `energy_bridge`, `meter`, `logger`; the set is fixed by `sungrow_datamodel::Family`).

```yaml
family: meter
title: "Energy meters on the inverter's RS485 bus"
device_types: [18]                # dev_type codes the family covers
reference: https://github.com/NickStallman/SG-Docs/blob/main/reference/devices/meters.md
notes: |                          # optional free text
  One table per meter model.
tables:
  other_dev18_st6100:             # table name = the name the firmware looks the table up by
    title: "Acrel DTSD1352 three-phase meter"
    subtype: 6100                 # optional: model / subtype code (meters, loggers)
    protocols: [2]                # optional: protocol types the table serves (inverters)
    rows:
    - {id: 0x2000, address: 4097, fn: 4, count: 2, type: f32, cycle: 1, flags: clear_offline}  # phase_a_voltage
signals:
- id: 0x2000
  key: phase_a_voltage
  name: "Phase A voltage"
  unit: V
  scale: 0.1
  device_class: voltage
  state_class: measurement
  category: core
  priority: 10
  confidence: confirmed
  evidence: "st6100@4097 st6200@4097; SG-Docs meters.md; readings: 240.1 V on the test meter"
fixes:                            # optional
- {table: other_dev18_st6100, id: 0x210E, stock_address: 4161, address: 4163, confidence: confirmed, evidence: "..."}
reads:                            # optional (inverter only today)
- {key: over_voltage_recovery, name: "...", unit: V, scale: 0.1, fn: 3, address: 32318, dtype: u16, group: protection, priority: 510, confidence: confirmed, evidence: "..."}
```

### Rows (`tables.<name>.rows`)

One entry per register block the firmware reads, in poll order. The order matters: the request
builder merges consecutive rows into Modbus requests (see "Poll terms" below).

| Field | Meaning |
|---|---|
| `id` | signal id, family-scoped: an id means the same quantity in every table of the family, so one signal definition covers every row that carries it. `0` = a gap-filling read whose value is not stored |
| `address` | first register (1-based) |
| `fn` | Modbus function: 4 input, 3 holding, 16 write-multiple, 119 proprietary |
| `count` | registers read |
| `type` | `s16 u16 s32 u32 s64 u64 f32 str` (`f32` = IEEE float over 2 registers) |
| `cycle` | poll cycle in 100 ms units (1 = every poll round, 600 = once a minute, 36000 = hourly, 0 = export map only). A request node is polled at the cycle of its first row |
| `flags` | space-separated: `end` (the row closes its request node), `clear_offline` (clear the value when the device goes offline), `priority` (node priority 2), `no_bulk_decode`, `keep_sentinel` (store the value even when it is the sentinel) |
| `bits` | `{first, count}`: the row is a status word whose bits are published as signals `first + bit` (`<parent key>_bit<n>`, hidden) |
| `capability` | `word.bit` into the capability bitmap (signal 0x2181): the row is polled only when that bit is set |
| `unit`, `scale` | only for rows without a signal definition (the display unit and multiplier of the raw value); rows with a definition take them from the signal |

Poll terms: a request node is one Modbus read. The builder walks the rows of one function code
in order; a row joins the current node while the span from the node's first register stays
within 120 registers (otherwise it starts a new node), a row with `end` closes the node after
itself, and a row whose cycle differs from the node's first row is still merged (the node is
polled at the first row's cycle; the one exception is register 0x396d, skipped in that case). The
reference implementation of these rules is `tests/fixtures/stock_request_nodes.py`;
`tests/test_request_nodes.py` checks `tools/gen_tables.py`'s mirror of the C++ builder against
it, and `tests/test_datamodel.cpp` the C++ builder itself.

### Signals (`signals`)

One entry per signal id of the family, in the order the entities appear in the generated device
packages (`devices/*.yaml`).

| Field | Meaning |
|---|---|
| `id`, `key` | the id and the stable entity key (lower-case snake_case, unique per family; YAML packages and MQTT use it) |
| `name` | English entity name |
| `unit`, `scale` | display unit (`kW`, `℃`, `kvar`, ... — the generator normalises to Home Assistant units) and raw multiplier. These go into the C++ rows and change decoding |
| `device_class`, `state_class` | Home Assistant classes (omitted = none) |
| `category` | `core` (core package of every device of the family), `core_hybrid` / `core_string` (core only in that inverter package, extra elsewhere), `extra` (the `*_extra` package, disabled by default), `hidden` (no package, still a key). Omitted = not placed by category; the package definition in `tools/gen_tables.py` (`DEVICES`) lists it explicitly (the SBR cell block) |
| `scope` | `hybrid` = meaningless on a string inverter, dropped from the string packages |
| `priority` | display sort rank for the SPA / MQTT (lower first; omitted = unranked) |
| `group` | `realtime` (default), `static` (device facts, HA diagnostic), `protection` |
| `accuracy` | display decimals (default: derived from the scale) |
| `enum` | name of a decode table in `tools/gen_tables.py` `ENUMS` (raw code → text) |
| `absent` | space-separated conditions under which a polled value is "not served": a raw value (`0xFFFF`), `single_phase` (the device reports output type 0), `no_meter` (no meter power valid) |
| `confidence` | `confirmed` (an independent public source agrees, or two devices read it as named), `likely` (one good source), `guess` (inferred from layout; verify on hardware) |
| `evidence` | where the id lives (`<table>@<address>`, `/fnN` for non-input rows) and the sources |

Key suffixes (inverter): a quantity that lives under several ids — one per register map, only
the connected inverter's protocol populates one — uses `_sg` (SG public map), `_sh` (SH public
map), `_pub` (shared by both public maps) and `_slave` (only on a :502 export map); the bare key
is the private map's id. The device packages collapse the suffixed keys into one entity with a
fallback list.

### Fixes, shared addresses and reads

- `fixes`: rows whose address differs from the one the stock dongle polls. The row already
  carries the corrected `address`; the entry records the `stock_address`, `confidence` and
  `evidence`, and the generator checks that the table has exactly one such row and none at the
  stock address.
- `shared` (under a table, mostly in the export maps): `{id, address, fn, evidence}` for two rows
  of the same id that legitimately share an address. Any other overlap fails the build.
- `reads`: holding registers that are not polled (no table row, no id) but shown read-only:
  `key, name, unit, scale, fn, address, dtype (u16 | s16), group, priority, enum, accuracy,
  confidence, evidence`. They reach `register_meta.py` `READS` and the SPA's protection panel.

Signals that the firmware derives (`total_yield`, per-MPPT power, fault lists, ...) are defined
in `tools/gen_tables.py` (`DERIVED`), not here.

## 2 Modbus-TCP export maps: `data/modbus_tcp/<map>.yaml`

The register map served on port 502 for a device type: `table` (the name the firmware looks it
up by), `family`, `device_types`, `title`, `rows` (`{id, address, fn, count, type}`; `cycle`
appears on the few rows that double as poll rows) and `shared`. Names, units and scales come
from the family's signal definitions.

## 3 Cloud point tables: `data/cloud/point_tables.yaml`

```yaml
tables:
- table_type: 0x23              # the table a device model selects (= its dev_type)
  cloud_device_type: 14         # devType the cloud expects in businessData
  title: "hybrid inverter (dev_type 35)"
  points:
  - {signal: 0x2080, point: 24, attrs: [rundata], format: 9, scale: 1000, size: 4, type: s32}
```

`signal` is read from the signal store (family of the device); `point` is the cloud point id
(`-1` = placeholder row); `attrs` lists the uplink messages that carry it (`attributes`,
`rundata`, `status`, `general`); `format` is the encoder (`3`/`4` int16, `5`/`6` int32, `8`
int64, `9` float32 × scale, `10` double × scale, `12` string, `13`/`101` hex;
`components/isolarcloud/rundata_format.cpp`); `size` is the value size in bytes and `type` its
type. Row order = publication order.

## 4 Grid-code profiles and installer catalogue: `data/grid_profiles/`

`index.yaml` names the settings families in `sungrow_modbus::XmlFamily` order (`none`, `str_pv`,
`res_es`, `res_new_pv`, `unit`, `unit_nodesc`; `file: true` = has a file), the `descriptors`
table that maps (inverter type, protocol type) to a family and its commit registers (country,
grid code, power switch), the `setup_countries` the Setup view offers and the unit family's real
company / grid-type registers.

`<family>.yaml`:

```yaml
family: unit
registers:                      # holding registers the family touches: width (+ English label)
  32362: {type: U32}
profiles:                       # the country / standard picker
- code: 13
  label: "AS/NZS 4777.2:2020 Australia A"
  writes_sets: true             # applying the profile writes its register sets
  variant_by_input_5479: true   # the variant is chosen by a live read of input register 5479
  variants:
  - input_5479: [1, 2]
    sets:
    - {address: 32318, words: [0x09E2, 0x0802]}   # words as they go on the wire
grid_type_profiles:             # grid-type options: their sets feed the allow-list / envelope
- {code: 1, label: "...", sets: [...]}
catalogue:                      # the installer page, grouped by page breadcrumb
- path: ["Settings", "Power control", "Active power settings"]
  write_priority: senior        # developer -> expert, senior -> advanced, else basic
  read_priority: senior         # optional
  items:
  - {address: 31202, label: "Active power ramp-up rate", control: number, type: U16, scale: 1, accuracy: 0, unit: "%/min"}
  - address: 31230
    label: "Volt-Watt"
    control: switch
    type: U16
    refresh: true               # optional: the page re-reads after a write
    options:
    - {value: 85, label: Disabled}
    - {value: 170, label: Enabled}
```

Catalogue items: `control` is `number` (`scale`, `accuracy`, `unit`), `enum` or `switch`
(`options` as `{value, label}`); `type` is `U16`, `S16` or `U32` (U32 spans two registers, low
word first). One item per register per family. Labels are English only.

`data/installer_rules.yaml` holds the value rules per family: `{address, label, type, scale,
unit, range, regulation}` where `range` lists intervals in display units and `regulation` is a
`;`-separated rule string with `@N` for the value of register N (`"@32322<@32319;@32322>=@32326"`);
simple `@a op @b` rules become server-enforced relations. `data/installer_help.yaml` maps
family → register → tooltip text.

## 5 Write allow-list: `data/write_registers.yaml`

`registers`: one entry per writable holding register: `profile` (`hybrid` | `string`), `key`,
`name`, `address`, `address_by_protocol` (`{protocol type: address}`), `type` (`u16` | `s16`),
`min` / `max` (raw), `values` (the only raw values accepted), `options` (`{value, label}` for
selects), `scale`, `unit`, `step`, `number_min` / `number_max` (display units), `interval`
(read-back period in s), `requires` (`battery`: left out of MQTT discovery and the web Devices
page while the inverter's battery type reads "No battery"), `mkaiser`, `confidence`, `evidence`. The header of the file documents
each field.

## 6 Power-stage ranges: `data/dsp_params/`

`models.yaml` lists each power-stage **firmware line** whose acceptance ranges are known: `key`
(identifier), `name` (SH-RS, SG-RS, SH-T), `family` (`hybrid` | `string`), `firmware` (the
power-stage image the ranges were read from), `csv`, `models` (the devinfo model strings the
line covers, as the inverter reports them: `SH5.0RS`, `SH15T`, `SH5.0RS-20`) and `dev_codes`
(`{model: devinfo device code}` for the models a dongle has reported). The CSV
(`doc_reg,count,min,max,label,unit`) gives one raw range per register (`count` 2 = U32). A device
is matched by its device code first, then by its model string, so every model of a line gets the
line's ranges; the same table applies to the whole line because the ranges live in the shared
power-stage image (model-specific limits are dynamic clamps the table never held).

## 7 Adding a new device family

Say a new RS485 device type 77 ("hub") appears with its own register map.

1. **Create `data/families/hub.yaml`** (the family name must also be added to
   `sungrow_datamodel::Family`, `tools/datalib.py` `FAMILIES` and the `family` lists of the
   components that switch on it). Header: `family`, `title`, `device_types: [77]`, `reference`
   (the SG-Docs page), `notes`.
2. **Describe the poll table** under `tables.<name>.rows` in the order the device should be
   read: one row per register block with `id`, `address`, `fn`, `count`, `type` and `cycle`
   (1 for live values, 36000 for identity blocks). Give each row an `end` flag where a request
   must stop, `bits` for status words, `capability` for optional blocks. Signal ids are
   family-scoped: pick ids that are unique within the family (hex, `0x2000`-based for the live
   values is the convention; derived signals use `0xF000+`).
3. **Define the signals**: one `signals` entry per id with `key`, `name`, `unit`, `scale`,
   `device_class`, `state_class`, `category` (`core` for the main package), `confidence` and
   `evidence`. Run `python3 tools/gen_tables.py -v` and read the data-quality notes: overlapping
   rows, unknown enums, missing ids and bad flags fail the build with the file and entry named.
4. **Export map (optional)**: a `data/modbus_tcp/hub.yaml` with `table`, `family`,
   `device_types`, and the public rows.
5. **Cloud (optional)**: a `tables` entry in `data/cloud/point_tables.yaml` with the device's
   `table_type` and points.
6. **Device package**: add a `DEVICES` entry in `tools/gen_tables.py` (title, family, tables,
   which categories are core, derived keys to insert) so `devices/hub.yaml` and
   `devices/hub_extra.yaml` are generated.
7. `make gen`, `make check-gen`, `make test-host`. `tests/test_request_nodes.py` checks the
   request grouping of the new table against the reference rules, `tests/test_gen_overrides.py`
   the key / address uniqueness.

For a new **settings family** (grid-code profiles): add it to `data/grid_profiles/index.yaml`
(`families` in `XmlFamily` order, a `descriptors` row for each (inverter type, protocol) that
selects it) and write `data/grid_profiles/<family>.yaml` with its `registers`, `profiles`,
`grid_type_profiles` and `catalogue`; rules go into `data/installer_rules.yaml`, help text into
`data/installer_help.yaml`. If the family's XML set is available (`tools/fetch_grid_profiles.py
--region <r> --to-project-format` or `tools/extract_init_param_xml.py --to-project-format`),
the converter writes the file for you; labels of items that the committed data does not know
fall back to readable words from the key and should be edited by hand.
