# Host unit tests for components/sungrow_modbus_tcp. Usage: make -C tests/modbus_tcp [run|clean]
CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra -Werror
TCP := ../../components/sungrow_modbus_tcp
DM := ../../components/sungrow_datamodel
SRCS := $(TCP)/mbtcp_core.cpp \
        $(DM)/decoder.cpp $(DM)/signal_store.cpp $(DM)/request_builder.cpp \
        $(DM)/derived.cpp $(DM)/register_tables.cpp
BUILD := build

all: run

$(BUILD)/test_modbus_tcp: test_modbus_tcp.cpp ../test_harness.h $(SRCS) $(wildcard $(TCP)/mbtcp_core.h) $(wildcard $(DM)/*.h)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(TCP) -I$(DM) -o $@ test_modbus_tcp.cpp $(SRCS)

run: $(BUILD)/test_modbus_tcp
	./$(BUILD)/test_modbus_tcp

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
