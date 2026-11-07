# Compiling your own image with ESPHome

Rewired-S is a set of ESPHome packages that ESPHome fetches from GitHub. Compile your own image
for compiled-in Wi-Fi, API key and MQTT, your own options, or the native-entities alternative to
MQTT. The [pre-built images](prebuilt.md) need none of this; a configuration for one only changes
its defaults or sends an ESPHome OTA. Your whole configuration is one short YAML file and a
`secrets.yaml`, with no clone of the repository.

## Your configuration

Make an **empty folder** with two files. `my-dongle.yaml` names the dongle and pulls in an
[entry](#entries) at a release:

```yaml
substitutions:
  name: house-inverter
  friendly_name: House Inverter
packages:
  rewired_s: github://NickStallman/Rewired-S/winet-s2.yaml@v0.9.2
```

`secrets.yaml` holds the nine keys of
[`secrets.yaml.example`](https://github.com/NickStallman/Rewired-S/blob/main/secrets.yaml.example):

| Secret | Used by |
|---|---|
| `wifi_ssid`, `wifi_password`, `fallback_ap_password` | `common/wifi.yaml` |
| `api_encryption_key`, `web_username`, `web_password` | `common/access_secrets.yaml` (native API and web login; the same key encrypts ESPHome OTA) |
| `mqtt_broker`, `mqtt_username`, `mqtt_password` | `common/mqtt_ha.yaml` |

A pre-built entry needs no secrets; this alone is a valid configuration:

```yaml
packages:
  rewired_s: github://NickStallman/Rewired-S/winet-s2-prebuilt.yaml@v0.9.2
```

The ESPHome dashboard's **Adopt** button writes this kind of file for a pre-built image, pinned to
the release the dongle runs.

- **Pin a release.** `@v0.9.2` is the documented form; `@main` tracks development. ESPHome keeps
  the fetched copy under `.esphome/packages/` and refreshes it daily; the long form
  (`rewired_s: {url: https://github.com/NickStallman/Rewired-S, ref: v0.9.2, files:
  [winet-s2.yaml], refresh: 1d}`) sets `refresh:`.
- **Keep the folder empty otherwise.** A `components/` or `boards/` folder next to your file
  shadows the package's own.
- The [substitutions](#substitutions) and [options](#options-worth-knowing) below go into the same
  file.

## Build and install

Python 3.11+ is the only requirement; ESPHome installs the ESP-IDF toolchain itself on the first
compile (a few GB under `~/.platformio` and `.esphome/`). On Windows use PowerShell or `cmd`, not
Git Bash.

```sh
python3 -m venv .venv && .venv/bin/pip install esphome==2026.9.1
.venv/bin/esphome compile my-dongle.yaml
```

The image is `.esphome/build/<name>/build/firmware.ota.bin`. Getting it onto a dongle:

- **Dongle still on stock firmware:** package and sign it with the
  [tools](tools_setup.md) and install it through the stock updater with your own public key
  ([installing through the updater](install_ota.md#build-your-own-package)); or, on a WiNet-S,
  write it over the [serial pads](install_serial.md).
- **Dongle already on Rewired-S:** `esphome run my-dongle.yaml --device <ip>` compiles and
  installs with a native ESPHome OTA; `esphome upload my-dongle.yaml --device <ip>` installs the
  last build. A pre-built image accepts the OTA with the web login password until Home Assistant
  has adopted it, then with the API key.

## Entries

| Entry | Dongle | Network |
|---|---|---|
| `winet-s.yaml` | WiNet-S | Wi-Fi (`common/wifi.yaml`), with a fallback access point and captive portal |
| `winet-s-ethernet.yaml` | WiNet-S | Ethernet (DP83848). No fallback AP; optional network fallback (`common/ethernet_fallback.yaml`) boots the other slot; otherwise recovery is over UART |
| `winet-s2.yaml` | WiNet-S2 | Wi-Fi (`common/wifi.yaml`), with a fallback access point and captive portal |
| `winet-s2-ethernet.yaml` | WiNet-S2 | Ethernet (W5500); status in [compatibility](compatibility.md) |
| `*-prebuilt.yaml` | as above | the same, provisioned at runtime instead of compiled in; the Ethernet ones include the network fallback |
| `winet-s-debug.yaml`, `winet-s-debug-ethernet.yaml`, `winet-s2-debug.yaml` | | the read-only [debug build](debug_build.md) |

ESPHome runs Wi-Fi or Ethernet, not both, so the network is part of the entry. The app descriptor
version carries `+wifi` / `+ethernet`, which the Boot slots card and the fallback read.

An entry is a list of packages; leaving one out or adding one (the commented lines) needs a
[checkout](#developing-on-a-checkout) or your own copy of the entry:

```yaml
substitutions:
  name: winet-s2
  friendly_name: WiNet-S2

packages:
  board: !include boards/winet_s2.yaml          # pins, PSRAM, partition table, product constants
  base: !include common/base.yaml               # ESPHome core, logger, OTA, web server, SNTP, safe mode
  access: !include common/access_secrets.yaml   # native API key and web login from secrets.yaml
  network: !include common/wifi.yaml            # or boards/winet_s2_ethernet.yaml
  board_io: !include common/board_io.yaml       # LEDs, button
  rs485: !include common/rs485.yaml             # Modbus-RTU master, discovery, tiered polling
  identity: !include common/identity.yaml       # external flash, read-only: serial number and cloud settings
  cloud: !include common/cloud.yaml             # iSolarCloud uplink (leave out to remove it)
  modbus_tcp: !include common/modbus_tcp.yaml   # Modbus-TCP server on :502 (leave out to remove it)
  control: !include common/control.yaml         # gated register writes, the "Inverter writes enabled" switch
  mqtt_ha: !include common/mqtt_ha.yaml         # per-device Home Assistant MQTT discovery
  web_ui: !include common/web_ui.yaml           # the web UI
  capture: !include common/capture.yaml         # I-V scan, waveform and fault recordings
  diagnostics: !include common/diagnostics.yaml # reset journal, core dumps, diagnostics report
  analytics: !include common/analytics.yaml     # usage reporting (switchable at runtime)
  # ha_native: !include common/native_entities.yaml   # instead of mqtt_ha: everything as native-API entities
  # syslog: !include common/syslog.yaml               # UDP syslog to syslog_host

external_components:
  - source:
      type: local
      path: components
```

## Substitutions

| Substitution | Default | Meaning |
|---|---|---|
| `name`, `friendly_name` | per entry | ESPHome node name (hostname, entity prefix) and display name |
| `cloud_firmware_version` | the stock version each board was developed against (S `WINET-SV200.001.00.P046`, S2 `WINET-SV300.001.03.P032`); `""` on pre-built images = detect from the stock app in the other slot | the version string reported to iSolarCloud ([cloud identity](cloud_identity.md)) |
| `log_level` | `INFO` | ESPHome log level (`DEBUG` for more; the debug build uses `VERY_VERBOSE`) |
| `analytics` | `"true"` | initial state of the **Usage reporting** switch ([usage reporting](analytics.md)) |
| `cloud_writes_restore` | `RESTORE_DEFAULT_OFF` | restore mode of the **Cloud writes** switch |
| `syslog_host`, `syslog_port` | none, `5514` | with `common/syslog.yaml` |
| `ap_password` | `rewired-s-setup` | password of the setup network on pre-built images |

Board substitutions (pins, PSRAM mode, LED polarity, button pull-up) are in `boards/*.yaml` and
documented in [board pins](board_pins.md); you should not need to change them.

## Options worth knowing

- **Control profile.** The firmware attaches the hybrid or string profile from the inverter's
  device type; the profile decides which controls exist. Fixed devices and profiles can be
  declared under `sungrow_control: devices:` ([control](control.md)).
- **Cloud on/off.** The **Cloud uplink** switch (restored across reboots, default on) disconnects
  from iSolarCloud; the **Cloud writes** switch (default off) gates settings written by the cloud.
  Leave the `cloud` package out of the entry to remove the uplink from the build entirely.
- **Modbus-TCP.** The **Modbus-TCP server** switch defaults to the inverter's country rule (on
  for Australia and a few others, off elsewhere) until you set it; `common/modbus_tcp.yaml` has
  `max_clients`, `allowed_ips` and `passthrough`; `write_allowed_ips` is in `common/control.yaml`.
- **MQTT or native entities.** Use `mqtt_ha` or `common/native_entities.yaml`, not both
  ([Home Assistant](home_assistant.md#why-mqtt-and-not-just-esphome-entities)).
- **Network fallback.** On a compiled Ethernet entry, add
  `fallback: github://NickStallman/Rewired-S/common/ethernet_fallback.yaml@v0.9.2` to `packages:`
  (in a checkout: `!include common/ethernet_fallback.yaml`; it sets `sungrow_board:
  network_fallback:` with `timeout` 3min, `phy_failed_timeout` 60s, `allow_stock` false). A build
  that never gets a network boots the other slot, but only into a Rewired-S image that is not an
  Ethernet build. The state shows on Settings → Boot slots.
- **Remote logs.** `syslog_host` plus `common/syslog.yaml`; `make syslog` runs a receiver.
- **Fast loop.** `sungrow_modbus: fast_loop: true` tightens the reply detection at some CPU cost
  ([update rates](update_rates.md)).

## Developing on a checkout

A clone is for changing the firmware: the `make` targets, site files under `sites/` and the
[debug build](debug_build.md). Python 3.11+ and a C compiler toolchain are the requirements.

```sh
git clone https://github.com/NickStallman/Rewired-S.git && cd Rewired-S
python3 -m venv .venv && .venv/bin/pip install esphome==2026.9.1 -r tools/requirements.txt
cp secrets.yaml.example secrets.yaml      # Wi-Fi, API key, web login, MQTT broker
make config                               # validates every entry
```

A site file under `sites/` includes an entry from the checkout instead of fetching it
([sites/README.md](../sites/README.md)); `make deploy NODE=sites/<name> DEVICE=<ip>` runs a native
ESPHome OTA, streams the logs across the reboot and exits 0 once the new build runs and is marked
valid.

### Make targets

```sh
make                 # = make test: generated files fresh, every host suite, the web suite
make test-host       # what CI runs on every push (needs g++, python3, zlib)
make web             # web UI typecheck + tests + embedded bundle check (npm ci once in components/sungrow_web/web)
make gen             # rerun the generators; make check-gen fails on a stale generated file
make config          # every entry, pre-built entry, debug entry and tests/config/*.yaml
make config-prebuilt # the pre-built entries, validated without a secrets.yaml
make compile         # build every entry; make compile-debug builds the debug entry
make deploy NODE=sites/<name> DEVICE=<ip>
make sgu APP=<firmware.ota.bin> SGU_BOARD=s|s2 SGU_BUILD=<P-number>   # signed package for the updater route
make sgu-keys        # your signing key pair under keys/
make hygiene         # the repository hygiene gate CI runs
make syslog          # UDP syslog receiver
```
