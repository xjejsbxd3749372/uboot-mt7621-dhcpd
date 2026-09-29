# SPDX-License-Identifier: GPL-2.0

ifeq ($(CONFIG_SPL_BUILD),)

ifeq ($(CONFIG_FAILSAFE_LEGACY_UI),y)
obj-y += fsdata-legacy/
else
obj-y += fsdata/
endif

obj-y += fs.o failsafe.o
ifneq ($(CONFIG_FAILSAFE_LEGACY_MODE),y)
obj-y += failsafe_sysbackup.o
obj-y += failsafe_xiaomi.o

# failsafe_xiaomi.o reaches the MT7621 clock helpers and the board flash
# helpers, which live outside this directory.
ccflags-y += -I $(srctree)/arch/mips/mach-mt7621
ccflags-y += -I $(srctree)/board/ralink/common
endif

endif
