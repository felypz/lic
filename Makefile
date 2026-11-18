CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L $(shell pkg-config --cflags lua5.4 2>/dev/null || echo "-I/usr/include/lua5.4")
LDFLAGS ?=
LIBS ?= -llua5.4 -lssl -lcrypto

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

SRC_DIR = src
BUILD_DIR = build
CONF_DIR = config

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))
TARGET = lic

.PHONY: all clean run install uninstall dirs

all: dirs $(TARGET)

dirs:
	@mkdir -p $(BUILD_DIR) $(CONF_DIR) $(CONF_DIR)/plugins
	@if [ ! -f $(CONF_DIR)/init.lua ] && [ -f $(SRC_DIR)/init.lua ]; then \
		cp $(SRC_DIR)/init.lua $(CONF_DIR)/init.lua; \
	fi

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

run: all
	./$(TARGET)

install: all
	@echo "Installing $(TARGET) to $(DESTDIR)$(BINDIR)..."
	@mkdir -p $(DESTDIR)$(BINDIR)
	@cp $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	@chmod 755 $(DESTDIR)$(BINDIR)/$(TARGET)

	@# Configuração XDG (~/.config/lic) - Apenas se não existir para não sobrescrever o usuário
	@XDG_CONFIG_HOME="$${XDG_CONFIG_HOME:-$$HOME/.config}"; \
	mkdir -p "$$XDG_CONFIG_HOME/lic/plugins"; \
	if [ ! -f "$$XDG_CONFIG_HOME/lic/init.lua" ]; then \
		cp $(CONF_DIR)/init.lua "$$XDG_CONFIG_HOME/lic/init.lua"; \
		echo "Created default config at $$XDG_CONFIG_HOME/lic/init.lua"; \
	else \
		echo "Existing config found at $$XDG_CONFIG_HOME/lic/init.lua (preserved)"; \
	fi

uninstall:
	@echo "Removing $(DESTDIR)$(BINDIR)/$(TARGET)..."
	@rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	@echo "Note: User configuration in ~/.config/lic was left untouched."

clean:
	@rm -rf $(BUILD_DIR) $(TARGET)
