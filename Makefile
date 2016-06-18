CC := gcc -O3
CFLAGS=-DPOSIX

_XBEE_SRC_DIR := thirdparty/xbee_ansic_library/src
_XBEE_NET_FILES := \
	$(_XBEE_SRC_DIR)/posix/xbee_platform_posix.o \
	$(_XBEE_SRC_DIR)/posix/xbee_readline.o \
	$(_XBEE_SRC_DIR)/posix/xbee_serial_posix.o \
	$(_XBEE_SRC_DIR)/xbee/xbee_atmode.o \
	$(_XBEE_SRC_DIR)/xbee/xbee_atcmd.o \
	$(_XBEE_SRC_DIR)/xbee/xbee_device.o \
	$(_XBEE_SRC_DIR)/xbee/xbee_wpan.o \
	$(_XBEE_SRC_DIR)/wpan/wpan_aps.o \
	$(_XBEE_SRC_DIR)/xbee/xbee_discovery.o \
	$(_XBEE_SRC_DIR)/zigbee/zigbee_zcl.o \
	$(_XBEE_SRC_DIR)/zigbee/zcl_types.o \
	$(_XBEE_SRC_DIR)/util/swapbytes.o \
	$(_XBEE_SRC_DIR)/util/hexstrtobyte.o \
	$(_XBEE_SRC_DIR)/wpan/wpan_types.o \
	src/hexdump.o \
	src/n_xbee.o

%.o: %.c
		$(CC) -c -o $@ $< $(CFLAGS)

CFLAGS += -I$(PWD)/thirdparty/xbee_ansic_library/include
# CFLAGS += -DPOSIX

# These will enabel verbosity in various parts.
# CFLAGS += -DXBEE_ATMODE_VERBOSE
# CFLAGS += -DXBEE_ATCMD_VERBOSE

# this is required
CFLAGS += -DXBEE_DEVICE_ENABLE_ATMODE -DXBEE_CMD_DISABLE_REMOTE

# If you enable this you will get a LOT of debugging output
# EXTRA_CFLAGS += -DXBEE_SERIAL_VERBOSE
# EXTRA_CFLAGS += -DXBEE_DEVICE_VERBOSE
# CFLAGS += -DWPAN_APS_VERBOSE

# Enabel verbsity of this module
# CFLAGS += -DN_XBEE_VERBOSE

# Handle arp packets in the driver
CFLAGS += -DN_XBEE_ARP_RESPONDER

# CFLAGS += -g
DEPS += -pthread

default: xbee_netdev
xbee_netdev: ensure-submodule $(_XBEE_NET_FILES)
	$(CC) -o xbee_netdev $(DEPS) $(CFLAGS) $(_XBEE_NET_FILES)
ensure-submodule:
	@if [ ! -f ./thirdparty/xbee_ansic_library/README.md ]; then \
		echo "Attempting to update submodule..." && \
		git submodule update --init \
		;fi
clean:
	-git clean -Xfd && cd ./thirdparty/xbee_ansic_library/ && git clean -Xfd
