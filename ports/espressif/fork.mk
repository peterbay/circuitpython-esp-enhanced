# This file is part of the CircuitPython project: https://circuitpython.org
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
#
# SPDX-License-Identifier: MIT

# Modules this fork adds to the espressif port. They live here rather than in
# the Makefile so that an upstream merge meets as few changed lines as possible:
# the Makefile includes this file once, appends FORK_IDF_COMPONENTS_LINK to its
# component list and puts FORK_SDKCONFIGS just before the board's sdkconfig, and
# CMakeLists.txt reads the exported switches.
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
BINARY_BLOBS += esp-idf/components/esp_phy/lib/$(IDF_TARGET)/libbtbb.a
# Tickless idle, without which the driver cannot be enabled; see the fragment.
# Appended without "+=": the list is ;-separated and must not gain spaces.
FORK_SDKCONFIGS := $(FORK_SDKCONFIGS)esp-idf-config/sdkconfig-ieee802154.defaults;
endif

# Zigbee, on the same radio. Not part of this fork yet; defined because
# ieee802154's driver callbacks hand frames on to the Zigbee stack when it, not
# a Radio, holds the radio.
CIRCUITPY_ZIGBEE ?= 0
CFLAGS += -DCIRCUITPY_ZIGBEE=$(CIRCUITPY_ZIGBEE)
