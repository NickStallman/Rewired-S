# Installing the tools

The install tools are a Python package. It puts six commands on your path; installing a
pre-built image needs the first two:

| Command | What it does |
|---|---|
| `rewired-s-deploy` | talks to the **stock** web UI: `status`, `import-key`, `upload`, `all` |
| `rewired-s-iconfig-server` | serves a package to the dongle on the LAN (the pull route) |
| `rewired-s-sgu` | inspects, extracts, verifies, builds and signs `.sgu` packages (`info`, `extract`, `verify`, `build`, `sign`, `keygen`) |
| `rewired-s-package-sgu` | packages your own compiled image |
| `rewired-s-flash-backup` | downloads and verifies the flash backup the [debug build](debug_build.md) serves |
| `rewired-s-read-identity` | decodes a dumped identity file ([cloud identity](cloud_identity.md)) |

They need Python **3.11 or newer**; the package brings its dependencies (`cryptography`) with
it. No clone, no Docker, no ESPHome, no compiler. ESPHome is only needed to compile your own
image ([ESPHome](esphome.md)).

## Install with pipx

[pipx](https://pipx.pypa.io/) installs the package into its own environment and puts the
commands on your path. The same line works on every platform:

```sh
pipx install "git+https://github.com/NickStallman/Rewired-S@v0.9.1#subdirectory=tools"
```

`pipx install` from a `git+` URL needs `git`. Without it, use the wheel (below).

### Windows

1. Install Python from <https://www.python.org/downloads/windows/> (tick **Add python.exe to
   PATH**) and `git` from <https://git-scm.com/download/win>.
2. In **PowerShell** or `cmd`: `py -m pip install --user pipx`, then `py -m pipx ensurepath`, and
   open a new window.
3. `pipx install "git+https://github.com/NickStallman/Rewired-S@v0.9.1#subdirectory=tools"`.
4. The first run of `rewired-s-iconfig-server` asks Windows Defender Firewall whether Python may
   accept connections: allow it on **private** networks, or the dongle cannot reach port 20001.
   Open the dongle's web UI in a browser **on this computer**: the dongle contacts the address of
   the browser that is logged in (check it with `ipconfig` if the dongle reports it cannot reach
   the server).
5. Serial flashing (WiNet-S only): `pipx install esptool`, install the USB-serial driver your
   adapter needs (CP210x, CH340 or FTDI), find the port in Device Manager (`COM3` and so on) and
   use it in place of `/dev/ttyUSB0` in [install over serial](install_serial.md).

### macOS

1. `brew install pipx` and `pipx ensurepath` (Homebrew), or Python from
   <https://www.python.org/downloads/macos/> and `python3 -m pip install --user pipx`.
2. `pipx install "git+https://github.com/NickStallman/Rewired-S@v0.9.1#subdirectory=tools"`.
3. macOS asks once whether Python may accept incoming connections; allow it. Your address is
   under *System Settings → Network* or `ipconfig getifaddr en0` (Wi-Fi) / `en1`.
4. Serial flashing: `pipx install esptool`; the adapter shows up as `/dev/cu.usbserial-XXXX` or
   `/dev/cu.SLAB_USBtoUART` (`ls /dev/cu.*`). CH340 adapters need the vendor driver on older
   macOS versions.

### Linux

1. `sudo apt install pipx git` (Debian, Ubuntu) or your distribution's equivalent, then
   `pipx ensurepath`.
2. `pipx install "git+https://github.com/NickStallman/Rewired-S@v0.9.1#subdirectory=tools"`.
3. If a firewall is active (`ufw`, `firewalld`), open TCP 20001 for the LAN. Your address:
   `ip -4 addr`.
4. Serial flashing: `pipx install esptool`; the adapter is `/dev/ttyUSB0` or `/dev/ttyACM0`. Add
   yourself to the `dialout` (Debian, Ubuntu) or `uucp` (Arch) group, or run with `sudo`.

## Without pipx: a virtual environment or the wheel

A plain virtual environment works the same way; the commands are then under `.venv/bin/`
(`.venv\Scripts\` on Windows) or on the path while the environment is active:

```sh
python3 -m venv .venv && .venv/bin/pip install "git+https://github.com/NickStallman/Rewired-S@v0.9.1#subdirectory=tools"
.venv/bin/rewired-s-deploy status --host 192.0.2.50
```

Without `git`, install the wheel from the
[release page](https://github.com/NickStallman/Rewired-S/releases):
`pip install <URL of rewired_s_tools-0.9.1-py3-none-any.whl>` (or `pipx install` with the same
URL).

## In a repository checkout

Developers run the same tools straight from the checkout as `python3 -I tools/<name>.py` after
`pip install -r tools/requirements.txt` (the `-I` keeps the current folder out of the import
path); the `Makefile` uses that form. `rewired-s-deploy` is `tools/deploy_debug.py`,
`rewired-s-iconfig-server` is `tools/fake_iconfig_server.py`, and the others match their names
(`tools/sgu.py`, `tools/package_sgu.py`, `tools/flash_backup.py`, `tools/read_identity.py`).

## Home Assistant users without a computer at hand

The tools run anywhere Python runs, including a Raspberry Pi or the **Terminal & SSH** add-on's
shell on a Home Assistant OS box (`apk add python3 py3-pip git`, then the virtual environment
above). Port 20001 must be reachable from the dongle, which is the case when the add-on runs on
the same network.

## Common problems

| Message | Meaning |
|---|---|
| `rewired-s-deploy: command not found` (or not recognised) | `pipx ensurepath` was not run, or the terminal was opened before it; open a new one |
| `ModuleNotFoundError: No module named 'cryptography'` | the tools were run from a repo checkout without `pip install -r tools/requirements.txt`; with pipx this cannot happen |
| `rewired-s-deploy: connection refused` / timeout | wrong address, or the dongle is on another network; the stock web UI must open in a browser at `https://<address>/` |
| `login failed` | the installer password; the stock default is in the dongle's manual, `--user` changes the account |
| The dongle says it found no update | the web UI was opened from another computer than the one running the server, the firewall blocked 20001, `--dist` does not contain the `.zip`, or the **device**-update dialog was used (the server logs `refused: dev_model=…`) |
| "iConfig tool not detected" | the server is not reachable from the dongle |
| `import-key` answers `result_code=120` | the same key version is already stored on the dongle's external flash (it survives stock updates); the tool continues and the install can go ahead |
| `MSys/Mingw is not supported` from `esphome compile` | run it from PowerShell or `cmd`, not Git Bash (Windows) |
| The dongle reports error 4104 or a signature failure | 4104: package without the signed trailer (rebuild with `rewired-s-package-sgu`); a signature failure: key not imported or a different key |
| `permission denied: /dev/ttyUSB0` | add your user to the `dialout` group (Linux) |
