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
endif

endif
