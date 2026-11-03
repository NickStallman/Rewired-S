# Build, generator and test targets (run from new_firmware/).
#
# Tests (host only: g++, python3, node for `web`; no hardware, no network beyond 127.0.0.1)
#   make, make test     check-gen, every host suite below, web
#   make test-host      check-gen and every host suite except web (what CI runs on a plain runner)
#   make datamodel | modbus | modbus_tcp | control | capture | debug | extflash | board |
#        analytics | settings | isolarcloud | isolarcloud-integration | python
#                       one suite each (isolarcloud-integration: round trip against a 127.0.0.1 broker)
#   make web            SPA typecheck + unit tests, then check-web-assets
#                       (needs `npm ci` in components/sungrow_web/web once)
#   The isolarcloud suites use the ArduinoJson copy ESPHome downloads: compile the firmware once
#   first, or pass ARDUINOJSON_DIR=/path/to/ArduinoJson/src.
#
# Generators (need PyYAML: pip install -r tools/requirements.txt; inputs under data/, see
#   docs/data_format.md)
#   make gen            rerun every generator in dependency order
#   make check-gen      exit 1 when a generated file is stale (every generator's --check;
#                       gen_tables.py is checked by tests/test_gen_deterministic.py)
#   make web-assets     rebuild components/sungrow_web/web_assets.h from the SPA sources
#   make check-web-assets  exit 1 when web_assets.h differs from a fresh build of the SPA
#
# Firmware (ESPHome; needs secrets.yaml, copy secrets.yaml.example)
#   make config         esphome config of every entry, prebuilt entry, debug entry and tests/config/*.yaml
#   make config-prebuilt
#                       esphome config of the prebuilt entries with secrets.yaml moved aside: proves
#                       no !secret is reachable from them (docs/prebuilt.md)
#   make compile        build the entries (ENTRIES) and the prebuilt images (PREBUILT_ENTRIES)
#   make compile-debug  build the WiNet-S debug firmware (DEBUG_ENTRY, docs/debug_build.md)
#   make deploy NODE=sites/<name> DEVICE=<ip>
#                       compile, archive the ELF, stream logs across a native OTA and verify the new
#                       build stays up (tools/deploy_node.py; logs + decoded crashes under dist/logs/)
#   make sgu            sign an app image into a .sgu + .zip for the dongle's updater with YOUR key
#                       (tools/package_sgu.py)  APP=<firmware.ota.bin> (default: the winet-<board>-debug build)
#                       SGU_BOARD=s|s2 (default s)  SGU_BUILD=<P-number, default 901>
#                       SGU_STOCK=<official .sgu> (optional: adds its web-UI part)  SGU_KEY=<private key>
#   make sgu-keys       generate the signing key pair under keys/ (git-ignored)
#   make hygiene        the repository hygiene gate CI runs (tools/hygiene_check.sh)
#   make syslog         run the syslog receiver for common/syslog.yaml (UDP 5514 -> dist/logs/<ip>/)
#   make clean          remove the host test build dirs
PYTHON   ?= python3
ESPHOME  ?= .venv/bin/esphome
ENTRIES       := winet-s.yaml winet-s-ethernet.yaml winet-s2.yaml winet-s2-ethernet.yaml
# Pre-built images: no secrets, everything provisioned at runtime (common/provisioning.yaml).
PREBUILT_ENTRIES := winet-s-prebuilt.yaml winet-s-ethernet-prebuilt.yaml winet-s2-prebuilt.yaml winet-s2-ethernet-prebuilt.yaml
DEBUG_ENTRIES := winet-s-debug.yaml winet-s-debug-ethernet.yaml winet-s2-debug.yaml
CONFIG_TESTS  := $(filter-out tests/config/secrets.yaml,$(wildcard tests/config/*.yaml))
DEBUG_ENTRY ?= winet-s-debug.yaml
SGU_BOARD   ?= s
DEBUG_APP   ?= .esphome/build/winet-$(SGU_BOARD)-debug/build/firmware.ota.bin
SGU_BUILD   ?= 901
SGU_OUT     ?= dist
# Python suites not run by a sub-Makefile: tests/Makefile's own python3 lines are read from it,
# tests/extflash runs test_read_identity.py, tests/isolarcloud integration runs local_broker_test.py.
SUB_PY_TESTS := $(addprefix tests/,$(shell sed -n 's/^\tpython3[^ ]* \(-I \)\{0,1\}\(test_[A-Za-z0-9_]*\.py\).*/\2/p' tests/Makefile))
PY_TESTS := $(filter-out $(SUB_PY_TESTS),$(wildcard tests/test_*.py))

.PHONY: all test test-host datamodel modbus modbus_tcp control capture debug extflash board analytics settings \
        isolarcloud isolarcloud-integration python web web-assets check-web-assets gen check-gen config \
        config-prebuilt compile compile-debug sgu sgu-keys hygiene deploy syslog clean
all: test

test: test-host web
	@echo "== all host suites passed"

test-host: check-gen datamodel modbus modbus_tcp control capture debug extflash board analytics settings isolarcloud isolarcloud-integration python
	@echo "== host suites passed"

datamodel:
	@echo "== tests (sungrow_datamodel + generator determinism)"
	$(MAKE) -C tests run

modbus:
	@echo "== tests/modbus"
	$(MAKE) -C tests/modbus run

modbus_tcp:
	@echo "== tests/modbus_tcp (:502 slave core incl. writes)"
	$(MAKE) -C tests/modbus_tcp run

control:
	@echo "== tests/control (write gate, allow-list, read-back, :502 writes end to end)"
	$(MAKE) -C tests/control run

capture:
	@echo "== tests/capture (YMODEM receiver, I-V scan and grid-waveform captures)"
	$(MAKE) -C tests/capture run

debug:
	@echo "== tests/debug (sungrow_debug trace + flash-layout cores)"
	$(MAKE) -C tests/debug run

extflash:
	@echo "== tests/extflash"
	$(MAKE) -C tests/extflash test

board:
	@echo "== tests/board (button + LED logic, reset journal, boot-slot guard)"
	$(MAKE) -C tests/board test

analytics:
	@echo "== tests/analytics (usage batch, throttle, schedule, upload cap, scrubber, report)"
	$(MAKE) -C tests/analytics test

settings:
	@echo "== tests/settings (web login, MQTT settings and cloud version records of the prebuilt images)"
	$(MAKE) -C tests/settings test

isolarcloud:
	@echo "== tests/isolarcloud (core + bindings)"
	$(MAKE) -C tests/isolarcloud run

isolarcloud-integration:
	@echo "== tests/isolarcloud integration (127.0.0.1 broker)"
	$(MAKE) -C tests/isolarcloud integration

python:
	@echo "== python: $(PY_TESTS)"
	@set -e; for t in $(PY_TESTS); do echo "-- $$t"; $(PYTHON) -I $$t; done

web:
	@echo "== web (SPA typecheck + unit tests, embedded bundle)"
	$(MAKE) -C tests web
	$(MAKE) check-web-assets

web-assets:
	$(PYTHON) -I tools/gen_web_assets.py

check-web-assets:
	$(PYTHON) -I tools/gen_web_assets.py --check

gen:
	$(PYTHON) -I tools/gen_tables.py
	$(PYTHON) -I tools/gen_web_meta.py
	$(PYTHON) -I tools/gen_mqtt_table.py
	$(PYTHON) -I tools/gen_write_registers.py
	$(PYTHON) -I tools/gen_grid_profiles.py
	$(PYTHON) -I tools/gen_installer_settings.py
	if test -f tools/gen_dsp_ranges.py; then $(PYTHON) -I tools/gen_dsp_ranges.py; fi
	$(PYTHON) -I components/sungrow_mqtt/ha_meta_gen.py
	$(PYTHON) -I tools/gen_web_assets.py

check-gen:
	@echo "== check-gen (generated files up to date)"
	$(PYTHON) -I tools/gen_web_meta.py --check
	$(PYTHON) -I tools/gen_mqtt_table.py --check
	$(PYTHON) -I tools/gen_write_registers.py --check
	$(PYTHON) -I tools/gen_grid_profiles.py --check
	$(PYTHON) -I tools/gen_installer_settings.py --check
	if test -f tools/gen_dsp_ranges.py; then $(PYTHON) -I tools/gen_dsp_ranges.py --check; fi
	$(PYTHON) -I components/sungrow_mqtt/ha_meta_gen.py --check

config:
	@set -e; for y in $(ENTRIES) $(PREBUILT_ENTRIES) $(DEBUG_ENTRIES) $(CONFIG_TESTS); do echo "== esphome config $$y"; $(ESPHOME) config $$y > /dev/null; done

# The prebuilt entries must validate without any secrets.yaml: it is moved aside for the run and
# put back whatever happens (the sites/ and tests/config/ links to it dangle meanwhile).
config-prebuilt:
	@set -e; moved=; if test -f secrets.yaml; then mv secrets.yaml secrets.yaml.config-prebuilt; moved=1; fi; \
	trap 'if test -n "$$moved"; then mv secrets.yaml.config-prebuilt secrets.yaml; fi' EXIT; \
	for y in $(PREBUILT_ENTRIES); do echo "== esphome config $$y (no secrets.yaml)"; $(ESPHOME) config $$y > /dev/null; done

compile:
	@set -e; for y in $(ENTRIES) $(PREBUILT_ENTRIES); do echo "== esphome compile $$y"; $(ESPHOME) compile $$y; done

compile-debug:
	$(ESPHOME) compile $(DEBUG_ENTRY)

SGU_STOCK ?=
SGU_KEY   ?= keys/rewired-s_priv.pem
APP       ?= $(DEBUG_APP)
sgu:
	@test -f "$(SGU_KEY)" || { echo "no signing key at $(SGU_KEY): run make sgu-keys first"; exit 2; }
	$(PYTHON) -I tools/package_sgu.py --board $(SGU_BOARD) --app $(APP) --build $(SGU_BUILD) --out-dir $(SGU_OUT) --key $(SGU_KEY) $(if $(SGU_STOCK),--stock-sgu $(SGU_STOCK),) --zip

sgu-keys:
	$(PYTHON) -I tools/sgu.py keygen --out-dir keys --name rewired-s

hygiene:
	tools/hygiene_check.sh

NODE   ?=
DEVICE ?=
deploy:
	@test -n "$(NODE)" -a -n "$(DEVICE)" || { echo "usage: make deploy NODE=sites/<name> DEVICE=<ip>   (e.g. NODE=sites/house DEVICE=192.0.2.10)"; exit 2; }
	$(PYTHON) -I tools/deploy_node.py $(NODE).yaml --device $(DEVICE)

syslog:
	$(PYTHON) -I tools/syslog_receiver.py

clean:
	$(MAKE) -C tests clean
	$(MAKE) -C tests/debug clean
	$(MAKE) -C tests/modbus clean
	$(MAKE) -C tests/modbus_tcp clean
	$(MAKE) -C tests/control clean
	$(MAKE) -C tests/capture clean
	$(MAKE) -C tests/extflash clean
	$(MAKE) -C tests/board clean
	$(MAKE) -C tests/analytics clean
	$(MAKE) -C tests/settings clean
	$(MAKE) -C tests/isolarcloud clean
