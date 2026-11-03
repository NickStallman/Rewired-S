# Host unit tests for components/sungrow_capture. Usage: make -C tests/capture [run|clean]
CXX ?= g++
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra -Werror
CAP := ../../components/sungrow_capture
MB := ../../components/sungrow_modbus
DM := ../../components/sungrow_datamodel
SRCS := $(CAP)/capture_core.cpp \
        $(MB)/modbus_rtu.cpp $(MB)/discovery.cpp $(MB)/master.cpp $(MB)/poll_plan.cpp \
        $(DM)/decoder.cpp $(DM)/signal_store.cpp $(DM)/request_builder.cpp \
        $(DM)/derived.cpp $(DM)/register_tables.cpp
BUILD := build

all: run

$(BUILD)/test_capture: test_capture.cpp ../test_harness.h $(SRCS) $(wildcard $(CAP)/*.h) $(wildcard $(MB)/*.h) $(wildcard $(DM)/*.h)
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(CAP) -I$(MB) -I$(DM) -o $@ test_capture.cpp $(SRCS)

run: $(BUILD)/test_capture
	./$(BUILD)/test_capture

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
