# Limitations

What the firmware does not do, does differently from the stock dongle, or does on values that
are not confirmed on hardware.

## Hardware

- The active-low polarity of the status LEDs is not verified on either board.
- The LEDs do not reproduce two stock patterns: the 1 s blue blink for the data state and the
  green network-configuration blink (the firmware has no network-configuration mode).
- A single button click does nothing (no SmartConfig); provisioning is the fallback AP and its
  captive portal.
- The WiNet-S button input (GPIO36, input-only) relies on an external pull-up that is not
  verified.
- The WiNet-S RS485 driver-enable pin (GPIO12) is also the flash-voltage strap; whether the
  transceiver pulls it at reset is not verified.
- The W5500 reset pin (GPIO5) of the WiNet-S2 Ethernet variant is not verified; Ethernet on the
  WiNet-S2 is untested.
- The timeout of the WiNet-S2 external supervisor is not known. A supervisor reset clears RTC
  memory, so the reset journal of such a boot is lost.
- On a brownout the firmware does not switch the WiNet-S2 supervisor off.
- No UART header is located on the WiNet-S2; it is installed and debugged over the network only.
- The 30 s button hold erases ESPHome's own settings only; the IDF Wi-Fi and PHY settings in NVS
  survive it, and the stock factory reset, which rewrites the external flash, is not provided.
- The external flash is mounted read-only, which skips LittleFS power-loss repair: after a torn
  stock write the identity file can read as its previous version.

## RS485 polling

- Every battery-table device is treated as an SBR stack (cell block reads).
- A battery stack that goes offline stops answering; the firmware sees timeouts, never 10709 = 0.
- Half duplex assumes no local echo; an echo costs a CRC resync.
- Values stay valid through timeouts until the device goes offline (10 lost frames spanning at
  least 10 s).
- Signals that the stock dongle derives with per-signal hooks are computed by the firmware's own
  derived signals and can differ from the stock values.
- The `fast_loop` option is off by default; its CPU cost has not been measured on the WiNet-S2.
  With it off, reply detection and the frame gap follow the ~16 ms main loop (about 37 ms
  overhead per inverter transaction, 21 ms per battery transaction).
- The bus runs at 9600 baud with an 18 ms minimum frame gap.
- On an SH10RS in EMS mode 0 or 8 the power-flow registers (5017-5018, 5031-5032, 5214-5215,
  13008-13011, 13022) hold means over about 40 s inside the inverter, so a 1 s poll does not make
  them faster; the meter block from 5601 is the responsive grid-power source.
- Register ranges the inverter forwards to its power stage are not in the cyclic plan, because
  the inverter blocks its other ports during a forward.
- Inverter rows 13039 and 13070-13079 are not polled.
- Meter registers 4125-4129 and 4133-4137 are named as per-phase reactive and apparent power by
  their order, not from a meter document; 4153 and 4154 are unnamed. The per-phase energy rows
  from 4163 are not checked against a live meter.
- Energy Bridge rows follow the Energy Bridge point table; a few of its signals are unnamed.
- The scale of total yield at 5004 on protocol 1 and 4 inverters, the unit of the BMS total
  discharge energy, and the names of MPPT 13-20 and strings 25-40 are not confirmed on hardware.

## Register writes

- Forced charge/discharge command and power may not read back as written outside Forced mode;
  the mismatch is logged and the entity shows the inverter's value.
- The forced-power unit is W on the SH*RS and SH*RT models; some RT documents state percent, so a
  new model needs its unit checked before the rule applies.
- The export-limit rule's 30 000 W maximum is a sanity cap; the inverter enforces its own range
  from input registers 5622/5623.
- The unit and scale of the string-inverter power-limit register 5039 (0.1 kW in public sources)
  are not confirmed on hardware.
- Start/stop follows the stock setup descriptor: 13000 on a protocol-5 hybrid, 30045 on a
  protocol-4 string inverter, 5006 otherwise; neither of the first two is tested on such a unit.
- There is no fault-clear command: the stock fault-clear and self-check are equipment-layer
  commands, not holding registers.
