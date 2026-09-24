HOST_CC ?= cc
CROSS_COMPILE ?= arm-linux-gnueabi-
TARGET_CC := $(CROSS_COMPILE)gcc
TARGET_STRIP := $(CROSS_COMPILE)strip

GEN_DIR := build/generated
GEN_VERSION := $(GEN_DIR)/version.h

SRC_DIR := src
CARRIER_DIR := $(SRC_DIR)/carrier
TRANSPORT_DIR := $(SRC_DIR)/transport
RUNTIME_DIR := $(SRC_DIR)/runtime
SERVICES_DIR := $(SRC_DIR)/services
HOST_SUBSYS_DIR := $(SRC_DIR)/host
ZEROMUX_DIR := $(HOST_SUBSYS_DIR)/zeromux
SUPERVISOR_DIR := $(SRC_DIR)/supervisor
LINK_DIR := $(SRC_DIR)/link
UNIT_TEST_DIR := tests/unit
INTEGRATION_TEST_DIR := tests/integration

COMMON_CFLAGS := -std=c11 -D_GNU_SOURCE -Wall -Wextra -I$(GEN_DIR) -I$(SRC_DIR) -I$(CARRIER_DIR) -I$(TRANSPORT_DIR) -I$(RUNTIME_DIR) -I$(SERVICES_DIR) -I$(HOST_SUBSYS_DIR) -I$(ZEROMUX_DIR) -I$(SUPERVISOR_DIR) -I$(LINK_DIR)
HOST_CFLAGS ?= -O2
TARGET_CFLAGS ?= -Os -march=armv5te -marm -mfloat-abi=soft
TARGET_LDFLAGS := -static

HOST_DIR := build/host
TARGET_DIR := build/target

