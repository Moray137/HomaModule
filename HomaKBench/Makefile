KDIR ?= /lib/modules/$(shell uname -r)/build
HOMA_DIR := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/..)

obj-m += kbench_client.o
obj-m += kbench_server.o

ccflags-y += -I$(HOMA_DIR)
KBUILD_EXTRA_SYMBOLS = $(HOMA_DIR)/Module.symvers

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
