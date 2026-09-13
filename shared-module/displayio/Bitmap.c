// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/displayio/Bitmap.h"

#include <string.h>

#include "py/runtime.h"
#include "py/gc.h"

enum { ALIGN_BITS = 8 * sizeof(uint32_t) };

static int stride(uint32_t width, uint32_t bits_per_value) {
    uint32_t row_width = width * bits_per_value;
    // align to uint32_t
    return (row_width + ALIGN_BITS - 1) / ALIGN_BITS;
}

void common_hal_displayio_bitmap_construct(displayio_bitmap_t *self, uint32_t width,
    uint32_t height, uint32_t bits_per_value) {
    common_hal_displayio_bitmap_construct_from_buffer(self, width, height, bits_per_value, NULL, false);
}

void common_hal_displayio_bitmap_construct_from_buffer(displayio_bitmap_t *self, uint32_t width,
    uint32_t height, uint32_t bits_per_value, uint32_t *data, bool read_only) {
    self->width = width;
    self->height = height;
    self->stride = stride(width, bits_per_value);
    self->data_alloc = false;
    if (!data) {
        data = m_malloc_without_collect(self->stride * height * sizeof(uint32_t));
        self->data_alloc = true;
    }
    self->data = data;
    self->read_only = read_only;
    self->bits_per_value = bits_per_value;

    if (bits_per_value > 8 && bits_per_value != 16 && bits_per_value != 32) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("Invalid bits per value"));
    }

    // Division and modulus can be slow because it has to handle any integer. We know bits_per_value
    // is a power of two. We divide and mod by bits_per_value to compute the offset into the byte
    // array. So, we can the offset computation to simplify to a shift for division and mask for mod.

    self->x_shift = 0; // Used to divide the index by the number of pixels per word. Its used in a
                       // shift which effectively divides by 2 ** x_shift.
    uint32_t power_of_two = 1;
    while (power_of_two < 8 / bits_per_value) {
        self->x_shift++;
        power_of_two <<= 1;
    }
    self->x_mask = (1u << self->x_shift) - 1u; // Used as a modulus on the x value
    self->bitmask = (1u << bits_per_value) - 1u;

    self->dirty_area.x1 = 0;
    self->dirty_area.x2 = width;
    self->dirty_area.y1 = 0;
    self->dirty_area.y2 = height;
}

void common_hal_displayio_bitmap_deinit(displayio_bitmap_t *self) {
    if (self->data_alloc) {
        gc_free(self->data);
    }
    self->data = NULL;
}

bool common_hal_displayio_bitmap_deinited(displayio_bitmap_t *self) {
    return self->data == NULL;
}

uint16_t common_hal_displayio_bitmap_get_height(displayio_bitmap_t *self) {
    return self->height;
}

uint16_t common_hal_displayio_bitmap_get_width(displayio_bitmap_t *self) {
    return self->width;
}

uint32_t common_hal_displayio_bitmap_get_bits_per_value(displayio_bitmap_t *self) {
    return self->bits_per_value;
}

uint32_t common_hal_displayio_bitmap_get_pixel(displayio_bitmap_t *self, int16_t x, int16_t y) {
    if (x >= self->width || x < 0 || y >= self->height || y < 0) {
        return 0;
    }
    int32_t row_start = y * self->stride;
    uint32_t *row = self->data + row_start;
    uint8_t bytes_per_value = self->bits_per_value / 8;
    uint8_t values_per_byte = 8 / self->bits_per_value;
    if (bytes_per_value < 1) {
        uint8_t bits = ((uint8_t *)row)[x >> self->x_shift];
        uint8_t bit_position = (values_per_byte - (x & self->x_mask) - 1) * self->bits_per_value;
        return (bits >> bit_position) & self->bitmask;
    } else {
        if (bytes_per_value == 1) {
            return ((uint8_t *)row)[x];
        } else if (bytes_per_value == 2) {
            return ((uint16_t *)row)[x];
        } else if (bytes_per_value == 4) {
            return ((uint32_t *)row)[x];
        }
    }
    return 0;
}

