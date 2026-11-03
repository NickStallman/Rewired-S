# Site configurations

A site file is the configuration of one real dongle: its name, the firmware version its stock
image reported, and any option that differs from the generic entry. Everything else comes from
the entry it includes. Site files are git-ignored (only this README is tracked) because they
describe your installation.

```yaml
# sites/house.yaml
substitutions:
  name: house-inverter
  friendly_name: House Inverter
  cloud_firmware_version: "WINET-SV300.001.03.P024"   # the version the dongle shipped with
  # analytics: "false"                                 # no usage report or automatic diagnostics uploads
packages:
  entry: !include ../winet-s2.yaml
```

ESPHome resolves `!secret`, the partition table and the `components/` path against the
directory of the file it is given, so `sites/` carries two tracked links (`boards`, `components`)
and needs one you create once for your secrets:

```sh
ln -s ../secrets.yaml sites/secrets.yaml
```

Build, validate and deploy it like an entry:

```sh
.venv/bin/esphome config sites/house.yaml
make deploy NODE=sites/house DEVICE=192.0.2.10
```

A site file can also sit on top of a pre-built entry (`../winet-s2-prebuilt.yaml`): the runtime
provisioning stays (captive portal, Settings-page login and broker, detected cloud version) and
the substitutions only change its defaults, such as `ap_password` for the setup access point or a
fixed `cloud_firmware_version`. `tests/config/prebuilt-site-example.yaml` is a worked example;
[../docs/prebuilt.md](../docs/prebuilt.md) describes the pre-built images.
