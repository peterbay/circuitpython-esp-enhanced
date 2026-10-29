# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(ENV{IDF_PATH} ${CMAKE_SOURCE_DIR}/esp-idf)

# The component list here determines what options we get in menuconfig and what the ninja file can build.
set(COMPONENTS bt driver esp_driver_dac esp_driver_gpio esp_driver_gptimer esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_pcnt esp_driver_rmt esp_driver_sdmmc esp_driver_spi esp_driver_touch_sens esp_driver_tsens esp_driver_uart esp_eth esp-tls esp_adc esp_adc_cal esp_event esp_netif esp_psram esp_security esp_wifi esptool_py freertos log lwip main mbedtls mdns pthread soc ulp wpa_supplicant esp-camera esp_lcd vfs esp_vfs_console esp_stdio sdmmc usb)
set(EXTRA_COMPONENT_DIRS "esp-protocols/components/mdns" "esp-camera")

if(CIRCUITPY_ESP_HOSTED)
    list(APPEND EXTRA_COMPONENT_DIRS "esp-hosted")
    list(APPEND COMPONENTS esp-hosted)
endif()

# CIRCUITPY-CHANGE: this fork's modules; the switches are exported by fork.mk.
if("$ENV{CIRCUITPY_IEEE802154}" STREQUAL "1")
    list(APPEND COMPONENTS esp_hal_ieee802154 ieee802154)
endif()
if("$ENV{CIRCUITPY_ESPIDF_SDM}" STREQUAL "1")
    list(APPEND COMPONENTS esp_driver_sdm)
endif()
if("$ENV{CIRCUITPY_ESPIDF_MCPWM}" STREQUAL "1")
    list(APPEND COMPONENTS esp_driver_mcpwm)
endif()
if("$ENV{CIRCUITPY_ESPIDF_ANA_CMPR}" STREQUAL "1")
    list(APPEND COMPONENTS esp_driver_ana_cmpr)
endif()
if("$ENV{CIRCUITPY_ZIGBEE}" STREQUAL "1")
    list(APPEND EXTRA_COMPONENT_DIRS "esp-zigbee-lib")
    list(APPEND COMPONENTS esp-zigbee-lib)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(ENV{ESP_IDF_VERSION} "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
project(circuitpython)

idf_build_set_property(__OUTPUT_SDKCONFIG 0)