void displayio_bitmap_set_dirty_area(displayio_bitmap_t *self, const displayio_area_t *dirty_area) {
    if (self->read_only) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Read-only"));
    }

    displayio_area_t area = *dirty_area;
    displayio_area_canon(&area);
    displayio_area_union(&area, &self->dirty_area, &area);
    displayio_area_t bitmap_area = {0, 0, self->width, self->height, NULL};
    displayio_area_compute_overlap(&area, &bitmap_area, &self->dirty_area);
}

void displayio_bitmap_write_pixel(displayio_bitmap_t *self, int16_t x, int16_t y, uint32_t value) {
    if (self->read_only) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Read-only"));
    }
    // Writes the color index value into a pixel position
    // Must update the dirty area separately

    // Don't write if out of area
    if (0 > x || x >= self->width || 0 > y || y >= self->height) {
        return;
    }

    // Update one pixel of data
    int32_t row_start = y * self->stride;
    uint32_t *row = self->data + row_start;
    uint8_t bytes_per_value = self->bits_per_value / 8;
    uint8_t values_per_byte = 8 / self->bits_per_value;
    if (bytes_per_value < 1) {
        uint8_t bits = ((uint8_t *)row)[x >> self->x_shift];
        uint8_t bit_position = (values_per_byte - (x & self->x_mask) - 1) * self->bits_per_value;
        bits &= ~(self->bitmask << bit_position);
        bits |= (value & self->bitmask) << bit_position;
        ((uint8_t *)row)[x >> self->x_shift] = bits;
    } else {
        if (bytes_per_value == 1) {
            ((uint8_t *)row)[x] = value;
        } else if (bytes_per_value == 2) {
            ((uint16_t *)row)[x] = value;
        } else if (bytes_per_value == 4) {
            ((uint32_t *)row)[x] = value;
        }
    }
}

// CIRCUITPY-CHANGE: span operations, see the header. The row accessors they are
// built on live there too, because the drawing primitives need them as well.


// Whether the pixels tile a byte exactly. 1, 2 and 4 bits per pixel do, and
// only then can a run of whole bytes be written without reading them first.
// The odd widths below a byte store one pixel per byte with the top bits spare,
// and those spare bits are left alone rather than assumed to be unused.
static inline bool sub_byte_packed(const displayio_bitmap_t *self) {
    return (self->x_mask + 1) * self->bits_per_value == 8;
}

void displayio_bitmap_fill_span(displayio_bitmap_t *self, int16_t x, int16_t y,
    uint16_t length, uint32_t value) {
    if (self->read_only) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Read-only"));
    }
    if (y < 0 || y >= self->height) {
        return;
    }
    int32_t start = x;
    int32_t end = start + (int32_t)length;
    if (start < 0) {
        start = 0;
    }
    if (end > self->width) {
        end = self->width;
    }
    if (start >= end) {
        return;
    }

    uint32_t *row = displayio_bitmap_row(self, y);
    switch (self->bits_per_value) {
        case 8:
            memset((uint8_t *)row + start, (uint8_t)value, (size_t)(end - start));
            return;
        case 16: {
            uint16_t *row16 = (uint16_t *)row;
            for (int32_t i = start; i < end; i++) {
                row16[i] = (uint16_t)value;
            }
            return;
        }
        case 32:
            for (int32_t i = start; i < end; i++) {
                row[i] = value;
            }
            return;
        default:
            break;
    }

    uint32_t masked = value & self->bitmask;
    if (!sub_byte_packed(self)) {
        for (int32_t i = start; i < end; i++) {
            displayio_bitmap_row_put(self, row, (uint32_t)i, masked);
        }
        return;
    }

    // The two ends of the run share a byte with pixels that have to survive, so
    // only what lies between them can be written flat.
    uint8_t pattern = 0;
    for (uint32_t i = 0; i <= self->x_mask; i++) {
        pattern = (uint8_t)((pattern << self->bits_per_value) | masked);
    }
    while (start < end && (start & (int32_t)self->x_mask) != 0) {
        displayio_bitmap_row_put(self, row, (uint32_t)start, masked);
        start++;
    }
    int32_t whole_bytes = (end - start) >> self->x_shift;
    if (whole_bytes > 0) {
        memset((uint8_t *)row + (start >> self->x_shift), pattern, (size_t)whole_bytes);
        start += whole_bytes << self->x_shift;
    }
    while (start < end) {
        displayio_bitmap_row_put(self, row, (uint32_t)start, masked);
        start++;
    }
}

