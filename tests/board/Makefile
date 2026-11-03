# Host tests for components/sungrow_board: board_logic.h (button decoder + LED patterns),
# diag_journal.h (reset journal, log tail, verdicts) and boot_slot.h ("Boot other slot" guard).
# Usage: make -C tests/board
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
COMP := ../../components/sungrow_board
BUILD := build

.PHONY: all test clean
all: test

$(BUILD)/test_board_logic: test_board_logic.cpp $(COMP)/board_logic.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(COMP) -o $@ test_board_logic.cpp

$(BUILD)/test_diag_journal: test_diag_journal.cpp $(COMP)/diag_journal.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(COMP) -o $@ test_diag_journal.cpp

$(BUILD)/test_boot_slot: test_boot_slot.cpp $(COMP)/boot_slot.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(COMP) -o $@ test_boot_slot.cpp

test: $(BUILD)/test_board_logic $(BUILD)/test_diag_journal $(BUILD)/test_boot_slot
	./$(BUILD)/test_board_logic
	./$(BUILD)/test_diag_journal
	./$(BUILD)/test_boot_slot

clean:
	rm -rf $(BUILD)
