// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 microDev
//
// SPDX-License-Identifier: MIT

#pragma once

#include "driver/spi_master.h"
#include "shared-bindings/microcontroller/Pin.h"

typedef struct {
    mp_obj_base_t base;

    const mcu_pin_obj_t *MOSI;
    const mcu_pin_obj_t *MISO;
    const mcu_pin_obj_t *clock;

    spi_host_device_t host_id;

    uint8_t bits;
    uint8_t phase;
    uint8_t polarity;
    // CIRCUITPY-CHANGE: the rate the caller asked for, so configure() can tell a
    // repeat of the same request from a new one, and the rate the peripheral
    // actually runs at, which is what the frequency property reports.
    uint32_t baudrate;
    uint32_t achieved_baudrate;

    SemaphoreHandle_t mutex;
} busio_spi_obj_t;
