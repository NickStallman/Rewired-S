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
*Flash size: 16MB, DIO*. The WiNet-S images are about 1.2 MB (Wi-Fi) and 0.8 MB (Ethernet); the
smallest stock app slot is 0x309000 bytes on the WiNet-S and 0x33B000 on the WiNet-S2.

`substitutions: debug_log_port: "2323"` enables a plain-TCP log stream (`nc <ip> 2323`). It has no
authentication and is off by default.

## 3. Serial backup (WiNet-S, optional)

The network backup in §7 covers the internal flash once the debug build runs. A serial backup
taken before anything is written is the definitive restore.

Wiring: ESP32 UART0 is GPIO1 (TX) / GPIO3 (RX), 115200 8N1, 3.3 V TTL. Connect adapter RX to TX,
TX to RX and GND, and power the dongle from its own supply. Download mode needs GPIO0 low at reset;
on the WiNet-S GPIO0 is also the 50 MHz RMII clock input, so hold it low through about 470 Ω and
release it after reset.

1. Passive boot log first: a terminal at 115200 and a power cycle print the partition table.
2. In download mode:
   ```sh
   espefuse.py --chip esp32 -p /dev/ttyUSB0 summary     # Secure Boot and flash encryption must be off
   esptool --chip esp32 -p /dev/ttyUSB0 flash_id        # 16 MB
   esptool --chip esp32 -p /dev/ttyUSB0 -b 460800 read_flash 0 0x1000000 full_16MB.bin
   ```
   Keep `full_16MB.bin` in two places. If Secure Boot or flash encryption is on, stop: no custom
   image boots.

The external NOR is not reachable over UART; back it up with `/dump/extflash` (§7).

## 4. Package

```sh
make sgu-keys                                            # once: keys/rewired-s_priv.pem + keys/rewired-s_pub.pem
make sgu APP=.esphome/build/winet-s-debug/build/firmware.ota.bin SGU_BOARD=s SGU_BUILD=901
#   SGU_BOARD=s|s2, SGU_BUILD=<P-number>, SGU_STOCK=<official .sgu> to add its web-UI part (only if the
#   app-only package is refused)
```

`tools/package_sgu.py` builds an app-only package (one type-2 part, nothing from a stock
package) and signs it with your key (`keys/`, never printed or copied; the public key is written
to `dist/publicKey_V1.pem`):
- with `SGU_STOCK` the stock web UI is copied unchanged from that package as the type-3 part, so
  the updater rewrites `oemdata` with identical content;
- an in-file signature trailer follows the body (the WiNet-S2 updater requires it, the WiNet-S
  ignores it);
- before signing it checks the ESP image magic, the chip id (0 for the WiNet-S, 9 for the
  WiNet-S2), the 16 MB header flash size, the app size against the smaller OTA slot, and that
  the version (`WINET-SV200.001.00.P901`, `WINET-SV300.001.03.P901`) is above the installed one.
  Raise `SGU_BUILD` for each new package.

Output in `dist/`: `<version>.sgu`, its detached `.bin` signature, the public key and, with
`--zip` (which `make sgu` passes), the `.zip` the stock web UI expects. The container format:
[SGU container](https://github.com/NickStallman/SG-Docs/blob/main/reference/firmware-update/sgu-container.md);
`tools/sgu.py info|extract|verify` inspect any package.

## 5. Install through the stock pull route

1. **Read the dongle** (read-only): `python3 -I tools/deploy_debug.py status --host 192.0.2.50`
   prints its serial number, versions and inverter. The password comes from `--password`,
   `$WINET_WEB_PASSWORD` or a prompt and is never printed.
2. **Import your public key** (one time):
   `python3 -I tools/deploy_debug.py import-key --host 192.0.2.50 --pubkey dist/publicKey_V1.pem`.
   It asks for a typed "yes". The stock firmware stores the key on its external flash.
3. **Serve the package** on the LAN:
   `python3 -I tools/fake_iconfig_server.py --dist dist` (HTTPS on :20001, self-signed
   certificate). It answers the dongle's package-server requests from `dist/` and serves the `.zip`.
   Keep the dongle off the internet during the install.
4. **Start the update** in the stock web UI as admin: System update → iConfig update package →
   the address of the host running the server. The dongle downloads the package, verifies it
   against the imported key, writes our app into the inactive slot, rewrites `oemdata` and
   otadata, and reboots into our app.
   On stock firmware that still exposes a browser upload (`/system/upgradeCheck`),
   `tools/deploy_debug.py upload --sgu dist/<version>.sgu` does the same in one step.
5. **Do not run a native OTA yet.** It writes the inactive slot, which now holds the stock app.
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
| `/dump/bootloader`, `/dump/ptable` | 0x1000..0x8000 / 0x8000 + 0xC00 |
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

1. **Without serial:** turn on **Allow boot switch to other slot**, then press **Boot other slot
   (stock app)**. It verifies the other slot's image, rewrites otadata and reboots. The switch is
   off after every boot. Use it only while the other slot holds the stock app.
2. **Serial, from the serial backup:** `esptool --chip esp32 -p /dev/ttyUSB0 write_flash 0x0 full_16MB.bin`.
3. **Serial, from the network backup:** `write_flash 0x0 backup/restore_full_16MB.bin`.

None of these writes the external NOR; `extflash.bin` is a backup only.

## 9. WiNet-S2 install

The WiNet-S2 is installed and updated without a serial connection.

- Package with `make sgu SGU_BOARD=s2 APP=<firmware.ota.bin>` and install
  through the stock pull route (§5). Start with `winet-s2-debug.yaml` to confirm the board stays up,
  or package a production entry's image with `DEBUG_APP=`.
- OTA rollback stays on for the WiNet-S2 (`boards/winet_s2.yaml`): an image that resets within
  30 s of boot should be reverted to the previous slot by the stock bootloader (unconfirmed on
  hardware; the partition state at the first boot after an OTA tells), the only recovery path on
  a board with no USB access. An image that runs but never gets a network needs the network
  fallback (`common/ethernet_fallback.yaml`) instead. The external supervisor resets the board unless it is fed; the feed starts before
  ESPHome's own setup, and every software restart and panic switches the supervisor off first.
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
