# Rewired-S

[![CI](https://github.com/NickStallman/Rewired-S/actions/workflows/ci.yml/badge.svg)](https://github.com/NickStallman/Rewired-S/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/NickStallman/Rewired-S?include_prereleases&label=release)](https://github.com/NickStallman/Rewired-S/releases)
[![Licence: GPL-3.0](https://img.shields.io/badge/licence-GPL--3.0-blue.svg)](LICENSE)
[![ESPHome](https://img.shields.io/badge/built%20with-ESPHome-1f8fcd)](https://esphome.io)
[![Sponsor](https://img.shields.io/badge/sponsor-%E2%9D%A4-ea4aaa)](https://github.com/sponsors/NickStallman)

Open-source replacement firmware for the **Sungrow WiNet-S and WiNet-S2** communication dongles,
built on [ESPHome](https://esphome.io). The dongle that plugs into your inverter becomes a local,
Home Assistant friendly gateway with inverter and battery control, while everything the stock
dongle did for other people (the iSolarCloud connection, the installer app, Modbus-TCP) keeps
working.

> [!WARNING]
> **Alpha software.** Rewired-S has been running for weeks on exactly one kind of system: an
> **SH10RS** hybrid inverter with **SBR192** battery stacks, and an **SG5.0RS** string inverter,
> each on its own WiNet-S or WiNet-S2 dongle. Other models are polled from the same register
> tables and are expected to work, but their controls have not been exercised on hardware. See
> the [hardware compatibility chart](docs/compatibility.md) and [limitations](docs/limitations.md),
> keep the way back to the stock firmware in mind ([docs/back_to_stock.md](docs/back_to_stock.md)),
> and report what you find.

## What you get

- **Every device in Home Assistant, locally.** The inverter, each battery stack and the meter
  appear through MQTT discovery as their own devices, with the same entity scheme as the
  [Winet Extractor add-on](https://github.com/NickStallman/home-assistant-repo/tree/main/winet-extractor):
  `sensor.sh10rs_<serial>_battery_soc`. Values update every second for power and every five
  seconds for MPPT and battery data, polled straight from the RS485 bus.
- **Control, with a safety gate.** EMS mode, forced charge and discharge, SOC limits, export
  limit, string-inverter power limit, start/stop. Every write passes an allow-list and range
  check, is read back, and only happens while the dongle's **Inverter writes enabled** switch is
  on (off by default).
- **A web UI on the dongle.** Dashboard, every signal of every device, the installer protection
  settings and the country / grid-code setup with a preview before anything is written,
  diagnostic captures (I-V curves, grid waveforms, fault recordings) with in-browser viewers,
  settings and logs.
- **iSolarCloud still works**, and is now a choice: the dongle presents itself to the cloud as
  the stock unit with its own serial number, so the Sungrow app and portal keep working. A
  **Cloud uplink** switch turns the connection off, and a separate **Cloud writes** switch
  (default off) decides whether the cloud may change inverter settings.
- **Modbus-TCP on port 502**, so [mkaiser's integration](https://github.com/mkaiser/Sungrow-SHx-Inverter-Modbus-Home-Assistant),
  SunGather and friends keep working against the usual register maps, with the same write gate.
- **Diagnostics built in.** A reset journal that survives reboots, a one-click diagnostics
  report with serial numbers removed, and a **Send diagnostics** button that is also how a new
  inverter model gets supported ([usage reporting](docs/analytics.md)).

| Dashboard | Device signals and controls | Settings |
|---|---|---|
| ![Dashboard](docs/images/dashboard.jpg) | ![Device detail](docs/images/device-detail.jpg) | ![Settings](docs/images/settings.jpg) |

More screenshots: [devices](docs/images/devices.jpg), [installer settings](docs/images/installer.jpg),
[grid-code setup](docs/images/setup.jpg), [captures](docs/images/captures.jpg),
[boot slots and cloud identity](docs/images/settings-boot-slots.jpg).

## Hardware

| Dongle | SoC | Network | Status |
|---|---|---|---|
| WiNet-S (V200) | ESP32, 4 MB PSRAM | Wi-Fi or Ethernet (DP83848) | tested (Ethernet on the string inverter) |
| WiNet-S2 (SV300) | ESP32-S3, 8 MB PSRAM | Wi-Fi | tested (on the hybrid inverter); pre-built image and stock-updater install verified by a user |
| WiNet-S2 (SV300) | ESP32-S3, 8 MB PSRAM | Ethernet (W5500) | verified by a user (link, DHCP, MQTT, cloud over the cable) |

Inverters and batteries: every inverter the WiNet-S supports works, because the firmware polls
each device from the register table of the protocol type it reports rather than by model name;
the [compatibility chart](docs/compatibility.md) lists every class, whether its power-stage
acceptance ranges are known, and how far it has been verified. In short: SH-RS hybrids and SG-RS
string inverters with SBR batteries are the tested line (daily use), the SH-T three-phase
hybrids are verified for reads on an SH15T and carry their ranges too; SH-RT and SG-RT
three-phase models, SBH batteries, meters, EV chargers and the Energy Bridge are polled from the
same tables but nobody has reported on them yet.

## Getting started

Two ways in. Both leave the stock bootloader, the partition table and the dongle's external
flash (its identity and cloud settings) untouched, and both keep the stock app in the other OTA
slot so you can go back.

### 1. Through the dongle's own updater, no tools or soldering (both dongles)

The stock firmware accepts a signed update package served from your LAN. Rewired-S ships as
such a package, signed with the project's release key, which you import into the dongle once.

1. Download the `.sgu` package, its `.bin` signature and the release public key
   (`publicKey_V1.pem`) from the
   [latest release](https://github.com/NickStallman/Rewired-S/releases), or build your own
   (`docs/install_ota.md`).
2. Import the public key and serve the package with the tools in `tools/`, then start the update
   from the stock web UI. The dongle writes the new image into its inactive slot and reboots into
   it. The web UI's **Boot other slot** button is the way back on both dongles (the WiNet-S2's
   stock bootloader is expected to roll a crashing image back by itself, which is not yet
   confirmed on hardware).
3. Join the `rewired-s-xxxxxx` setup network (Wi-Fi images), enter your Wi-Fi, open the web UI
   and set the MQTT broker and a new web password.

Full procedure, checks and recovery: **[docs/install_ota.md](docs/install_ota.md)**. The tools run
on Windows, macOS and Linux with nothing but Python: **[docs/tools_setup.md](docs/tools_setup.md)**.

### 2. Over the serial pads (WiNet-S only)

Two screws open the WiNet-S. With a 3.3 V USB-serial adapter on the UART pads and GPIO0 held low
at reset, `esptool` writes the image into the first OTA slot, exactly as for any ESP32 project.
Take a full flash backup first; it is the definitive way back. The release page also offers a
browser-based installer (ESP Web Tools) for this route.

Full procedure: **[docs/install_serial.md](docs/install_serial.md)**.

### Pre-built or compile your own?

- **Pre-built images** (the release assets) need no toolchain. Wi-Fi is set through the setup
  network, the MQTT broker and the web login on the Settings page, the native API key when Home
  Assistant adopts the dongle, and updates arrive through an **Update** entity. See
  [docs/prebuilt.md](docs/prebuilt.md).
- **Compile your own** when you want Wi-Fi, MQTT and the API key compiled in, a site file with
  your own options, the native-entities alternative to MQTT, or the debug build:
  [docs/configuration.md](docs/configuration.md).

## Home Assistant

The dongle itself is a native ESPHome device (the writes gate, the cloud switches, reset reason,
restart buttons). Everything behind it comes through **MQTT discovery**, because which devices
sit on the RS485 bus is only known at runtime: ESPHome entities are fixed when the firmware is
compiled, MQTT discovery lets the firmware publish one Home Assistant device per inverter,
battery stack or meter it actually finds, keyed by model and serial number. The MQTT broker is
the only extra service you need. Why, what gets published and how to build dashboards:
[docs/home_assistant.md](docs/home_assistant.md). Ready-made dashboards: [dashboards/](dashboards/).

Prefer Modbus? Leave the MQTT settings empty and point mkaiser's package or SunGather at the
dongle's port 502. Writes go through the same **Inverter writes enabled** gate.

## Going back to stock

Three ways, none of which touches the dongle's identity or settings:

1. **Boot other slot** on the web UI, while the stock app is still in the other slot.
2. Extract the app image from an official Sungrow package you download yourself
   (`tools/sgu.py extract`) and send it with ESPHome's OTA.
3. Over serial on the WiNet-S, from your backup or from that extracted image.

Details and what is preserved: [docs/back_to_stock.md](docs/back_to_stock.md).

## Usage reporting and diagnostics

Once a day the dongle sends an anonymous usage report to a collector run by the project
(firmware and board versions, the models and firmware versions of the connected devices, which
features are on, uptime, memory, reset reason) and short error events when something notable
happens. The dongle is identified by a hash of its serial number; serials, addresses, host names
and Wi-Fi names are never sent. It exists so that problems and new hardware show up without
anyone filing an issue. The **Usage reporting** switch turns it off.

**Send diagnostics** on the Settings page uploads a report of the build, the detected devices
with their identification reply and register map, feature state, memory and the recent log,
with serial numbers, the node name and addresses replaced. Quote the reference it shows in an
issue, or attach the **Download report** file instead. For a device that is not on the
compatibility chart this report is everything needed to add it. Full description:
[docs/analytics.md](docs/analytics.md).

## Documentation

| | |
|---|---|
| [Compatibility](docs/compatibility.md) | dongles, inverter families, batteries and accessories, with status |
| [Install through the updater](docs/install_ota.md) | signed package, key import, LAN pull route, go / no-go checks |
| [Install over serial](docs/install_serial.md) | WiNet-S pads, backup, flashing, ESP Web Tools |
| [Running the tools](docs/tools_setup.md) | Python setup and the tool commands on Windows, macOS and Linux |
| [Pre-built images](docs/prebuilt.md) | setup network, default login, MQTT settings, updates |
| [Configuration](docs/configuration.md) | entries, site files, secrets, substitutions, native entities |
| [Home Assistant](docs/home_assistant.md) | MQTT discovery, entity names, the dongle's own entities, dashboards |
| [Control](docs/control.md) | every writable setting, the safety model, the write paths |
| [Back to stock](docs/back_to_stock.md) | three ways back and what is preserved |
| [Cloud identity](docs/cloud_identity.md) | how the dongle presents itself to iSolarCloud |
| [Usage reporting](docs/analytics.md) | what is sent, when, and how to turn it off |
| [Debug build](docs/debug_build.md) | the read-only build for new hardware, flash backup, bus tools |
| [Limitations](docs/limitations.md) | what is unverified, different from stock, or deliberately left out |
| [Data format](docs/data_format.md) | the register, point and profile tables, and how to add a device family |
| [Device types, Setup, Installer](docs/device_types_setup_installer.md) · [Registers](docs/inverter_registers.md) · [Battery](docs/battery_sbr.md) · [Captures](docs/capture.md) · [Update rates](docs/update_rates.md) · [Board pins](docs/board_pins.md) | reference |

How the dongles, inverters, batteries and the cloud protocol work is documented separately in
**[SG-Docs](https://github.com/NickStallman/SG-Docs)**; this repository cites it wherever a
protocol or hardware fact matters.

## Development

```sh
python3 -m venv .venv && .venv/bin/pip install esphome -r tools/requirements.txt
cp secrets.yaml.example secrets.yaml
make            # host suites, web suite, generated files up to date
make config     # validate every entry
make compile    # build every entry
```

Layout: `winet-s*.yaml` entries, `boards/` pins and partition tables, `common/` packages,
`components/` the ESPHome components, `data/` the project's device tables, `tools/` generators and
the `.sgu` tooling, `dashboards/`, `docs/`, `tests/`. [CONTRIBUTING.md](CONTRIBUTING.md) covers
the rules (no stock files, no serials) and how to add a device family.

## Support the project

Rewired-S is free and will stay free. If it is useful to you,
[sponsoring](https://github.com/sponsors/NickStallman) helps pay for the inverters, batteries and
dongles that testing new hardware needs.

## Licence

GPL-3.0 ([LICENSE](LICENSE)). Third-party components are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Sungrow, iSolarCloud, WiNet, SH10RS, SG5.0RS
and SBR are trademarks or product names of Sungrow Power Supply Co., Ltd. This project is not
affiliated with or endorsed by Sungrow.
