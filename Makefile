# Kernel module Makefile
obj-m += mchose_battery.o

PWD := $(CURDIR)
KDIR ?= /lib/modules/$(shell uname -r)/build
KERNEL_VER ?= $(shell uname -r)
MODULE_DEST ?= /lib/modules/$(KERNEL_VER)/kernel/drivers/hid

UDEV_RULES_DIR ?= /etc/udev/rules.d
UDEV_RULE := rules.d/50-mchose-battery.rules

MODULE_KO := mchose_battery.ko

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
	install -Dm 644 $(MODULE_KO) $(DESTDIR)$(MODULE_DEST)/mchose_battery.ko
	install -Dm 644 $(UDEV_RULE) $(DESTDIR)$(UDEV_RULES_DIR)/50-mchose-battery.rules
	depmod -a $(KERNEL_REL)
	-udevadm control --reload-rules 2>/dev/null
	@echo ""
	@echo "Module installed. To load, run as a root:"
	@echo "  modprobe mchose-battery"

uninstall:
	rm -f $(DESTDIR)$(MODULE_DEST)/mchose_battery.ko
	rm -f $(DESTDIR)$(UDEV_RULES_DIR)/50-mchose-battery.rules
	depmod -a $(KERNEL_REL) 2>/dev/null || true
	-udevadm control --reload-rules 2>/dev/null
	@echo ""
	@echo "module uninstalled"

.PHONY: all clean install uninstall
