obj-m := n_xbee.o

KERNELDIR := /usr/src/linux

PWD := $(shell pwd)
CC := gcc

default: driver

clean:
	rm -f *.ko *.o *.mod.c Module.symvers modules.order

driver:
	@echo "Building driver"
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

daemon:
	@echo "Building ldisc setter"
	$(CC) -o ldisc_daemon ldisc_daemon.c
