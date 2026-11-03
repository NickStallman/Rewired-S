# Host unit tests for components/isolarcloud (g++, system zlib, no ESPHome).
#   make -C tests/isolarcloud            build + run (core + bindings)
#   make -C tests/isolarcloud bindings   only the adapter tests (test_bindings.cpp)
#   make -C tests/isolarcloud dump       write sample topics/payloads to build/samples
#   make -C tests/isolarcloud integration  local 127.0.0.1 broker round trip (local_broker_test.py)
# ArduinoJson: the copy ESPHome downloaded (7.4.3, same as the firmware). Override with
#   make ARDUINOJSON_DIR=/path/to/ArduinoJson/src
ROOT      := $(abspath ../..)
COMP      := $(ROOT)/components/isolarcloud
ARDUINOJSON_DIR ?= $(firstword $(wildcard $(ROOT)/.esphome/.espressif/*/bblanchon__arduinojson_7.4.3_*/src) \
                     $(wildcard $(ROOT)/.esphome/build/*/managed_components/bblanchon__arduinojson/src))
CXX       ?= g++
CXXFLAGS  += -std=gnu++20 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-maybe-uninitialized -DISOLARCLOUD_HOST \
             -I$(COMP)/.. -isystem $(ARDUINOJSON_DIR) -fsanitize=address,undefined
LDLIBS    += -lz
SRCS      := $(COMP)/topics.cpp $(COMP)/zcodec.cpp $(COMP)/envelope.cpp $(COMP)/inbound.cpp \
             $(COMP)/rundata_format.cpp $(COMP)/server_switch.cpp $(COMP)/identity.cpp \
             $(COMP)/mqtt_point_table.cpp $(COMP)/cloud_time.cpp $(COMP)/scheduler.cpp \
             $(COMP)/publish_queue.cpp $(COMP)/commands.cpp
BUILD     := build

all: run

$(BUILD)/test_isolarcloud: test_isolarcloud.cpp $(SRCS) $(wildcard $(COMP)/*.h) | $(BUILD)
	@test -n "$(ARDUINOJSON_DIR)" || (echo "ArduinoJson not found: compile the firmware once or set ARDUINOJSON_DIR" && false)
	$(CXX) $(CXXFLAGS) -o $@ test_isolarcloud.cpp $(SRCS) $(LDLIBS)

$(BUILD):
	mkdir -p $@

# Adapters (adapters.cpp) against the host cores of sungrow_extflash / _datamodel / _modbus.
BIND_FLAGS := -DISOLARCLOUD_BIND_EXTFLASH -DISOLARCLOUD_BIND_DATAMODEL -DISOLARCLOUD_BIND_MODBUS
DM        := $(ROOT)/components/sungrow_datamodel
MB        := $(ROOT)/components/sungrow_modbus
BIND_SRCS := $(COMP)/adapters.cpp $(COMP)/identity.cpp $(COMP)/rundata_format.cpp \
             $(COMP)/mqtt_point_table.cpp $(COMP)/cloud_time.cpp \
             $(ROOT)/components/sungrow_extflash/sigdata.cpp \
             $(DM)/decoder.cpp $(DM)/signal_store.cpp $(DM)/request_builder.cpp $(DM)/derived.cpp \
             $(DM)/register_tables.cpp $(MB)/modbus_rtu.cpp $(MB)/discovery.cpp $(MB)/master.cpp $(MB)/poll_plan.cpp

$(BUILD)/test_bindings: test_bindings.cpp ../test_harness.h $(BIND_SRCS) $(wildcard $(COMP)/*.h) | $(BUILD)
	@test -n "$(ARDUINOJSON_DIR)" || (echo "ArduinoJson not found: compile the firmware once or set ARDUINOJSON_DIR" && false)
	$(CXX) $(CXXFLAGS) $(BIND_FLAGS) -o $@ test_bindings.cpp $(BIND_SRCS)

bindings: $(BUILD)/test_bindings
	./$(BUILD)/test_bindings

run: $(BUILD)/test_isolarcloud $(BUILD)/test_bindings
	./$(BUILD)/test_isolarcloud
	./$(BUILD)/test_bindings

dump: $(BUILD)/test_isolarcloud
	./$(BUILD)/test_isolarcloud --dump $(BUILD)/samples

# Local-only broker test (127.0.0.1, pure-Python minimal broker; never contacts Sungrow).
integration: dump
	python3 local_broker_test.py ./$(BUILD)/test_isolarcloud $(BUILD)/samples

# Device zcodec path (miniz tdefl/tinfl, as in ESP ROM) on the host against a standalone miniz.c.
MINIZ_DIR ?=
miniz-check: | $(BUILD)
	@test -n "$(MINIZ_DIR)" || (echo "set MINIZ_DIR to a directory with miniz.c/miniz.h" && false)
	gcc -c -O1 -w -DMINIZ_NO_STDIO -DMINIZ_NO_ARCHIVE_APIS -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES -o $(BUILD)/miniz.o $(MINIZ_DIR)/miniz.c
	$(CXX) -std=gnu++20 -O1 -g -Wall -Wextra -Werror -I$(COMP)/.. -Istub -I$(MINIZ_DIR) -fsanitize=address,undefined \
	  -o $(BUILD)/miniz_check miniz_check.cpp $(COMP)/zcodec.cpp $(BUILD)/miniz.o -lz
	./$(BUILD)/miniz_check

clean:
	rm -rf $(BUILD)

.PHONY: all run bindings dump integration miniz-check clean
