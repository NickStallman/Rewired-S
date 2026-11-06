# Going back to the stock firmware

Rewired-S leaves the way back open: it never writes the bootloader, the
partition table or the `oemdata` partition (the stock web UI), it mounts the external flash that
holds the dongle's serial number, cloud settings and grid profiles **read-only**, and it keeps
its own settings in ESPHome's own NVS namespace. Whatever route you take back, the dongle comes
up as the stock unit with its identity, cloud registration and installer settings intact.

| Partition / chip | Stock content | Rewired-S |
|---|---|---|
| bootloader, partition table | stock | never written |
| `ota_0` / `ota_1` | stock app in one slot | Rewired-S in the other slot; the first ESPHome OTA overwrites the stock slot |
| `otadata` | selects the slot | rewritten by every OTA and by **Boot other slot** |
| `nvs` | Wi-Fi and PHY calibration, stock settings | ESPHome's `esphome` namespace added; the 30 s button hold erases only that |
| `oemdata` | stock web UI (LittleFS) | never written (the updater route rewrites it with identical content only when a package carries the stock part) |
| external SPI NOR | identity, cloud settings, grid-code XMLs, public key | read-only mount |

Stock firmware details: [flash layout](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/flash-layout.md),
[boot and runtime](https://github.com/NickStallman/SG-Docs/blob/main/reference/hardware/boot-and-runtime.md).

## 1. Boot other slot (no tools)

While the stock app is still in the other OTA slot, that is right after the first install and
before any ESPHome OTA: open the web UI, **Settings → Boot slots → Boot other slot**, confirm.
The firmware checks that the other slot holds a valid app image, points `otadata` at it and
restarts. The stock app boots with everything as it was. The debug build has no web UI: turn on
its **Allow boot switch to other slot** switch, then press **Boot other slot (stock app)**.

## 2. Put the stock app back with an ESPHome OTA

After an update the stock app is gone from the flash, but any official Sungrow package for
your dongle contains it. Download the package yourself from Sungrow's support site, the
iSolarCloud portal or the update server the dongle normally uses; the project cannot redistribute
it.

```sh
rewired-s-sgu info    WINET-SV300.001.03.P032.sgu                 # version, parts, integrity
rewired-s-sgu extract WINET-SV300.001.03.P032.sgu --out-dir stock  # stock/<version>.app.bin, stock/<version>.oemdata.bin
```

`extract` writes the type-2 part, which is the ESP-IDF app image exactly as the dongle's updater
would flash it, and the type-3 part (the web UI image, which you do not need to write back
because Rewired-S never changed that partition). Check it looks right: `info` shows the app
descriptor (`WINET-SV300.001.03.P032…`, project `WiNet-S2`).

Then send the stock app with ESPHome's OTA, which writes the inactive slot and switches to it:

```sh
esphome upload my-dongle.yaml --device 192.0.2.50 --file stock/WINET-SV300.001.03.P032.app.bin
```

`my-dongle.yaml` is your ESPHome configuration ([ESPHome](esphome.md#your-configuration)); for a
pre-built image, a file holding only the `packages:` line of its pre-built entry is enough.

The stock app boots from that slot and marks itself valid. The Rewired-S image stays in the other
slot; the stock firmware will overwrite it with its next own update.

> [!NOTE]
> This route is not yet run on hardware. The mechanism is the ordinary ESPHome OTA with a foreign
> app image; whether the WiNet-S2's stock bootloader rolls a failed image back is unconfirmed (see
> [limitations](limitations.md#hardware)), so count on **Boot other slot** rather than on a
> rollback; please report your result. **Boot other slot** (route 1) is verified on the WiNet-S.
> On pre-built images ESPHome's OTA accepts the web login password (or the API key once Home
> Assistant has adopted the dongle); see [pre-built images](prebuilt.md).

## 3. Over serial (WiNet-S)

Either restore the full backup taken before the install:

```sh
esptool --chip esp32 -p /dev/ttyUSB0 write_flash 0x0 winet-s-full-16MB.bin
```

or write just the extracted stock app into the first slot and blank the selector, which leaves
NVS and the web UI partition untouched:

```sh
esptool --chip esp32 -p /dev/ttyUSB0 erase_region 0x109000 0x2000
esptool --chip esp32 -p /dev/ttyUSB0 write_flash 0x110000 stock/WINET-SV200.001.00.P046.app.bin
```

Wiring and download mode: [install over serial](install_serial.md). The network backup made by
the debug build (`rewired-s-flash-backup`) assembles the same kind of restore image with the
stock app put back.

## What survives, what does not

- **Survives:** serial number and cloud identity, iSolarCloud registration and plant, the
  inverter's settings (they live in the inverter), the installer password of the stock web UI,
  the grid profile, the imported public key (the stock firmware keeps verifying packages against
  the last key imported; import Sungrow's key again if you want its own updates, or leave it,
  the cloud push channel does not use it).
- **Does not survive:** Rewired-S's own settings (web login, MQTT, the writes gate), which are
  irrelevant to the stock app, and the Wi-Fi credentials you entered through the setup network,
  since the stock app keeps its own. Re-enter the Wi-Fi in the stock app if needed.
- **Never changed:** the inverter and the batteries. Rewired-S writes inverter registers only
  through its gated controls, all of which are the same settings the stock app and the cloud
  write; nothing is left behind in the inverter that the stock firmware does not understand.
