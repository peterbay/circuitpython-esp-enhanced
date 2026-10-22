// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2025 SamantazFox
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/displayio/OnDiskBitmap.h"
#include "shared-bindings/displayio/ColorConverter.h"
#include "shared-bindings/displayio/Palette.h"
#include "shared-module/displayio/ColorConverter.h"
#include "shared-module/displayio/Palette.h"

#include <string.h>

#include "py/mperrno.h"
#include "py/runtime.h"
#include "py/stream.h"
#if CIRCUITPY_STORAGE_MAP_FILE
#include "shared-bindings/storage/__init__.h"
#endif


#define DISPLAYIO_ODBMP_DEBUG(...) (void)0
// #define DISPLAYIO_ODBMP_DEBUG(...) mp_printf(&mp_plat_print __VA_OPT__(,) __VA_ARGS__)


#if CIRCUITPY_STORAGE_MAP_FILE
static void add_run(void *self_in, const uint8_t *addr, size_t len) {
    displayio_ondiskbitmap_t *self = self_in;
    if (self->map_count < MP_ARRAY_SIZE(self->map)) {
        self->map[self->map_count].addr = addr;
        self->map[self->map_count].len = len;
        self->map_count++;
    } else {
        self->map_count = MP_ARRAY_SIZE(self->map) + 1;  // too fragmented: read through the cache
    }
}

// Flash address of len bytes at offset if they lie in one run, else NULL.
static const uint8_t *map_at(displayio_ondiskbitmap_t *self, uint32_t offset, uint32_t len) {
    for (uint8_t i = 0; i < self->map_count; i++) {
        if (offset < self->map[i].len) {
            return offset + len <= self->map[i].len ? self->map[i].addr + offset : NULL;
        }
        offset -= self->map[i].len;
    }
    return NULL;
}

const uint8_t *displayio_ondiskbitmap_row(displayio_ondiskbitmap_t *self, int16_t y) {
    return map_at(self, self->data_offset + (self->height - y - 1) * self->stride, self->width);
}
#endif

static uint32_t read_word(uint16_t *bmp_header, uint16_t index) {
    // CIRCUITPY-CHANGE: the high half promotes to signed int before the shift, so a
    // value from 0x8000 up produced a result the type cannot represent.
    return bmp_header[index] | ((uint32_t)bmp_header[index + 1] << 16);
}

