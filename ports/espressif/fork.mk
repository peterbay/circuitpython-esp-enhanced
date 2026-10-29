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

# espidf.ftm() and espidf.ftm_responder(): Wi-Fi Fine Timing Measurement
# (802.11mc), the distance to a responder from the round trip time of
# timestamped frames. Off unless a board asks for it, like CSI; both ends of a
# measurement need it.
CIRCUITPY_ESPIDF_FTM ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_FTM=$(CIRCUITPY_ESPIDF_FTM)
ifneq ($(CIRCUITPY_ESPIDF_FTM),0)
FORK_SDKCONFIGS := $(FORK_SDKCONFIGS)esp-idf-config/sdkconfig-ftm.defaults;
endif

# espidf.SigmaDelta: the sigma-delta modulator, a DAC on any pin through an RC
# filter. The port builds neither the IDF's SDM driver nor its component
# otherwise, so both come in here and in CMakeLists.txt, which reads the
# exported switch. Off unless a board asks for it.
CIRCUITPY_ESPIDF_SDM ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_SDM=$(CIRCUITPY_ESPIDF_SDM)
export CIRCUITPY_ESPIDF_SDM
ifneq ($(CIRCUITPY_ESPIDF_SDM),0)
INC += -isystem esp-idf/components/esp_driver_sdm/include
FORK_IDF_COMPONENTS_LINK += esp_driver_sdm
endif

# espidf.MotorPWM: the MCPWM peripheral, for complementary outputs with dead
# time. Brought in like the SDM driver, with its HAL component. Off unless a
# board asks for it.
CIRCUITPY_ESPIDF_MCPWM ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_MCPWM=$(CIRCUITPY_ESPIDF_MCPWM)
export CIRCUITPY_ESPIDF_MCPWM
ifneq ($(CIRCUITPY_ESPIDF_MCPWM),0)
INC += -isystem esp-idf/components/esp_driver_mcpwm/include
INC += -isystem esp-idf/components/esp_hal_mcpwm/include
INC += -isystem esp-idf/components/esp_hal_mcpwm/$(IDF_TARGET)/include
FORK_IDF_COMPONENTS_LINK += esp_driver_mcpwm esp_hal_mcpwm
endif

# espidf.Comparator: the analog comparator. Only for the C5 so far: the S3 has
# none, and the fixed pins it uses are per chip. Brought in like the SDM driver.
# Off unless a board asks.
CIRCUITPY_ESPIDF_ANA_CMPR ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_ANA_CMPR=$(CIRCUITPY_ESPIDF_ANA_CMPR)
export CIRCUITPY_ESPIDF_ANA_CMPR
ifneq ($(CIRCUITPY_ESPIDF_ANA_CMPR),0)
ifneq ($(IDF_TARGET),esp32c5)
$(error CIRCUITPY_ESPIDF_ANA_CMPR is only implemented for the esp32c5)
endif
INC += -isystem esp-idf/components/esp_driver_ana_cmpr/include
INC += -isystem esp-idf/components/esp_hal_ana_cmpr/include
INC += -isystem esp-idf/components/esp_hal_ana_cmpr/$(IDF_TARGET)/include
FORK_IDF_COMPONENTS_LINK += esp_driver_ana_cmpr esp_hal_ana_cmpr
endif

# espidf.PinLink and Comparator(output=): the Event Task Matrix, peripheral
# events routed to GPIO tasks with no CPU. The C5 has it, the S3 not. The ETM
# core and the GPIO side are already in components the port builds. Off unless
# a board asks for it.
# espidf.ParallelOut and ParallelIn: the parallel IO peripheral, up to eight
# pins clocked out of or sampled into memory by DMA. C5 only here. Its driver
# needs the BitScrambler driver as well. Off unless a board asks for it.
CIRCUITPY_ESPIDF_PARLIO ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_PARLIO=$(CIRCUITPY_ESPIDF_PARLIO)
export CIRCUITPY_ESPIDF_PARLIO
ifneq ($(CIRCUITPY_ESPIDF_PARLIO),0)
ifneq ($(IDF_TARGET),esp32c5)
$(error CIRCUITPY_ESPIDF_PARLIO is only implemented for the esp32c5)
endif
INC += -isystem esp-idf/components/esp_driver_parlio/include
INC += -isystem esp-idf/components/esp_hal_parlio/include
INC += -isystem esp-idf/components/esp_hal_parlio/$(IDF_TARGET)/include
FORK_IDF_COMPONENTS_LINK += esp_driver_parlio esp_hal_parlio esp_driver_bitscrambler
endif