LMUX := $(SRC_DIR)/lmux.c
LXF_SRC := $(SRC_DIR)/lxf.c
LXF_HDR := $(SRC_DIR)/lxf.h
LMUXD := $(RUNTIME_DIR)/lmuxd.c
COMMON_SRC := $(TRANSPORT_DIR)/protocol.c $(LXF_SRC)
LMUX2_SRC := $(TRANSPORT_DIR)/lmux2_protocol.c $(TRANSPORT_DIR)/lmux2_core.c $(TRANSPORT_DIR)/lmux2_control.c
CARRIER_SRC := $(CARRIER_DIR)/carrier.c $(CARRIER_DIR)/carrier_tty.c $(CARRIER_DIR)/carrier_fd.c $(CARRIER_DIR)/carrier_unix.c
SERVICES_SRC := $(SERVICES_DIR)/services.c $(SERVICES_DIR)/service_pty.c $(SERVICES_DIR)/service_unix.c $(SERVICES_DIR)/service_file.c
HOST_SUBSYS_SRC := $(HOST_SUBSYS_DIR)/host_services.c $(HOST_SUBSYS_DIR)/host_endpoints.c $(HOST_SUBSYS_DIR)/host_control.c $(HOST_SUBSYS_DIR)/host_link.c
ZEROMUX_SRC := $(ZEROMUX_DIR)/zeromux.c $(ZEROMUX_DIR)/session.c $(ZEROMUX_DIR)/probe.c $(ZEROMUX_DIR)/sender.c $(ZEROMUX_DIR)/sender_uudecode.c
SUPERVISOR_SRC := $(SUPERVISOR_DIR)/supervisor.c $(SUPERVISOR_DIR)/supervisor_worker.c $(SUPERVISOR_DIR)/supervisor_proxy.c $(SUPERVISOR_DIR)/supervisor_link.c $(SUPERVISOR_DIR)/supervisor_storage.c
LINK_SRC := $(LINK_DIR)/link_protocol.c $(LINK_DIR)/link_endpoint.c $(LINK_DIR)/boot_protocol.c
LMUX2_RUNTIME_SRC := $(RUNTIME_DIR)/lmux2_runtime.c
CARRIER_HDRS := $(CARRIER_DIR)/carrier.h
TRANSPORT_HDRS := $(TRANSPORT_DIR)/lmux.h $(TRANSPORT_DIR)/lmux2_protocol.h $(TRANSPORT_DIR)/lmux2_core.h $(TRANSPORT_DIR)/lmux2_control.h $(TRANSPORT_DIR)/lmux2_service.h
SERVICES_HDRS := $(SERVICES_DIR)/services.h
HOST_SUBSYS_HDRS := $(HOST_SUBSYS_DIR)/host.h
ZEROMUX_HDRS := $(ZEROMUX_DIR)/zeromux.h $(ZEROMUX_DIR)/session.h $(ZEROMUX_DIR)/probe.h $(ZEROMUX_DIR)/sender.h
SUPERVISOR_HDRS := $(SUPERVISOR_DIR)/supervisor.h
LINK_HDRS := $(LINK_DIR)/link_protocol.h $(LINK_DIR)/link_endpoint.h $(LINK_DIR)/boot_protocol.h
RUNTIME_HDRS := $(RUNTIME_DIR)/lmux2_runtime.h
TEST_PROTOCOL := $(UNIT_TEST_DIR)/test_protocol.c
TEST_LMUX2 := $(UNIT_TEST_DIR)/test_lmux2.c
TEST_LMUX_CLIENT := $(UNIT_TEST_DIR)/test_lmux_client.c
TEST_CARRIER := $(UNIT_TEST_DIR)/test_carrier.c
TEST_SUPERVISOR := $(UNIT_TEST_DIR)/test_supervisor.c
TEST_LINK := $(UNIT_TEST_DIR)/test_link.c
TEST_ZEROMUX := $(UNIT_TEST_DIR)/test_zeromux.c
TEST_LMUX2_INTEGRATION := $(INTEGRATION_TEST_DIR)/test_lmux2_integration.c
TEST_LMUX2_PTY_INTEGRATION := $(INTEGRATION_TEST_DIR)/test_lmux2_pty_integration.c
TEST_LMUX2_FILE_INTEGRATION := $(INTEGRATION_TEST_DIR)/test_lmux2_file_integration.c
TEST_SUPERVISOR_INTEGRATION := $(INTEGRATION_TEST_DIR)/test_supervisor_integration.c
VERSION_INPUTS := VERSION Makefile $(LMUX) $(LMUXD) $(TRANSPORT_HDRS) $(CARRIER_HDRS) $(SERVICES_HDRS) $(HOST_SUBSYS_HDRS) $(ZEROMUX_HDRS) $(SUPERVISOR_HDRS) $(LINK_HDRS) $(LXF_SRC) $(LXF_HDR) $(COMMON_SRC) $(LMUX2_SRC) $(CARRIER_SRC) $(SERVICES_SRC) $(HOST_SUBSYS_SRC) $(ZEROMUX_SRC) $(SUPERVISOR_SRC) $(LINK_SRC) $(LMUX2_RUNTIME_SRC) $(RUNTIME_HDRS) $(TEST_PROTOCOL) $(TEST_LMUX2) $(TEST_CARRIER) $(TEST_SUPERVISOR) $(TEST_LINK) $(TEST_ZEROMUX) $(TEST_LMUX2_INTEGRATION) $(TEST_LMUX2_PTY_INTEGRATION) $(TEST_LMUX2_FILE_INTEGRATION) $(TEST_SUPERVISOR_INTEGRATION) $(TEST_LMUX_CLIENT)

.PHONY: all host target strip-target clean selftest new-build bump-version

all: host target

host: $(HOST_DIR)/lmuxd $(HOST_DIR)/lmux $(HOST_DIR)/test_protocol $(HOST_DIR)/test_lmux2 $(HOST_DIR)/test_carrier $(HOST_DIR)/test_supervisor $(HOST_DIR)/test_link $(HOST_DIR)/test_zeromux $(HOST_DIR)/test_lmux2_integration $(HOST_DIR)/test_lmux2_pty_integration $(HOST_DIR)/test_lmux2_file_integration $(HOST_DIR)/test_supervisor_integration $(HOST_DIR)/test_lmux_client

target: $(TARGET_DIR)/lmuxd $(TARGET_DIR)/lmux $(TARGET_DIR)/test_lmux2

