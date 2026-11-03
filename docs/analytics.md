# Usage reporting and diagnostics uploads

Rewired-S reports anonymous usage to a collector run by the project, and can upload a diagnostics
report when something goes wrong or when you ask it to. Reporting is on in every entry and one
substitution turns it off (below). The reports exist so that the install base, the hardware mix
and the errors people hit are visible without anyone having to file an issue, and so that a
problem or an unrecognised device can be looked at from the dongle's own evidence.

## What is sent

Everything goes to `https://posthog.nickstallman.net` over HTTPS (the ESP-IDF certificate bundle
verifies the server). The dongle is identified by `distinct_id`, the SHA-256 hash of its serial
number: stable for the life of the dongle, not reversible. A dongle whose external flash yields no
serial uses a random id kept in its preferences instead.

| Event | When | Properties |
|---|---|---|
| `ping` | 60–120 s after boot, then every 24 h | `version` (Rewired-S), `esphome_version`, `stock_version` (the version string the dongle presents to iSolarCloud), `board` (`winet_s` / `winet_s2`), `chip`, `network` (`wifi` / `ethernet`), `uptime_s`, `reset_reason`, `cloud_uplink`, `mqtt`, `modbus_tcp`, `control_writes`, `psram_kb`, `heap_min_kb`, `winetVersion` 4, `winetCount` 1 |
| `device_registered` | with every ping, one per connected device | `device` (model), `role` (`inverter` / `battery` / `charger` / `bridge` / `meter`), `dev_type`, `protocol`, the inverter type code and the device's firmware-version registers |
| `error` | when a notable fault occurs | `type`, `error`, `count`; see the table below |

A repeating error is sent once, then at most once an hour with a count, as the Home Assistant
add-on does.

| `type` | Meaning | `error` |
|---|---|---|
| `crash`, `watchdog`, `stall` | the previous boot ended in a panic, a watchdog reset or a stall | the crash class (abort, task watchdog, the exception name); never addresses |
| `modbus_offline` | a device that was online this boot has not answered for 10 minutes while another device still does | the model as `device` |
| `cloud_connect` | the iSolarCloud uplink is on and has failed to connect for 15 minutes | the uplink's error text |
| `heap_low` | internal free heap under 24 KB on three consecutive samples | the free heap |
| `modbus_burst` | 30 or more timeouts and exceptions from one device within a minute | the model as `device` |
| `unsupported_device` | discovery found a device the firmware has no table or catalogue entry for | `<why>:<dev_type>/<protocol>/<dev_code>[/<type code>]`, `why` one of `devinfo_rejected`, `devinfo_unparsed`, `no_table`, `child_no_table`, `child_unparsed`, `uncatalogued` |

The timestamps are taken from SNTP; before the clock is valid the collector uses its own receive
time. The batch is built on the main loop and sent from a low-priority task, so the inverter
polling never waits for it; a failed send is retried three times, three seconds apart, and then
dropped.

## The diagnostics report

The report is a generated plain-text document, not a raw log. It contains:

- **Build**: Rewired-S, ESPHome and ESP-IDF versions, the configuration hash and build time,
  board, chip, flash and PSRAM sizes, network kind.
- **Devices**, one section per detected device:
  - its identity as discovered: bus address, how it was found (inverter search, reported by its
    inverter), device code, model, protocol code, protocol type, device type and where that came
    from, the inverter type code, the register table chosen and whether it is the fallback, the
    grid-code family and descriptor;
  - the raw identification reply (the device-information block or the class identity read) as a
    hex dump, with the serial-number registers masked;
  - the register block map: every polled block with its function code, start, count, tier, whether
    the device serves or rejects it, the last Modbus exception code and how many exceptions it
    returned; the link counters (sent, received, timeouts, CRC errors, exceptions);
  - the values of the static-tier registers (capabilities, rated values, versions, battery type
    and similar identity registers) as `key=value`;
  - the firmware-version registers of its family.
- **Features**: cloud uplink state and connect failures, MQTT connection, Modbus-TCP state and
  counters, whether inverter writes are enabled and the write counters, any running capture.
- **Memory, uptime and reset reason**, then the reset journal (`/api/diag`: this boot's and the
  previous boot's records, the core-dump summary, the previous boot's log tail) and the log tail
  of this boot.

Before it leaves the dongle the report is scrubbed: the dongle serial, every device serial, the
node name, the Wi-Fi network name and every IPv4 address are replaced by `<sn>`, `<serial>`,
`<host>`, `<ssid>` and `<ip>`.

### Unrecognised devices

When a device is discovered that the firmware has no register table or catalogue entry for, the
report carries what is needed to add it: the raw identification reply shows how the device
describes itself, the block map shows which registers it serves and which it rejects, and the
static values show its capabilities. The dongle sends an `unsupported_device` event and, with
reporting on, uploads one report for that device once its first poll round is done. The dongle
remembers the device codes it has uploaded a report for, so each unrecognised model is reported
once per install, not once per boot. That upload is the preferred way to get a new model
supported; a manual upload or the downloaded report works as well.

### Automatic uploads

With reporting on, the dongle uploads a report by itself:

| Trigger | When |
|---|---|
| `crash`, `watchdog`, `stall` | the previous boot ended in a panic, a watchdog or supervisor reset, or a stall, or a core dump is stored: once, 30 s after the network is up |
| `offline` | a device that was online this boot has been silent for 10 minutes while another still answers: once per device per boot |
| `cloud` | the uplink has failed for 15 consecutive minutes: once per boot |
| `heap` | internal free heap under 24 KB on three consecutive samples |
| `modbus` | 30 or more timeouts and exceptions from one device within a minute |
| `device` | an unrecognised device, once its first poll round is done; once per device code per install |

Automatic uploads are limited to three per 24 hours; the count is kept in the dongle's
preferences, so a restart loop cannot exceed it. Without a valid clock the window never resets.
The collector enforces its own limits as well. A device that is dark at night (an inverter with
no PV) is not an offline fault: the trigger needs another device to be online.

### Manual upload and download

The web UI's **Settings** page, under **Diagnostics**, has **Send diagnostics**. It shows what
the report contains, uploads it and displays a reference such as `k7Qx2mVb`: quote that in a bug
report so the upload can be found. **Download report** saves the same scrubbed text
(`/api/diag_report`) for attaching to an issue. Both work with reporting turned off.

## Turning it off

The **Usage reporting** switch (web UI Settings → Diagnostics; `switch.<node>_usage_reporting`
in Home Assistant) turns the pings, error events and automatic uploads off and is remembered
across restarts. Its default comes from the site file:

```yaml
substitutions:
  analytics: "false"
```

Either way the component stays in the build so that **Send diagnostics** and **Download report**
keep working. Leaving the `analytics` package out of the entry removes the component entirely.
The `stock_version` property is the version the board resolves (YAML, detected from the stock
slot, or the web UI override; [cloud_identity.md](cloud_identity.md)).

## Collector

`common/analytics.yaml` shows the `endpoint` and `api_key` options. The key is public and only
selects the data source at the collector; the endpoint accepts a PostHog-style batch at
`/batch/` and the report at `/diag/`, so a private collector that speaks the same two routes can
replace it.
