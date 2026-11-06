# Site configurations

A site file is the configuration of one real dongle in a repository checkout: its name, the
firmware version its stock image reported, and any option that differs from the generic entry.
Everything else comes from the entry it includes. Site files are git-ignored (only this README is
tracked) because they describe your installation. Without a checkout, the same file pulls the
entry from GitHub instead ([ESPHome](../docs/esphome.md#your-configuration)).

```yaml
# sites/house.yaml
substitutions:
  name: house-inverter
  friendly_name: House Inverter
  cloud_firmware_version: "WINET-SV300.001.03.P032"   # the version the dongle shipped with
  # analytics: "false"                                 # no usage report or automatic diagnostics uploads
packages:
  entry: !include ../winet-s2.yaml
```

The entries find their components and partition tables next to themselves. `!secret` is read
from the directory of the file you compile, so link the repository's secrets once:
`ln -s ../secrets.yaml sites/secrets.yaml` (git-ignored).

Build, validate and deploy it like an entry:

```sh
.venv/bin/esphome config sites/house.yaml
make deploy NODE=sites/house DEVICE=192.0.2.10
```

On a compiled Ethernet entry, add `fallback: !include ../common/ethernet_fallback.yaml` to the
site's `packages:` to turn on the network fallback
([ESPHome](../docs/esphome.md#options-worth-knowing)).

A site file can also sit on top of a pre-built entry (`../winet-s2-prebuilt.yaml`): the runtime
provisioning stays (captive portal, Settings-page login and broker, detected cloud version) and
the substitutions only change its defaults, such as `ap_password` for the setup access point or a
fixed `cloud_firmware_version`. `tests/config/prebuilt-site-example.yaml` is a worked example;
[../docs/prebuilt.md](../docs/prebuilt.md) describes the pre-built images.