selftest: $(HOST_DIR)/lmuxd $(HOST_DIR)/lmux $(HOST_DIR)/test_protocol $(HOST_DIR)/test_lmux2 $(HOST_DIR)/test_carrier $(HOST_DIR)/test_supervisor $(HOST_DIR)/test_link $(HOST_DIR)/test_zeromux $(HOST_DIR)/test_lmux2_integration $(HOST_DIR)/test_lmux2_pty_integration $(HOST_DIR)/test_lmux2_file_integration $(HOST_DIR)/test_supervisor_integration $(HOST_DIR)/test_lmux_client
	$(HOST_DIR)/test_protocol
	$(HOST_DIR)/test_lmux2
	$(HOST_DIR)/test_carrier
	$(HOST_DIR)/test_supervisor
	$(HOST_DIR)/test_link
	$(HOST_DIR)/test_zeromux
	$(HOST_DIR)/test_lmux_client
	$(HOST_DIR)/test_lmux2_integration
	$(HOST_DIR)/test_lmux2_pty_integration
	$(HOST_DIR)/test_lmux2_file_integration
	$(HOST_DIR)/test_supervisor_integration
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

$(HOST_DIR)/lmuxd: $(LMUXD) $(COMMON_SRC) $(LMUX2_SRC) $(CARRIER_SRC) $(SERVICES_SRC) $(HOST_SUBSYS_SRC) $(SUPERVISOR_SRC) $(LINK_SRC) $(LMUX2_RUNTIME_SRC) $(TRANSPORT_HDRS) $(CARRIER_HDRS) $(SERVICES_HDRS) $(HOST_SUBSYS_HDRS) $(SUPERVISOR_HDRS) $(LINK_HDRS) $(LXF_HDR) $(RUNTIME_HDRS) $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(LMUXD) $(COMMON_SRC) $(LMUX2_SRC) $(CARRIER_SRC) $(SERVICES_SRC) $(HOST_SUBSYS_SRC) $(SUPERVISOR_SRC) $(LINK_SRC) $(LMUX2_RUNTIME_SRC)

$(HOST_DIR)/lmux: $(LMUX) $(COMMON_SRC) $(ZEROMUX_SRC) $(CARRIER_DIR)/carrier_tty.c $(TRANSPORT_DIR)/lmux.h $(LXF_HDR) $(ZEROMUX_HDRS) $(CARRIER_HDRS) $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(LMUX) $(COMMON_SRC) $(ZEROMUX_SRC) $(CARRIER_DIR)/carrier_tty.c

$(HOST_DIR)/test_protocol: $(TEST_PROTOCOL) $(COMMON_SRC) $(TRANSPORT_DIR)/lmux.h $(LXF_HDR) $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(TEST_PROTOCOL) $(COMMON_SRC)

$(HOST_DIR)/test_lmux2: $(TEST_LMUX2) $(COMMON_SRC) $(LMUX2_SRC) $(TRANSPORT_HDRS) $(LXF_HDR) $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(TEST_LMUX2) $(COMMON_SRC) $(LMUX2_SRC)

$(HOST_DIR)/test_carrier: $(TEST_CARRIER) $(CARRIER_SRC) $(CARRIER_HDRS) $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(TEST_CARRIER) $(CARRIER_SRC)

$(HOST_DIR)/test_supervisor: $(TEST_SUPERVISOR) $(SUPERVISOR_DIR)/supervisor_proxy.c $(SUPERVISOR_DIR)/supervisor_worker.c $(SUPERVISOR_DIR)/supervisor_storage.c $(SUPERVISOR_HDRS) $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(TEST_SUPERVISOR) $(SUPERVISOR_DIR)/supervisor_proxy.c $(SUPERVISOR_DIR)/supervisor_worker.c $(SUPERVISOR_DIR)/supervisor_storage.c

$(HOST_DIR)/test_link: $(TEST_LINK) $(LINK_SRC) $(LINK_HDRS) $(TRANSPORT_DIR)/lmux.h $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(TEST_LINK) $(LINK_SRC) $(TRANSPORT_DIR)/protocol.c

$(HOST_DIR)/test_zeromux: $(TEST_ZEROMUX) $(ZEROMUX_SRC) $(ZEROMUX_HDRS) $(CARRIER_DIR)/carrier_tty.c $(CARRIER_HDRS) $(LXF_SRC) $(LXF_HDR) $(TRANSPORT_DIR)/protocol.c $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(TEST_ZEROMUX) $(ZEROMUX_SRC) $(CARRIER_DIR)/carrier_tty.c $(LXF_SRC) $(TRANSPORT_DIR)/protocol.c

