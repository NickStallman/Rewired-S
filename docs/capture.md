# Diagnostic captures: I-V scan, grid waveform, fault recordings

The web UI **Captures** page (`#/capture`) asks the inverter for one of three diagnostic files and
offers it as a download:

| Capture | Inverters | What the inverter does | File |
|---|---|---|---|
| I-V scan | SG5.0RS, SH10RS (functionEnum bit 3) | Its DSP sweeps the PV inputs one at a time (voltage reference stepped from the operating point down to 50 V, about 25-30 s per input); the ARM stores the curve data | `VI_ALL` (binary, 258-byte records, 3 per PV input: voltage in 1 V, current in 0.01 A, current repeated) |
| Grid waveform | SH10RS only (functionEnum bit 19) | Its DSP records 30 channels (AC voltage, DC bus, battery voltage, AC currents, ...); the ARM copies them to flash | `GridWaveRecord` (19 962 bytes: 6-byte prefix + 30 channels x 320 samples) |
| Fault recordings | SH10RS only (functionEnum bit 19) | Nothing new is recorded: the control board copies the recordings it stored at its last trips (up to 10, one per trip, newest kept) into its flash and serves them as one file | `FaultUpload_packet_01` (6-byte prefix: u32 record count low word first, u16 record length; then N records of 19 956 bytes in the grid-waveform format with 1 cycle) |

The files are kept raw for offline analysis; the browser viewers decode the grid-waveform and
fault records (`components/sungrow_web/web/src/gridwave.ts`) and the I-V curves
(`components/sungrow_web/web/src/ivcurve.ts`). The I-V series
are voltage (1 V per count), current (0.01 A per count) and a repeat of the current series, 128
points per input. Nothing is published to MQTT or Home Assistant.

