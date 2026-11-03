# Host unit tests for components/sungrow_control. Usage: make -C tests/control [run|clean]
CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra -Werror
CTL := ../../components/sungrow_control
MB := ../../components/sungrow_modbus
TCP := ../../components/sungrow_modbus_tcp
DM := ../../components/sungrow_datamodel
SRCS := $(CTL)/control_core.cpp $(CTL)/write_registers.cpp $(CTL)/setup_profile.cpp \
        $(CTL)/grid_profiles.gen.cpp $(TCP)/mbtcp_core.cpp \
        $(MB)/modbus_rtu.cpp $(MB)/discovery.cpp $(MB)/master.cpp $(MB)/poll_plan.cpp \
        $(DM)/decoder.cpp $(DM)/signal_store.cpp $(DM)/request_builder.cpp \
        $(DM)/derived.cpp $(DM)/register_tables.cpp
BUILD := build

all: run

$(BUILD)/test_control: test_control.cpp ../test_harness.h $(SRCS) $(wildcard $(CTL)/*.h) $(wildcard $(MB)/*.h) $(TCP)/mbtcp_core.h $(wildcard $(DM)/*.h)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(CTL) -I$(MB) -I$(TCP) -I$(DM) -o $@ test_control.cpp $(SRCS)

run: $(BUILD)/test_control
	./$(BUILD)/test_control
	python3 -I ../../tools/gen_write_registers.py --check
	python3 -I ../../tools/gen_dsp_ranges.py --check

clean:
	rm -rf $(BUILD)

.PHONY: all run clean

# Setup profile resolver over the generated stock profiles (pure; no ControlCore).
SETUP_SRCS := $(CTL)/setup_profile.cpp $(CTL)/grid_profiles.gen.cpp

$(BUILD)/test_setup_profile: test_setup_profile.cpp ../test_harness.h $(SETUP_SRCS) $(CTL)/setup_profile.h $(CTL)/grid_profiles.gen.h \
                             $(CTL)/installer_allow.gen.h $(CTL)/installer_rules.gen.h $(MB)/discovery.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(CTL) -o $@ test_setup_profile.cpp $(SETUP_SRCS)

setup_profile: $(BUILD)/test_setup_profile
	./$(BUILD)/test_setup_profile
	python3 -I ../../tools/gen_grid_profiles.py --check
	python3 -I ../../tools/gen_installer_settings.py --check

run: setup_profile

.PHONY: setup_profile