void displayio_bitmap_read_span(displayio_bitmap_t *self, int16_t x, int16_t y,
    uint16_t length, uint32_t *values) {
    if (length == 0) {
        return;
    }
    int32_t start = x;
    int32_t end = start + (int32_t)length;
    int32_t first = start < 0 ? 0 : start;
    int32_t last = end > self->width ? self->width : end;
    if (y < 0 || y >= self->height || first >= last) {
        memset(values, 0, (size_t)length * sizeof(uint32_t));
        return;
    }
    // Whatever hangs off either edge reads as 0, so the caller's indexing still
    // lines up with the run it asked for.
    for (int32_t i = start; i < first; i++) {
        values[i - start] = 0;
    }
    for (int32_t i = last; i < end; i++) {
        values[i - start] = 0;
    }

    const uint32_t *row = displayio_bitmap_row(self, y);
    uint32_t *out = values + (first - start);
    switch (self->bits_per_value) {
        case 8: {
            const uint8_t *row8 = (const uint8_t *)row;
            for (int32_t i = first; i < last; i++) {
                *out++ = row8[i];
            }
            return;
        }
        case 16: {
            const uint16_t *row16 = (const uint16_t *)row;
            for (int32_t i = first; i < last; i++) {
                *out++ = row16[i];
            }
            return;
        }
        case 32:
            for (int32_t i = first; i < last; i++) {
                *out++ = row[i];
            }
            return;
        default:
            for (int32_t i = first; i < last; i++) {
                *out++ = displayio_bitmap_row_get(self, row, (uint32_t)i);
            }
            return;
    }
}

void displayio_bitmap_copy_span(displayio_bitmap_t *destination, int16_t x, int16_t y,
    displayio_bitmap_t *source, int16_t source_x, int16_t source_y, uint16_t length) {
    if (destination->read_only) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Read-only"));
    }
    if (y < 0 || y >= destination->height || source_y < 0 || source_y >= source->height) {
        return;
    }

    // Clip in the run's own coordinate so that both ends stay paired.
    int32_t first = 0;
    int32_t last = (int32_t)length;
    if (-(int32_t)x > first) {
        first = -(int32_t)x;
    }
    if (-(int32_t)source_x > first) {
        first = -(int32_t)source_x;
    }
    if ((int32_t)destination->width - x < last) {
        last = (int32_t)destination->width - x;
    }
    if ((int32_t)source->width - source_x < last) {
        last = (int32_t)source->width - source_x;
    }
    if (first >= last) {
        return;
    }
    int32_t dst = x + first;
    int32_t src = source_x + first;
    int32_t count = last - first;

    uint32_t *drow = displayio_bitmap_row(destination, y);
    uint32_t *srow = displayio_bitmap_row(source, source_y);
    bool same_row = source == destination && source_y == y;

    if (source->bits_per_value == destination->bits_per_value) {
        switch (destination->bits_per_value) {
            case 8:
                memmove((uint8_t *)drow + dst, (const uint8_t *)srow + src, (size_t)count);
                return;
            case 16:
                memmove((uint16_t *)drow + dst, (const uint16_t *)srow + src,
                    (size_t)count * sizeof(uint16_t));
                return;
            case 32:
                memmove(drow + dst, srow + src, (size_t)count * sizeof(uint32_t));
                return;
            default:
                break;
        }
        // Runs that sit at the same offset inside their bytes share the byte
        // boundaries, so everything between the partial ends moves wholesale.
        // Overlapping runs within one row are left to the pixel loop below,
        // where the direction is chosen rather than assumed.
        if (sub_byte_packed(destination) && !same_row &&
            (dst & (int32_t)destination->x_mask) == (src & (int32_t)source->x_mask)) {
            while (count > 0 && (dst & (int32_t)destination->x_mask) != 0) {
                displayio_bitmap_row_put(destination, drow, (uint32_t)dst, displayio_bitmap_row_get(source, srow, (uint32_t)src));
                dst++;
                src++;
                count--;
            }
            int32_t whole_bytes = count >> destination->x_shift;
            if (whole_bytes > 0) {
                memmove((uint8_t *)drow + (dst >> destination->x_shift),
                    (const uint8_t *)srow + (src >> source->x_shift), (size_t)whole_bytes);
                int32_t moved = whole_bytes << destination->x_shift;
                dst += moved;
                src += moved;
                count -= moved;
            }
            while (count > 0) {
                displayio_bitmap_row_put(destination, drow, (uint32_t)dst, displayio_bitmap_row_get(source, srow, (uint32_t)src));
                dst++;
                src++;
                count--;
            }
            return;
        }
    }

    // Formats that differ, or a packing the moves above cannot take wholesale.
    // Still one row and no bounds test per pixel, only the unpacking remains.
    bool overlaps = same_row && dst < src + count && src < dst + count;
    if (overlaps && dst > src) {
        for (int32_t i = count - 1; i >= 0; i--) {
            displayio_bitmap_row_put(destination, drow, (uint32_t)(dst + i), displayio_bitmap_row_get(source, srow, (uint32_t)(src + i)));
        }
        return;
    }
    for (int32_t i = 0; i < count; i++) {
        displayio_bitmap_row_put(destination, drow, (uint32_t)(dst + i), displayio_bitmap_row_get(source, srow, (uint32_t)(src + i)));
    }
}

