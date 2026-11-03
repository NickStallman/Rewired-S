# Host unit tests for components/sungrow_debug (trace core). Usage: make -C tests/debug [run|clean]
CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra -Werror
DBG := ../../components/sungrow_debug
MB := ../../components/sungrow_modbus
DM := ../../components/sungrow_datamodel
SRCS := $(DBG)/trace_core.cpp $(MB)/modbus_rtu.cpp $(DM)/decoder.cpp
BUILD := build

all: run

$(BUILD)/test_debug: test_debug.cpp ../test_harness.h $(SRCS) $(wildcard $(DBG)/*.h) $(MB)/modbus_rtu.h $(DM)/decoder.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(DBG) -o $@ test_debug.cpp $(SRCS)

$(BUILD)/test_flash_layout: test_flash_layout.cpp ../test_harness.h $(DBG)/flash_layout.cpp $(DBG)/flash_layout.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(DBG) -o $@ test_flash_layout.cpp $(DBG)/flash_layout.cpp

run: $(BUILD)/test_debug $(BUILD)/test_flash_layout
	./$(BUILD)/test_debug
	./$(BUILD)/test_flash_layout

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
