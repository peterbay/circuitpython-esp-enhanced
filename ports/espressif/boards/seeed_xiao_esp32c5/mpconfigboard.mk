CIRCUITPY_CREATOR_ID =  0x000C2886
CIRCUITPY_CREATION_ID = 0x00C50001

IDF_TARGET = esp32c5

CIRCUITPY_ESP_FLASH_MODE = qio
CIRCUITPY_ESP_FLASH_FREQ = 80m
CIRCUITPY_ESP_FLASH_SIZE = 8MB

# 2816K app partitions instead of the stock 2048K, which the Zigbee stack does
# not fit in. Set here because the Makefile picks the partition table from the
# flash size and the bootloader alone.
FLASH_SIZE_SDKCONFIG = esp-idf-config/sdkconfig-flash-8MB-no-uf2-large-app.defaults

# 8 MB of quad PSRAM on the module, sharing the flash bus with SPICS1 as its
# chip select -- which is why GPIO15 is in the never-reset mask in Pin.c.
CIRCUITPY_ESP_PSRAM_SIZE = 8MB
CIRCUITPY_ESP_PSRAM_MODE = qio
CIRCUITPY_ESP_PSRAM_FREQ = 80m

CIRCUITPY_ZIGBEE = 1

# Channel state information on a Wi-Fi 6 part, see espidf.CSI.
CIRCUITPY_ESPIDF_CSI = 1

# TrueType text, see ttfrast.
CIRCUITPY_TTFRAST = 1
