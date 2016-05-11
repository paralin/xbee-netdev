_XBEE_NET_FILES := ./src/n_xbee.o

obj-m := $(_XBEE_NET_FILES)

KERNELDIR := /usr/src/linux

PWD := $(shell pwd)
CC := gcc -Wall -Werror

default: driver daemon

clean:
	cd src && rm -f *.ko *.o *.mod.c Module.symvers modules.order

driver:
	@echo "Building driver"
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

daemon:
	@echo "Building ldisc setter"
	$(CC) -o ldisc_daemon ./src/ldisc_daemon.c
