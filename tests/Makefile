# Host unit tests for the sungrow_datamodel core. Usage: make -C tests [run|web|clean]
#   web: the SPA's typecheck + node unit tests (needs npm ci in components/sungrow_web/web once).
CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra -Werror
COMP := ../components/sungrow_datamodel
SRCS := $(COMP)/decoder.cpp $(COMP)/signal_store.cpp $(COMP)/request_builder.cpp \
        $(COMP)/derived.cpp $(COMP)/register_tables.cpp
BUILD := build
WEB := ../components/sungrow_web/web

all: run

$(BUILD)/test_datamodel: test_datamodel.cpp test_harness.h $(SRCS) $(wildcard $(COMP)/*.h)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(COMP) -o $@ test_datamodel.cpp $(SRCS)

run: $(BUILD)/test_datamodel
	./$(BUILD)/test_datamodel
	python3 test_gen_deterministic.py
	python3 test_gen_overrides.py
	python3 test_gen_installer_settings.py
	python3 test_gen_ha_meta.py
	python3 test_gen_dsp_ranges.py
	python3 test_deadband.py

web:
	cd $(WEB) && npm run --silent typecheck && npm run --silent test

clean:
	rm -rf $(BUILD)

.PHONY: all run web clean