- Power-stage acceptance ranges are known for the SH-RS, SG-RS and SH-T lines only
  (`data/dsp_params/models.yaml`); the SH-RT, SG-RT and every other line are checked against
  the installer catalogue ranges only. The SH-RS and SG-RS tables were read on the SH10RS and
  SG5.0RS, the SH-T table on the SH15T's firmware; their siblings share the power-stage image.
- A 100 ms settle gap follows every write and each register is rate-limited to one write per
  second; both are fixed.
- Polling continues while writes are queued; writes go out between poll transactions (the stock
  dongle pauses polling during a parameter operation).
- The hybrid start/stop rule has no protocol-4 alternative, so a hybrid on protocol 4 uses 5006
  although its parameter descriptor names 30045.
- A control profile is attached by device type (1 and 21: string, 35: hybrid, any other type:
  none, so its writes are refused); a device listed under `sungrow_control: devices:` keeps the
  profile that entry names. The web UI offers the controls of the attached profile.
- A device that refuses the devinfo read (fallback table) or has a fixed `table:` gets no control
  profile and is not published over MQTT; it is shown in the web UI, reported to iSolarCloud and
  served on :502.
- Without static devices the device index follows discovery order; YAML entity and control
  packages use `device: 0` for the inverter.
- Installer ranges are the stock static ranges widened to the XML values; the stock per-device
  range rules (rated voltage, 50/60 Hz, model) are not applied, so a value inside the range can
  still be outside the inverter's live stock range.
- Item-level `writePri` / `readPri` and writable items inside read-only XML categories are not in
  the Installer catalogue.
- Setup offers Australia only (country code 6), from the bundled AU grid-code XMLs.
- Whether writing the country register makes the inverter reload its default protection values is
  not established; Setup and Installer write the commit registers first.
- Protocol-4 devices (family `none`, descriptor 30643 / 30644 / 30045) and the protocol-5
  start/stop register 13000 are untested on hardware; Setup offers no profile on protocol 4.
- On a hybrid on protocol 1 or 4, which of 30500, 30643 and 30646 it implements is not known.
- ZS devices whose protocol code lacks "AA" are treated as protocol 3 (family `unit`) on both
  dongles.

## Modbus-TCP

- Multi-register values are low word first, following the public Sungrow register documents.
- The `poll_fallback` option is off by default: registers that only the slave map names stay
  empty on a protocol-5 inverter.
- A value whose width or scale differs between the poll table and the slave map is converted;
  an absent value is served as all-ones words.
- A device exception on a live read or write is reported as Modbus exception 4; the device's own
  exception code is not forwarded.
- Unknown unit ids answer an exception by default (`unknown_unit: ignore` drops them silently, as
  the stock dongle does).
