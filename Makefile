CC := gcc

LUA_PKG := $(shell pkg-config --exists lua5.4 && echo lua5.4 || \
                   (pkg-config --exists lua-5.4 && echo lua-5.4 || echo lua))

CFLAGS  := -std=c11 -Wall -Wextra -Wpedantic -O2 -g \
           -D_POSIX_C_SOURCE=200809L \
           $(shell pkg-config --cflags $(LUA_PKG)) \
           $(shell pkg-config --cflags openssl)

LDFLAGS := $(shell pkg-config --libs $(LUA_PKG)) \
           $(shell pkg-config --libs openssl)

SRC_DIR   := src
BUILD_DIR := build
CONF_DIR  := config
TARGET    := lic

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))

.PHONY: all clean dirs run

all: dirs $(TARGET)

dirs:
	@mkdir -p $(BUILD_DIR) $(CONF_DIR)
	@if [ ! -f $(CONF_DIR)/init.lua ] && [ -f $(SRC_DIR)/init.lua ]; then \
		cp $(SRC_DIR)/init.lua $(CONF_DIR)/init.lua; \
	fi

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: all
	./$(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
