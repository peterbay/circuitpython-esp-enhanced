// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2020 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include "supervisor/filesystem.h"
#include "supervisor/port.h"
#include "supervisor/shared/stack.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CIRCUITPY_ESP_USB_SERIAL_JTAG
#include "supervisor/usb_serial_jtag.h"
#endif

void port_background_tick(void) {
    // Yield with zero delay in case FreeRTOS wants to switch to something else.
    port_task_yield();
}

void port_background_task(void) {
    #if CIRCUITPY_ESP_USB_SERIAL_JTAG
    // CIRCUITPY-CHANGE: pick up console input the receive interrupt missed.
    // Runs from the VM loop rather than the tick, because the tick timer is only
    // started when something asks for it, while this runs for as long as Python
    // code does. See usb_serial_jtag_rx_tick() for what it is recovering from.
    usb_serial_jtag_rx_tick();
    #endif
}

void port_start_background_tick(void) {
}

void port_finish_background_tick(void) {
}
