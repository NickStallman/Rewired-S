# Installing over the serial pads (WiNet-S)

The WiNet-S is a plain ESP32 with its UART0 brought out to pads on the board, so it can be
flashed like any ESP32 project. This route needs no package signing and gives you a full backup
of the flash first, which is the definitive way back to stock. The WiNet-S2 has no accessible
UART ([limitations](limitations.md)); use the [updater route](install_ota.md) for it.

Pinout and board details: [WiNet-S hardware](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s.md)
(`#console-uart0`, `#strapping-pin-cautions`).

## What you need

- A 3.3 V USB-serial adapter (never 5 V on the data lines), three or four wires, a resistor of
  about 470 Ω, and a small Phillips screwdriver.
- `esptool` (`pipx install esptool`) or ESP Web Tools on the
  [install page](https://nickstallman.github.io/Rewired-S/). Port names and
  drivers per operating system: [installing the tools](tools_setup.md).
- The image: `winet-s-prebuilt-<version>.ota.bin` or `winet-s-ethernet-prebuilt-…` from the
  [releases](https://github.com/NickStallman/Rewired-S/releases), or your own
  `.esphome/build/<name>/build/firmware.ota.bin`.

> [!WARNING]
> Flash **only the app image** (`firmware.ota.bin`) into an OTA slot. Never write
> `firmware.factory.bin`, a bootloader or a partition table: that overwrites the stock bootloader
> and partition table, which nothing in this project needs. `boards/partitions_winet_s.stock.csv`
> is the layout the dongle already has.

## 1. Open the dongle

Unplug the dongle from the inverter. Two screws hold the back of the case; the board lifts out.
Find the UART pads: TX (GPIO1), RX (GPIO3), GND and 3V3, and the GPIO0 pad or test point.

Wiring: adapter **RX → TX**, **TX → RX**, **GND → GND**. Power the board from the adapter's 3.3 V
output on the 3V3 pad (the dongle normally takes its power from the inverter's RS485 connector).

## 2. Enter download mode

The ESP32 enters the ROM download mode when GPIO0 is low at reset. On the WiNet-S, GPIO0 is also
the 50 MHz clock input of the Ethernet PHY, so do not short it hard to ground: pull it low
**through the 470 Ω resistor**, apply power (or pulse EN / reset), then release it.

```sh
esptool --chip esp32 -p /dev/ttyUSB0 chip_id             # answers in download mode
espefuse --chip esp32 -p /dev/ttyUSB0 summary            # Secure Boot and flash encryption must be off
```

If `espefuse` reports Secure Boot or flash encryption enabled, stop: no custom image will boot.
Every dongle seen so far has both off.

## 3. Back up the whole flash

```sh
esptool --chip esp32 -p /dev/ttyUSB0 -b 460800 read_flash 0 0x1000000 winet-s-full-16MB.bin
```

Keep this file in two places. It contains the bootloader, the partition table, both app slots
and the stock web UI, so `write_flash 0x0 winet-s-full-16MB.bin` restores the dongle to exactly
this state at any time. The external flash with the identity is a separate chip and is not part
of this image (and nothing in Rewired-S writes it).

## 4. Write Rewired-S into the first OTA slot

The on-flash partition table has two app slots and no factory slot
([flash layout](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/flash-layout.md#partition-table)):
`ota_0` at `0x110000`, `ota_1` at `0x490000`, and the slot selector `otadata` at `0x109000`. A
blank selector makes the bootloader start `ota_0`.

```sh
esptool --chip esp32 -p /dev/ttyUSB0 -b 460800 erase_region 0x109000 0x2000
esptool --chip esp32 -p /dev/ttyUSB0 -b 460800 write_flash 0x110000 winet-s-prebuilt-0.9.2.ota.bin
```

This overwrites whatever was in `ota_0` (usually the stock app; the other slot keeps whatever it
had). Reconnect the adapter without GPIO0 pulled low, or power-cycle, and watch the boot log at
115200 baud.

ESP Web Tools on the [install page](https://nickstallman.github.io/Rewired-S/) does
exactly these two writes (a blank `otadata` and the app at `0x110000`) through Web Serial; it
does not erase the chip and does not touch the bootloader.

## 5. First boot

- Pre-built image: the setup network `rewired-s-xxxxxx` appears; join it, enter your Wi-Fi, then
  open the web UI (`admin` / `rewired-s`, change it) and set the MQTT broker.
  Ethernet image: it takes a DHCP lease.
- Compiled image: it joins the Wi-Fi from `secrets.yaml`.

A bad image shows as a boot loop on the serial console (the WiNet-S has no OTA rollback;
[limitations](limitations.md#pre-built-images)); write the backup back or try another image.

## Updates afterwards

ESPHome OTA (`make deploy`, or the **Firmware** update entity on pre-built images) writes the
inactive slot and switches to it. The first OTA therefore overwrites whatever stock app was still
in `ota_1`; your flash backup is the way back after that ([back to stock](back_to_stock.md)).