$(HOST_DIR)/test_lmux2_integration: $(TEST_LMUX2_INTEGRATION) $(COMMON_SRC) $(TRANSPORT_DIR)/lmux2_protocol.c $(TRANSPORT_DIR)/lmux.h $(TRANSPORT_DIR)/lmux2_protocol.h $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(TEST_LMUX2_INTEGRATION) $(COMMON_SRC) $(TRANSPORT_DIR)/lmux2_protocol.c

$(HOST_DIR)/test_lmux2_pty_integration: $(TEST_LMUX2_PTY_INTEGRATION) $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(TEST_LMUX2_PTY_INTEGRATION)

$(HOST_DIR)/test_lmux2_file_integration: $(TEST_LMUX2_FILE_INTEGRATION) $(COMMON_SRC) $(TRANSPORT_DIR)/lmux.h $(LXF_HDR) $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(TEST_LMUX2_FILE_INTEGRATION) $(COMMON_SRC)

$(HOST_DIR)/test_supervisor_integration: $(TEST_SUPERVISOR_INTEGRATION) $(COMMON_SRC) $(TRANSPORT_DIR)/lmux.h $(LXF_HDR) $(LINK_DIR)/link_protocol.c $(LINK_HDRS) $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(TEST_SUPERVISOR_INTEGRATION) $(COMMON_SRC) $(LINK_DIR)/link_protocol.c

$(HOST_DIR)/test_lmux_client: $(TEST_LMUX_CLIENT) $(LMUX) $(COMMON_SRC) $(TRANSPORT_DIR)/lmux.h $(LXF_HDR) $(GEN_VERSION) | $(HOST_DIR)
	$(HOST_CC) $(COMMON_CFLAGS) $(HOST_CFLAGS) -o $@ $(TEST_LMUX_CLIENT) $(COMMON_SRC)

$(TARGET_DIR)/lmuxd: $(LMUXD) $(COMMON_SRC) $(LMUX2_SRC) $(CARRIER_SRC) $(SERVICES_SRC) $(HOST_SUBSYS_SRC) $(SUPERVISOR_SRC) $(LINK_SRC) $(LMUX2_RUNTIME_SRC) $(TRANSPORT_HDRS) $(CARRIER_HDRS) $(SERVICES_HDRS) $(HOST_SUBSYS_HDRS) $(SUPERVISOR_HDRS) $(LINK_HDRS) $(LXF_HDR) $(RUNTIME_HDRS) $(GEN_VERSION) | $(TARGET_DIR)
	$(TARGET_CC) $(COMMON_CFLAGS) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $@ $(LMUXD) $(COMMON_SRC) $(LMUX2_SRC) $(CARRIER_SRC) $(SERVICES_SRC) $(HOST_SUBSYS_SRC) $(SUPERVISOR_SRC) $(LINK_SRC) $(LMUX2_RUNTIME_SRC)

$(TARGET_DIR)/lmux: $(LMUX) $(COMMON_SRC) $(ZEROMUX_SRC) $(CARRIER_DIR)/carrier_tty.c $(TRANSPORT_DIR)/lmux.h $(LXF_HDR) $(ZEROMUX_HDRS) $(CARRIER_HDRS) $(GEN_VERSION) | $(TARGET_DIR)
	$(TARGET_CC) $(COMMON_CFLAGS) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $@ $(LMUX) $(COMMON_SRC) $(ZEROMUX_SRC) $(CARRIER_DIR)/carrier_tty.c

$(TARGET_DIR)/test_lmux2: $(TEST_LMUX2) $(COMMON_SRC) $(LMUX2_SRC) $(TRANSPORT_HDRS) $(LXF_HDR) $(GEN_VERSION) | $(TARGET_DIR)
	$(TARGET_CC) $(COMMON_CFLAGS) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) -o $@ $(TEST_LMUX2) $(COMMON_SRC) $(LMUX2_SRC)

strip-target: target
	$(TARGET_STRIP) $(TARGET_DIR)/lmuxd $(TARGET_DIR)/lmux

clean:
	rm -rf build
