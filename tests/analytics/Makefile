# Host tests for components/sungrow_analytics: analytics_core.h (usage batch, error throttle,
# ping schedule, upload cap, scrubber, diagnostics report sections).
# Usage: make -C tests/analytics
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
COMP := ../../components/sungrow_analytics
BOARD := ../../components/sungrow_board
BUILD := build

.PHONY: all test clean
all: test

$(BUILD)/test_analytics_core: test_analytics_core.cpp ../test_harness.h $(COMP)/analytics_core.h $(BOARD)/diag_journal.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(COMP) -I$(BOARD) -o $@ test_analytics_core.cpp

test: $(BUILD)/test_analytics_core
	./$(BUILD)/test_analytics_core

clean:
	rm -rf $(BUILD)
