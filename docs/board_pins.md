# Board pins

The pin assignments the firmware uses on each dongle. They are set as substitutions at the top of
`boards/winet_s.yaml` and `boards/winet_s2.yaml` and in the Ethernet packages
(`boards/winet_*_ethernet.yaml`). Hardware background:
[WiNet-S](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s.md),
[WiNet-S2](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/winet-s2.md),
[LEDs and button](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/leds-and-button.md).

## WiNet-S2 (SV300, ESP32-S3)
| Function | GPIO |
|---|---|
| RS485 UART1 TX / RX / DE (RTS) | 18 / 17 / 8 |
| External NOR on SPI2: MOSI / MISO / SCLK / CS | 11 / 13 / 12 / 10 (DIO, 40 MHz) |
| Supervisor enable (low = off) | 2 |
| Supervisor kick | 7 |
| W5500 on SPI3: SCLK / MOSI / MISO | 39 / 40 / 41 |
| W5500 CS / INT / RST | 42 / 6 / 5 (36 MHz, SPI mode 0) |
| LED red / blue / green | 19 / 38 / 20 (active-low) |
| Button (pressed = low, internal pull-up) | 4 |
| PSRAM | octal, 8 MB, 80 MHz |

GPIO19 and GPIO20 are the USB D- / D+ pins and carry LEDs, so the logger runs on UART0, not
USB-Serial-JTAG. The W5500 takes the chip's base MAC + 3 as its Ethernet address (IDF's
`ESP_MAC_ETH` slot), so the dongle's wired and Wi-Fi addresses differ by three.

**Supervisor feed.** An external supervisor resets the WiNet-S2 unless it is kicked.
`sungrow_board: watchdog:` runs this sequence as the first statement of the generated `setup()`,
before the logger and the rest of the application start:
1. Configure GPIO2 and GPIO7 as input-output.
2. Set GPIO2 = 0.
3. Ten times: wait 100 ms, write GPIO7 = s and invert s (s starts at 1).
4. Set GPIO2 = 1.
5. Start a periodic 500 ms `esp_timer` that toggles GPIO7.

The kick runs from the timer, so a slow `setup()` or `loop()` cannot starve it; a real hang
still lets the supervisor reset the board. Every software restart switches the supervisor off
first (GPIO2 low for 50 ms in a shutdown handler), so the next boot starts with a fresh
supervisor.

## WiNet-S (V200, ESP32)
| Function | GPIO |
|---|---|
| RS485 UART1 TX / RX / DE (RTS) | 13 / 35 (input-only) / 12 |
| External NOR on HSPI (SPI2): MOSI / MISO / SCLK / CS | 2 / 34 (input-only) / 14 / 15 (40 MHz) |
| Ethernet RMII, PHY DP83848 at address 1: MDC / MDIO | 23 / 18 |
| RMII clock | GPIO0 input (external 50 MHz) |
| PHY reset | 5 |
| LED red / blue / green | 4 / 32 / 33 (active-low) |
| Button (pressed = low, external pull-up; input-only pin) | 36 |
| PSRAM | quad, 4 MB, 80 MHz |
| External supervisor | none |

The RMII data pins are fixed by the ESP32: TXD0 19, TXD1 22, TX_EN 21, RXD0 25, RXD1 26,
CRS_DV 27. GPIO12 (DE) is the MTDI strap that selects the flash voltage: it is driven by the
UART RTS and must never get a pull-up. GPIO0 is both the download strap and the RMII clock
input.

## Button / LED behaviour (both boards)
`components/sungrow_board/board_logic.h` implements it and `common/board_io.yaml` wires it up.
The button is polled every 50 ms and the LEDs are updated every 100 ms, as on the stock dongle.

**Button.** A press of at most 1 s is a click; clicks are dispatched 1 s after the last release.
A longer press is a long press; it counts only when the button was idle for at least 3 s
before it, and it acts on release.

| Action | Effect |
|---|---|
| 1 click | nothing (provisioning is the fallback AP and captive portal) |
| 2 clicks | fallback AP off; restarts the dongle when the AP is running |
| 3 or more clicks | fallback AP on; it starts the next time the station cannot connect |
| hold 5-30 s | restart |
| hold ≥ 30 s | erase ESPHome's own NVS settings (Wi-Fi credentials from the captive portal, restored entity states, the cloud server-switch state, the AP flag) and restart |

The AP flag is stored in ESPHome's NVS preferences. Nothing the button does writes the external
flash, other NVS namespaces or the app partitions.

**LEDs.**
- **Blue** (network and cloud): off without a network link; solid with a link and the cloud
  connected; with a link and no cloud, solid for 60 s, then a 1 s flash every 4 s; a 200 ms blink
  while the fallback AP is up.
- **Green:** toggles every 1 s with no faults, off with faults.
- **Red:** on with faults.

A fault is a found RS485 device (one with a register table) that is offline, ignored for the
first 60 s after boot, or a cloud uplink that is enabled and in error (down for more than 60 s,
connect failed, or no usable identity).

## Other boards

The firmware is written for the two dongles, but nothing in it needs Sungrow's silicon: the
inverter's COM port (RJ45) carries RS485 and the dongle's supply, so any ESP32 or ESP32-S3
development board with an RS485 transceiver on a UART can run it. What a new board needs:

- its own `boards/<board>.yaml` with the RS485 pins (`rs485_tx_pin`, `rs485_rx_pin`,
  `rs485_de_pin`), the LEDs and button if any, and no `watchdog:` block (the supervisor feed is
  a WiNet-S2 thing), plus a partition table that fits its flash;
- a cloud identity, if the iSolarCloud uplink is wanted: the dongles read theirs from the
  external NOR (`sungrow_extflash`); a dev board has to be given one (see
  [cloud_identity.md](cloud_identity.md));
- the pin-out of the inverter-side RJ45 (which pins carry RS485 A/B and the supply) is not
  documented here; the SG-Docs hardware pages cover the dongle side only.

The `winet_s` and `winet_s2` variants of `sungrow_board` are the only ones the component
validates today, so a dev board starts from a copy of the closest board file.
