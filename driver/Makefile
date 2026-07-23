obj-m := gki_ntsync.o

ccflags-y += -I$(src)/include
ccflags-y += -I$(src)/include/uapi

KDIR ?= /lib/modules/$(shell uname -r)/build
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

# KernelSU resolves non-exported symbols at load time through kallsyms.
KBUILD_MODPOST_WARN ?= 1

.PHONY: all clean

all:
	$(MAKE) -C $(KDIR) M=$(MDIR) KBUILD_MODPOST_WARN=$(KBUILD_MODPOST_WARN) modules

clean:
	$(MAKE) -C $(KDIR) M=$(MDIR) clean
