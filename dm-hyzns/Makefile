# Out-of-tree build for dm-hyzns.
#
#   make            # build dm-hyzns.ko against the running kernel
#   make KDIR=...   # build against a specific kernel source tree
#   make clean      # remove build artifacts

obj-m += dm-hyzns.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
