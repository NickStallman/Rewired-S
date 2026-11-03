# Contributing

Thanks for helping. Rewired-S is alpha software maintained in spare time; the most valuable
contributions right now are **reports from hardware the project has not seen** and fixes that
come with a test.

## Reporting

- A device that is not on the [compatibility chart](docs/compatibility.md): press **Send
  diagnostics** on the Settings page and open a *new device report* with the reference it shows.
  The report carries everything needed to add the device and has serial numbers removed.
- A bug: the *bug report* template asks for the version, the dongle, the devices and the
  diagnostics reference. Log lines from the Logs page help.
- Questions go to [Discussions](https://github.com/NickStallman/Rewired-S/discussions).

## Rules for what goes into the repository

1. **No Sungrow files, images or packages.** No `.sgu`, no extracted app or web-UI images, no
   grid-code XMLs, label files or parameter tables copied from a dongle, no firmware dumps. The
   tools in `tools/` read such files when *you* provide them; the repository carries only the
   project's own data under `data/` ([data format](docs/data_format.md)).
2. **No decompiled-function names, firmware addresses or disassembly.** Protocol and hardware
   facts are cited to [SG-Docs](https://github.com/NickStallman/SG-Docs), to Sungrow's public
   Modbus documents, to another open project (mkaiser, SunGather) or to what was observed on the
   bus. `tools/hygiene_check.sh` runs in CI and fails on the obvious patterns.
3. **No serial numbers, plant names or addresses**, yours or anyone else's, in code, data, docs,
   dashboards, screenshots or tests. Use the documented placeholders.
4. **Generated files are never edited by hand.** `components/**/*.gen.*`,
   `register_tables.*`, `register_meta.py`, `mqtt_point_table.*`, `ha_meta_gen.*`, `devices/*.yaml`
   and `web/src/*.gen.ts` come from `make gen`; `make check-gen` fails when they are stale.
5. Keep the safety model: a new write path goes through `sungrow_control` and the allow-list,
   never straight to the bus.

## Development

```sh
python3 -m venv .venv && .venv/bin/pip install esphome -r tools/requirements.txt
cp secrets.yaml.example secrets.yaml
make                      # host suites + web suite + generated files check
make config               # every entry validates
make compile              # every entry builds
make hygiene              # the rules above
```

Host suites run without ESPHome or hardware (g++, python3, zlib; the isolarcloud suites need the
ArduinoJson sources ESPHome downloads, or `ARDUINOJSON_DIR=`). The web UI lives in
`components/sungrow_web/web` (`npm ci` once; `make web-assets` rebuilds the embedded bundle).

### Adding a device family

1. Add `data/families/<family>.yaml` with the signals the device serves (addresses, types,
   scales, names, Home Assistant classes) and, for a device that is also served on port 502, a
   `data/modbus_tcp/` map. The format and a worked example are in [docs/data_format.md](docs/data_format.md).
2. Run `make gen`, then `make`.
3. Add the model to `docs/compatibility.md` with the status you can vouch for, and a line to
   `CHANGELOG.md` under `[Unreleased]`.

### Pull requests

One topic per PR, a sentence or two on what and why, how it was tested (which dongle, which
inverter, or "host tests only"), and a changelog line. CI runs the hygiene gate, the host and
web suites, the generated-file check, config validation of every entry and a compile of each
entry.

## Releases

Maintainer notes. Versions follow semantic versioning and stay `0.x` while alpha.

1. Move the `[Unreleased]` section of `CHANGELOG.md` under a new version heading with the date;
   bump `project_version` in `common/base.yaml` and the version in
   `components/sungrow_web/web/package.json` (`tests/test_version.py` keeps them in step); add
   the version to `tools/sgu_build_number.py`.
2. `make && make config && make compile`.
3. Tag `vX.Y.Z` and push the tag. The release workflow builds the pre-built and debug images,
   signs the app-only packages with the release key (`SGU_SIGNING_KEY` secret; public key in
   `tools/release_key_pub.pem`), publishes a pre-release with the changelog section as notes, and
   updates the GitHub Pages site with the manifests the update entity and ESP Web Tools use.
