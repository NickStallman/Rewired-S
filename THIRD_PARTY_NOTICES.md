# Third-party material

## ESPHome and ESP-IDF

Rewired-S is a set of external components for [ESPHome](https://esphome.io) and is compiled
against the ESPHome runtime and Espressif's ESP-IDF at build time, each under its own licence
(ESPHome: MIT for the Python side and GPL-3.0 for the C++ runtime; ESP-IDF: Apache-2.0). The
pre-built images contain both.

## ArduinoJson

The `isolarcloud` component uses [ArduinoJson](https://arduinojson.org) (MIT), which ESPHome
downloads at build time; the host test suites compile against the same sources.

## Device tables

The register maps, bit-field definitions, cloud point tables, grid-code profiles and installer
rules under `data/` are the project's own files in the project's own format, written from the
behaviour documented in [SG-Docs](https://github.com/NickStallman/SG-Docs) (CC BY 4.0), from
Sungrow's public Modbus register documents and from observation of the devices on the bus. They
describe register addresses, scales and names, which are facts about the devices' interfaces.
No file originating from Sungrow is redistributed here.

## Trademarks

Sungrow, iSolarCloud, WiNet, SH10RS, SG5.0RS and SBR are trademarks or product names of Sungrow
Power Supply Co., Ltd. This project is not affiliated with or endorsed by Sungrow.
