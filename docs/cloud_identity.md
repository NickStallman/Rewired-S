# Cloud identity and the MQTT connection

The `isolarcloud` component presents the dongle to iSolarCloud as the stock unit it replaces.
Everything the cloud uses to recognise the dongle comes from the dongle's own external flash,
which the firmware mounts read-only, with YAML values taking precedence. Background on the
protocol: [cloud-mqtt](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/cloud-mqtt.md),
[cloud-envelope](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/cloud-envelope.md),
[cloud-messages](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/cloud-messages.md),
[cloud-commands](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/cloud-commands.md).

## Identity in brief
- The **serial number** (logger signal 0x0002) is the only per-device value. It is the MQTT
  client id and the `%s` in every topic.
- The broker username and password are shared by every stock dongle. They are the defaults of the
  component's `username` / `password` options and never need setting.
- Broker, port, key and flags have usable defaults: `iot.isolarcloud.com:16668`, TLS, topic
  suffix `enum_0_0_1` (zlib compression on).
- The cloud keys the downstream devices by `devCode`, which is the device's Modbus address; the
  cloud device type is mapped from the stock table type (for example 35 → 14).

## Reported firmware version
The dongle reports a firmware version string to iSolarCloud (logger signal 0x1000, in the point
uploads). It is part of the cloud identity and must match the stock dongle: a different string
reads to the cloud as a firmware change. The board files set it through the
`cloud_firmware_version` substitution, defaulting to the stock image each board is built against
(WiNet-S `WINET-SV200.001.00.P046`, WiNet-S2 `WINET-SV300.001.03.P032`). A site file
(`sites/README.md`) sets the version its dongle shipped with, read from the stock web UI's About
page before the first install:

```yaml
substitutions:
  cloud_firmware_version: "WINET-SV200.001.00.P044"
```

An empty `cloud_firmware_version` (the pre-built images, [prebuilt.md](prebuilt.md)) makes the
board detect it: on the first boot `sungrow_board` reads the app descriptor of the stock image in
the other OTA slot, keeps its version in NVS, and reports that. Precedence, highest first: an
override set on the web UI's **Settings → iSolarCloud identity** (`POST /api/cloud_version`), the
YAML value, the detected value, the board default. The version is re-read on every identity
resolve, so an override applies without a restart.

The product type (WiNet-S 1813, WiNet-S2 1824) and product name come from `sungrow_board` and
also match the stock values.

## Logger signals used by the cloud path
P = persistent in the external-flash identity file.

