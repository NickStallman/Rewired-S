# Host unit tests for components/sungrow_modbus. Usage: make -C tests/modbus [run|clean]
CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra -Werror
MB := ../../components/sungrow_modbus
DM := ../../components/sungrow_datamodel
SRCS := $(MB)/modbus_rtu.cpp $(MB)/discovery.cpp $(MB)/master.cpp $(MB)/poll_plan.cpp \
        $(DM)/decoder.cpp $(DM)/signal_store.cpp $(DM)/request_builder.cpp \
        $(DM)/derived.cpp $(DM)/register_tables.cpp
BUILD := build

all: run

$(BUILD)/test_modbus: test_modbus.cpp ../test_harness.h $(SRCS) $(wildcard $(MB)/*.h) $(wildcard $(DM)/*.h)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(MB) -I$(DM) -o $@ test_modbus.cpp $(SRCS)

run: $(BUILD)/test_modbus
	./$(BUILD)/test_modbus

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
