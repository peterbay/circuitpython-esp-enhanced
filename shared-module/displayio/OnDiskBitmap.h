// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"

#include "extmod/vfs_fat.h"

typedef struct {
    mp_obj_base_t base;
    // CIRCUITPY-CHANGE: width, height and the data offset come out of a 32 bit
    // header and the stride is computed from the width, but all four were stored
    // in 16 bits. A negative height, which is how a valid top-down BMP is written,
    // became a large positive one, and a file whose pixel data starts past 65535
    // seeked somewhere else entirely.
    uint32_t width;
    uint32_t height;
    uint32_t data_offset;
    uint32_t stride;
    uint32_t r_bitmask;
    uint32_t g_bitmask;
    uint32_t b_bitmask;
    // CIRCUITPY-CHANGE: one row of the file, so that get_pixel does not seek and
    // read per pixel. cached_row holds the file row currently in it, counted the
    // way get_pixel counts, or UINT32_MAX when nothing is cached.
    uint8_t *row_cache;
    uint32_t cached_row;
    pyb_file_obj_t *file;
    union {
        mp_obj_base_t *pixel_shader_base;
        struct displayio_palette *palette;
        struct displayio_colorconverter *colorconverter;
    };
    bool bitfield_compressed;
    uint8_t bits_per_pixel;
} displayio_ondiskbitmap_t;
