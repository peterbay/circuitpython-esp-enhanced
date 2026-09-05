CIRCUITPY_CREATOR_ID =  0x000C2886
CIRCUITPY_CREATION_ID = 0x00C50001

IDF_TARGET = esp32c5

CIRCUITPY_ESP_FLASH_MODE = qio
CIRCUITPY_ESP_FLASH_FREQ = 80m
CIRCUITPY_ESP_FLASH_SIZE = 8MB

# 2304K app partitions instead of the stock 2048K, which BLE does not fit in.
# Set here rather than picked up from the flash size because the Makefile's
# choice is driven only by flash size and whether there is a UF2 bootloader.
FLASH_SIZE_SDKCONFIG = esp-idf-config/sdkconfig-flash-8MB-no-uf2-large-app.defaults

# 8 MB of quad PSRAM on the module, sharing the flash bus with SPICS1 as its
# chip select -- which is why GPIO15 is in the never-reset mask in Pin.c.
CIRCUITPY_ESP_PSRAM_SIZE = 8MB
CIRCUITPY_ESP_PSRAM_MODE = qio
CIRCUITPY_ESP_PSRAM_FREQ = 80m

# The reason for this board: CSI on a Wi-Fi 6 part. Needs
# CONFIG_ESP_WIFI_CSI_ENABLED=y in the board sdkconfig as well.
CIRCUITPY_ESPIDF_CSI = 1