void common_hal_displayio_ondiskbitmap_construct(displayio_ondiskbitmap_t *self, mp_obj_t file) {
    self->file = file;
    self->cache_len = 0;
    // CIRCUITPY-CHANGE: a 12 byte BITMAPCOREHEADER only fills the start of this,
    // and bits_per_pixel below is read from a word past it. Zeroing does not make
    // the parse correct, but it stops it reading whatever was on the stack.
    uint16_t bmp_header[69] = {0};
    int errcode = 0;

    mp_stream_seek(file, 0, MP_SEEK_SET, &errcode);
    if (errcode != 0) {
        mp_raise_OSError(errcode);
    }

    // Read the minimum amount of bytes required to parse a BITMAPCOREHEADER.
    // If needed, we will read more bytes down below.
    mp_uint_t bytes_read = mp_stream_rw(file, bmp_header, 26, &errcode, MP_STREAM_RW_ONCE);
    if (errcode != 0) {
        mp_raise_OSError(errcode);
    }
    if (bytes_read != 26 || memcmp(bmp_header, "BM", 2) != 0) {
        mp_arg_error_invalid(MP_QSTR_file);
    }

    // Read header size to determine if more header bytes needs to be read.
    uint32_t header_size = read_word(bmp_header, 7);
    DISPLAYIO_ODBMP_DEBUG("header_size: %d\n", header_size);

    if (header_size == 40 || header_size == 108 || header_size == 124) {
        // Read the remaining header bytes
        bytes_read = mp_stream_rw(file, bmp_header + 13, header_size - 12, &errcode, MP_STREAM_RW_ONCE);
        if (errcode != 0) {
            mp_raise_OSError(errcode);
        }
        DISPLAYIO_ODBMP_DEBUG("bytes_read: %d\n", bytes_read);
        if (bytes_read != (header_size - 12)) {
            mp_arg_error_invalid(MP_QSTR_file);
        }
    } else if (header_size != 12) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("Only Windows format, uncompressed BMP supported: given header size is %d"), header_size);
    }


    uint32_t compression = read_word(bmp_header, 15);
    DISPLAYIO_ODBMP_DEBUG("compression: %d\n", compression);

    // 0 is uncompressed; 3 is bitfield compressed. 1 and 2 are RLE compression.
    if (compression != 0 && compression != 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("RLE-compressed BMP not supported"));
    }

    // We can't cast because we're not aligned.
    self->data_offset = read_word(bmp_header, 5);

    self->bitfield_compressed = (compression == 3);
    // CIRCUITPY-CHANGE: a BITMAPCOREHEADER was accepted but then read entirely
    // through Windows INFOHEADER offsets: 16 bit width and height at bytes 18 and
    // 20 were read as one 32 bit width, the bit count at byte 24 was read from byte
    // 28, and compression was read from bytes the file does not even have. Every
    // field of a CORE bitmap was wrong. Parse it where it actually lives.
    if (header_size == 12) {
        self->width = bmp_header[9];
        self->height = bmp_header[10];
        self->bits_per_pixel = bmp_header[12];
    } else {
        self->bits_per_pixel = bmp_header[14];
        self->width = read_word(bmp_header, 9);
        int32_t signed_height = (int32_t)read_word(bmp_header, 11);
        // A negative height is a valid top-down BMP; it used to become a huge
        // positive one on the way into a uint16_t field.
        self->height = signed_height < 0 ? (uint32_t)-signed_height : (uint32_t)signed_height;
    }

    // CIRCUITPY-CHANGE: the depth came straight out of the file and nothing checked
    // it. Zero divides by zero computing pixels_per_byte; 9 to 15 make
    // pixels_per_byte zero and the single-byte branch in get_pixel then takes
    // x % 0; and from 40 up bytes_per_pixel is 5 or more, so the f_read in
    // get_pixel reads that many bytes into a uint32_t local and walks the stack.
    // Only the depths the rest of this file actually handles are allowed through.
    switch (self->bits_per_pixel) {
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
        case 24:
        case 32:
            break;
        default:
            mp_raise_ValueError_varg(MP_ERROR_TEXT("Invalid %q"), MP_QSTR_bits_per_pixel);
    }

    DISPLAYIO_ODBMP_DEBUG("data offset: %d\n", self->data_offset);
    DISPLAYIO_ODBMP_DEBUG("width: %d\n", self->width);
    DISPLAYIO_ODBMP_DEBUG("height: %d\n", self->height);
    DISPLAYIO_ODBMP_DEBUG("bpp: %d\n", self->bits_per_pixel);


    displayio_colorconverter_t *colorconverter =
        mp_obj_malloc(displayio_colorconverter_t, &displayio_colorconverter_type);
    common_hal_displayio_colorconverter_construct(colorconverter, false, DISPLAYIO_COLORSPACE_RGB888);
    self->colorconverter = colorconverter;

    if (self->bits_per_pixel == 16) {
        if (((header_size >= 56)) || (self->bitfield_compressed)) {
            self->r_bitmask = read_word(bmp_header, 27);
            self->g_bitmask = read_word(bmp_header, 29);
            self->b_bitmask = read_word(bmp_header, 31);

        } else { // no compression or short header means 5:5:5
            self->r_bitmask = 0x7c00;
            self->g_bitmask = 0x3e0;
            self->b_bitmask = 0x1f;
        }
    } else if (self->bits_per_pixel <= 8) { // indexed
        uint32_t number_of_colors = 0;
        if (header_size >= 40) {
            number_of_colors = read_word(bmp_header, 23);
        }
        // CIRCUITPY-CHANGE: colors_used is a 32 bit field in the file, but the
        // palette constructor, the size below and the loop counter were all 16 bit.
        // A large declared count truncated differently in different places, so the
        // allocation stopped matching the iteration and the counter could wrap. An
        // indexed BMP can never have more entries than its depth allows, so clamp
        // there and keep the arithmetic in size_t.
        uint32_t max_colors = 1u << self->bits_per_pixel;
        if (number_of_colors == 0 || number_of_colors > max_colors) {
            number_of_colors = max_colors;
        }

        displayio_palette_t *palette = mp_obj_malloc(displayio_palette_t, &displayio_palette_type);
        common_hal_displayio_palette_construct(palette, number_of_colors, false);

        if (number_of_colors > 1) {
            // CIRCUITPY-CHANGE: a BITMAPCOREHEADER palette is 3 byte RGBTRIPLEs, not
            // 4 byte RGBQUADs. Reading it as quads shifted every colour and ran into
            // the pixel data.
            size_t entry_size = (header_size == 12) ? 3 : sizeof(uint32_t);
            size_t palette_size = (size_t)number_of_colors * entry_size;
            uint16_t palette_offset = 0xe + header_size;

            uint32_t *palette_data = m_malloc_without_collect(
                (size_t)number_of_colors * sizeof(uint32_t));

            mp_stream_seek(file, palette_offset, MP_SEEK_SET, &errcode);
            if (errcode != 0) {
                mp_raise_OSError(errcode);
            }

            mp_uint_t palette_bytes_read = mp_stream_rw(file, palette_data, palette_size, &errcode, MP_STREAM_RW_ONCE);
            if (errcode != 0) {
                mp_raise_OSError(errcode);
            }
            if (palette_bytes_read != palette_size) {
                mp_raise_ValueError(MP_ERROR_TEXT("Unable to read color palette data"));
            }
            const uint8_t *raw = (const uint8_t *)palette_data;
            for (uint32_t i = 0; i < number_of_colors; i++) {
                // Both layouts store blue, green, red in that order; the quad has a
                // fourth ignored byte.
                const uint8_t *e = raw + (size_t)i * entry_size;
                common_hal_displayio_palette_set_color(palette, i,
                    ((uint32_t)e[2] << 16) | ((uint32_t)e[1] << 8) | e[0]);
            }

            #if MICROPY_MALLOC_USES_ALLOCATED_SIZE
            m_free(palette_data, (size_t)number_of_colors * sizeof(uint32_t));
            #else
            m_free(palette_data);
            #endif
        } else {
            // CIRCUITPY-CHANGE: one declared colour allocates one entry, but this
            // wrote index 1 as well, one past the palette.
            common_hal_displayio_palette_set_color(palette, 0, 0x0);
        }
        self->palette = palette;
    }

    uint8_t bytes_per_pixel = (self->bits_per_pixel / 8)  ? (self->bits_per_pixel / 8) : 1;
    uint8_t pixels_per_byte = 8 / self->bits_per_pixel;
    if (pixels_per_byte == 0) {
        self->stride = (self->width * bytes_per_pixel);
        // Rows are word aligned.
        if (self->stride % 4 != 0) {
            self->stride += 4 - self->stride % 4;
        }
    } else {
        uint32_t bit_stride = self->width * self->bits_per_pixel;
        if (bit_stride % 32 != 0) {
            bit_stride += 32 - bit_stride % 32;
        }
        self->stride = (bit_stride / 8);
    }

    #if CIRCUITPY_STORAGE_MAP_FILE
    self->map_count = 0;
    if (storage_map_runs(file, add_run, self) != 0 || self->map_count > MP_ARRAY_SIZE(self->map)) {
        self->map_count = 0;
    }
    // BMP rows don't start on a 4-byte boundary, so in a split file some row crosses a run.
    self->rows_mapped = self->bits_per_pixel == 8 &&
        map_at(self, self->data_offset, (uint32_t)(self->height - 1) * self->stride + self->width) != NULL;
    #endif
}


