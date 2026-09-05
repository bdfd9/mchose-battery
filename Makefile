# Kernel module Makefile

obj-m += mouse_battery.o
# mouse_battery-objs := mouse_battery.c

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(CURDIR)
LDFLAGS = -fuse-ld=lld

# Pass extra CFLAGS for debug builds:
#   make DEBUG=1
ifdef DEBUG
ccflags-y += -DDEBUG
endif

all:
	$(MAKE) -C $(KDIR) M=$(PWD) LD=/usr/bin/ld.bfd modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
