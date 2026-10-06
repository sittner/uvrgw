# uvrgw Raspberry Pi image (see README "Raspberry Pi image").
#
#   make -C br            configure (first run) and build output/images/sdcard.img
#   make -C br <target>   any Buildroot target, e.g. menuconfig, uvrgw-rebuild

O ?= $(CURDIR)/output
DEFCONFIG = uvrgw_rpi0w_defconfig

BR = $(MAKE) -C $(CURDIR)/buildroot O=$(O) BR2_EXTERNAL=$(CURDIR)/external

all: $(O)/.config
	$(BR)

$(O)/.config:
	$(BR) $(DEFCONFIG)

Makefile: ;

%: $(O)/.config
	$(BR) $@

.PHONY: all
