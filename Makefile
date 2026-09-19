APP := dnsl
SRC_DIR := src
BUILD_DIR := build
PREFIX ?= /usr/local

PKGS := gtk+-3.0 glib-2.0 gio-2.0 gio-unix-2.0 ayatana-appindicator3-0.1 openssl uuid fontconfig libnetfilter_conntrack
CFLAGS += -Wall -Wextra -std=gnu11 -O2 $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs $(PKGS)) -lm

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

.PHONY: all clean install uninstall run icons test test-netns

all: $(APP)

test: $(BUILD_DIR)/resolved_snapshot_test $(BUILD_DIR)/proxy_harness $(BUILD_DIR)/controller_test $(BUILD_DIR)/dot_pool_test
	./$(BUILD_DIR)/resolved_snapshot_test
	./$(BUILD_DIR)/controller_test
	python3 tests/interception_test.py
	python3 tests/dot_pool_test.py

$(BUILD_DIR)/resolved_snapshot_test: tests/resolved_snapshot_test.c $(SRC_DIR)/resolved_ctl.c $(SRC_DIR)/resolved_ctl.h | $(BUILD_DIR)
	$(CC) -Wall -Wextra -Werror -std=gnu11 -g $(shell pkg-config --cflags gio-2.0) $< -o $@ $(shell pkg-config --libs gio-2.0)

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

$(BUILD_DIR)/proxy_harness: tests/proxy_harness.c tests/fake_dot_pool.c src/dns_proxy.c src/intercept_ctl.c | $(BUILD_DIR)
	$(CC) -Wall -Wextra -Werror -std=gnu11 -g -Isrc $(shell pkg-config --cflags gio-2.0 libnetfilter_conntrack) $^ -o $@ $(shell pkg-config --libs gio-2.0 libnetfilter_conntrack)

test-netns: $(APP) $(BUILD_DIR)/proxy_harness $(BUILD_DIR)/lifecycle_harness
	DNSL_TEST_PARENT_NET=$$(readlink /proc/self/ns/net) unshare --user --map-root-user --net python3 tests/interception_test.py --netns

$(BUILD_DIR)/lifecycle_harness: tests/lifecycle_harness.c tests/fake_dot_pool.c src/protection_controller.c src/dns_proxy.c src/intercept_ctl.c src/dns_provider.c src/ipc_protocol.c src/json_min.c src/ipc_server.c src/settings_store.c src/resolved_ctl.c | $(BUILD_DIR)
	$(CC) -Wall -Wextra -Werror -std=gnu11 -g -Isrc $(shell pkg-config --cflags gio-2.0 uuid libnetfilter_conntrack) $(filter-out src/ipc_server.c src/settings_store.c,$^) -o $@ $(shell pkg-config --libs gio-2.0 uuid libnetfilter_conntrack)

$(BUILD_DIR)/controller_test: tests/controller_test.c src/dns_provider.c src/json_min.c src/protection_controller.c src/settings_store.c | $(BUILD_DIR)
	$(CC) -Wall -Wextra -Werror -std=gnu11 -g -Isrc $(shell pkg-config --cflags gio-2.0 uuid) $(filter-out src/protection_controller.c src/settings_store.c,$^) -o $@ $(shell pkg-config --libs gio-2.0 uuid)

$(BUILD_DIR)/dot_pool_test: tests/dot_pool_test.c src/dot_pool.c src/dns_provider.c | $(BUILD_DIR)
	$(CC) -Wall -Wextra -Werror -std=gnu11 -g -Isrc $(shell pkg-config --cflags gio-2.0 uuid openssl) $^ -o $@ $(shell pkg-config --libs gio-2.0 uuid openssl)
