# Debug build and first install

A WiNet dongle leaves the factory with the stock bootloader, the stock app in one OTA slot and the
stock web UI in `oemdata`. The first Rewired-S image goes in through the stock updater's pull
route, signed with your own key; after that, every update is a native ESPHome OTA
(`make deploy`). This page covers the debug builds, the backup, the install, the checks that
decide whether to go on, and the way back to stock.

Background on the update channels and the package format:
[update channels](https://github.com/NickStallman/SG-Docs/blob/main/reference/firmware-update/update-channels.md),
[SGU container](https://github.com/NickStallman/SG-Docs/blob/main/reference/firmware-update/sgu-container.md).

## 1. The debug builds

| Entry | Board | Contents |
|---|---|---|
| `winet-s-debug.yaml` | WiNet-S, Wi-Fi | the WiNet-S firmware without the iSolarCloud uplink and without register writes, plus `common/debug.yaml` |
| `winet-s-debug-ethernet.yaml` | WiNet-S, Ethernet (DP83848) | the same on Ethernet |
| `winet-s2-debug.yaml` | WiNet-S2, Wi-Fi | a minimal boot test: board, Wi-Fi, external-flash diagnostics, the supervisor feed, LEDs and button, flash-layout reports and the `/dump` downloads; RS485 polling, :502, the cloud and writes are left out |

`common/debug.yaml` (component `sungrow_debug`) adds verbose logs, an RS485 raw-frame and decode
trace, on-demand reads and bus scans, store and poll-plan dumps, bus and system sensors, and
read-only flash downloads over HTTP.

Safety properties of every debug build:
- `block_rtu_writes: true` drops every RS485 frame that is not fn 3 or fn 4 before it reaches the
  bus, and the write package (`common/control.yaml`) is not included, so :502 fn 6/16 answer
  exception 1.
- The external NOR flash (identity, `/usrdataout`) is mounted read-only, and the dump tooling
  reads it only through the read-only partition.
- There is no factory reset. A 30 s button hold erases only ESPHome's own NVS namespace.
- The only flash write the debug build can make is the "Boot other slot (stock app)" rollback
  (§8), which rewrites otadata and has to be armed with a switch that is never restored.
  `tests/test_debug_readonly.py` fails if any other write, erase or OTA-write call appears in
  `components/sungrow_debug`.

> [!WARNING]
> Never serial-flash a bootloader, a partition table or `firmware.factory.bin` to a dongle. It
> overwrites the stock bootloader and partition table, which the install route below does not
> need. `boards/partitions_winet_s.stock.csv` and `boards/partitions_winet_s2.stock.csv` are the
> real stock tables; the app reads the on-flash table at runtime and runs from either slot.

## 2. Build

```sh
cp secrets.yaml.example secrets.yaml                    # Wi-Fi, API key, web login (also guards /dump)
make compile-debug                                      # winet-s-debug.yaml
make compile-debug DEBUG_ENTRY=winet-s-debug-ethernet.yaml
make compile-debug DEBUG_ENTRY=winet-s2-debug.yaml
```

The image to install is `.esphome/build/<name>/build/firmware.ota.bin`;
`firmware.factory.bin` is never flashed. `esp32: flash_size: 16MB` is mandatory (IDF sizes the
usable flash from the app header); `esptool --chip esp32 image_info firmware.ota.bin` shows
*Flash size: 16MB, DIO*. `tools/package_sgu.py` refuses an image larger than the smaller stock
app slot (0x309000 bytes on the WiNet-S, 0x33B000 on the WiNet-S2).

`substitutions: debug_log_port: "2323"` enables a plain-TCP log stream (`nc <ip> 2323`). It has no
authentication and is off by default.

## 3. Serial backup (WiNet-S, optional)

A serial backup taken before anything is written is the definitive restore: wiring, download
mode and the `read_flash` command are in [install over serial](install_serial.md) §2-3. The
network backup (§7) covers the internal flash once the debug build runs; the external NOR is not
reachable over UART, back it up with `/dump/extflash` (§7).

## 4. Package

Package and sign the debug image as in
[Build your own package](install_ota.md#build-your-own-package). Without `APP=`, `make sgu`
packages the debug image of `SGU_BOARD` (`DEBUG_APP`, the `winet-<board>-debug` build):

```sh
make sgu-keys                                  # once: keys/rewired-s_priv.pem + keys/rewired-s_pub.pem
make sgu SGU_BOARD=s SGU_BUILD=992             # .esphome/build/winet-s-debug/build/firmware.ota.bin
make sgu APP=.esphome/build/winet-s-debug-eth/build/firmware.ota.bin SGU_BOARD=s SGU_BUILD=992
```

A self-built package must carry a build number above the installed one: releases use P99x
(`tools/sgu_build_number.py`), so use `SGU_BUILD=992` or higher on a dongle running a release, and
raise it for each new package. The package and your public key land in `dist/`;
`tools/sgu.py info|extract|verify` inspect any package.

## 5. Install through the stock pull route

Follow [install through the updater](install_ota.md) §1-4 with your own key
(`--pubkey dist/publicKey_V1.pem`) and `python3 -I tools/fake_iconfig_server.py --dist dist`.
Log in as admin from a browser on the host running the server; open **System → System
maintenance → System update → iConfig update package** (not the device-update dialog) and start.

**Do not run a native OTA yet.** It writes the inactive slot, which now holds the stock app.
Take the network backup (§7) first.

## 6. Go / no-go

Each gate must pass before the next step. A failed gate means stop and restore (§8).

| Gate | Pass when |
|---|---|
| Before packaging | `deploy_debug.py status` answers; Secure Boot and flash encryption are off (serial check, WiNet-S) or the stock updater accepts signed packages (WiNet-S2) |
| Before installing | `package_sgu.py` reports every check passed; the key import succeeded |
| After the first boot | the dongle joins the network, the ESPHome API and web server answer, "Running partition" names the slot the updater wrote, and safe mode was not entered |
| Before the first native OTA | `tools/flash_backup.py all` finished with every region verified (except `nvs`, which may change while it is read) and `extflash.bin` is stored twice |
| Before the production image | the checklist below is clean for an hour: no unexpected reset, the identity read, the inverter discovered and decoding sensibly |

### First-boot checklist

The `winet-s2-debug.yaml` boot test has no RS485 polling and no :502, so the RS485, decoding,
bus and Modbus-TCP rows apply to the WiNet-S builds only.

| Subsystem | Expected |
|---|---|
| Flash | `flash_id … physical 16 MB, usable 16 MB`; bootloader magic 0xE9, DIO; the partition table with a valid MD5; otadata names the running slot; the inactive slot shows the stock app and its version |
| Reset and rollback | the reset-reason sensor reports a clean boot; WiNet-S: `Running partition … valid` at the first boot; WiNet-S2: `pending verify` at the first boot after an OTA (that is the bootloader doing its part; `new` means it does not) and `valid` 30 s later |
| Supervisor (WiNet-S2) | "Watchdog running" on and "Watchdog edge count" rising; no "power-on" resets after restarts |
| Network | Wi-Fi connected with an address, or the Ethernet link up |
| External flash | JEDEC id, superblock `v2.0 block_size 4096`, mounted read-only, the identity file found, serial number present |
| RS485 discovery | the devinfo line (`sgdbg.disc`): device code, model, protocol and table, then the poll plan |
| Decoding | yields, PV power, battery SOC and grid power sign agree with the inverter display or the stock app |
| Bus health | timeouts near 0, CRC errors 0, latency 50-200 ms per transaction |
| Modbus-TCP | `mbpoll -m tcp -a 1 -r 5001 -c 10 -t 3 <ip>` returns the inverter's values |
| Memory | "Heap free" and "PSRAM free" stable over an hour; "PSRAM size" not 0 |

## 7. Network backup

The debug build serves `/dump/*` on port 80 behind the web login:

| URL | Content |
|---|---|
| `/dump/index` | JSON: every region with offset, size and SHA-256 (`?sha=0` skips hashing) |
| `/dump/report`, `/dump/ptable.csv` | the flash report / the partition table as gen_esp32part CSV |
| `/dump/bootloader`, `/dump/ptable` | 0x1000..0x8000 (WiNet-S) / 0x0..0x8000 (WiNet-S2); 0x8000 + 0xC00 |
| `/dump/nvs`, `/dump/otadata`, `/dump/phy`, `/dump/coredump`, `/dump/oemdata` | by subtype or label from the on-flash table |
| `/dump/inactive_app` | the other OTA slot: the stock app after the first install |
| `/dump/running_app` | the running build |
| `/dump/extflash` | the whole external NOR, through the read-only partition |
| `/dump/flash` | the whole internal 16 MB |
| `/dump/raw?offset=0x..&len=0x..` | any internal range |

Each download carries `Content-Length`, `X-SHA256` and `X-Region-Offset/-Size` and is streamed
in 4 KB chunks; RS485 polling keeps running. One dump runs at a time (503 otherwise).

```sh
python3 -I tools/flash_backup.py all --host 192.0.2.50 --user admin --out backup/ \
        --stock-sgu <stock .sgu>
```

It downloads every region, verifies each SHA-256 (three tries; a region that keeps changing is
kept and marked `verified: false`), writes `manifest.json` and assembles `restore_full_16MB.bin`:
everything as dumped, with the running slot replaced by the stock app from the `.sgu` (or 0xFF)
and otadata pointed at the stock slot (`--otadata keep` leaves it). `RESTORE.txt` lists the
regions and the restore commands. `--full` also downloads `/dump/flash`; `assemble --from-full`
starts from it.

## 8. Back to stock

The routes are in [back to stock](back_to_stock.md). In the debug build, **Boot other slot (stock
app)** needs **Allow boot switch to other slot** turned on first; the switch is off after every
boot. The restore image from the network backup goes in over serial (WiNet-S) with
`esptool --chip esp32 -p /dev/ttyUSB0 write_flash 0x0 backup/restore_full_16MB.bin`. None of the
routes writes the external NOR; `extflash.bin` is a backup only.

## 9. WiNet-S2 install

The WiNet-S2 is installed and updated without a serial connection.

- Package with `make sgu SGU_BOARD=s2 APP=<firmware.ota.bin>` and install
  through the stock pull route (§5). Start with `winet-s2-debug.yaml` to confirm the board stays up,
  or package a production entry's image with `DEBUG_APP=`.
- The stock bootloader is expected to revert an image that resets within 30 s (unconfirmed:
  check for `pending verify` at the first boot after an OTA). The way back is **Boot other slot**.
  An image that runs but never gets a network needs the network fallback
  (`common/ethernet_fallback.yaml`) instead.
- The external supervisor resets the board unless it is fed; the feed starts before ESPHome's own
  setup, and every software restart and panic switches the supervisor off first.
- After the first install, update with native OTA (`make deploy NODE=sites/<name> DEVICE=<ip>`).

## 10. Debug tools

From Home Assistant (Developer Tools → Actions, `esphome.<node>_<action>`):

```yaml
action: esphome.winet_s_debug_debug_read        # raw hex + decoded rows (the device's table)
data: {address: 1, fn: 4, start: 5000, count: 10, table: ""}   # table e.g. "inv_proto2" to force
---
action: esphome.winet_s_debug_debug_scan        # probe slaves one at a time
data: {first: 1, last: 247, start: 5000, count: 1, interval_ms: 300, timeout_ms: 0}
#  timeout_ms > 0 lowers the response timeout for the whole bus during the scan.
#  Responders get a devinfo 2515+66 follow-up read (protocol type, dev_type, table).
---
action: esphome.winet_s_debug_debug_trace_filter
data: {addresses: "1", registers: "5000-5100,13000+50"}       # "" = all
---
action: esphome.winet_s_debug_debug_trace
data: {rtu: true, decode: true, only_changed: true, tcp: true}
---
action: esphome.winet_s_debug_set_log_level
data: {tag: "sgdbg.hex", level: "DEBUG"}                       # hide raw hex lines
---
action: esphome.winet_s_debug_dump_signal_store                # sigid, key, raw, scaled, valid, age
data: {device: -1}                                             # -1 = all devices
---
action: esphome.winet_s_debug_dump_poll_plan                   # fn, range, tier, period, last sent
data: {device: 0}
---
action: esphome.winet_s_debug_dump_system                      # flash layout, heap, PSRAM, tasks, bus stats
---
action: esphome.winet_s_debug_dump_file                        # external flash file, read-only
data: {path: "log/sigdata/dev_file_247_1813_2"}
---
action: esphome.winet_s_debug_list_files
data: {path: ""}
```

Buttons, switches and sensors cover the same: dumps, bus scan, trace switches, the log-level
select, per-device request / response / timeout / CRC / exception counters and latency, heap,
PSRAM, loop time, reset reason, running partition and versions.

Logs: `.venv/bin/esphome logs winet-s-debug.yaml` (API) or `--device /dev/ttyUSB0` (UART0). Tags:
`sgdbg.rtu` (TX/RX summaries), `sgdbg.hex` (raw bytes), `sgdbg.decode` (rows), `sgdbg.disc`
(discovery and scan), `sgdbg.tcp` (:502), `sgdbg.dump`, `sgdbg.flash`. Sample lines, with the
log-level prefix left out:

```
[sgdbg.rtu]: #42 t=123456 TX addr 1 fn 4 (read input) start 5003 count 120 (..5122) | 01 04 13 8A 00 78 ..
[sgdbg.rtu]: #42 RX 245 B in 141 ms: addr 1 fn 4 240 B = 120 regs CRC ok
[sgdbg.decode]: 5003+1 fn4 0x2004 daily_yield u16 [0012] raw=18 x0.1 -> 1.8 kWh
[sgdbg.decode]: 5017+2 fn4 0x2010 - u32 [FFFF FFFF] SENTINEL (absent)
[sgdbg.rtu]: #43 TIMEOUT after 2100 ms: addr 3 fn 4 ...; 0 B received, 0 CRC-bad candidates
[sgdbg.disc]: addr 1 devinfo 2515+66: dev_code 0x0E0F model 'SH10RT' ... -> protocol type 2, dev_type 35 -> table inv_proto2
```
