// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Peter Vavrin
//
// SPDX-License-Identifier: MIT

#pragma once

// Board setup
//
// Pin assignments are taken from the Seeed wiki page for this board and have not
// yet been checked against the hardware. Everything below is a candidate until
// it has been read off a real board.

#define MICROPY_HW_BOARD_NAME       "Seeed Studio XIAO ESP32-C5"
#define MICROPY_HW_MCU_NAME         "ESP32-C5"

// User LED, marked "Light_Yellow" on the wiki.
#define MICROPY_HW_LED_STATUS (&pin_GPIO27)

#define CIRCUITPY_BOARD_I2C         (1)
#define CIRCUITPY_BOARD_I2C_PIN     {{.scl = &pin_GPIO24, .sda = &pin_GPIO23}}

#define CIRCUITPY_BOARD_SPI         (1)
#define CIRCUITPY_BOARD_SPI_PIN     {{.clock = &pin_GPIO8, .mosi = &pin_GPIO10, .miso = &pin_GPIO9}}

#define CIRCUITPY_BOARD_UART        (1)
#define CIRCUITPY_BOARD_UART_PIN    {{.tx = &pin_GPIO11, .rx = &pin_GPIO12}}

// For entering safe mode, use BOOT button
#define CIRCUITPY_BOOT_BUTTON       (&pin_GPIO28)

// The XIAO ESP32-C6 reduces tx power to 15 for its antenna. Whether this board
// wants the same is not known; it ships with a U.FL connector and an external
// antenna, so the default is left alone until it can be measured.
