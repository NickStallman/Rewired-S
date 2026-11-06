# Installing through the dongle's own updater

Both dongles can take Rewired-S without opening the case, through the update route the stock
firmware already has: it downloads a signed package from a server on the LAN, verifies the
signature against a public key it holds, writes the new app into the **inactive** OTA slot and
reboots into it. The stock app stays in the other slot. Nothing writes the bootloader, the
partition table or the external flash that holds the dongle's serial number and cloud settings.

Background: [update channels](https://github.com/NickStallman/SG-Docs/blob/main/reference/firmware-update/update-channels.md)
and [the package format](https://github.com/NickStallman/SG-Docs/blob/main/reference/firmware-update/sgu-container.md)
in SG-Docs.

## What you need

- The dongle on your LAN, reachable from a computer that can run Python 3. The stock web UI
  login (`admin` and the installer password; the default is in the dongle's manual).
- A **package** for your dongle. Either the release package (`winet-s-prebuilt-…sgu` and so on,
  with its `.bin` signature and the release public key `publicKey_V1.pem`, from the
  [releases page](https://github.com/NickStallman/Rewired-S/releases); the key is also inside
  every package `.zip`), or one you build and sign yourself (below). The key file must keep the
  `publicKey_V<n>.pem` name: the stock updater derives the key version from it.
- Python 3.11+ with `pip install -r tools/requirements.txt`. Per-platform setup (Windows
  PowerShell, macOS, Linux), firewall notes and the exact commands: [running the tools](tools_setup.md).

> [!NOTE]
> The release packages are **app-only**: they contain the Rewired-S app and nothing else, so the
> dongle's stock web UI partition is left exactly as it is. The stock updater of a WiNet-S2 on
> firmware `P033` has installed such a package on hardware (reported in
> [#10](https://github.com/NickStallman/Rewired-S/issues/10)). If your dongle refuses an
> app-only package (the update log says nothing was found to install), build a package with
> `--stock-sgu` as described under *Build your own package*: it copies the web UI part from an
> official package you download, so the updater rewrites that partition with identical content.

## 1. Read the dongle and note its firmware version

```sh
python3 -I tools/deploy_debug.py status --host 192.0.2.50
```

It prints the serial number, the versions and the inverter it sees. Write down the **firmware
version** the stock web UI shows on its About page (for example `WINET-SV300.001.03.P032`): a
compiled-from-source build needs it as `cloud_firmware_version` ([cloud identity](cloud_identity.md));
pre-built images read it from the stock app left in the other slot.

The password comes from `--password`, the `WINET_WEB_PASSWORD` environment variable or a prompt,
and is never printed.

## 2. Import the public key (once)

```sh
python3 -I tools/deploy_debug.py import-key --host 192.0.2.50 --pubkey publicKey_V1.pem
```

The tool asks for a typed `yes`. The stock firmware stores the key on its external flash and from
then on verifies packages against it. The key survives stock cloud updates, so a dongle that had
it imported before answers a second import with `result_code=120`:
the tool reports that as "already stored" and the install can go ahead (`fake_iconfig_server.py
--no-key` skips offering the key again). For a self-signed package use your own public key, named
`publicKey_V<n>.pem` as `tools/package_sgu.py` writes it (`dist/publicKey_V1.pem`).

## 3. Serve the package

```sh
python3 -I tools/fake_iconfig_server.py --dist release/     # the directory with the .sgu, .bin, Version_signature.xml or the .zip
```

It answers the dongle's package-server requests over HTTPS on port 20001 with a self-signed
certificate. Keep the dongle off the internet during the install so nothing else answers. The
server answers both the `/v2/web/…` requests of current stock firmware and the `/web/…` requests
of older versions (`P016`).

## 4. Start the update in the stock web UI

Open the stock web UI **from the computer that runs the server**: the dongle contacts the
address of the browser that is logged in, not an address you type. Log in as `admin`, open
**System → System maintenance → System update → iConfig update package** and start. The dongle
downloads the package, verifies it, writes the app into its inactive slot and reboots into
Rewired-S. Expect two to three minutes; do not power the dongle off while the progress bar is
moving.

> [!CAUTION]
> The same page has a second, similar dialog for updating the **inverter** (device update). It
> asks the package server for the inverter's model and would send what it gets to the inverter
> over RS485. Use only the **System update** dialog. `tools/fake_iconfig_server.py` refuses a
> request that does not name a WiNet (it answers an empty package list and logs `refused`), so
> the wrong dialog ends with "no package found" rather than an inverter update attempt.

On stock firmware old enough to still offer a browser upload, `tools/deploy_debug.py upload
--sgu <package>.sgu` does steps 3 and 4 in one go.

## 5. First boot

| | WiNet-S2 | WiNet-S |
|---|---|---|
| Boot test | the stock bootloader is expected to roll an image that resets within 30 s back to the stock slot (not yet confirmed on hardware: the first boot after an OTA logs `Bootloader rollback: supported` under `safe_mode` when it works; please report that line) | no automatic rollback (the stock bootloader would undo good images too); a bad image is recovered over UART |
| No network after boot (Ethernet images) | the **network fallback** boots the other slot after 3 min (the Wi-Fi image or stock app that was there), see [prebuilt.md](prebuilt.md) | the same |
| Way back while the stock app is in the other slot | **Settings → Boot other slot** in the web UI | the same button |
| Pre-built image | the `rewired-s-xxxxxx` setup network appears (Wi-Fi) or the dongle takes a DHCP lease (Ethernet) | the same |
| Compiled image | joins the Wi-Fi from `secrets.yaml` | the same |

Then: open `http://<dongle>/`, log in (pre-built: `admin` / `rewired-s`, change it on the Settings
page), set the MQTT broker, and watch the Devices page fill up as the inverter and batteries are
discovered. Add the dongle to Home Assistant from the ESPHome integration's discovery.

> [!IMPORTANT]
> **Do not run an ESPHome OTA yet** if you want to keep the stock app available: ESPHome's OTA
> writes the inactive slot, which is where the stock app now lives. Take the flash backup first
> (the [debug build](debug_build.md) serves the whole flash over HTTP), or extract the stock app
> from an official package so you can put it back later ([back to stock](back_to_stock.md)).

## Go / no-go

| Gate | Pass when |
|---|---|
| Before importing the key | `deploy_debug.py status` answers and shows your inverter |
| Before starting the update | `tools/sgu.py verify <pkg>.sgu --sig <pkg>.bin --pubkey <key>.pem` passes every check |
| After the first boot | the dongle is on the network, the web UI answers, the Settings page shows the running slot, and no reset within the first minute |
| Before the first ESPHome OTA | you have a backup of the stock app (flash backup or extracted image) |
| Before enabling inverter writes | the Devices page values agree with the inverter display; the control you want to use is listed for your model in [compatibility](compatibility.md) |

## Build your own package

```sh
make sgu-keys                                   # once: keys/rewired-s_priv.pem (keep it) and keys/rewired-s_pub.pem
make compile                                    # or .venv/bin/esphome compile sites/house.yaml
make sgu APP=.esphome/build/<name>/build/firmware.ota.bin SGU_BOARD=s2 SGU_BUILD=901
#   SGU_STOCK=<official .sgu>   adds the stock web-UI part (only if the app-only package is refused)
```

On Windows run `esphome compile` from **PowerShell** or `cmd`, not from Git Bash or another
MSYS/MinGW shell: ESP-IDF's installer stops with `MSys/Mingw is not supported` there.

`tools/package_sgu.py` checks the image (ESP magic, chip id, 16 MB header, size against the OTA
slot, a version above the installed one), builds the package with the in-file trailer the
WiNet-S2 requires, signs it with your key, and writes `dist/<version>.sgu`, the detached `.bin`
signature, `Version_signature.xml`, your public key and the `.zip` the updater expects. Raise
`SGU_BUILD` for every new package you install: the updater refuses a version that is not above
the running one.

## Updates after the first install

- Pre-built images: Home Assistant shows an **Update** entity for the dongle; the firmware
  downloads the new image from the release page over HTTPS. The update lands in the other slot,
  so it overwrites the stock app (or the Wi-Fi image) that was there. The WiNet-S2's bootloader is
  expected to roll back an update that resets within 30 s; the Ethernet images also boot the
  other slot when the new build never gets a network. The WiNet-S has neither, so keep a serial
  adapter in reach.
- Pre-built images also take an ESPHome OTA (`esphome upload … --device <ip>`) authenticated with
  the web login password, or with the API key after Home Assistant adoption.
- Compiled images: `make deploy NODE=sites/<name> DEVICE=<ip>` runs an encrypted ESPHome OTA,
  streams the logs across the reboot and exits 0 only when the new build is up and marked valid.

Either way the first update overwrites the stock app in the other slot; see
[back to stock](back_to_stock.md) for the ways back after that.