- Unit 247 (the dongle's own virtual registers) is not served, and the stock logger remap of unit
  1 to 247 is not applied; unit 1 is always the inverter.
- With an allow-list set, IPv6 peers that are not IPv4-mapped are refused.
- A pending request times out after 5 s even when the RS485 queue is longer under load.
- fn 5 and fn 0x60 are refused (exception 1), and the stock reply quirks (fn 5 echo without the
  address, wire address 0 omitted) are not reproduced.

## Cloud

- Whether iSolarCloud treats the reported firmware version specially is not known;
  `cloud_firmware_version` should be the version the dongle's stock image reported.
- Without PSRAM the compressor state cannot be allocated and payloads go out uncompressed.
- Differs from the stock dongle: cloud writes (setparam) are behind the "Cloud writes" switch,
  default off; protected register ranges (power-stage forward windows, firmware channel, baud
  and slave registers, raw relay) are never written; only data types 3-8 are written; each
  point is one fn 16 frame.
- Differs from the stock dongle: commands with no handler (10383, 10451, 10453, 10375, 10369,
  10306 and 10326 for the I-V task, 10329 log export, 10452, 10308 and 10376 upgrade) get no
  reply, and the retained command is still cleared; getparam mode 1 (proprietary function codes)
  is refused; the battery-heating time-period variant is not recognised.
- Differs from the stock dongle: the uplink connects only after SNTP has set the clock;
  reconnects back off from 5 s to 5 min with a 30 s connect watchdog; the TLS client sends SNI.
- Differs from the stock dongle: publishing is serialised, one chunk in flight, re-sent after
  20 s up to three times; a rundata countdown still pending at the next period boundary is sent.
- Differs from the stock dongle: getparam renders data types 12 and 9 and above; commands run
  concurrently; 10384 with an unknown point answers readResult 5; point 4 is range-checked
  0-1440; device exceptions are reported as readResult "4"; an offline or unknown devCode
  answers result 0 with no data; chunks 2..n carry the message's enqueue time as sendTime.
- Differs from the stock dongle: devinfo 38781 is zero; rsp/apidata is not subscribed; a
  remote-control mask change reconnects; the previous broadcast topic is unsubscribed on a zone
  change; the port 19999 plain-TCP rule also applies after a server switch; the IDF CA bundle is
  used when the cafile is missing; `apply_cloud_timezone` and `cloud_settime` are YAML options;
  an empty broker host in the external flash falls back to the default host.
- Differs from the stock dongle: a server switch that changes only the TLS flag is ignored; an
  unconfirmed server switch keeps the new server when the remote-control mask differs from the
  default.
- The identity file is chosen by the first plausible serial number, and at most its first 1 MB is
  parsed.
- Moving an external flash between dongle models loses the identity, because the identity file
  name is per product type.
- Not implemented: devpointdata, faultmergedata, faultdata and its history, rundata history,
  fcas/second, monitor generaldata, attribute-change propertydata, logger run-data points (sent
  as `{}`), WiNet-S specific point tables, logger devpointdata and offline faults in the
  reconnect burst, the EV-charger forward of a time-zone change.

## Home Assistant

- ESPHome text sensors have no "unavailable" state; an invalid signal publishes an empty string.
- The `reactive_energy` device class needs a Home Assistant release that has it.
- Enum maps marked "authored" in the register tables come from public sources and are not
  checked against device codes.
- Entity ids differ from winet-extractor's; the device identifiers (`<model>_<serial>`) match.
- No native-API entity packages exist for loggers, EV chargers or Energy Bridges.
- The `_sg` / `_sh` public-map entities have no value on protocol-3 inverters such as the SH10RS
  and SG5.0RS.
- The SH10RS DRM state (5503) gets an entity only while a DRM mode 1-9 is asserted, because its
  capability bit (5521 bit 2) is clear otherwise. On the SG5.0RS the value 255 does not
  distinguish "DRM disabled" from "enabled and idle".

## Diagnostics

- A core dump reported just before a supervisor reset is reported once more and then erased.
- The `/dump` flash downloads exist in the debug build only; a live region such as `nvs` can
  change between hash and read and is then kept unverified.
- A debug bus scan with `timeout_ms` set lowers the response timeout for the whole bus while it
  runs, which can push slow devices offline.
- The I-V scan progress is an estimate (PV inputs × about 30 s); the inverter does not report
  which input it is sweeping.
- The I-V state word is decoded low word first, falling back to the other order when only that
  decodes.
- Grid-waveform recordings and fault-recording exports are available on the SH10RS only; the
  SG5.0RS supports the I-V scan only.
- The I-V sweep stops at 50 V, so the curves reach neither open circuit nor short circuit.
- Captures are kept in PSRAM until the dongle restarts, at most one I-V, four grid and one fault
  capture; grid and fault captures are refused without PSRAM.
- Waveform channel 173 is not identified; the viewer shows it in raw counts.
- One capture runs at a time, and an I-V scan can start at most every 2 minutes.
- Stock-OTA packages always carry the stock web UI as their second part; an app-only package is
  untested.

## Usage reporting

- The automatic-upload allowance (three per 24 hours) only rolls over with a valid SNTP clock;
  without one it never resets until the preferences are erased.
- An inverter that is dark at night is not reported as offline; the offline trigger needs another
  device on the bus to be answering.
- The diagnostics report's log tail is the 6 KB RTC ring at the configured log level; longer
  histories need `common/syslog.yaml`.
- The collector is trusted through the ESP-IDF certificate bundle; a private collector needs a
  certificate that chains to it.

## Battery

- The bits of the BMS capability word (10699, constant 0x0F) are not named; the entity is hidden.
- Low-SOC rest calibration (10684) and the capacity-mode balancing it starts (10683 = 1) are
  described from the battery controller's logic, not seen on a running system; they occur only
  after a long rest near empty.
- 10697 dropping below 10696 under module derating, and the hour counters 10694 / 10695 (time
  under an under-temperature alarm), are not confirmed on a running system.
- The 10666 request-flag bits (bit 0 charge prohibited, bit 1 discharge prohibited, bit 2
  forced-charge request) are not confirmed on a running system; bit 4 is not named. The 100 %,
  cut-off and emergency SOC flag meanings are inferred.
- How the 8-module pack over- and under-voltage limits scale for stacks with fewer modules is not
  known.
- The battery-controller meaning of protection bits 17-19 and warning bits 0, 1, 8, 12, 13, 15-17
  and 19 is not known; their inverter fault codes show as numbers or with the inverter's names.
- The emergency charging SOC (19941) is not polled.
- The SH10RS does not serve 10601-10664, 10752-10756, 10807-10820 or 5637, so those battery rows
  have no value.
- 5673 / 5674 carry only the low 16 bits of the total battery charge; the firmware reads the
  total from 13041 / 13042 instead.

## Pre-built images

- Until Home Assistant adopts the dongle the native API is plaintext; the native OTA accepts the
  web login password (and the API key once there is one). Set a web password before putting the
  dongle on a network you do not control.
- While the setup access point is up its captive portal takes an unauthenticated firmware
  upload; the access-point password is the only guard.
- The MQTT password and the web login are stored in NVS in clear; the dongles have no flash
  encryption.
- Cloud version detection only works while the stock app is still in the other OTA slot; the
  value is remembered from then on, and the Settings page takes an override.
- Improv over serial exists on the WiNet-S image only (the WiNet-S2 has no reachable UART);
  there is no Improv over Bluetooth, which the WiNet-S has no spare RAM for.
- The four images are Wi-Fi or Ethernet, not both.
- The WiNet-S has no OTA rollback, so a failed update from the **Firmware** entity is recovered
  over UART.

## Features deliberately not provided

- **115200 baud** over the temporary baud registers: the inverter's own channel settings stay
  untouched and 9600 baud carries the poll plan.
- **fn 0x65 change-stream polling:** tiered fn 3/4 polling covers every supported model.
- **Frame gap below 18 ms:** no gain at 9600 baud worth the risk to slower devices.
- **Always-on `SG-<SN>` hotspot and its WiMeter / WiProxy transport:** the stock hotspot is open
  and exposes every Modbus-TCP port, including unit-247 writes; ESPHome's WPA2 fallback AP covers
  provisioning.
- **Modbus-TCP over TLS and the extra servers 503-516:** the stock TLS uses one key pair shared
  by every dongle with no client authentication.
- **RS485 transparent gateway:** it takes the only RS485 UART away from the master, so all data
  stops; :502 serves the same data.
- **Meter sync to the inverter:** after every write the inverter prefers the dongle's meter data
  over its own meter for 60 s; a configured meter is polled only.
- **YMODEM log export over RS485 (cloud 10329):** it needs inverter writes, a baud change and a
  bus lock of up to about 20 minutes with polling stopped.
- **rdswitch remote maintenance:** the window only unlocks a stock vendor login this firmware
  does not have, so it is refused with readResult 5.
- **ICMP ping watchdog:** the local paths do not depend on the internet, and ESPHome restarts a
  node Home Assistant cannot reach.
- **Modbus-TCP client (call-home) links:** an unauthenticated outbound write channel.
- **Cloud-driven feed-in schedule writes 99783 / 99785:** export limiting is driven from Home
  Assistant through 13074 / 13087 behind the writes switch; they answer readResult 5.
