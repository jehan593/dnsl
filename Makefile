APP := dnsl
SRC_DIR := src
BUILD_DIR := build
PREFIX ?= /usr/local

PKGS := gtk+-3.0 glib-2.0 gio-2.0 gio-unix-2.0 ayatana-appindicator3-0.1 openssl uuid fontconfig
CFLAGS += -Wall -Wextra -std=gnu11 -O2 $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs $(PKGS)) -lm

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean install uninstall run icons

all: $(APP)

$(APP): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Regenerates data/icons/dnsl-{enabled,disabled}-*.png — only needed after touching
# tools/generate_icon.c's palette/motif, not part of the normal app build.
icons: $(BUILD_DIR)/generate_icon
	$(BUILD_DIR)/generate_icon data/icons

$(BUILD_DIR)/generate_icon: tools/generate_icon.c | $(BUILD_DIR)
	$(CC) -Wall -Wextra -O2 $(shell pkg-config --cflags cairo) $< -o $@ $(shell pkg-config --libs cairo) -lm

clean:
	rm -rf $(BUILD_DIR) $(APP)

run: all
	./$(APP)

install: all
	./install.sh "$(PREFIX)"

uninstall:
	./install.sh --uninstall "$(PREFIX)"
