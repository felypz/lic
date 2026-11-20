CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -g
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L $(shell pkg-config --cflags lua5.4 2>/dev/null || echo "-I/usr/include/lua5.4")
LDFLAGS ?=
LIBS ?= -llua5.4 -lssl -lcrypto

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

SRC_DIR = src
BUILD_DIR = build

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS))
TARGET = lic

.PHONY: all clean run install uninstall

all: $(TARGET)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)

run: all
	./$(TARGET)

install: all
	@mkdir -p $(DESTDIR)$(BINDIR)
	@cp $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	@chmod 755 $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	@rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	@rm -rf $(BUILD_DIR) $(TARGET)