# espidf.bitscramble(): the BitScrambler, memory to memory. C5 only here.
# Off unless a board asks for it.
CIRCUITPY_ESPIDF_BITSCRAMBLER ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_BITSCRAMBLER=$(CIRCUITPY_ESPIDF_BITSCRAMBLER)
export CIRCUITPY_ESPIDF_BITSCRAMBLER
ifneq ($(CIRCUITPY_ESPIDF_BITSCRAMBLER),0)
ifneq ($(IDF_TARGET),esp32c5)
$(error CIRCUITPY_ESPIDF_BITSCRAMBLER is only implemented for the esp32c5)
endif
INC += -isystem esp-idf/components/esp_driver_bitscrambler/include
FORK_IDF_COMPONENTS_LINK += esp_driver_bitscrambler
endif

# espidf.LPCore: the C5's LP core, a RISC-V core that runs while the main ones
# sleep. espulp, which does this for the S2's and S3's ULPs, is off on the C5;
# the ulp component comes in here instead, configured for the LP core with
# 8 KiB of LP RAM for the program (an LP core app must be built with the same
# reserve). Off unless a board asks for it.
CIRCUITPY_ESPIDF_LPCORE ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_LPCORE=$(CIRCUITPY_ESPIDF_LPCORE)
ifneq ($(CIRCUITPY_ESPIDF_LPCORE),0)
ifneq ($(IDF_TARGET),esp32c5)
$(error CIRCUITPY_ESPIDF_LPCORE is only implemented for the esp32c5)
endif
INC += -isystem esp-idf/components/ulp/lp_core/include
INC += -isystem esp-idf/components/ulp/lp_core/shared/include
# ulp_lp_core.h asks for the ROM's capabilities header.
INC += -isystem esp-idf/components/esp_rom/$(IDF_TARGET)
FORK_IDF_COMPONENTS_LINK += ulp
FORK_SDKCONFIGS := $(FORK_SDKCONFIGS)esp-idf-config/sdkconfig-lpcore.defaults;
endif

# espidf.PeriodicAdvertiser and PeriodicSync: Bluetooth 5 periodic
# advertising. NimBLE has it switched off in the port's BLE configuration; the
# fragment turns it on, with periodic sync and a second extended advertising
# instance, so _bleio's own advertising keeps instance 0. Off unless a board
# asks for it.
CIRCUITPY_ESPIDF_BLE_PERIODIC ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_BLE_PERIODIC=$(CIRCUITPY_ESPIDF_BLE_PERIODIC)
ifneq ($(CIRCUITPY_ESPIDF_BLE_PERIODIC),0)
ifeq ($(CIRCUITPY_BLEIO_NATIVE),0)
$(error CIRCUITPY_ESPIDF_BLE_PERIODIC needs CIRCUITPY_BLEIO_NATIVE)
endif
FORK_SDKCONFIGS := $(FORK_SDKCONFIGS)esp-idf-config/sdkconfig-ble-periodic.defaults;
endif

# espidf.BLEMesh: Bluetooth Mesh through ESP-BLE-MESH, which the bt component
# compiles in when the fragment configures it. Its headers are spread over the
# component, and the port lists include directories itself. Off unless a board
# asks for it; it comes after the periodic fragment, whose instance count it
# raises.
CIRCUITPY_ESPIDF_BLE_MESH ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_BLE_MESH=$(CIRCUITPY_ESPIDF_BLE_MESH)
ifneq ($(CIRCUITPY_ESPIDF_BLE_MESH),0)
ifeq ($(CIRCUITPY_BLEIO_NATIVE),0)
$(error CIRCUITPY_ESPIDF_BLE_MESH needs CIRCUITPY_BLEIO_NATIVE)
endif
BLE_MESH_DIR = esp-idf/components/bt/esp_ble_mesh
INC += $(addprefix -isystem $(BLE_MESH_DIR)/,common/include core core/include core/storage \
	btc/include models/common/include models/client/include models/server/include \
	api/core/include api/models/include api)
FORK_SDKCONFIGS := $(FORK_SDKCONFIGS)esp-idf-config/sdkconfig-ble-mesh.defaults;
endif