Component: `components/sungrow_capture` (`capture_core.{h,cpp}` is the host-tested core),
package `common/capture.yaml`. Protocol background:
[SG-Docs: diagnostics and log export](https://github.com/NickStallman/SG-Docs/blob/main/reference/protocols/diagnostics-and-log-export.md).

## Sequence

I-V scan:

1. fn 16 doc 32000 = 0x01A0 starts the scan. Exception 04 means the inverter is busy or has used
   its 1000 starts since boot.
2. fn 3 doc 32005 x 5 once a second: u32 state (bit 15 success, 16 scanning, 18 failed, 17 idle),
   u32 upload code, u16 fault code. The core decodes the u32 low word first and falls back to the
   other order when only that decodes; the raw words are in the timeline. Limit 180 s. While the
   scan runs the Captures page shows "scanning · N s of about M s", estimated from the number of
   PV inputs at about 30 s each (the inverter does not report which input it is sweeping), and the
   refusal or abort reason when the fault word is set.
3. Bus held. fn 16 doc 31999 = 0 (format flag), fn 16 doc 32000 = 0x00A0 (select `VI_ALL`).
4. YMODEM receive.

Grid waveform:

1. fn 4 doc 30079 (progress %) is read once.
2. fn 16 doc 32010-32011 = [0xAA, cycles], cycles 1..8. The inverter ignores a bad start without
   an exception, so the start is confirmed by progress only. The cycle count sets the sample rate,
   18 kHz / (4 x cycles), over a fixed 320 samples: 1 = 4.5 kHz (71 ms, about 3.5 mains cycles),
   8 = 562.5 Hz (569 ms, about 28 cycles).
3. fn 4 doc 30079 once a second: 0 while the DSP records, 20 when the copy starts, up to 100. The
   inverter has no failure value; a failure stops the percentage. The core accepts 100 only after
   it has seen a value below 100 (otherwise an old recording would be exported), fails after 15 s
   at 100, after 240 s without change, or after 600 s.
4. Bus held. fn 16 doc 32000 = 0x01B0 (select `GridWaveRecord`; also sets the format flag).
5. YMODEM receive.

Fault recordings:

1. fn 4 doc 30078-30079 (record count, progress %) are read once.
2. fn 16 doc 30034 = 0xAA asks the control board to re-read its record catalogue (it does so only
   when idle).
3. fn 16 doc 32000 = 0x01A1 starts the copy of the stored records into the control board's flash
   (and sets the format flag). It is refused with exception 04 while the catalogue count is still
   0, so the core repeats it once a second; after 30 s of refusals the capture ends with "the
   inverter has no stored fault recordings".
4. fn 4 doc 30078-30079 once a second: the record count of the fetched store and the copy
   progress, with the grid-waveform guards (100 accepted only after a value below 100, 15 s stale,
   240 s stall, 600 s total). The copy takes about 20 s per record.
5. Bus held. fn 16 doc 32000 = 0x00B0 (select `FaultUpload_packet_01`).
6. YMODEM receive. The record count in the file prefix is reported as `records`.

The trigger writes go through `ControlCore::write_diag`: only the registers and values above are
accepted, from no other source (cloud, MQTT, Modbus TCP and the normal web write endpoint cannot
reach them). The "Inverter writes enabled" switch does not apply; each start needs the web UI
confirmation (confirm token bound to address 65532).

## Bus use

Normal polling continues while the inverter scans, records or copies. The bus is held only for
the file transfer (`ModbusMaster::acquire_exclusive` + raw mode): the master sends nothing and the
capture runs YMODEM on the UART at 9600 baud. The hold ends with the transfer, on any failure, or
at the safety limit (60 s until the header, then size / 900 B/s + 30 s). An I-V file (about
1.5 KB) takes about 2 s; a grid recording (19 962 bytes on the SH10RS) about 21 s; a fault export
about 21 s per record. No device goes offline because of the pause. The high-frequency loop request the capture holds during the transfer is the same ESPHome
mechanism the Modbus hub uses for its `fast_loop` option; each component holds its own requester and
the loop runs fast while either is active.

YMODEM: 'C' (repeated every 3 s, 3 times), header block 0, STX 1024 / SOH 128 data blocks,
CRC-16/XMODEM, ACK per block, NAK on a bad block (10 errors cancel), EOT, ACK. The data are cut to
the size in the header.

## Effects on the inverter

- I-V scan: the inverter sweeps one PV input at a time in fixed-voltage mode while the others keep
  tracking, about 25-30 s per input (about 2 minutes for four), so PV output drops while it runs. The
  inverter's own DSP polling stops for the scan (its own limit is a 200 s silence time-out), so
  the values it serves do not change during that time.
- Grid waveform: the inverter pauses its own DSP polling while it copies the recording (AC and
  battery values freeze for that time).
- Fault recordings: nothing new is recorded and the stored recordings stay on the inverter. The
  inverter pauses its own DSP polling while the control board copies the records (about 20 s per
  record), so its readings freeze for that time.
- The dongle allows one capture at a time and an I-V start at most every 2 minutes.

## Results and downloads

The newest I-V capture, the four newest grid captures and the newest fault export are kept in RAM
(PSRAM; grid and fault captures are refused without PSRAM) until the dongle restarts.

| Endpoint | Content |
|---|---|
| `GET /api/capture` | status: running step / progress, supported captures per inverter, results |
| `POST /api/capture_start?device=D&kind=iv\|grid\|fault[&cycles=N]&confirm=T` | start (token from `POST /api/confirm?device=D&address=65532`); `cycles` 1..8 for `grid` only |
| `POST /api/capture_cancel` | stop a running capture and release the bus |
| `GET /api/capture_file?id=N` | the raw file (`<kind>_<id>_<UTC start>.bin`) |
| `GET /api/capture_meta?id=N` | metadata JSON: result, header name and size, YMODEM counters, the last state / progress registers, record count, scan duration, a timeline of every poll answer and step, and a snapshot of every numeric signal of the inverter (value, function code and register) before the start and after the scan or recording |

`GET /api/capture` fields:

- `running`: `true` with `device` (store index of the inverter), `kind` (`iv` | `grid` |
  `fault`), `step` (text), `progress` (0..100 or -1) and `elapsed_ms`, all top-level; an I-V scan
  adds `scan_ms` (ms since the inverter first reported scanning, 0 before), `iv_fault` (the fault
  word of the latest state poll) and `iv_fault_text` (its meaning; `null` while the word is 0 or
  unknown).
- `devices[]`: `device`, `online`, `model`, `iv` / `grid` / `fault` (supported), `mppt` (PV inputs
  the dongle has valid MPPT voltages for).
- `results[]` (newest first): `id`, `kind`, `ok`, `size`, `cycles`, `records` (records in the
  file: grid 1, fault as counted in its prefix), `wall_start`, `duration_ms`, `result`,
  `file_name`, `inverter`.

## Waveform viewer

`#/wave` (Captures page: "Open the waveform viewer", or "view" on a grid or fault result) decodes
the raw file in the browser (`components/sungrow_web/web/src/gridwave.ts`, unit-tested in
`gridwave.test.ts`):

- Rail of the recordings on the dongle, newest first, grouped into grid recordings and fault
  recordings (one entry per record of a fault export), each with a thumbnail of its AC voltage at
  its true time span; `[` / `]` or Newer / Older step through them.
- "Status at the trigger" card above the lanes: the fault codes set in the record's status words
  (named from the inverter's fault table; an unknown code keeps its number), or "No fault bits set
  at the trigger", and the raw status words. A fault export is split into its records, and each
  record opens at `#/wave/<id>/<k>`.
- Lanes on one shared time axis, one unit per lane: AC voltage (100 grid, 103 inverter output),
  AC current (107 inverter, 146 backup port), DC bus (143), bus halves (144, 145), battery voltage
  (160) and current (161), PV voltages 1-4 (110-113), PV currents 1-3 (127-129), phase angles (172
  grid, 170 output), leakage current (147, 0.1 mA) and the bridge's PWM compare (109). One
  cursor reads every channel at the same sample; legend chips show the value at the cursor, rms and
  mains frequency (AC) or ripple peak-to-peak (DC), and switch a trace on or off.
- Drag across a trace to zoom, double-click or Escape to zoom out, + / - zoom about the cursor,
  arrow keys move the cursor (Shift: 10 samples).
- The other channels as small multiples (raw counts), and every channel as a table.

## I-V viewer

`#/iv/<id>` ("view" on an I-V result) decodes `VI_ALL` in the browser
(`components/sungrow_web/web/src/ivcurve.ts`, unit-tested in `ivcurve.test.ts`). A rail lists the
I-V scans on the dongle. For every PV input the body plots current against voltage (I-V) and power
against voltage (P-V) on one shared voltage axis and marks the maximum-power point ("MPP … W at
… V / … A"); a table lists per input the point count, voltage and current range, the MPP voltage,
current and power, and whether the repeated current series matches. The sweep runs from the
operating point down to 50 V, so the curves reach neither open circuit nor short circuit; the
viewer says so above the plots.

## Analysis workflow

1. Run an I-V scan around midday on a clear day and download the file.
2. Split `VI_ALL` into 258-byte records (256 data bytes, 0x80, last-of-group flag); three records
   per PV input, 128 big-endian u16 each.
3. Check the series against the snapshot (PV voltage and current per MPPT, DC power): the first
   series is the voltage in 1 V steps (from the operating point down to 50 V), the second the
   current in 0.01 A, the third repeats the second; the product of the first two peaks near the
   operating point.
4. Grid waveform and fault records: the 6-byte prefix is followed by records of 19 956 bytes
   (header with slot number, DSP clock and the status words at the trigger, then 30 channels x 320
   big-endian samples). The viewer names every channel except 173.
