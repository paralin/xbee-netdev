_XBEE_SRC_DIR := ./thirdparty/xbee_ansic_library/src
_XBEE_NET_FILES := ./src/n_xbee.o \
	$(_XBEE_SRC_DIR)/kernel/xbee_platform_kernel.o \
	$(_XBEE_SRC_DIR)/kernel/xbee_readline.o \
	$(_XBEE_SRC_DIR)/kernel/xbee_serial_kernel.o

obj-m := $(_XBEE_NET_FILES)

KERNELDIR := /usr/src/linux
EXTRA_CFLAGS += -I$(PWD)/thirdparty/xbee_ansic_library/include

EXTRA_CFLAGS += -DXBEE_ATMODE_VERBOSE

CC := gcc -Wall -Werror

default: driver daemon

clean:
	-rm -f Module.symvers modules.order
	-find . -type f -name '*.o' -delete
	-find . -type f -name '*.ko' -delete
	-find . -type f -name '*.mod.c' -delete

driver:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

daemon:
	$(CC) -o ldisc_daemon ./src/ldisc_daemon.c