# espidf.WiFiAware: Wi-Fi Aware (NAN) discovery and messages. The IDF builds
# its NAN app only with NAN synchronisation configured, which the fragment
# does. Chips with SOC_WIFI_NAN_SUPPORT only (here the C5, not the S3). Off
# unless a board asks for it.
CIRCUITPY_ESPIDF_NAN ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_NAN=$(CIRCUITPY_ESPIDF_NAN)
ifneq ($(CIRCUITPY_ESPIDF_NAN),0)
ifeq ($(CIRCUITPY_WIFI),0)
$(error CIRCUITPY_ESPIDF_NAN needs CIRCUITPY_WIFI)
endif
INC += -isystem esp-idf/components/esp_wifi/wifi_apps/nan_app/include
FORK_SDKCONFIGS := $(FORK_SDKCONFIGS)esp-idf-config/sdkconfig-nan.defaults;
endif

CIRCUITPY_ESPIDF_ETM ?= 0
CFLAGS += -DCIRCUITPY_ESPIDF_ETM=$(CIRCUITPY_ESPIDF_ETM)
ifneq ($(CIRCUITPY_ESPIDF_ETM),0)
ifneq ($(IDF_TARGET),esp32c5)
$(error CIRCUITPY_ESPIDF_ETM is only implemented for the esp32c5)
endif
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

# hashlib: CPython's named constructors, hexdigest(), copy(), name, block_size
# and md5/sha224/sha384/sha512, in hashlib/extended.c. Wired the same way as
# ttfrast; the upstream tables take it in through one macro line each.
CIRCUITPY_HASHLIB_EXTENDED ?= $(CIRCUITPY_HASHLIB_MBEDTLS)
CFLAGS += -DCIRCUITPY_HASHLIB_EXTENDED=$(CIRCUITPY_HASHLIB_EXTENDED)
ifeq ($(CIRCUITPY_HASHLIB_EXTENDED),1)
SRC_SHARED_MODULE_ALL += hashlib/extended.c
endif

# espidf.expose_partition(): a flash partition as a FAT filesystem and a second
# USB drive. Needs the TinyUSB device stack, so it follows CIRCUITPY_USB_DEVICE
# (the S2, S3 and P4). supervisor/shared/usb/usb_msc_flash.c tests the switch with
# defined(), so other ports need nothing. The pure Python partition_disk module in
# frozen/partition_disk works on any board, USB or not.
CIRCUITPY_PARTITION_DISK ?= $(CIRCUITPY_USB_DEVICE)
CFLAGS += -DCIRCUITPY_PARTITION_DISK=$(CIRCUITPY_PARTITION_DISK)
ifeq ($(CIRCUITPY_PARTITION_DISK),1)
SRC_C += supervisor/partition_disk.c
endif

# usb_tmc: a USBTMC (USB488) instrument the host talks to with VISA. Compiled in
# wherever there is a TinyUSB device stack, but presented to the host only after
# usb_tmc.enable() in boot.py. TinyUSB's class switch is set here rather than in
# tusb_config.h, which leaves it at 0 unless defined; usb_desc.c tests ours with
# defined(), so other ports need nothing.
CIRCUITPY_USB_TMC ?= $(CIRCUITPY_USB_DEVICE)
CFLAGS += -DCIRCUITPY_USB_TMC=$(CIRCUITPY_USB_TMC)
CIRCUITPY_USB_TMC_ENABLED_DEFAULT ?= 0
CFLAGS += -DCIRCUITPY_USB_TMC_ENABLED_DEFAULT=$(CIRCUITPY_USB_TMC_ENABLED_DEFAULT)
ifeq ($(CIRCUITPY_USB_TMC),1)
SRC_PATTERNS += usb_tmc/%
SRC_SHARED_MODULE_ALL += usb_tmc/__init__.c
SRC_C += lib/tinyusb/src/class/usbtmc/usbtmc_device.c
CFLAGS += -DCFG_TUD_USBTMC=1 -DCFG_TUD_USBTMC_ENABLE_488=1
endif

