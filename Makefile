#=============================================================================#
# Project
#=============================================================================#

PROJECT_NAME := mka-core-audio
APP := app

#=============================================================================#
# Tools & Compiler
#=============================================================================#

CPP_COMPILER := g++
DEBUGGER := gdb

CPP_VERSION := -std=c++26
DBG_FLAGS := -g -Og -fdiagnostics-all-candidates
CPP_FLAGS := -Wall -Wextra -Werror -Wpedantic -Werror=unused-result
PW_FLAGS := $(shell pkg-config --cflags --libs libpipewire-0.3)

LIBS := $(PW_FLAGS) -lasound

#=============================================================================#
# Directories
#=============================================================================#

BUILD_DIR := build
SRC_DIR := src
TEST_DIR := test
OBJ_DIR := $(BUILD_DIR)/obj
INCLUDE_DIRS := $(addprefix -I,$(shell find $(SRC_DIR) -type d))

EXAMPLE_DIR := example
EXAMPLE_BUILD_DIR := $(BUILD_DIR)/examples

#=============================================================================#
# Sources
#=============================================================================#

MODULE_SRCS := \
	$(SRC_DIR)/abstract_backend.cppm  \
    $(SRC_DIR)/impl/alsa_backend.cppm
#   $(SRC_DIR)/utils/block.cppm \
#   $(SRC_DIR)/utils/routing_table.cppm \
#   $(SRC_DIR)/utils/audio_engine.cppm
#	$(SRC_DIR)/impl/alsa_impl.cppm

MODULE_OBJS := $(patsubst $(SRC_DIR)/%.cppm,$(OBJ_DIR)/%.o,$(MODULE_SRCS))

MAIN_SRC := $(SRC_DIR)/main.cpp

EXAMPLE_SRCS := $(wildcard $(EXAMPLE_DIR)/*.cpp)
EXAMPLE_BINS := $(patsubst $(EXAMPLE_DIR)/%.cpp,$(EXAMPLE_BUILD_DIR)/%,$(EXAMPLE_SRCS))

#=============================================================================#
# Build targets
#=============================================================================#

all: $(APP)

# Compile all modules
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cppm
	@mkdir -p $(dir $@)
	$(CPP_COMPILER) $(CPP_VERSION) $(LIBS) $(DBG_FLAGS) -fmodules -c $< -o $@

# Link main app
$(APP): $(MODULE_OBJS) $(MAIN_SRC)
	$(CPP_COMPILER) $(CPP_VERSION) $(DBG_FLAGS) -fmodules $(MAIN_SRC) $(MODULE_OBJS) $(LIBS) -o $(BUILD_DIR)/$@

run:
	./$(BUILD_DIR)/$(APP)

test:
	$(CPP_COMPILER) $(TEST_DIR)/testALSA.cpp $(CPP_VERSION) -lgtest -lgtest_main -pthread -o testALSA -fmodules $(MODULE_OBJS) $(LIBS)
	./testALSA$(LIBS)
#=============================================================================#
# Utilities
#=============================================================================#

$(BUILD_DIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)/* gcm.cache

.PHONY: all clean run test