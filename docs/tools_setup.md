# Running the tools on Windows, macOS and Linux

The tools in `tools/` are plain Python 3 scripts. The ones you need to install a pre-built
image are:

| Tool | What it does |
|---|---|
| `tools/deploy_debug.py` | talks to the **stock** web UI: `status`, `import-key`, `upload` |
| `tools/fake_iconfig_server.py` | serves a package to the dongle on the LAN (the pull route) |
| `tools/sgu.py` | inspects, extracts, verifies and builds `.sgu` packages |
| `tools/package_sgu.py` | packages your own compiled image |

They need Python **3.11 or newer** and the two packages in `tools/requirements.txt`
(`cryptography`, `pyyaml`). Nothing else: no Docker, no ESPHome, no compiler. ESPHome is only
needed to compile your own image ([configuration](configuration.md)).

> [!TIP]
> Every command below is run from the repository directory (the folder that contains `tools/`).
> Get it with `git clone https://github.com/NickStallman/Rewired-S.git` or the **Code → Download
> ZIP** button on GitHub, then open a terminal in that folder.

## Windows

1. Install Python from <https://www.python.org/downloads/windows/> (tick **Add python.exe to
   PATH**) or from the Microsoft Store. The `py` launcher comes with the python.org installer.
2. Open **PowerShell** in the repository folder (Shift + right-click → *Open PowerShell window
   here*, or `cd C:\Users\you\Rewired-S`).
3. Create a virtual environment and install the requirements:

   ```powershell
   py -3 -m venv .venv
   .\.venv\Scripts\Activate.ps1          # if this is refused: Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
   pip install -r tools\requirements.txt
   ```

4. Run the tools with `python -I` (the `-I` keeps the current folder out of the import path):

   ```powershell
   python -I tools\deploy_debug.py status --host 192.168.1.50
   python -I tools\deploy_debug.py import-key --host 192.168.1.50 --pubkey rewired-s-release-public-key.pem
   python -I tools\fake_iconfig_server.py --dist release
   ```

   The first run of the package server asks Windows Defender Firewall whether Python may accept
   connections: allow it on **private** networks, or the dongle cannot reach port 20001. Find
   your computer's address with `ipconfig` (the IPv4 address of the adapter on the same network
   as the dongle); that is the address you enter in the dongle's web UI.

5. Serial flashing (WiNet-S only): `pip install esptool`, install the USB-serial driver your
   adapter needs (CP210x, CH340 or FTDI), find the port in Device Manager (`COM3` and so on) and
   use it in place of `/dev/ttyUSB0` in [install over serial](install_serial.md):

   ```powershell
   esptool --chip esp32 -p COM3 -b 460800 read_flash 0 0x1000000 winet-s-full-16MB.bin
   ```

## macOS

1. Install Python 3 from <https://www.python.org/downloads/macos/> or with Homebrew
   (`brew install python`). The system `python3` on recent macOS also works if it is 3.11+.
2. Open **Terminal** in the repository folder (`cd ~/Rewired-S`).
3. Virtual environment and requirements:

   ```sh
   python3 -m venv .venv
   source .venv/bin/activate
   pip install -r tools/requirements.txt
   ```

4. Run the tools with `python3 -I`:

   ```sh
   python3 -I tools/deploy_debug.py status --host 192.168.1.50
   python3 -I tools/deploy_debug.py import-key --host 192.168.1.50 --pubkey rewired-s-release-public-key.pem
   python3 -I tools/fake_iconfig_server.py --dist release
   ```

   macOS asks once whether Python may accept incoming connections; allow it. Your address is
   under *System Settings → Network* or `ipconfig getifaddr en0` (Wi-Fi) / `en1`.

5. Serial flashing: `pip install esptool`; the adapter shows up as `/dev/cu.usbserial-XXXX` or
   `/dev/cu.SLAB_USBtoUART` (`ls /dev/cu.*`). CH340 adapters need the vendor driver on older
   macOS versions.

## Linux

1. Python 3 is normally installed; otherwise `sudo apt install python3 python3-venv` (Debian,
   Ubuntu) or your distribution's equivalent.
2. In the repository folder:

   ```sh
   python3 -m venv .venv
   source .venv/bin/activate
   pip install -r tools/requirements.txt
   python3 -I tools/deploy_debug.py status --host 192.168.1.50
   python3 -I tools/deploy_debug.py import-key --host 192.168.1.50 --pubkey rewired-s-release-public-key.pem
   python3 -I tools/fake_iconfig_server.py --dist release
   ```

   If a firewall is active (`ufw`, `firewalld`), open TCP 20001 for the LAN. Your address:
   `ip -4 addr`.

3. Serial flashing: `pip install esptool`; the adapter is `/dev/ttyUSB0` or `/dev/ttyACM0`. Add
   yourself to the `dialout` (Debian, Ubuntu) or `uucp` (Arch) group, or run with `sudo`.

## Home Assistant users without a computer at hand

The tools run anywhere Python runs, including a Raspberry Pi or the **Terminal & SSH** add-on's
shell on a Home Assistant OS box (`apk add python3 py3-pip git`, then the Linux steps in a
`venv`). Port 20001 must be reachable from the dongle, which is the case when the add-on runs on
the same network.

## Common problems

| Message | Meaning |
|---|---|
| `ModuleNotFoundError: No module named 'cryptography'` | the virtual environment is not active, or `pip install -r tools/requirements.txt` was skipped |
| `deploy_debug.py: connection refused` / timeout | wrong address, or the dongle is on another network; the stock web UI must open in a browser at `https://<address>/` |
| `login failed` | the installer password; the stock default is in the dongle's manual, `--user` changes the account |
| The dongle says it found no update | the computer's address was mistyped, the firewall blocked 20001, or `--dist` does not contain the `.zip` |
| The dongle reports an error 4104 or a signature failure | the public key was not imported, or the package was built with a different key |
| `permission denied: /dev/ttyUSB0` | add your user to the `dialout` group (Linux) |

On Windows, paths use backslashes (`tools\sgu.py`) and the interpreter is `python`; on macOS
and Linux it is `python3` with forward slashes. Everything else is the same.
