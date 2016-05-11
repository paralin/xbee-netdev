_XBEE_NET_FILES := ./src/n_xbee.o

obj-m := $(_XBEE_NET_FILES)

PWD := $(shell pwd)
KERNELDIR := /usr/src/linux
EXTRA_CFLAGS += -I$(PWD)/thirdparty/xbee_ansic_library/include

CC := gcc -Wall -Werror

default: driver daemon

clean:
	cd src && rm -f *.ko *.o *.mod.c Module.symvers modules.order

driver:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

daemon:
	$(CC) -o ldisc_daemon ./src/ldisc_daemon.c
