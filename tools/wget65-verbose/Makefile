# WGET65V — fork of cc65/ip65 apps/wget65 with on-screen W5100 handshake logging.
# Requires a checkout of https://github.com/cc65/ip65 (unmodified upstream).
#
# Usage:
#   export IP65_ROOT=/path/to/ip65     # optional; default: ../../ip65 from this dir (= MegaFlash/ip65)
#   make
#
# Prerequisite: cc65 (cl65) on PATH; build ip65 + drivers once:
#   $(MAKE) -C $(IP65_ROOT)/ip65
#   $(MAKE) -C $(IP65_ROOT)/drivers ip65_apple2_uther2.lib

IP65_ROOT ?= $(abspath ../..)/ip65

IP65_LIB := $(IP65_ROOT)/ip65/ip65.lib
A2_UTH2   := $(IP65_ROOT)/drivers/ip65_apple2_uther2.lib

SRCS := wget65.c w5100.c w5100_http.c linenoise.c wget65_verbose_regs.c

.PHONY: all clean ip65_libs

all: wget65v.bin

ip65_libs: $(IP65_LIB) $(A2_UTH2)

$(IP65_LIB):
	$(MAKE) -C $(IP65_ROOT)/ip65

$(A2_UTH2):
	$(MAKE) -C $(IP65_ROOT)/drivers ip65_apple2_uther2.lib

wget65v.bin: $(SRCS) $(IP65_LIB) $(A2_UTH2) Makefile
	cl65 -o $@ -Or -t apple2enh -m $@.map \
		-I$(IP65_ROOT)/inc \
		$(SRCS) \
		$(IP65_LIB) $(A2_UTH2)

clean:
	rm -f wget65v.bin wget65v.bin.map *.o