uint32_t common_hal_displayio_ondiskbitmap_get_pixel(displayio_ondiskbitmap_t *self,
    int16_t x, int16_t y) {
    // The dimensions are unsigned now, so compare after establishing the sign.
    if (x < 0 || y < 0 || (uint32_t)x >= self->width || (uint32_t)y >= self->height) {
        return 0;
    }

    uint8_t bytes_per_pixel = (self->bits_per_pixel / 8)  ? (self->bits_per_pixel / 8) : 1;
    uint8_t pixels_per_byte = 8 / self->bits_per_pixel;

    uint32_t location;
    if (pixels_per_byte == 0) {
        location = self->data_offset + (self->height - y - 1) * self->stride + x * bytes_per_pixel;
    } else {
        location = self->data_offset + (self->height - y - 1) * self->stride + x / pixels_per_byte;
    }
    uint32_t pixel_data = 0;
    #if CIRCUITPY_STORAGE_MAP_FILE
    if (self->map_count > 0) {
        // Byte by byte, so a pixel split across two runs reads right too. The cache can't
        // help there: it shares memory with the run table.
        for (uint8_t i = 0; i < bytes_per_pixel; i++) {
            const uint8_t *p = map_at(self, location + i, 1);
            if (p == NULL) {
                return 0;
            }
            ((uint8_t *)&pixel_data)[i] = *p;
        }
    } else
    #endif
    {
        int errcode = 0;
        if (location < self->cache_start || location + bytes_per_pixel > self->cache_start + self->cache_len) {
            mp_stream_seek(self->file, location, MP_SEEK_SET, &errcode);
            if (errcode != 0) {
                return 0;
            }
            self->cache_start = location;
            self->cache_len = mp_stream_rw(self->file, self->cache, sizeof(self->cache), &errcode, MP_STREAM_RW_ONCE);
            if (errcode != 0 || self->cache_len < bytes_per_pixel) {
                self->cache_len = 0;
                return 0;
            }
        }
        memcpy(&pixel_data, self->cache + (location - self->cache_start), bytes_per_pixel);
    }
    uint32_t tmp = 0;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    if (bytes_per_pixel == 1) {
        uint8_t offset = (x % pixels_per_byte) * self->bits_per_pixel;
        uint8_t mask = (1 << self->bits_per_pixel) - 1;

        return (pixel_data >> ((8 - self->bits_per_pixel) - offset)) & mask;
    } else if (bytes_per_pixel == 2) {
        if (self->g_bitmask == 0x07e0) { // 565
            red = ((pixel_data & self->r_bitmask) >> 11);
            green = ((pixel_data & self->g_bitmask) >> 5);
            blue = ((pixel_data & self->b_bitmask) >> 0);
        } else { // 555
            red = ((pixel_data & self->r_bitmask) >> 10);
            green = ((pixel_data & self->g_bitmask) >> 4);
            blue = ((pixel_data & self->b_bitmask) >> 0);
        }
        tmp = (red << 19 | green << 10 | blue << 3);
        return tmp;
    } else if ((bytes_per_pixel == 4) && (self->bitfield_compressed)) {
        return pixel_data & 0x00FFFFFF;
    } else {
        return pixel_data;
    }
}

uint16_t common_hal_displayio_ondiskbitmap_get_height(displayio_ondiskbitmap_t *self) {
    return self->height;
}

uint16_t common_hal_displayio_ondiskbitmap_get_width(displayio_ondiskbitmap_t *self) {
    return self->width;
}

mp_obj_t common_hal_displayio_ondiskbitmap_get_pixel_shader(displayio_ondiskbitmap_t *self) {
    return MP_OBJ_FROM_PTR(self->pixel_shader_base);
}
