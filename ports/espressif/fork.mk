# This file is part of the CircuitPython project: https://circuitpython.org
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
#
# SPDX-License-Identifier: MIT

# Modules this fork adds to the espressif port. They live here rather than in
# the Makefile so that an upstream merge meets as few changed lines as possible:
# the Makefile includes this file once, appends FORK_IDF_COMPONENTS_LINK and
# FORK_BINARY_BLOBS to its own lists and puts FORK_SDKCONFIGS just before the
# board's sdkconfig, and CMakeLists.txt reads the exported switches. Nothing
# here may add to BINARY_BLOBS directly: the Makefile assigns it with "=" after
# this file is included, and anything added before is lost.
#
# The sdkconfig header is regenerated only when the board files or
# CMakeLists.txt change, not when a fragment listed here does: after editing
# one, build from clean.

# Raw IEEE 802.15.4. Only the esp32c5, esp32c6 and esp32h2 have the radio
# (SOC_IEEE802154_SUPPORTED). Elsewhere the IDF component compiles nothing, and
# since IDF 6.1 a component with no sources has no archive, so asking the linker
# for it fails the build with "unknown target libieee802154.a".
ifneq ($(filter $(IDF_TARGET),esp32c5 esp32c6 esp32h2),)
CIRCUITPY_IEEE802154 ?= 1
else
CIRCUITPY_IEEE802154 ?= 0
endif
CFLAGS += -DCIRCUITPY_IEEE802154=$(CIRCUITPY_IEEE802154)
# CMakeLists.txt adds the IDF components only when this is 1. Listing them
# unconditionally would switch CONFIG_IEEE802154_ENABLED on, its default where
# the radio exists, for every such board whether or not it uses the module.
export CIRCUITPY_IEEE802154

ifneq ($(CIRCUITPY_IEEE802154),0)
INC += -isystem esp-idf/components/ieee802154/include
# esp_ieee802154.h includes esp_coex_i154.h for its coexistence types.
INC += -isystem esp-idf/components/esp_coex/include
SRC_C += $(wildcard bindings/ieee802154/*.c)
# esp_hal_ieee802154 carries ieee802154_periph, the peripheral descriptor the
# driver dereferences on enable; without it every entry point fails to link.
FORK_IDF_COMPONENTS_LINK += ieee802154 esp_hal_ieee802154
FORK_BINARY_BLOBS += esp-idf/components/esp_phy/lib/$(IDF_TARGET)/libbtbb.a
# Tickless idle, without which the driver cannot be enabled; see the fragment.
# Appended without "+=": the list is ;-separated and must not gain spaces.
FORK_SDKCONFIGS := $(FORK_SDKCONFIGS)esp-idf-config/sdkconfig-ieee802154.defaults;
endif

# Zigbee, on the same radio. The stack is Espressif's prebuilt esp-zigbee-lib,
# vendored in esp-zigbee-lib/ with binaries for the esp32c5 only, so a board
# turns this on itself. It costs about 230K of flash, more than the stock 2048K
# app partition leaves: such a board also needs FLASH_SIZE_SDKCONFIG =
# esp-idf-config/sdkconfig-flash-8MB-no-uf2-large-app.defaults.
CIRCUITPY_ZIGBEE ?= 0
CFLAGS += -DCIRCUITPY_ZIGBEE=$(CIRCUITPY_ZIGBEE)
export CIRCUITPY_ZIGBEE

ifneq ($(CIRCUITPY_ZIGBEE),0)
ifeq ($(CIRCUITPY_IEEE802154),0)
$(error CIRCUITPY_ZIGBEE needs CIRCUITPY_IEEE802154)
endif
INC += -isystem esp-zigbee-lib/include
SRC_C += $(wildcard bindings/zigbee/*.c)
# Three prebuilt archives. The role is fixed at link time, and zczr is the one
# to take: its symbols are a strict superset of zed's, and
# ezb_nwk_set_device_type() then picks coordinator, router or end device at run
# time. Linking zed as well is not an option -- they collide on every symbol
# they share, which is all of zed's.
FORK_IDF_COMPONENTS_LINK += esp-zigbee-lib nvs_flash
FORK_BINARY_BLOBS += esp-zigbee-lib/lib/$(IDF_TARGET)/libesp-zigbee.release.a \
	esp-zigbee-lib/lib/$(IDF_TARGET)/libesp-zigbee-core.zczr.release.a \
	esp-zigbee-lib/lib/$(IDF_TARGET)/libesp-zigbee-idf.native.release.a
FORK_SDKCONFIGS := $(FORK_SDKCONFIGS)esp-idf-config/sdkconfig-zigbee.defaults;
endif

# _bleio: extended advertisements reassembled from their fragments, and scanning
# on the Coded PHY too; see common-hal/_bleio/extended_scan.c.
ifneq ($(CIRCUITPY_BLEIO_NATIVE),0)
SRC_C += common-hal/_bleio/extended_scan.c
SRC_C += common-hal/_bleio/extended_adv.c
endif

# espidf's Wi-Fi functions reach the EAP client, whose header lives in the
# wpa_supplicant component the port already links.
ifneq ($(CIRCUITPY_WIFI),0)
INC += -isystem esp-idf/components/wpa_supplicant/esp_supplicant/include
endif

# espidf.CSI, channel state information from the Wi-Fi radio. Off unless a board
# asks for it: the driver's CSI support costs flash and RAM everywhere it is
# compiled in, whether or not anything listens.
CIRCUITPY_ESPIDF_CSI ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_CSI=$(CIRCUITPY_ESPIDF_CSI)
ifneq ($(CIRCUITPY_ESPIDF_CSI),0)
FORK_SDKCONFIGS := $(FORK_SDKCONFIGS)esp-idf-config/sdkconfig-csi.defaults;
endif

# ttfrast, TrueType glyphs rasterised from the outline, a shared module. It is
# wired in here rather than in py/circuitpy_defns.mk: SRC_SHARED_MODULE is a
# filter of SRC_PATTERNS over SRC_SHARED_MODULE_ALL, expanded only when used,
# so adding to both after the defns are read is enough. Off unless a board asks.
CIRCUITPY_TTFRAST ?= 0
CFLAGS += -DCIRCUITPY_TTFRAST=$(CIRCUITPY_TTFRAST)
ifeq ($(CIRCUITPY_TTFRAST),1)
SRC_PATTERNS += ttfrast/%
SRC_SHARED_MODULE_ALL += ttfrast/__init__.c ttfrast/Label.c
endif
