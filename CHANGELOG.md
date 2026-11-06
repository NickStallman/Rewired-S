# Changelog

All notable changes to Rewired-S are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow semantic versioning.

## [Unreleased]

### Added
- `tools/pyproject.toml`: the install tools as a pip package
  (`pipx install "git+https://github.com/NickStallman/Rewired-S@v0.9.1#subdirectory=tools"`):
  `rewired-s-deploy`, `rewired-s-iconfig-server`, `rewired-s-sgu`, `rewired-s-package-sgu`,
  `rewired-s-flash-backup`, `rewired-s-read-identity`.

### Changed
- The entries work as ESPHome remote packages
  (`packages: x: github://NickStallman/Rewired-S/<entry>.yaml@v0.9.1`): the board files name their
  partition tables relative to themselves, the `sites/` and `tests/config/` links are gone, and
  `dashboard_import` pins the running release.

### Fixed
- The ESPHome dashboard's adoption of a pre-built image failed on the partition-table path.

## [0.9.1] - 2026-11-07

Feedback round after the first public release: the SH15T (SH-T line) verified on hardware,
the WiNet-S2 install route and Ethernet build verified, the install tooling and docs fixed, a
network fallback for the Ethernet images, and a privacy fix in the diagnostics log tail.

### Added
- Power-stage acceptance ranges for the SH-T line (`data/dsp_params/sh15t.csv`, PEARL-H). The
  range tables are keyed by firmware line and matched by device code or devinfo model string, so
  every listed SH-RS, SG-RS and SH-T model gets them (#9).
- Network fallback (`sungrow_board` `network_fallback:`, `common/ethernet_fallback.yaml`, on in
  the pre-built Ethernet entries): a build that has never had a network boots the other OTA slot
  after 3 min (1 min when the Ethernet driver fails), if that slot holds a non-Ethernet Rewired-S
  image (#11). The app descriptor's version carries the network kind (`0.9.1+wifi` / `+ethernet`).
- Network fallback state on Settings → Boot slots and in the diagnostics report.
- `docs/board_pins.md` "Other boards": what an ESP32 or ESP32-S3 board needs to run the firmware.

### Changed
- Battery controls (`requires: battery` in `data/write_registers.yaml`) are left out of MQTT
  discovery and the web Devices page on a hybrid that reports "No battery" (#9).
- `docs/compatibility.md`: every inverter class with its firmware line, reads, controls,
  power-stage ranges and hardware verification; WiNet-S2 Ethernet and the stock-updater route
  recorded as verified (#9, #10, #11).
- The analytics `uncatalogued` check uses `dsp_model_for(dev_code, model)`: a device is
  unrecognised only when no range table matches its code or model string.

### Fixed
- Diagnostics: values ESPHome logs as secrets (Wi-Fi password, API key) enter the log tail as
  `<secret>`; the report scrubber replaces the Wi-Fi and MQTT passwords (`<psk>`, `<mqtt-password>`).
- `tools/fake_iconfig_server.py` offers the package only to a request that names a WiNet (the
  stock device-update dialog asks with the inverter's model) and matches `/web/<name>` with or
  without the `/v2` prefix (stock `P016`) (#10).
- `tools/deploy_debug.py import-key` treats `result_code=120` as "key already stored" and continues.
- The release public key is published as `publicKey_V1.pem`, the name the stock updater's key
  import requires; the install guide names the exact menu path, warns about the device-update
  dialog, and notes the browser-address and PowerShell requirements.

## [0.9.0] - 2026-10-31

First public release (alpha). Runs on the WiNet-S (ESP32) and WiNet-S2 (ESP32-S3) dongles with
an SG string inverter or an SH hybrid inverter with SBR battery stacks.

### Added
- Per-device Home Assistant MQTT discovery for the connected inverter, battery stacks and meter,
  with a Problem binary sensor carrying the active fault codes and names.
- Web UI: dashboard, devices and signals, installer and grid-code setup pages, diagnostic captures
  (I-V scan, grid waveform, fault recordings) with browser viewers for waveforms and I-V curves,
  log console.
- Gated inverter register writes (EMS mode, forced charge/discharge, SOC limits, export limit,
  power limitation, start/stop) with an allow-list, range checks, read-back verification, a
  start/stop follow-up that reports when the inverter has actually stopped or restarted, and a
  power-limit re-assert for string inverters after a power-stage restart.
- Power-stage acceptance ranges per inverter model (`data/dsp_params/`) applied to installer and
  profile writes.
- Modbus-TCP slave on port 502 served from the signal store.
- iSolarCloud uplink presenting the dongle as the stock unit.
- Tiered RS485 polling (1 s / 5 s / 30 s / 5 min) with a filtered `/api/signals` endpoint.
- Reset diagnostics: RTC journal with the previous boot's log tail and core-dump summary, optional
  UDP syslog, `make deploy` with log streaming across the OTA.
- Stored core dumps: summarised in "Last reset" and `/api/diag`, served at `/api/coredump`, and
  erased by the boot after the one that reported them.
- Inverter clock sync from SNTP local time after boot and daily, independent of the writes switch.
- Four generic entries (`winet-s.yaml`, `winet-s-ethernet.yaml`, `winet-s2.yaml`,
  `winet-s2-ethernet.yaml`) with the production package set, and debug entries for a first install.
- Site files under a git-ignored `sites/` directory that set the node name,
  `cloud_firmware_version` and local options on top of an entry.
- `cloud_firmware_version` substitution for the version string reported to iSolarCloud.
- Native-API device entities as an opt-in package (`common/native_entities.yaml`) in place of
  MQTT discovery.
- Generator inputs under `data/` (register maps, grid-code XMLs, translations, stock parameter
  tables, power-stage ranges), `make gen` and `make check-gen`.
- `fast_loop` option of the RS485 master: the main loop runs without its idle delay while a
  transaction is in flight (default off).
- Stock-OTA packaging, network flash backup and package-server tools for the first install
  (`tools/package_sgu.py`, `tools/flash_backup.py`, `tools/deploy_debug.py`,
  `tools/fake_iconfig_server.py`).
- CI: host suites, web suite, generated-file check and `esphome config` on every push; firmware
  builds on tags.
- Anonymous usage reporting (`sungrow_analytics`): a daily ping with versions, board, network
  kind, feature state and the connected device models, plus throttled error events. On in every
  entry; `analytics: "false"` turns it off.
- Diagnostics report: a generated plain-text report of the build, the detected devices with
  their raw identification reply, register block map and static register values, feature state,
  memory, the reset journal and the log tail, scrubbed of serials, addresses and the Wi-Fi name.
  Uploaded automatically on crash-class resets, runtime faults and unrecognised devices (three
  per day at most), or from the web UI's **Send diagnostics** button; **Download report** saves
  it locally.
- `unsupported_device` reporting when discovery finds a device without a register table or
  catalogue entry.
- Pre-built images (`winet-s-prebuilt.yaml`, `winet-s-ethernet-prebuilt.yaml`,
  `winet-s2-prebuilt.yaml`, `winet-s2-ethernet-prebuilt.yaml`; `docs/prebuilt.md`): Wi-Fi from
  the captive portal (Improv Serial on the WiNet-S), the API key from Home Assistant at adoption,
  a **Firmware** update entity fed by the release manifest, and no `!secret` anywhere
  (`make config-prebuilt`, `tests/test_prebuilt_no_secrets.py`).
- Web login changeable at runtime (`sungrow_web` `login: runtime`, Settings → Web login, stored
  in NVS; default `admin` / `rewired-s` with a banner until changed). The native OTA password of
  the pre-built images follows it.
- MQTT broker settings at runtime (`sungrow_mqtt` `runtime_settings`, Settings → Home Assistant):
  host, port, credentials, discovery prefix and TLS stored in NVS, a `connected` binary sensor
  and the last broker error on the page. `broker` is optional with `runtime_settings`.
- Cloud version detection: `cloud_firmware_version: ""` reads the stock app's version from the
  other OTA slot on first boot; Settings → iSolarCloud identity shows the source and takes an
  override that applies without a restart.
- `sungrow_board` `boot_slot:`: the `Allow boot switch` switch (never restored) and `Boot other
  slot` button in every entry, plus Settings → Boot slots with a confirm dialog, for the way back
  to the stock app. `sungrow_debug` reuses the same code.
- **Usage reporting** switch (`sungrow_analytics` `reporting:`), remembered across restarts; the
  `analytics` substitution is its default.
- `tools/sgu.py`: inspect, extract, verify, build and sign `.sgu` packages, and `keygen` for a
  signing key pair; `tools/package_sgu.py` builds **app-only** packages by default (no stock
  content needed) and takes `--stock-sgu` to add the stock web-UI part. `make sgu`, `make sgu-keys`.
- Release workflow: pre-built and debug images, app-only `.sgu` packages signed with the project
  release key, a GitHub pre-release with the changelog section as notes, and a GitHub Pages site
  with ESP Web Tools (WiNet-S serial route) and the update manifests.
- `tools/hygiene_check.sh` (`make hygiene`, CI): no stock files, firmware images, keys,
  decompiler names, stock firmware addresses or serial numbers in the tree.
- Documentation for publishing: compatibility chart, install guides (updater route, serial
  route), running the tools on Windows / macOS / Linux, back to stock, configuration, Home
  Assistant, contributing; sample dashboards under `dashboards/`.

### Changed
- Power-limit reason code 1 is labelled "Temperature or microgrid derating".
- Default log level INFO.
- `common/base.yaml` no longer carries `api:`, `ota:` (native) and `web_server.auth`; they come
  from `common/access_secrets.yaml`, which every compiled entry includes (resulting configuration
  unchanged).
- The usage report's `stock_version` is the version the board resolves (YAML, detected or
  override) rather than the build-time constant.
- Every data input under `data/` is now in the project's own schema (`docs/data_format.md`):
  `data/families/<family>.yaml` (register tables, bit fields, naming and Home Assistant metadata
  in one file per device family), `data/modbus_tcp/`, `data/cloud/point_tables.yaml`,
  `data/grid_profiles/`, `data/installer_rules.yaml`, `data/write_registers.yaml`. The generators
  read them through `tools/datalib.py` (PyYAML, `tools/requirements.txt`); the generated tables
  are unchanged apart from their header comments. `tools/extract_init_param_xml.py` and
  `tools/fetch_grid_profiles.py` convert a dongle's grid-code files into that format.

### Removed
- The stock-format inputs (`data/register_map/*.csv`, `bitfields.csv`, `mqtt_point_table.csv`,
  the grid-code XML and label files, `stock_param_tables.csv`, the web UI catalogue) and
  `tools/register_overrides.yaml`, replaced by the files above. Nothing that originates from a
  Sungrow firmware image is in the repository.

[Unreleased]: https://github.com/NickStallman/Rewired-S/compare/v0.9.1...HEAD
[0.9.1]: https://github.com/NickStallman/Rewired-S/compare/v0.9.0...v0.9.1
[0.9.0]: https://github.com/NickStallman/Rewired-S/releases/tag/v0.9.0