# usb_net: an Ethernet adapter over USB, with DHCP and DNS on the board, so a
# browser on the host reaches a server on the board by name. Wired like usb_tmc:
# compiled wherever there is a TinyUSB device stack and wifi (the netif lives in
# lwIP), presented only after usb_net.enable() in boot.py. The class is CDC-NCM;
# CIRCUITPY_USB_NET_RNDIS = 1 builds RNDIS instead, which Windows binds but does
# not start for this device. TinyUSB's two drivers define the same symbols, so
# exactly one is built. The port brings the netif up from post_usb_init(), a weak
# hook in usb.c.
CIRCUITPY_USB_NET ?= $(call enable-if-all,$(CIRCUITPY_USB_DEVICE) $(CIRCUITPY_WIFI))
CFLAGS += -DCIRCUITPY_USB_NET=$(CIRCUITPY_USB_NET)
CIRCUITPY_USB_NET_ENABLED_DEFAULT ?= 0
CFLAGS += -DCIRCUITPY_USB_NET_ENABLED_DEFAULT=$(CIRCUITPY_USB_NET_ENABLED_DEFAULT)
CIRCUITPY_USB_NET_RNDIS ?= 0
CFLAGS += -DCIRCUITPY_USB_NET_RNDIS=$(CIRCUITPY_USB_NET_RNDIS)
ifeq ($(CIRCUITPY_USB_NET),1)
SRC_PATTERNS += usb_net/%
SRC_SHARED_MODULE_ALL += usb_net/__init__.c
SRC_C += supervisor/usb_net_netif.c
ifeq ($(CIRCUITPY_USB_NET_RNDIS),1)
SRC_C += lib/tinyusb/src/class/net/ecm_rndis_device.c lib/tinyusb/lib/networking/rndis_reports.c
CFLAGS += -isystem $(TOP)/lib/tinyusb/lib/networking -DCFG_TUD_ECM_RNDIS=1
else
SRC_C += lib/tinyusb/src/class/net/ncm_device.c
CFLAGS += -DCFG_TUD_NCM=1
endif
endif

# usb_video MJPEG: the encoder is Espressif's prebuilt esp_new_jpeg, vendored in
# esp_new_jpeg/ with a library for the esp32s3 only. It is not an IDF component:
# its headers and one archive are all that is used, so a board that does not
# stream MJPEG pays nothing. The switch and the frame-buffer size live in
# supervisor/supervisor.mk with the rest of usb_video; a board or the make
# command line sets CIRCUITPY_USB_VIDEO=1 CIRCUITPY_USB_VIDEO_JPEG=1.
#
# PSRAM is a requirement, not a preference. The encoder writes past the end of
# the output buffer instead of reporting that a frame did not fit, so the buffer
# has to be larger than any frame it can produce -- two of
# CIRCUITPY_USB_VIDEO_JPEG_FRAME_BYTES -- far more internal RAM than a board has.
ifeq ($(CIRCUITPY_USB_VIDEO_JPEG),1)
ifeq ($(CIRCUITPY_ESP_PSRAM_SIZE),0)
$(error CIRCUITPY_USB_VIDEO_JPEG needs a board with PSRAM)
endif
SRC_C += supervisor/usb_video_jpeg.c
CFLAGS += -I$(CURDIR)/esp_new_jpeg/include
FORK_BINARY_BLOBS += esp_new_jpeg/lib/$(IDF_TARGET)/libesp_new_jpeg.a
endif

# espsdr: raw I/Q snapshots from the Wi-Fi receiver through the undocumented
# dump engine ESP-SDR describes. ESP32-S3 only for now: the register addresses
# and the reserved SRAM bank are per chip, and on the C5 CircuitPython's own
# static data sits in the bank the engine writes. Off unless a board asks. The
# manual gain call is in librftest, which the IDF links only for
# CONFIG_ESP_PHY_ENABLE_CERT_TEST; added here instead, so the linker takes the
# one object it needs and nothing else of the certification code comes along.
CIRCUITPY_ESPSDR ?= 0
CFLAGS += -DCIRCUITPY_ESPSDR=$(CIRCUITPY_ESPSDR)
ifneq ($(CIRCUITPY_ESPSDR),0)
ifneq ($(IDF_TARGET),esp32s3)
$(error CIRCUITPY_ESPSDR is only implemented for the esp32s3)
endif
ifeq ($(CIRCUITPY_WIFI),0)
$(error CIRCUITPY_ESPSDR needs CIRCUITPY_WIFI)
endif
SRC_C += $(wildcard bindings/espsdr/*.c)
FORK_BINARY_BLOBS += esp-idf/components/esp_phy/lib/$(IDF_TARGET)/librftest.a
endif