void common_hal_displayio_bitmap_set_pixel(displayio_bitmap_t *self, int16_t x, int16_t y, uint32_t value) {
    if (self->read_only) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Read-only"));
    }
    // update the dirty region
    displayio_area_t a = {x, y, x + 1, y + 1, NULL};
    displayio_bitmap_set_dirty_area(self, &a);

    // write the pixel
    displayio_bitmap_write_pixel(self, x, y, value);

}

displayio_area_t *displayio_bitmap_get_refresh_areas(displayio_bitmap_t *self, displayio_area_t *tail) {
    if (self->dirty_area.x1 == self->dirty_area.x2 || self->read_only) {
        return tail;
    }
    self->dirty_area.next = tail;
    return &self->dirty_area;
}

void displayio_bitmap_finish_refresh(displayio_bitmap_t *self) {
    if (self->read_only) {
        return;
    }
    self->dirty_area.x1 = 0;
    self->dirty_area.x2 = 0;
}

void common_hal_displayio_bitmap_fill(displayio_bitmap_t *self, uint32_t value) {
    if (self->read_only) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Read-only"));
    }
    displayio_area_t a = {0, 0, self->width, self->height, NULL};
    displayio_bitmap_set_dirty_area(self, &a);

    // build the packed word
    uint32_t word = 0;
    for (uint8_t i = 0; i < 32 / self->bits_per_value; i++) {
        word |= (value & self->bitmask) << (32 - ((i + 1) * self->bits_per_value));
    }
    // copy it in
    for (uint32_t i = 0; i < self->stride * self->height; i++) {
        self->data[i] = word;
    }
}

int common_hal_displayio_bitmap_get_buffer(displayio_bitmap_t *self, mp_buffer_info_t *bufinfo, mp_uint_t flags) {
    if ((flags & MP_BUFFER_WRITE) && self->read_only) {
        return 1;
    }
    bufinfo->len = self->stride * self->height * sizeof(uint32_t);
    bufinfo->buf = self->data;
    switch (self->bits_per_value) {
        case 32:
            bufinfo->typecode = 'I';
            break;
        case 16:
            bufinfo->typecode = 'H';
            break;
        default:
            bufinfo->typecode = 'B';
            break;
    }
    return 0;
}
