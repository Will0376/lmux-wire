HOST_CC ?= cc
CROSS_COMPILE ?= arm-linux-gnueabi-
TARGET_CC := $(CROSS_COMPILE)gcc
TARGET_STRIP := $(CROSS_COMPILE)strip

GEN_DIR := build/generated
GEN_VERSION := $(GEN_DIR)/version.h

COMMON_CFLAGS := -std=c11 -D_GNU_SOURCE -Wall -Wextra -I$(GEN_DIR)
HOST_CFLAGS ?= -O2
TARGET_CFLAGS ?= -Os -march=armv5te -marm -mfloat-abi=soft
TARGET_LDFLAGS := -static

HOST_DIR := build/host
TARGET_DIR := build/target

COMMON_SRC := protocol.c lxf.c
VERSION_INPUTS := VERSION Makefile lmux.c lmuxd.c lmux.h lxf.c lxf.h protocol.c test_protocol.c test_lmuxd_state.c test_lmuxd_integration.c test_lmux_client.c

.PHONY: all host target strip-target clean selftest new-build bump-version

all: host target

host: $(HOST_DIR)/lmuxd $(HOST_DIR)/lmux $(HOST_DIR)/test_protocol $(HOST_DIR)/test_lmuxd_state $(HOST_DIR)/test_lmuxd_integration $(HOST_DIR)/test_lmux_client

target: $(TARGET_DIR)/lmuxd $(TARGET_DIR)/lmux

selftest: $(HOST_DIR)/lmuxd $(HOST_DIR)/lmux $(HOST_DIR)/test_protocol $(HOST_DIR)/test_lmuxd_state $(HOST_DIR)/test_lmuxd_integration $(HOST_DIR)/test_lmux_client
	$(HOST_DIR)/test_protocol
	$(HOST_DIR)/test_lmuxd_state
	$(HOST_DIR)/test_lmuxd_integration
	$(HOST_DIR)/test_lmux_client
	test "$$($(HOST_DIR)/lmux --version | sed 's/^lmux //')" = "$$($(HOST_DIR)/lmuxd --version | sed 's/^lmuxd //')"
	grep -F -q "$$($(HOST_DIR)/lmux --version | sed 's/^lmux //' | sed 's/ (LMUX Wire)//')" $(GEN_DIR)/version.h
	test "$$($(HOST_DIR)/lmux 2>&1 | sed -n '1p')" = "$$($(HOST_DIR)/lmux --version)"
	test "$$($(HOST_DIR)/lmuxd 2>&1 | sed -n '1p')" = "$$($(HOST_DIR)/lmuxd --version)"

$(HOST_DIR) $(TARGET_DIR) $(GEN_DIR):
	mkdir -p $@

$(GEN_VERSION): $(VERSION_INPUTS) scripts/gen-version.sh | $(GEN_DIR)
	sh scripts/gen-version.sh

new-build bump-version:
	rm -f $(GEN_VERSION) $(GEN_DIR)/version.txt
	$(MAKE) $(GEN_VERSION)

$(HOST_DIR)/lmuxd: lmuxd.c $(COMMON_SRC) lmux.h lxf.h $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ lmuxd.c $(COMMON_SRC)

$(HOST_DIR)/lmux: lmux.c $(COMMON_SRC) lmux.h lxf.h $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ lmux.c $(COMMON_SRC)

$(HOST_DIR)/test_protocol: test_protocol.c $(COMMON_SRC) lmux.h lxf.h $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ test_protocol.c $(COMMON_SRC)

$(HOST_DIR)/test_lmuxd_state: test_lmuxd_state.c lmuxd.c $(COMMON_SRC) lmux.h lxf.h $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ test_lmuxd_state.c $(COMMON_SRC)

$(HOST_DIR)/test_lmuxd_integration: test_lmuxd_integration.c lmuxd.c $(COMMON_SRC) lmux.h lxf.h $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ test_lmuxd_integration.c $(COMMON_SRC)

$(HOST_DIR)/test_lmux_client: test_lmux_client.c lmux.c $(COMMON_SRC) lmux.h lxf.h $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ test_lmux_client.c $(COMMON_SRC)

$(TARGET_DIR)/lmuxd: lmuxd.c $(COMMON_SRC) lmux.h lxf.h $(GEN_VERSION) | $(TARGET_DIR)
	$(TARGET_CC) $(COMMON_CFLAGS) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $@ lmuxd.c $(COMMON_SRC)

$(TARGET_DIR)/lmux: lmux.c $(COMMON_SRC) lmux.h lxf.h $(GEN_VERSION) | $(TARGET_DIR)
	$(TARGET_CC) $(COMMON_CFLAGS) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $@ lmux.c $(COMMON_SRC)

strip-target: target
	$(TARGET_STRIP) $(TARGET_DIR)/lmuxd $(TARGET_DIR)/lmux

clean:
	rm -rf build
