// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/displayio/area.h"

typedef struct {
    mp_obj_base_t base;
    uint16_t width;
    uint16_t height;
    uint32_t *data;
    uint16_t stride; // uint32_t's
    uint8_t bits_per_value;
    uint8_t x_shift;
    size_t x_mask;
    displayio_area_t dirty_area;
    uint16_t bitmask;
    bool read_only;
    bool data_alloc; // did bitmap allocate data or someone else
} displayio_bitmap_t;

// CIRCUITPY-CHANGE: one row of a bitmap, resolved once. displayio_bitmap_write_pixel
// and common_hal_displayio_bitmap_get_pixel re-derive the row address, re-read
// the format fields and re-check the bounds for every single pixel, and being
// in another translation unit they cannot be inlined away. A primitive that
// already knows its pixels are inside the bitmap can hold the row instead and
// pay none of that.
//
// The caller is responsible for the bounds: y must be inside the bitmap when
// the row is taken, and x when it is used. Neither is checked here.
static inline uint32_t *displayio_bitmap_row(const displayio_bitmap_t *self, int16_t y) {
    return self->data + (int32_t)y * self->stride;
}

// How far up its byte a pixel sits, for the formats narrower than one. The
// constructor reduced the count of pixels per byte to x_mask, so this is the
// same position the public accessors compute, without the division.
static inline uint32_t displayio_bitmap_sub_byte_shift(const displayio_bitmap_t *self, uint32_t x) {
    return (uint32_t)((self->x_mask - (x & self->x_mask)) * self->bits_per_value);
}

static inline uint32_t displayio_bitmap_row_get(const displayio_bitmap_t *self,
    const uint32_t *row, uint32_t x) {
    switch (self->bits_per_value) {
        case 8:
            return ((const uint8_t *)row)[x];
        case 16:
            return ((const uint16_t *)row)[x];
        case 32:
            return row[x];
        default: {
            uint8_t bits = ((const uint8_t *)row)[x >> self->x_shift];
            return (bits >> displayio_bitmap_sub_byte_shift(self, x)) & self->bitmask;
        }
    }
}

static inline void displayio_bitmap_row_put(const displayio_bitmap_t *self,
    uint32_t *row, uint32_t x, uint32_t value) {
    switch (self->bits_per_value) {
        case 8:
            ((uint8_t *)row)[x] = (uint8_t)value;
            return;
        case 16:
            ((uint16_t *)row)[x] = (uint16_t)value;
            return;
        case 32:
            row[x] = value;
            return;
        default: {
            uint8_t *slot = &((uint8_t *)row)[x >> self->x_shift];
            uint32_t shift = displayio_bitmap_sub_byte_shift(self, x);
            *slot = (uint8_t)((*slot & ~(self->bitmask << shift)) | ((value & self->bitmask) << shift));
            return;
        }
    }
}

void displayio_bitmap_finish_refresh(displayio_bitmap_t *self);
displayio_area_t *displayio_bitmap_get_refresh_areas(displayio_bitmap_t *self, displayio_area_t *tail);
void displayio_bitmap_set_dirty_area(displayio_bitmap_t *self, const displayio_area_t *area);
void displayio_bitmap_write_pixel(displayio_bitmap_t *self, int16_t x, int16_t y, uint32_t value);

// CIRCUITPY-CHANGE: horizontal runs. A drawing primitive that goes through
// displayio_bitmap_write_pixel pays for the bounds test, the row address and
// the unpacking of the storage format on every single pixel. These resolve all
// of that once and then walk one row, which for the byte wide formats collapses
// into a memset or a memmove over the whole run.
//
// Like displayio_bitmap_write_pixel they leave the dirty area to the caller.
// Runs are clipped to the bitmap, and read_span reports pixels outside it as 0,
// which is what common_hal_displayio_bitmap_get_pixel returns for them.
void displayio_bitmap_fill_span(displayio_bitmap_t *self, int16_t x, int16_t y,
    uint16_t length, uint32_t value);
void displayio_bitmap_read_span(displayio_bitmap_t *self, int16_t x, int16_t y,
    uint16_t length, uint32_t *values);
void displayio_bitmap_copy_span(displayio_bitmap_t *destination, int16_t x, int16_t y,
    displayio_bitmap_t *source, int16_t source_x, int16_t source_y, uint16_t length);
