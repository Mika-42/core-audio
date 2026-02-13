PROJECT_NAME := mka-core-audio
APP := app

#=============================================================================#

CPP_COMPILER := g++
DEBUGGER := gdb

SRC_DIR := src
OBJ_DIR := obj
BUILD_DIR := build
INCLUDE_DIRS := $(addprefix -I,$(shell find $(SRC_DIR) -type d))

CPP_VERSION := -std=c++26
DBG_FLAGS := -g -Og
CPP_FLAGS := -Wall -Wextra -Werror -Wpedantic -Werror=unused-result

MODULE_SRCS := \
	$(SRC_DIR)/utils/error.cppm \
    $(SRC_DIR)/utils/config.cppm \
    $(SRC_DIR)/utils/block.cppm \
    $(SRC_DIR)/abstract_core.cppm \
    $(SRC_DIR)/impl/alsa_impl.cppm \
    $(SRC_DIR)/impl/jack_impl.cppm

MODULE_OBJS := $(patsubst $(SRC_DIR)/%.cppm,$(OBJ_DIR)/%.o,$(MODULE_SRCS))

MAIN_SRC := $(SRC_DIR)/main.cpp

LIBS := -lasound -ljack

#=============================================================================#
# Build target
all: $(APP)

# Compile all modules
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cppm
	@mkdir -p $(dir $@)
	$(CPP_COMPILER) $(CPP_VERSION) $(DBG_FLAGS) -fmodules -c $< -o $@

# Link everything
$(APP): $(MODULE_OBJS) $(MAIN_SRC)
	$(CPP_COMPILER) $(CPP_VERSION) $(DBG_FLAGS) -fmodules $(MAIN_SRC) $(MODULE_OBJS) $(LIBS) -o $@

#=============================================================================#
# Utilities

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(OBJ_DIR) $(APP)

.PHONY: all clean
