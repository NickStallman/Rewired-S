# Host tests for the runtime-settings headers of the pre-built images (docs/prebuilt.md):
# components/sungrow_web/login_prefs.h, components/sungrow_mqtt/mqtt_settings.h and
# components/sungrow_board/cloud_version.h. Usage: make -C tests/settings
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
WEB := ../../components/sungrow_web
MQTT := ../../components/sungrow_mqtt
BOARD := ../../components/sungrow_board
BUILD := build

.PHONY: all test clean
all: test

$(BUILD)/test_settings: test_settings.cpp ../test_harness.h $(WEB)/login_prefs.h $(MQTT)/mqtt_settings.h $(BOARD)/cloud_version.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(WEB) -I$(MQTT) -I$(BOARD) -o $@ test_settings.cpp

test: $(BUILD)/test_settings
	./$(BUILD)/test_settings

clean:
	rm -rf $(BUILD)
