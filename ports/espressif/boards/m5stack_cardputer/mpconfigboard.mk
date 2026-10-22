USB_VID = 0x303A
USB_PID = 0x81DA
USB_PRODUCT = "M5Stack Cardputer - CircuitPython"
USB_MANUFACTURER = "M5STACK"

IDF_TARGET = esp32s3

CIRCUITPY_ESP_FLASH_MODE = qio
CIRCUITPY_ESP_FLASH_FREQ = 80m
CIRCUITPY_ESP_FLASH_SIZE = 8MB

# Very few pins.
CIRCUITPY_ESPCAMERA = 0
CIRCUITPY_MAX3421E = 0

SRC_C += module/cardputer_keyboard.c

CIRCUITPY_ESPIDF_CSI = 1
FROZEN_MPY_DIRS += $(TOP)/frozen/register_map
FROZEN_MPY_DIRS += $(TOP)/frozen/partition_disk

CIRCUITPY_PARTITION_DISK = 1

# Was a #define in mpconfigboard.h; upstream has since made this a make variable
# with a -D of its own, which the header then redefined.
CIRCUITPY_DISPLAY_AREA_BUFFER_SIZE = 4096
