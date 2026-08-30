// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared/runtime/buffer_helper.h"

void normalize_buffer_bounds(int32_t *start, int32_t end, size_t *length) {
    if (end < 0) {
        end += *length;
    } else if (((size_t)end) > *length) {
        end = *length;
    }
    if (*start < 0) {
        *start += *length;
        // CIRCUITPY-CHANGE: a start below -length stayed negative, and the caller
        // then computed buf + start and a length reaching past the end. From Python:
        // spi.readinto(bytearray(10), start=-11) wrote 11 bytes from buf - 1, and
        // i2c.readfrom_into(addr, bytearray(10), start=-1000) wrote 1000 from
        // buf - 990. CPython clamps the same way: bytearray(10)[-1000:] is the whole
        // object. Every caller in the tree relies on this function for the bound.
        if (*start < 0) {
            *start = 0;
        }
    }
    if (end < *start) {
        *length = 0;
    } else {
        *length = end - *start;
    }
}