| sig | len | type | default | meaning |
|---|---|---|---|---|
| 0x0002 | 20 | str P | — | SN = MQTT client id = topic `%s` |
| 0x0001 / 0x0005 | 64 / 16 | str P | "WiNet-S2" | model / product name |
| 0x0004 | 2 | u16 P | 1824; WiNet-S 1813 | product type (the uplink's publicData uses the board constant) |
| 0x0020 | 2 | u16 P | 0 | encrypt mode |
| 0x0021 | 2 | u16 P | 0 | key number |
| 0x0022 | 2 | u16 P | 1 | compress mode: zlib on |
| 0x0023 | 2 | u16 P | 2 | broker TLS flag: 0 plain TCP, 2 TLS (CA `/usrdataout/tlsca/cafile`, chain verified). Forced to 0 when the port is 19999 |
| 0x0024 | 2 | u16 P | 2 | TLS flag of the backup server |
| 0x1071 | 64 | str P | `iot.isolarcloud.com` | broker host |
| 0x1072 | 2 | u16 P | 16668 | broker port |
| 0x1013 | 16 | str P | (stock default) | MQTT key (AES-128) |
| 0x1957 / 0x1958 / 0x1959 | 64 / 2 / 16 | P | same defaults | backup server, restored when a server switch is not confirmed |
| 0x107a | 2 | u16 P | 8 | time-zone id: `%d` in the broadcast topic `isolarcloud/devicedata/cmd/%d/enum_0_0_0` |
| 0x1089 | 16 | str P | "UTC+8:00" | time-zone string, written by the settimezone command |
| 0x1015 | 2 | u16 P | 5 | rundata period in minutes (0 = 5; 5 for the first 30 min of a boot); 1-1440 accepted |
| 0x1017 / 0x1018 / 0x1019 | 2 each | u16 P | 0 | silence window enable / start hour / end hour |
| 0x6492 | 4 | u32 P | 0x4FF | remote-control enable mask (WiNet-S2 file; the WiNet-S always uses 0x4EF) |

The YAML options `sn`, `broker_host`, `port`, `ssl_flag`, `mqtt_key`, `encrypt`, `keynum`,
`compress`, `timezone_id`, `timezone`, `rundata_period`, `silence`, `remote_ctl_mask`,
`product_type`, `product_name` and `firmware_version` override the file. The last good identity
is cached in NVS.

## MQTT CONNECT
- MQTT 3.1.1, clean session, keepalive 120 s.
- Client id = SN; the shared username and password.
- TLS to 0x1071:0x1072 according to the 0x0023 flag, trusting `/usrdataout/tlsca/cafile`; the
  ESP-IDF CA bundle is used when that file is missing.
- Will: topic `isolarcloud/devicedata/pub/willdata/<SN>/enum_<0x20>_<0x21>_<0x22>`, QoS 1,
  retained, payload zlib-compressed when 0x0022 ≠ 0:
  `{"messageId":"<SN>_<sendTime>_%05d","packgeCount":"1","packgeId":"1","publicData":{"productType":"1824","productName":"WiNet-S2","productSN":"<SN>","sendTime":"YYYYMMDDhhmmss"},"businessData":[{"devCode":247,"devType":22,"beat":0}]}`
- The uplink connects once SNTP has set the clock. Reconnects back off from 5 s to 5 min
  (`reconnect: min_delay / max_delay`).

## Server switching
cmdType 10384 point 10457 with `dataValue:"host;port;key16"` and an `sslFlag` moves the dongle to
another broker. After `server_switch: delay` (default 10 s) the component applies the new host,
port and key, reconnects, and confirms the switch once connected for longer than
`server_switch: confirm_after` (default 5 s); the new server then also becomes the backup.
Without a connection within `server_switch: timeout` (default 10 min) it returns to the backup
server, unless the remote-control mask differs from the default, which keeps the new server. A
request that changes only the TLS flag is ignored. The servers are persisted in NVS; the
external flash is never written.

devinfo (`pub/devinfo`, logger only: points 38338 / 38337 = new / old `host;port;key`, 38781,
5767 / 5768) is sent:

| when | WiNet-S | WiNet-S2 |
|---|---|---|
| after the first rundata of a boot | no | yes |
| at a CONNACK | every CONNACK | reconnects only |
| on server-switch confirm | yes | yes |

`devinfo_on_connect: auto` selects the row above from the board; `true` / `false` force it. Point
38781 is always sent as zeros.

## Identity file format
The identity lives in `/usrdataout/log/sigdata/dev_file_247_<productType>_2` (WiNet-S2
`…_1824_2`, WiNet-S `…_1813_2`) on the external flash
([flash-layout](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/flash-layout.md),
[WiNet-S](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s.md),
[WiNet-S2](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s2.md)).

- Concatenated little-endian records `u16 id | u16 len | data[len]`, with no header, magic or
  CRC. The record length cap is 0x420 on the WiNet-S2 and 0x100 on the WiNet-S; an over-cap
  length is treated as 0 and parsing resyncs 4 bytes on. The first 0xFFFFE bytes are parsed.
- Candidates in order: `log/sigdata`, `logbak/sigdata`, `log/factory/dev_file_factory`,
  `logbak/factory/dev_file_factory`. `sungrow_extflash` takes the first candidate whose SN
  (0x0002) is plausible; when none has one, it takes the first file of at least 10 bytes for the
  other values (broker, key, flags). The log names the file and the rule that chose it, and notes
  when the stock dongle would have used a different file.
- A record is used only when its id and length match the signal's declared size. Strings are
  NUL-padded to their full length. Lookup is a linear first match; a missing record falls back
  to the table default.
- An empty broker host (0x1071 or 0x1957 all NUL) falls back to `iot.isolarcloud.com`, so a
  blanked record cannot strand the dongle.

`decode_identity()` in `components/sungrow_extflash/sigdata.cpp` implements the decoding;
`tools/read_identity.py` decodes a dumped identity file the same way.
