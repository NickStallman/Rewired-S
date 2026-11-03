# Host tests for sungrow_extflash (sigdata parser) and tools/read_identity.py.
# Usage: make -C tests/extflash
CXX ?= g++
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
COMP := ../../components/sungrow_extflash
BUILD := build

.PHONY: all test clean
all: test

$(BUILD)/test_sigdata: test_sigdata.cpp $(COMP)/sigdata.cpp $(COMP)/sigdata.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(COMP) -o $@ test_sigdata.cpp $(COMP)/sigdata.cpp

test: $(BUILD)/test_sigdata
	@mkdir -p $(BUILD)/tmp
	./$(BUILD)/test_sigdata $(BUILD)/tmp
	python3 -I test_read_identity.py

clean:
	rm -rf $(BUILD)
