KERNELDIR := /usr/src/linux
CC := gcc -Wall -Werror

_XBEE_SRC_DIR := thirdparty/xbee_ansic_library/src
_XBEE_NET_FILES := \
	$(_XBEE_SRC_DIR)/kernel/xbee_platform_kernel.o \
	$(_XBEE_SRC_DIR)/kernel/xbee_readline.o \
	$(_XBEE_SRC_DIR)/kernel/xbee_serial_kernel.o \
	$(_XBEE_SRC_DIR)/xbee/xbee_atmode.o \
	$(_XBEE_SRC_DIR)/xbee/xbee_atcmd.o \
	$(_XBEE_SRC_DIR)/xbee/xbee_device.o \
	$(_XBEE_SRC_DIR)/util/swapbytes.o \
	$(_XBEE_SRC_DIR)/util/hexstrtobyte.o \
	$(_XBEE_SRC_DIR)/wpan/wpan_types.o \
	src/n_xbee.o

EXTRA_CFLAGS += -I$(PWD)/thirdparty/xbee_ansic_library/include

# These will enabel verbosity in various parts.
EXTRA_CFLAGS += -DXBEE_ATMODE_VERBOSE
EXTRA_CFLAGS += -DXBEE_ATCMD_VERBOSE
EXTRA_CFLAGS += -DXBEE_DEVICE_ENABLE_ATMODE

# If you enable this you will get a LOT of debugging output
# EXTRA_CFLAGS += -DXBEE_SERIAL_VERBOSE
# EXTRA_CFLAGS += -DXBEE_DEVICE_VERBOSE

EXTRA_CFLAGS += -DN_XBEE_VERBOSE

obj-m += n_xbee.o
n_xbee-objs += $(_XBEE_NET_FILES)

default: driver daemon
clean:
	-git clean -Xfd && cd ./thirdparty/xbee_ansic_library/ && git clean -Xfd
	#-rm -f Module.symvers modules.order
	#-find . -type f -name '*.o' -delete
	#-find . -type f -name '*.ko' -delete
	#-find . -type f -name '*.mod.c' -delete
driver:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

daemon:
	$(CC) -o ldisc_daemon ./src/ldisc_daemon.c
