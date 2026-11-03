# Configuration

Rewired-S is a set of ESPHome packages. An **entry** picks the board and the network and pulls
in the feature packages; a **site file** sits on top of an entry with the values that belong to
one real dongle. Pre-built images are the four `*-prebuilt.yaml` entries compiled without
secrets ([pre-built images](prebuilt.md)); everything below is for compiling your own.

## Setup

```sh
python3 -m venv .venv && .venv/bin/pip install esphome -r tools/requirements.txt   # ESPHome 2026.9.x
cp secrets.yaml.example secrets.yaml      # Wi-Fi, API key, web login, MQTT broker
ln -s ../secrets.yaml sites/secrets.yaml  # site files read secrets from their own directory
make config                               # validates every entry
```

`secrets.yaml` (git-ignored):

| Secret | Used by |
|---|---|
| `wifi_ssid`, `wifi_password`, `fallback_ap_password` | `common/wifi.yaml` |
| `api_encryption_key`, `web_username`, `web_password` | `common/access_secrets.yaml` (native API and web login; the same key encrypts ESPHome OTA) |
| `mqtt_broker`, `mqtt_username`, `mqtt_password` | `common/mqtt_ha.yaml` |

## Entries

| Entry | Dongle | Network |
|---|---|---|
| `winet-s.yaml` | WiNet-S | Wi-Fi, with a fallback access point and captive portal |
| `winet-s-ethernet.yaml` | WiNet-S | Ethernet (DP83848). No fallback AP: recovery is over UART |
| `winet-s2.yaml` | WiNet-S2 | Wi-Fi |
| `winet-s2-ethernet.yaml` | WiNet-S2 | Ethernet (W5500), untested |
| `*-prebuilt.yaml` | as above | the same, provisioned at runtime instead of compiled in |
| `winet-s-debug.yaml`, `winet-s-debug-ethernet.yaml`, `winet-s2-debug.yaml` | | the read-only [debug build](debug_build.md) |

ESPHome runs Wi-Fi or Ethernet, not both, so the network is part of the entry.

An entry is a list of packages:

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

## Site files

A site file describes one dongle and is git-ignored (`sites/`). The minimum:

```yaml
# sites/house.yaml
substitutions:
  name: house-inverter
  friendly_name: House Inverter
  cloud_firmware_version: "WINET-SV300.001.03.P024"   # what the stock web UI's About page showed
packages:
  entry: !include ../winet-s2.yaml
```

```sh
.venv/bin/esphome config sites/house.yaml
make deploy NODE=sites/house DEVICE=192.0.2.50       # compile, encrypted OTA, logs across the reboot
```

### Substitutions

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

### Options worth knowing

- **Control profile.** The firmware attaches the hybrid or string profile from the inverter's
  device type; the profile decides which controls exist. Fixed devices and profiles can be
  declared under `sungrow_control: devices:` ([control](control.md)).
- **Cloud on/off.** The **Cloud uplink** switch (restored across reboots, default on) disconnects
  from iSolarCloud; the **Cloud writes** switch (default off) gates settings written by the cloud.
  Leave the `cloud` package out of the entry to remove the uplink from the build entirely.
- **Modbus-TCP.** The **Modbus-TCP server** switch defaults to the inverter's country rule (on
  for Australia and a few others, off elsewhere) until you set it; `common/modbus_tcp.yaml` has
  `max_clients`, `write_allowed_ips` and the passthrough options.
- **MQTT or native entities.** `mqtt_ha` publishes each device over MQTT discovery
  ([Home Assistant](home_assistant.md)). Without a broker, replace it with
  `common/native_entities.yaml`, which folds the inverter (and, with the extra packages listed in
  that file, a battery stack and a meter) into the dongle's native-API device. Do not use both.
- **Remote logs.** `syslog_host` plus `common/syslog.yaml`; `make syslog` runs a receiver.
- **Fast loop.** `rs485: fast_loop: true` tightens the reply detection at some CPU cost
  ([update rates](update_rates.md)).

## Make targets

```sh
make                 # = make test: generated files fresh, every host suite, the web suite
make test-host       # what CI runs on every push (needs g++, python3, zlib)
make web             # web UI typecheck + tests + embedded bundle check (npm ci once in components/sungrow_web/web)
make gen             # rerun the generators; make check-gen fails on a stale generated file
make config          # esphome config of every entry, debug entry and tests/config/*.yaml
make config-prebuilt # the pre-built entries, validated without a secrets.yaml
make compile         # build every entry; make compile-debug builds the debug entry
make deploy NODE=sites/<name> DEVICE=<ip>
make sgu APP=<firmware.ota.bin> SGU_BOARD=s|s2 SGU_BUILD=<P-number>   # signed package for the updater route
make sgu-keys        # your signing key pair under keys/
make hygiene         # the repository hygiene gate CI runs
make syslog          # UDP syslog receiver
```
