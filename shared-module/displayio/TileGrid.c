// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/displayio/TileGrid.h"

#include "py/runtime.h"
#include "shared-bindings/displayio/Bitmap.h"
#include "shared-bindings/displayio/ColorConverter.h"
#include "shared-bindings/displayio/OnDiskBitmap.h"
#include "shared-bindings/displayio/Palette.h"
#if CIRCUITPY_TILEPALETTEMAPPER
#include "shared-bindings/tilepalettemapper/TilePaletteMapper.h"
#endif

#include "supervisor/shared/serial.h"

void common_hal_displayio_tilegrid_construct(displayio_tilegrid_t *self, mp_obj_t bitmap,
    uint16_t bitmap_width_in_tiles, uint16_t bitmap_height_in_tiles,
    mp_obj_t pixel_shader, uint16_t width, uint16_t height,
    uint16_t tile_width, uint16_t tile_height, uint16_t x, uint16_t y, uint16_t default_tile) {

    // CIRCUITPY-CHANGE: both operands are uint16_t, so this multiplied in int and
    // overflowed for a large grid -- 46341 x 46341 is 2147488281, past INT_MAX. The
    // byte count below then wrapped in size_t, the allocation succeeded small, and
    // the fill loop wrote the full count. The binding allows either dimension up to
    // 0xffff, so this was reachable from three lines of Python.
    uint32_t total_tiles = (uint32_t)width * (uint32_t)height;
    if (total_tiles > SIZE_MAX / sizeof(uint16_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Tile grid is too large"));
    }
    self->bitmap_width_in_tiles = bitmap_width_in_tiles;
    self->tiles_in_bitmap = bitmap_width_in_tiles * bitmap_height_in_tiles;

    // Determine if we need uint16_t or uint8_t for tile indices
    bool use_uint16 = self->tiles_in_bitmap > 255;

    // Sprites will only have one tile so save a little memory by inlining values in the pointer.
    uint8_t inline_tiles = sizeof(void *) / (use_uint16 ? sizeof(uint16_t) : sizeof(uint8_t));

    if (total_tiles <= inline_tiles) {
        self->tiles = 0;
        // Pack values into the pointer since there are only a few.
        if (use_uint16) {
            for (uint32_t i = 0; i < inline_tiles && i < total_tiles; i++) {
                ((uint16_t *)&self->tiles)[i] = default_tile;
            }
        } else {
            for (uint32_t i = 0; i < inline_tiles && i < total_tiles; i++) {
                ((uint8_t *)&self->tiles)[i] = (uint8_t)default_tile;
            }
        }
        self->inline_tiles = true;
    } else {
        if (use_uint16) {
            uint16_t *tiles16 = (uint16_t *)m_malloc_without_collect(total_tiles * sizeof(uint16_t));
            for (uint32_t i = 0; i < total_tiles; i++) {
                tiles16[i] = default_tile;
            }
            self->tiles = tiles16;
        } else {
            uint8_t *tiles8 = (uint8_t *)m_malloc_without_collect(total_tiles);
            for (uint32_t i = 0; i < total_tiles; i++) {
                tiles8[i] = (uint8_t)default_tile;
            }
            self->tiles = tiles8;
        }
        self->inline_tiles = false;
    }

    self->width_in_tiles = width;
    self->height_in_tiles = height;
    self->x = x;
    self->y = y;
    self->pixel_width = width * tile_width;
    self->pixel_height = height * tile_height;
    self->tile_width = tile_width;
    self->tile_height = tile_height;
    self->bitmap = bitmap;
    self->pixel_shader = pixel_shader;
    self->in_group = false;
    self->hidden = false;
    self->hidden_by_parent = false;
    self->previous_area.x1 = 0xffff;
    self->previous_area.x2 = self->previous_area.x1;
    self->flip_x = false;
    self->flip_y = false;
    self->transpose_xy = false;
    self->absolute_transform = NULL;
    #if CIRCUITPY_TILEPALETTEMAPPER
    if (mp_obj_is_type(self->pixel_shader, &tilepalettemapper_tilepalettemapper_type)) {
        tilepalettemapper_tilepalettemapper_bind(self->pixel_shader, self);
    }
    #endif
}


bool common_hal_displayio_tilegrid_get_hidden(displayio_tilegrid_t *self) {
    return self->hidden;
}

bool displayio_tilegrid_get_rendered_hidden(displayio_tilegrid_t *self) {
    return self->rendered_hidden;
}

void common_hal_displayio_tilegrid_set_hidden(displayio_tilegrid_t *self, bool hidden) {
    self->hidden = hidden;
    self->rendered_hidden = false;
    if (!hidden) {
        self->full_change = true;
    }
}

void displayio_tilegrid_set_hidden_by_parent(displayio_tilegrid_t *self, bool hidden) {
    self->hidden_by_parent = hidden;
    self->rendered_hidden = false;
    if (!hidden) {
        self->full_change = true;
    }
}

bool displayio_tilegrid_get_previous_area(displayio_tilegrid_t *self, displayio_area_t *area) {
    if (self->previous_area.x1 == self->previous_area.x2) {
        return false;
    }
    displayio_area_copy(&self->previous_area, area);
    return true;
}

static void _update_current_x(displayio_tilegrid_t *self) {
    uint16_t width;
    if (self->transpose_xy) {
        width = self->pixel_height;
    } else {
        width = self->pixel_width;
    }

    // If there's no transform, substitute an identity transform so the calculations will work.
    const displayio_buffer_transform_t *absolute_transform =
        self->absolute_transform == NULL
        ? &null_transform
        : self->absolute_transform;

    if (absolute_transform->transpose_xy) {
        self->current_area.y1 = absolute_transform->y + absolute_transform->dy * self->x;
        self->current_area.y2 = absolute_transform->y + absolute_transform->dy * (self->x + width);
        if (self->current_area.y2 < self->current_area.y1) {
            int16_t temp = self->current_area.y2;
            self->current_area.y2 = self->current_area.y1;
            self->current_area.y1 = temp;
        }
    } else {
        self->current_area.x1 = absolute_transform->x + absolute_transform->dx * self->x;
        self->current_area.x2 = absolute_transform->x + absolute_transform->dx * (self->x + width);
        if (self->current_area.x2 < self->current_area.x1) {
            int16_t temp = self->current_area.x2;
            self->current_area.x2 = self->current_area.x1;
            self->current_area.x1 = temp;
        }
    }
}

static void _update_current_y(displayio_tilegrid_t *self) {
    uint16_t height;
    if (self->transpose_xy) {
        height = self->pixel_width;
    } else {
        height = self->pixel_height;
    }

    // If there's no transform, substitute an identity transform so the calculations will work.
    const displayio_buffer_transform_t *absolute_transform =
        self->absolute_transform == NULL
        ? &null_transform
        : self->absolute_transform;

    if (absolute_transform->transpose_xy) {
        self->current_area.x1 = absolute_transform->x + absolute_transform->dx * self->y;
        self->current_area.x2 = absolute_transform->x + absolute_transform->dx * (self->y + height);
        if (self->current_area.x2 < self->current_area.x1) {
            int16_t temp = self->current_area.x2;
            self->current_area.x2 = self->current_area.x1;
            self->current_area.x1 = temp;
        }
    } else {
        self->current_area.y1 = absolute_transform->y + absolute_transform->dy * self->y;
        self->current_area.y2 = absolute_transform->y + absolute_transform->dy * (self->y + height);
        if (self->current_area.y2 < self->current_area.y1) {
            int16_t temp = self->current_area.y2;
            self->current_area.y2 = self->current_area.y1;
            self->current_area.y1 = temp;
        }
    }
}

void displayio_tilegrid_update_transform(displayio_tilegrid_t *self,
    const displayio_buffer_transform_t *absolute_transform) {
    self->in_group = absolute_transform != NULL;
    self->absolute_transform = absolute_transform;
    if (absolute_transform != NULL) {
        self->moved = true;

        _update_current_x(self);
        _update_current_y(self);
    }
}

mp_int_t common_hal_displayio_tilegrid_get_x(displayio_tilegrid_t *self) {
    return self->x;
}
void common_hal_displayio_tilegrid_set_x(displayio_tilegrid_t *self, mp_int_t x) {
    if (self->x == x) {
        return;
    }

    self->moved = true;

    self->x = x;
    if (self->absolute_transform != NULL) {
        _update_current_x(self);
    }
}
mp_int_t common_hal_displayio_tilegrid_get_y(displayio_tilegrid_t *self) {
    return self->y;
}

void common_hal_displayio_tilegrid_set_y(displayio_tilegrid_t *self, mp_int_t y) {
    if (self->y == y) {
        return;
    }
    self->moved = true;
    self->y = y;
    if (self->absolute_transform != NULL) {
        _update_current_y(self);
    }
}

mp_obj_t common_hal_displayio_tilegrid_get_pixel_shader(displayio_tilegrid_t *self) {
    return self->pixel_shader;
}

void common_hal_displayio_tilegrid_set_pixel_shader(displayio_tilegrid_t *self, mp_obj_t pixel_shader) {
    self->pixel_shader = pixel_shader;
    self->full_change = true;
    #if CIRCUITPY_TILEPALETTEMAPPER
    if (mp_obj_is_type(self->pixel_shader, &tilepalettemapper_tilepalettemapper_type)) {
        tilepalettemapper_tilepalettemapper_bind(self->pixel_shader, self);
    }
    #endif
}

mp_obj_t common_hal_displayio_tilegrid_get_bitmap(displayio_tilegrid_t *self) {
    return self->bitmap;
}

void common_hal_displayio_tilegrid_set_bitmap(displayio_tilegrid_t *self, mp_obj_t bitmap) {
    self->bitmap = bitmap;
    self->full_change = true;
}

uint16_t common_hal_displayio_tilegrid_get_width(displayio_tilegrid_t *self) {
    return self->width_in_tiles;
}

uint16_t common_hal_displayio_tilegrid_get_height(displayio_tilegrid_t *self) {
    return self->height_in_tiles;
}

uint16_t common_hal_displayio_tilegrid_get_tile_width(displayio_tilegrid_t *self) {
    return self->tile_width;
}

uint16_t common_hal_displayio_tilegrid_get_tile_height(displayio_tilegrid_t *self) {
    return self->tile_height;
}

uint16_t common_hal_displayio_tilegrid_get_tile(displayio_tilegrid_t *self, uint16_t x, uint16_t y) {
    void *tiles = self->tiles;
    if (self->inline_tiles) {
        tiles = &self->tiles;
    }
    if (tiles == NULL) {
        return 0;
    }

    uint32_t index = y * self->width_in_tiles + x;
    if (self->tiles_in_bitmap > 255) {
        return ((uint16_t *)tiles)[index];
    } else {
        return ((uint8_t *)tiles)[index];
    }
}

void displayio_tilegrid_mark_tile_dirty(displayio_tilegrid_t *self, uint16_t x, uint16_t y) {
    displayio_area_t temp_area;
    displayio_area_t *tile_area;
    if (!self->partial_change) {
        tile_area = &self->dirty_area;
    } else {
        tile_area = &temp_area;
    }
    int16_t tx = (x - self->top_left_x) % self->width_in_tiles;
    if (tx < 0) {
        tx += self->width_in_tiles;
    }
    tile_area->x1 = tx * self->tile_width;
    tile_area->x2 = tile_area->x1 + self->tile_width;
    int16_t ty = (y - self->top_left_y) % self->height_in_tiles;
    if (ty < 0) {
        ty += self->height_in_tiles;
    }
    tile_area->y1 = ty * self->tile_height;
    tile_area->y2 = tile_area->y1 + self->tile_height;

    if (self->partial_change) {
        displayio_area_union(&self->dirty_area, &temp_area, &self->dirty_area);
    }
    self->partial_change = true;
}

void common_hal_displayio_tilegrid_set_tile(displayio_tilegrid_t *self, uint16_t x, uint16_t y, uint16_t tile_index) {
    if (tile_index >= self->tiles_in_bitmap) {
        mp_raise_ValueError(MP_ERROR_TEXT("Tile index out of bounds"));
    }

    void *tiles = self->tiles;
    if (self->inline_tiles) {
        tiles = &self->tiles;
    }
    if (tiles == NULL) {
        return;
    }

    uint32_t index = y * self->width_in_tiles + x;
    if (self->tiles_in_bitmap > 255) {
        ((uint16_t *)tiles)[index] = tile_index;
    } else {
        ((uint8_t *)tiles)[index] = (uint8_t)tile_index;
    }
    displayio_tilegrid_mark_tile_dirty(self, x, y);
}

void common_hal_displayio_tilegrid_set_all_tiles(displayio_tilegrid_t *self, uint16_t tile_index) {
    if (tile_index >= self->tiles_in_bitmap) {
        mp_raise_ValueError(MP_ERROR_TEXT("Tile index out of bounds"));
    }

    void *tiles = self->tiles;
    if (self->inline_tiles) {
        tiles = &self->tiles;
    }
    if (tiles == NULL) {
        return;
    }

    if (self->tiles_in_bitmap > 255) {
        uint16_t *tiles16 = (uint16_t *)tiles;
        for (uint16_t y = 0; y < self->height_in_tiles; y++) {
            for (uint16_t x = 0; x < self->width_in_tiles; x++) {
                tiles16[y * self->width_in_tiles + x] = tile_index;
            }
        }
    } else {
        uint8_t *tiles8 = (uint8_t *)tiles;
        for (uint16_t y = 0; y < self->height_in_tiles; y++) {
            for (uint16_t x = 0; x < self->width_in_tiles; x++) {
                tiles8[y * self->width_in_tiles + x] = (uint8_t)tile_index;
            }
        }
    }

    self->full_change = true;
}

bool common_hal_displayio_tilegrid_get_flip_x(displayio_tilegrid_t *self) {
    return self->flip_x;
}

void common_hal_displayio_tilegrid_set_flip_x(displayio_tilegrid_t *self, bool flip_x) {
    if (self->flip_x == flip_x) {
        return;
    }
    self->flip_x = flip_x;
    self->full_change = true;
}

bool common_hal_displayio_tilegrid_get_flip_y(displayio_tilegrid_t *self) {
    return self->flip_y;
}

void common_hal_displayio_tilegrid_set_flip_y(displayio_tilegrid_t *self, bool flip_y) {
    if (self->flip_y == flip_y) {
        return;
    }
    self->flip_y = flip_y;
    self->full_change = true;
}

bool common_hal_displayio_tilegrid_get_transpose_xy(displayio_tilegrid_t *self) {
    return self->transpose_xy;
}

void common_hal_displayio_tilegrid_set_transpose_xy(displayio_tilegrid_t *self, bool transpose_xy) {
    if (self->transpose_xy == transpose_xy) {
        return;
    }
    self->transpose_xy = transpose_xy;

    // Square TileGrids do not change dimensions when transposed.
    if (self->pixel_width == self->pixel_height) {
        self->full_change = true;
        return;
    }

    _update_current_x(self);
    _update_current_y(self);

    self->moved = true;
}

bool common_hal_displayio_tilegrid_contains(displayio_tilegrid_t *self, uint16_t x, uint16_t y) {
    uint16_t right_edge = self->x + (self->width_in_tiles * self->tile_width);
    uint16_t bottom_edge = self->y + (self->height_in_tiles * self->tile_height);
    return x >= self->x && x < right_edge &&
           y >= self->y && y < bottom_edge;
}

void common_hal_displayio_tilegrid_set_top_left(displayio_tilegrid_t *self, uint16_t x, uint16_t y) {
    self->top_left_x = x;
    self->top_left_y = y;
    self->full_change = true;
}

// Largest palette the fast path below can tabulate, one entry per colour.
#define TILEGRID_FAST_MAX_COLORS (256)

// Set to 1 to build a checking firmware: the fast path then writes into shadow
// buffers, the general loop still produces the real output, and the two are
// compared pixel by pixel with any difference reported on the serial console.
#ifndef CIRCUITPY_TILEGRID_VERIFY_FASTPATH
#define CIRCUITPY_TILEGRID_VERIFY_FASTPATH (0)
#endif

#if CIRCUITPY_TILEGRID_VERIFY_FASTPATH
static uint32_t verify_ok_count = 0;
static uint32_t verify_bad_count = 0;
// Fast path calls whose area did not fit the shadow buffers below, so nothing
// was compared. Printed alongside the others: a passing run means nothing if
// this is where all the traffic went.
static uint32_t verify_skipped_count = 0;
// The same firmware also recomputes the general loop's tile lookup the long way
// and compares it against the walked counters, pixel by pixel.
static uint32_t walk_ok_count = 0;
static uint32_t walk_bad_count = 0;
typedef struct {
    bool valid;
    int16_t x, y;
    uint16_t scale, width_in_tiles, height_in_tiles, tile_width, tile_height;
    uint16_t top_left_x, top_left_y;
    uint16_t xi, chk_xi, yi, chk_yi;
    uint16_t tile, chk_tile, tx, chk_tx, ty, chk_ty;
} tilegrid_walk_report_t;
static tilegrid_walk_report_t walk_bad;
#endif

typedef struct {
    displayio_bitmap_t *bitmap;
    const void *tiles;
    bool wide_tiles;
    int16_t start_x, end_x, start_y, end_y;
    int16_t x_shift, y_shift, y_stride;
    uint16_t width_in_tiles, height_in_tiles;
    uint16_t bitmap_width_in_tiles;
    uint16_t tile_width, tile_height;
    uint16_t top_left_x, top_left_y;
    bool full_coverage;
} tilegrid_fast_ctx_t;

// Specialisation of the loop in displayio_tilegrid_fill_area for the most common
// composition: an unscaled, unrotated grid of a plain Bitmap shaded by a
// non-dithering Palette into a 16 bit buffer. The general loop recomputes tile
// indices and unpacks the bitmap through common_hal_displayio_bitmap_get_pixel
// for every pixel, which is about ten integer divisions and two calls per pixel
// even though almost all of it changes only once per tile column.
//
// The walk is therefore two levels. The outer one steps whole tiles and does the
// tile lookup and its two divisions once per crossing; the inner one runs along
// a fixed bitmap row. Runs are cut at whichever comes first, the tile boundary or
// the mask word boundary, so the inner body never has to ask which tile it is in.
//
// The colours are resolved through displayio_palette_get_color itself, once per
// palette entry instead of once per pixel, so the output matches the general
// path by construction rather than by reimplementing the conversion. Mask
// handling, transparency and the coverage result are reproduced exactly.
static bool tilegrid_fill_area_fast(const tilegrid_fast_ctx_t *ctx,
    const _displayio_colorspace_t *colorspace, mp_obj_t pixel_shader,
    uint32_t *mask, uint32_t *buffer) {

    displayio_palette_t *palette = MP_OBJ_TO_PTR(pixel_shader);
    // Only indices the palette actually holds; anything above is out of range and
    // therefore transparent, same as displayio_palette_get_color decides.
    uint32_t lut_len = common_hal_displayio_palette_get_len(palette);

    uint16_t colors[TILEGRID_FAST_MAX_COLORS];
    uint32_t opaque[TILEGRID_FAST_MAX_COLORS / 32];
    memset(opaque, 0, sizeof(opaque));

    displayio_input_pixel_t lut_input;
    displayio_output_pixel_t lut_output;
    memset(&lut_input, 0, sizeof(lut_input));
    for (uint32_t i = 0; i < lut_len; i++) {
        lut_input.pixel = i;
        lut_output.pixel = 0;
        lut_output.opaque = true;
        displayio_palette_get_color(palette, colorspace, &lut_input, &lut_output);
        colors[i] = lut_output.pixel;
        if (lut_output.opaque) {
            opaque[i / 32] |= 1u << (i % 32);
        }
    }

    displayio_bitmap_t *bitmap = ctx->bitmap;
    uint8_t bits_per_value = bitmap->bits_per_value;
    uint8_t bytes_per_value = bits_per_value / 8;
    uint8_t values_per_byte = bytes_per_value ? 1 : 8 / bits_per_value;
    uint8_t bitmap_x_shift = bitmap->x_shift;
    size_t bitmap_x_mask = bitmap->x_mask;
    uint16_t bitmask = bitmap->bitmask;
    uint16_t *out = (uint16_t *)buffer;
    bool full_coverage = ctx->full_coverage;

    // When every value the bitmap can hold maps to an opaque colour, no pixel can
    // be skipped or fall out of range, so the inner run needs neither test.
    bool all_opaque = true;
    for (uint32_t i = 0; i < lut_len; i++) {
        if ((opaque[i / 32] & (1u << (i % 32))) == 0) {
            all_opaque = false;
            break;
        }
    }
    if (bits_per_value >= 32 || (1u << bits_per_value) > lut_len) {
        all_opaque = false;
    }
    // The mask only matters to layers drawn underneath this one, and
    // displayio_group_fill_area returns as soon as a layer reports full coverage,
    // so nothing reads it back after that. all_opaque also means full_coverage can
    // no longer be cleared below, hence skipping the writes is safe exactly here.
    bool skip_mask = all_opaque && full_coverage;

    const void *tiles = ctx->tiles;
    const bool wide_tiles = ctx->wide_tiles;
    const uint16_t tile_width = ctx->tile_width;
    const uint16_t tile_height = ctx->tile_height;
    const uint16_t width_in_tiles = ctx->width_in_tiles;
    const uint16_t height_in_tiles = ctx->height_in_tiles;
    const uint16_t bitmap_width_in_tiles = ctx->bitmap_width_in_tiles;

    // Every row enters at the same column, so it enters the same tile at the same
    // offset into it. Only the tile row changes from one row to the next.
    const uint16_t row_x_in_tile = (uint16_t)(ctx->start_x % tile_width);
    const uint16_t row_x_tile_index =
        (uint16_t)((ctx->start_x / tile_width + ctx->top_left_x) % width_in_tiles);

    // One instantiation of the walk per bitmap format, so the format test does not
    // sit in the inner loop. A run whose mask bits are all clear is the normal case
    // for the top layer and skips the per-pixel test entirely.
    #define TILEGRID_FAST_WALK(READ_VALUE)                                              \
    for (int16_t y = ctx->start_y; y < ctx->end_y; y++) {                               \
        int32_t row_offset = (y - ctx->start_y + ctx->y_shift) * ctx->y_stride;          \
        uint32_t tile_row_base =                                                        \
            (uint32_t)((y / tile_height + ctx->top_left_y) % height_in_tiles)           \
            * width_in_tiles;                                                            \
        uint32_t local_y_in_tile = (uint32_t)(y % tile_height);                          \
        uint32_t offset = (uint32_t)(row_offset + ctx->x_shift);                         \
        uint32_t remaining = (uint32_t)(ctx->end_x - ctx->start_x);                      \
        uint16_t x_in_tile = row_x_in_tile;                                              \
        uint16_t x_tile_index = row_x_tile_index;                                        \
        /* Kept as uint32_t * like common_hal_displayio_bitmap_get_pixel does, so */     \
        /* the narrower views below only ever relax the alignment requirement. */        \
        const uint32_t *row = NULL;                                                      \
        uint32_t bitmap_x_base = 0;                                                      \
        bool tile_changed = true;                                                        \
        while (remaining > 0) {                                                          \
            if (tile_changed) {                                                          \
                tile_changed = false;                                                    \
                uint32_t tile = wide_tiles                                               \
                    ? ((const uint16_t *)tiles)[tile_row_base + x_tile_index]            \
                    : ((const uint8_t *)tiles)[tile_row_base + x_tile_index];            \
                bitmap_x_base = (tile % bitmap_width_in_tiles) * tile_width;             \
                uint32_t bitmap_y =                                                      \
                    (tile / bitmap_width_in_tiles) * tile_height + local_y_in_tile;      \
                row = bitmap->data + bitmap_y * bitmap->stride;                          \
            }                                                                            \
            uint32_t word = offset >> 5;                                                 \
            uint32_t bit = offset & 31;                                                  \
            uint32_t run = 32 - bit;                                                     \
            uint32_t to_tile_end = (uint32_t)(tile_width - x_in_tile);                   \
            if (run > to_tile_end) {                                                     \
                run = to_tile_end;                                                       \
            }                                                                            \
            if (run > remaining) {                                                       \
                run = remaining;                                                         \
            }                                                                            \
            uint32_t bitmap_x = bitmap_x_base + x_in_tile;                               \
            /* A tile boundary can cut a run short of the mask word, so the bits */      \
            /* outside this run may already be set by an earlier segment of the */       \
            /* same word. Test and merge only the run's own bits. */                     \
            uint32_t run_mask =                                                          \
                (run == 32 ? 0xffffffffu : ((1u << run) - 1)) << bit;                    \
            uint32_t m = mask[word];                                                     \
            if ((m & run_mask) == 0 && all_opaque) {                                     \
                for (uint32_t i = 0; i < run; i++) {                                     \
                    out[offset + i] = colors[READ_VALUE(bitmap_x + i)];                  \
                }                                                                        \
                if (!skip_mask) {                                                        \
                    mask[word] = m | run_mask;                                           \
                }                                                                        \
            } else {                                                                     \
                for (uint32_t i = 0; i < run; i++) {                                     \
                    if ((m & (1u << (bit + i))) != 0) {                                  \
                        continue;                                                        \
                    }                                                                    \
                    uint32_t value = READ_VALUE(bitmap_x + i);                           \
                    if (value < lut_len && (opaque[value / 32] & (1u << (value % 32)))) { \
                        m |= 1u << (bit + i);                                            \
                        out[offset + i] = colors[value];                                 \
                    } else {                                                             \
                        /* A pixel is transparent so we haven't fully covered the */     \
                        /* area ourselves. */                                            \
                        full_coverage = false;                                           \
                    }                                                                    \
                }                                                                        \
                mask[word] = m;                                                          \
            }                                                                            \
            offset += run;                                                               \
            remaining -= run;                                                            \
            x_in_tile += run;                                                            \
            if (x_in_tile == tile_width) {                                               \
                x_in_tile = 0;                                                            \
                if (++x_tile_index == width_in_tiles) {                                  \
                    x_tile_index = 0;                                                     \
                }                                                                        \
                tile_changed = true;                                                      \
            }                                                                            \
        }                                                                                \
    }

    #define TILEGRID_READ_SUB(bx) ((((const uint8_t *)row)[(bx) >> bitmap_x_shift] >> \
    ((values_per_byte - ((bx) & bitmap_x_mask) - 1) * bits_per_value)) & bitmask)
    #define TILEGRID_READ_8(bx) (((const uint8_t *)row)[(bx)])
    #define TILEGRID_READ_16(bx) (((const uint16_t *)row)[(bx)])
    #define TILEGRID_READ_32(bx) (row[(bx)])

    switch (bytes_per_value) {
        case 0:
            TILEGRID_FAST_WALK(TILEGRID_READ_SUB)
            break;
        case 1:
            TILEGRID_FAST_WALK(TILEGRID_READ_8)
            break;
        case 2:
            TILEGRID_FAST_WALK(TILEGRID_READ_16)
            break;
        default:
            TILEGRID_FAST_WALK(TILEGRID_READ_32)
            break;
    }

    #undef TILEGRID_FAST_WALK
    #undef TILEGRID_READ_SUB
    #undef TILEGRID_READ_8
    #undef TILEGRID_READ_16
    #undef TILEGRID_READ_32

    return full_coverage;
}

// common_hal_displayio_bitmap_get_pixel returns 0 for a read outside the bitmap,
// and 0 is a real colour index, so the general loop silently shades those pixels
// with palette entry 0. Reproducing that would put a bounds test back into the
// inner run, so instead every tile the area touches is checked once up front and
// anything that would need the clamp is left to the general loop. A tile is
// wholly inside the bitmap exactly when it is below max_tile, given the caller
// has established that a full row of tiles fits across the bitmap's width.
//
// Both Python routes to a tile value are now validated against tiles_in_bitmap,
// which equals max_tile, so this cannot fail from Python. It is kept because
// common_hal_displayio_tilegrid_set_tile is also called from C -- terminalio
// passes glyph indices straight from the font -- and those callers bypass the
// binding's range check entirely.
static bool tilegrid_tiles_fit(const displayio_tilegrid_t *self, const void *tiles,
    uint16_t first_tile_x, uint16_t n_tile_x,
    uint16_t first_tile_y, uint16_t n_tile_y, uint32_t max_tile) {

    bool wide_tiles = self->tiles_in_bitmap > 255;
    uint16_t ty = first_tile_y;
    for (uint16_t j = 0; j < n_tile_y; j++) {
        uint32_t base = (uint32_t)ty * self->width_in_tiles;
        uint16_t tx = first_tile_x;
        for (uint16_t i = 0; i < n_tile_x; i++) {
            uint32_t tile = wide_tiles
                ? ((const uint16_t *)tiles)[base + tx]
                : ((const uint8_t *)tiles)[base + tx];
            if (tile >= max_tile) {
                return false;
            }
            if (++tx == self->width_in_tiles) {
                tx = 0;
            }
        }
        if (++ty == self->height_in_tiles) {
            ty = 0;
        }
    }
    return true;
}

// CIRCUITPY-CHANGE: the general loop below computed nine divisions for every
// pixel, but only two of them can change from one pixel to the next, and even
// those change at most once per tile column. This walks the column with counters
// instead. start_x is never negative -- it is a transformed overlap measured
// from current_area -- so the counters can rely on truncation matching floor.
// tile_changed stays set until the body consumes it, because the mask check
// skips pixels without clearing the crossing that happened underneath.
typedef struct {
    uint16_t scale;
    uint16_t scale_phase;
    uint16_t tile_width;
    uint16_t width_in_tiles;
    uint16_t x_in_tile;
    uint16_t x_tile_index;
    bool tile_changed;
} tilegrid_x_walk_t;

static inline void tilegrid_x_walk_step(tilegrid_x_walk_t *walk) {
    if (++walk->scale_phase < walk->scale) {
        return;
    }
    walk->scale_phase = 0;
    if (++walk->x_in_tile < walk->tile_width) {
        return;
    }
    walk->x_in_tile = 0;
    if (++walk->x_tile_index == walk->width_in_tiles) {
        walk->x_tile_index = 0;
    }
    walk->tile_changed = true;
}

bool displayio_tilegrid_fill_area(displayio_tilegrid_t *self,
    const _displayio_colorspace_t *colorspace, const displayio_area_t *area,
    uint32_t *mask, uint32_t *buffer) {
    // If no tiles are present we have no impact.
    void *tiles = self->tiles;
    if (self->inline_tiles) {
        tiles = &self->tiles;
    }
    if (tiles == NULL) {
        return false;
    }

    bool hidden = self->hidden || self->hidden_by_parent;
    if (hidden) {
        return false;
    }

    displayio_area_t overlap;
    if (!displayio_area_compute_overlap(area, &self->current_area, &overlap)) {
        return false;
    }

    int16_t x_stride = 1;
    int16_t y_stride = displayio_area_width(area);

    bool flip_x = self->flip_x;
    bool flip_y = self->flip_y;
    if (self->transpose_xy != self->absolute_transform->transpose_xy) {
        bool temp_flip = flip_x;
        flip_x = flip_y;
        flip_y = temp_flip;
    }

    // How many pixels are outside of our area between us and the start of the row.
    uint16_t start = 0;
    if ((self->absolute_transform->dx < 0) != flip_x) {
        start += (area->x2 - area->x1 - 1) * x_stride;
        x_stride *= -1;
    }
    if ((self->absolute_transform->dy < 0) != flip_y) {
        start += (area->y2 - area->y1 - 1) * y_stride;
        y_stride *= -1;
    }

    // Track if this layer finishes filling in the given area. We can ignore any remaining
    // layers at that point.
    bool full_coverage = displayio_area_equal(area, &overlap);

    // TODO(tannewt): Skip coverage tracking if all pixels outside the overlap have already been
    // set and our palette is all opaque.

    // TODO(tannewt): Check to see if the pixel_shader has any transparency. If it doesn't then we
    // can either return full coverage or bulk update the mask.
    displayio_area_t transformed;
    displayio_area_transform_within(flip_x != (self->absolute_transform->dx < 0), flip_y != (self->absolute_transform->dy < 0), self->transpose_xy != self->absolute_transform->transpose_xy,
        &overlap,
        &self->current_area,
        &transformed);

    int16_t start_x = (transformed.x1 - self->current_area.x1);
    int16_t end_x = (transformed.x2 - self->current_area.x1);
    int16_t start_y = (transformed.y1 - self->current_area.y1);
    int16_t end_y = (transformed.y2 - self->current_area.y1);

    int16_t y_shift = 0;
    int16_t x_shift = 0;
    if ((self->absolute_transform->dx < 0) != flip_x) {
        x_shift = area->x2 - overlap.x2;
    } else {
        x_shift = overlap.x1 - area->x1;
    }
    if ((self->absolute_transform->dy < 0) != flip_y) {
        y_shift = area->y2 - overlap.y2;
    } else {
        y_shift = overlap.y1 - area->y1;
    }

    // This untransposes x and y so it aligns with bitmap rows.
    if (self->transpose_xy != self->absolute_transform->transpose_xy) {
        int16_t temp_stride = x_stride;
        x_stride = y_stride;
        y_stride = temp_stride;
        int16_t temp_shift = x_shift;
        x_shift = y_shift;
        y_shift = temp_shift;
    }

    // Everything tilegrid_fill_area_fast leaves out is only loop invariant under
    // these conditions, so anything unusual keeps the general loop below.
    #if CIRCUITPY_TILEGRID_VERIFY_FASTPATH
    bool verify_ran = false;
    bool verify_coverage = false;
    // Large enough for a whole 240x135 screen at 16bpp, so a full refresh is
    // compared rather than silently skipped. Checking firmware only.
    static uint32_t verify_buffer[16384];
    static uint32_t verify_mask[1100];
    #endif
    if (colorspace->depth == 16 && !colorspace->dither &&
        self->absolute_transform->scale == 1 &&
        self->transpose_xy == self->absolute_transform->transpose_xy &&
        x_stride == 1 && start == 0 &&
        y_stride == (int16_t)displayio_area_width(area) &&
        // The walk divides by these and steps forwards from start_x, start_y.
        self->tile_width > 0 && self->tile_height > 0 &&
        start_x >= 0 && start_y >= 0 && start_x < end_x && start_y < end_y &&
        mp_obj_is_type(self->bitmap, &displayio_bitmap_type) &&
        mp_obj_is_type(self->pixel_shader, &displayio_palette_type) &&
        !common_hal_displayio_palette_get_dither(MP_OBJ_TO_PTR(self->pixel_shader)) &&
        common_hal_displayio_palette_get_len(MP_OBJ_TO_PTR(self->pixel_shader)) <= TILEGRID_FAST_MAX_COLORS &&
        // Building the colour table has to stay cheaper than the area it serves.
        common_hal_displayio_palette_get_len(MP_OBJ_TO_PTR(self->pixel_shader)) <= displayio_area_size(&overlap)) {

        displayio_bitmap_t *fast_bitmap = MP_OBJ_TO_PTR(self->bitmap);
        uint16_t fast_bwt = self->bitmap_width_in_tiles;
        // With a whole row of tiles fitting across the bitmap, no tile's columns
        // can leave it, so only the tile row has to be bounded.
        uint32_t max_tile = 0;
        if (fast_bwt > 0 && (uint32_t)fast_bwt * self->tile_width <= fast_bitmap->width) {
            max_tile = (fast_bitmap->height / self->tile_height) * fast_bwt;
        }

        // Tile columns and rows the area actually reaches, so the check below
        // scales with the dirty region rather than with the whole grid.
        uint16_t first_tile_x =
            (uint16_t)((start_x / self->tile_width + self->top_left_x) % self->width_in_tiles);
        uint32_t n_tile_x = (uint32_t)((end_x - 1) / self->tile_width)
            - (uint32_t)(start_x / self->tile_width) + 1;
        if (n_tile_x > self->width_in_tiles) {
            n_tile_x = self->width_in_tiles;
        }
        uint16_t first_tile_y =
            (uint16_t)((start_y / self->tile_height + self->top_left_y) % self->height_in_tiles);
        uint32_t n_tile_y = (uint32_t)((end_y - 1) / self->tile_height)
            - (uint32_t)(start_y / self->tile_height) + 1;
        if (n_tile_y > self->height_in_tiles) {
            n_tile_y = self->height_in_tiles;
        }

        if (max_tile > 0 && tilegrid_tiles_fit(self, tiles, first_tile_x, (uint16_t)n_tile_x,
            first_tile_y, (uint16_t)n_tile_y, max_tile)) {

            tilegrid_fast_ctx_t ctx = {
                .bitmap = fast_bitmap,
                .tiles = tiles,
                .wide_tiles = self->tiles_in_bitmap > 255,
                .start_x = start_x, .end_x = end_x,
                .start_y = start_y, .end_y = end_y,
                .x_shift = x_shift, .y_shift = y_shift, .y_stride = y_stride,
                .width_in_tiles = self->width_in_tiles,
                .height_in_tiles = self->height_in_tiles,
                .bitmap_width_in_tiles = fast_bwt,
                .tile_width = self->tile_width,
                .tile_height = self->tile_height,
                .top_left_x = self->top_left_x,
                .top_left_y = self->top_left_y,
                .full_coverage = full_coverage,
            };
            #if CIRCUITPY_TILEGRID_VERIFY_FASTPATH
            size_t area_pixels = displayio_area_size(area);
            size_t mask_words = (area_pixels / 32) + 1;
            if (area_pixels * 2 <= sizeof(verify_buffer) && mask_words * 4 <= sizeof(verify_mask)) {
                memcpy(verify_buffer, buffer, area_pixels * 2);
                memcpy(verify_mask, mask, mask_words * 4);
                verify_coverage = tilegrid_fill_area_fast(&ctx, colorspace, self->pixel_shader,
                    verify_mask, verify_buffer);
                verify_ran = true;
            } else {
                verify_skipped_count++;
            }
            #else
            return tilegrid_fill_area_fast(&ctx, colorspace, self->pixel_shader, mask, buffer);
            #endif
        }
    }

    displayio_input_pixel_t input_pixel;
    displayio_output_pixel_t output_pixel;

    // The grid geometry cannot change while this runs, so read it once instead of
    // chasing self through two pointers for every pixel.
    const uint16_t scale = self->absolute_transform->scale;
    const uint16_t top_left_x = self->top_left_x;
    const uint16_t top_left_y = self->top_left_y;
    const uint16_t width_in_tiles = self->width_in_tiles;
    const uint16_t height_in_tiles = self->height_in_tiles;
    const uint16_t tile_width = self->tile_width;
    const uint16_t tile_height = self->tile_height;
    const uint16_t bitmap_width_in_tiles = self->bitmap_width_in_tiles;

    // Every row starts at the same column, so its counters are the same too.
    const int16_t local_x_start = start_x / scale;
    const uint16_t row_scale_phase = start_x % scale;
    const uint16_t row_x_in_tile = local_x_start % tile_width;
    const uint16_t row_x_tile_index = (local_x_start / tile_width + top_left_x) % width_in_tiles;

    tilegrid_x_walk_t walk = {
        .scale = scale,
        .tile_width = tile_width,
        .width_in_tiles = width_in_tiles,
    };

    for (input_pixel.y = start_y; input_pixel.y < end_y; ++input_pixel.y) {
        int16_t row_start = start + (input_pixel.y - start_y + y_shift) * y_stride; // in pixels
        int16_t local_y = input_pixel.y / scale;
        uint16_t y_tile_index = (local_y / tile_height + top_left_y) % height_in_tiles;
        uint16_t tile_row_base = y_tile_index * width_in_tiles;
        uint16_t local_y_in_tile = local_y % tile_height;

        walk.scale_phase = row_scale_phase;
        walk.x_in_tile = row_x_in_tile;
        walk.x_tile_index = row_x_tile_index;
        walk.tile_changed = true;
        uint16_t tile_x_base = 0;

        for (input_pixel.x = start_x; input_pixel.x < end_x; ++input_pixel.x, tilegrid_x_walk_step(&walk)) {
            // Compute the destination pixel in the buffer and mask based on the transformations.
            int16_t offset = row_start + (input_pixel.x - start_x + x_shift) * x_stride; // in pixels

            // This is super useful for debugging out of range accesses. Uncomment to use.
            // if (offset < 0 || offset >= (int32_t) displayio_area_size(area)) {
            //     asm("bkpt");
            // }

            // Check the mask first to see if the pixel has already been set.
            if ((mask[offset / 32] & (1 << (offset % 32))) != 0) {
                continue;
            }
            if (walk.tile_changed) {
                walk.tile_changed = false;
                uint16_t tile_location = tile_row_base + walk.x_tile_index;
                if (self->tiles_in_bitmap > 255) {
                    input_pixel.tile = ((uint16_t *)tiles)[tile_location];
                } else {
                    input_pixel.tile = ((uint8_t *)tiles)[tile_location];
                }
                tile_x_base = (input_pixel.tile % bitmap_width_in_tiles) * tile_width;
                input_pixel.tile_y = (input_pixel.tile / bitmap_width_in_tiles) * tile_height + local_y_in_tile;
            }
            input_pixel.tile_x = tile_x_base + walk.x_in_tile;

            #if CIRCUITPY_TILEGRID_VERIFY_FASTPATH
            {
                // Nothing may be printed from in here. The console is the display
                // terminal, so writing to it scrolls the very TileGrid being
                // rendered; top_left_y then changes underneath the comparison and
                // it manufactures its own mismatches. The first difference is
                // recorded and reported once the loop has finished.
                int16_t chk_local_x = input_pixel.x / scale;
                uint16_t chk_x_tile_index = (chk_local_x / tile_width + top_left_x) % width_in_tiles;
                uint16_t chk_y_tile_index = (local_y / tile_height + top_left_y) % height_in_tiles;
                uint16_t chk_location = chk_y_tile_index * width_in_tiles + chk_x_tile_index;
                uint8_t chk_tile = self->tiles_in_bitmap > 255
                    ? ((uint16_t *)tiles)[chk_location] : ((uint8_t *)tiles)[chk_location];
                uint16_t chk_tile_x = (chk_tile % bitmap_width_in_tiles) * tile_width
                    + chk_local_x % tile_width;
                uint16_t chk_tile_y = (chk_tile / bitmap_width_in_tiles) * tile_height
                    + local_y % tile_height;
                if (chk_x_tile_index != walk.x_tile_index || chk_y_tile_index != y_tile_index ||
                    chk_tile != input_pixel.tile ||
                    chk_tile_x != input_pixel.tile_x || chk_tile_y != input_pixel.tile_y) {
                    walk_bad_count++;
                    if (!walk_bad.valid) {
                        walk_bad = (tilegrid_walk_report_t) {
                            .valid = true,
                            .x = input_pixel.x, .y = input_pixel.y, .scale = scale,
                            .width_in_tiles = width_in_tiles, .height_in_tiles = height_in_tiles,
                            .tile_width = tile_width, .tile_height = tile_height,
                            .top_left_x = top_left_x, .top_left_y = top_left_y,
                            .xi = walk.x_tile_index, .chk_xi = chk_x_tile_index,
                            .yi = y_tile_index, .chk_yi = chk_y_tile_index,
                            .tile = input_pixel.tile, .chk_tile = chk_tile,
                            .tx = input_pixel.tile_x, .chk_tx = chk_tile_x,
                            .ty = input_pixel.tile_y, .chk_ty = chk_tile_y,
                        };
                    }
                } else {
                    walk_ok_count++;
                }
            }
            #endif

            output_pixel.pixel = 0;
            input_pixel.pixel = 0;

            // We always want to read bitmap pixels by row first and then transpose into the destination
            // buffer because most bitmaps are row associated.
            if (mp_obj_is_type(self->bitmap, &displayio_bitmap_type)) {
                input_pixel.pixel = common_hal_displayio_bitmap_get_pixel(self->bitmap, input_pixel.tile_x, input_pixel.tile_y);
            } else if (mp_obj_is_type(self->bitmap, &displayio_ondiskbitmap_type)) {
                input_pixel.pixel = common_hal_displayio_ondiskbitmap_get_pixel(self->bitmap, input_pixel.tile_x, input_pixel.tile_y);
            }

            output_pixel.opaque = true;
            #if CIRCUITPY_TILEPALETTEMAPPER
            if (mp_obj_is_type(self->pixel_shader, &tilepalettemapper_tilepalettemapper_type)) {
                tilepalettemapper_tilepalettemapper_get_color(self->pixel_shader, colorspace, &input_pixel, &output_pixel, walk.x_tile_index, y_tile_index);
            }
            #endif
            if (self->pixel_shader == mp_const_none) {
                output_pixel.pixel = input_pixel.pixel;
            } else if (mp_obj_is_type(self->pixel_shader, &displayio_palette_type)) {
                displayio_palette_get_color(self->pixel_shader, colorspace, &input_pixel, &output_pixel);
            } else if (mp_obj_is_type(self->pixel_shader, &displayio_colorconverter_type)) {
                displayio_colorconverter_convert(self->pixel_shader, colorspace, &input_pixel, &output_pixel);
            }
            if (!output_pixel.opaque) {
                // A pixel is transparent so we haven't fully covered the area ourselves.
                full_coverage = false;
            } else {
                mask[offset / 32] |= 1 << (offset % 32);
                if (colorspace->depth == 16) {
                    *(((uint16_t *)buffer) + offset) = output_pixel.pixel;
                } else if (colorspace->depth == 32) {
                    *(((uint32_t *)buffer) + offset) = output_pixel.pixel;
                } else if (colorspace->depth == 24) {
                    memcpy(((uint8_t *)buffer) + offset * 3, &output_pixel.pixel, 3);
                } else if (colorspace->depth == 8) {
                    *(((uint8_t *)buffer) + offset) = output_pixel.pixel;
                } else if (colorspace->depth < 8) {
                    uint8_t pixels_per_byte = 8 / colorspace->depth;

                    // Reorder the offsets to pack multiple rows into a byte (meaning they share a column).
                    if (!colorspace->pixels_in_byte_share_row) {
                        uint16_t width = displayio_area_width(area);
                        uint16_t row = offset / width;
                        uint16_t col = offset % width;
                        // Dividing by pixels_per_byte does truncated division even if we multiply it back out.
                        offset = col * pixels_per_byte + (row / pixels_per_byte) * pixels_per_byte * width + row % pixels_per_byte;
                        // Also useful for validating that the bitpacking worked correctly.
                        // if (offset > displayio_area_size(area)) {
                        //     asm("bkpt");
                        // }
                    }
                    uint8_t shift = (offset % pixels_per_byte) * colorspace->depth;
                    if (colorspace->reverse_pixels_in_byte) {
                        // Reverse the shift by subtracting it from the leftmost shift.
                        shift = (pixels_per_byte - 1) * colorspace->depth - shift;
                    }
                    ((uint8_t *)buffer)[offset / pixels_per_byte] |= output_pixel.pixel << shift;
                }
            }
        }
    }

    #if CIRCUITPY_TILEGRID_VERIFY_FASTPATH
    if (walk_bad.valid) {
        tilegrid_walk_report_t r = walk_bad;
        walk_bad.valid = false;
        mp_printf(&mp_plat_print,
            "WALK x=%d y=%d scale %u tiles %ux%u tile %ux%u topleft %u,%u\n",
            r.x, r.y, r.scale, r.width_in_tiles, r.height_in_tiles,
            r.tile_width, r.tile_height, r.top_left_x, r.top_left_y);
        mp_printf(&mp_plat_print,
            "  xi %u/%u yi %u/%u tile %u/%u tx %u/%u ty %u/%u  celkem %u/%u\n",
            r.xi, r.chk_xi, r.yi, r.chk_yi, r.tile, r.chk_tile,
            r.tx, r.chk_tx, r.ty, r.chk_ty,
            (unsigned)walk_bad_count, (unsigned)walk_ok_count);
    } else if (walk_ok_count >= 200000) {
        mp_printf(&mp_plat_print, "WALK shodne: %u, rozdilne: %u\n",
            (unsigned)walk_ok_count, (unsigned)walk_bad_count);
        walk_ok_count = 0;
    }
    if (verify_ran) {
        size_t area_pixels = displayio_area_size(area);
        size_t mask_words = (area_pixels / 32) + 1;
        bool mismatch = false;
        if (memcmp(verify_buffer, buffer, area_pixels * 2) != 0) {
            mismatch = true;
            for (size_t i = 0; i < area_pixels; i++) {
                if (((uint16_t *)verify_buffer)[i] != ((uint16_t *)buffer)[i]) {
                    uint16_t area_w = displayio_area_width(area);
                    int16_t gx = i % area_w + start_x - x_shift;
                    int16_t gy = i / area_w + start_y - y_shift;
                    displayio_bitmap_t *dbg = MP_OBJ_TO_PTR(self->bitmap);
                    mp_printf(&mp_plat_print,
                        "FASTPATH pixel %u: fast %04x general %04x\n",
                        (unsigned)i, ((uint16_t *)verify_buffer)[i], ((uint16_t *)buffer)[i]);
                    mp_printf(&mp_plat_print,
                        "  area %d,%d-%d,%d  start %d,%d end %d,%d shift %d,%d ystride %d\n",
                        area->x1, area->y1, area->x2, area->y2,
                        start_x, start_y, end_x, end_y, x_shift, y_shift, y_stride);
                    mp_printf(&mp_plat_print,
                        "  grid %d,%d  bitmap %dx%d bpv %d stride %d  value %u  mask %d\n",
                        gx, gy, dbg->width, dbg->height, dbg->bits_per_value, dbg->stride,
                        (unsigned)common_hal_displayio_bitmap_get_pixel(dbg, gx, gy),
                        (int)((verify_mask[i / 32] >> (i % 32)) & 1));
                    mp_printf(&mp_plat_print,
                        "  palette len %u\n",
                        (unsigned)common_hal_displayio_palette_get_len(MP_OBJ_TO_PTR(self->pixel_shader)));
                    break;
                }
            }
        }
        // When the fast path reports full coverage it deliberately leaves the mask
        // untouched, because the group walk stops there and nothing reads it back.
        // Only compare it while it is still live.
        if (!verify_coverage && memcmp(verify_mask, mask, mask_words * 4) != 0) {
            mismatch = true;
            mp_printf(&mp_plat_print, "FASTPATH mask differs\n");
        }
        if (verify_coverage != full_coverage) {
            mismatch = true;
            mp_printf(&mp_plat_print, "FASTPATH coverage: fast %d general %d\n",
                verify_coverage, full_coverage);
        }
        // Positive evidence that the comparison actually ran, otherwise a passing
        // check could just mean the fast path never triggered.
        if (mismatch) {
            verify_bad_count++;
        } else {
            verify_ok_count++;
            if (verify_ok_count % 100 == 0) {
                mp_printf(&mp_plat_print,
                    "FASTPATH shodne: %u, rozdilne: %u, neporovnano: %u\n",
                    (unsigned)verify_ok_count, (unsigned)verify_bad_count,
                    (unsigned)verify_skipped_count);
            }
        }
    }
    #endif
    return full_coverage;
}

void displayio_tilegrid_finish_refresh(displayio_tilegrid_t *self) {
    bool first_draw = self->previous_area.x1 == self->previous_area.x2;
    bool hidden = self->hidden || self->hidden_by_parent;
    if (!first_draw && hidden) {
        self->previous_area.x2 = self->previous_area.x1;
    } else if (self->moved || first_draw) {
        displayio_area_copy(&self->current_area, &self->previous_area);
    }

    self->moved = false;
    self->full_change = false;
    self->partial_change = false;
    if (mp_obj_is_type(self->pixel_shader, &displayio_palette_type)) {
        displayio_palette_finish_refresh(self->pixel_shader);
    } else if (mp_obj_is_type(self->pixel_shader, &displayio_colorconverter_type)) {
        displayio_colorconverter_finish_refresh(self->pixel_shader);
    }
    if (mp_obj_is_type(self->bitmap, &displayio_bitmap_type)) {
        displayio_bitmap_finish_refresh(self->bitmap);
    } else if (mp_obj_is_type(self->bitmap, &displayio_ondiskbitmap_type)) {
        // OnDiskBitmap changes will trigger a complete reload so no need to
        // track changes.
    }
    // TODO(tannewt): We could double buffer changes to position and move them over here.
    // That way they won't change during a refresh and tear.
}

displayio_area_t *displayio_tilegrid_get_refresh_areas(displayio_tilegrid_t *self, displayio_area_t *tail) {
    bool first_draw = self->previous_area.x1 == self->previous_area.x2;
    bool hidden = self->hidden || self->hidden_by_parent;
    // Check hidden first because it trumps all other changes.
    if (hidden) {
        self->rendered_hidden = true;
        if (!first_draw) {
            self->previous_area.next = tail;
            return &self->previous_area;
        } else {
            return tail;
        }
    } else if (self->moved && !first_draw) {
        displayio_area_union(&self->previous_area, &self->current_area, &self->dirty_area);
        if (displayio_area_size(&self->dirty_area) <= 2U * self->pixel_width * self->pixel_height) {
            self->dirty_area.next = tail;
            return &self->dirty_area;
        }
        self->previous_area.next = tail;
        self->current_area.next = &self->previous_area;
        return &self->current_area;
    }

    // If we have an in-memory bitmap, then check it for modifications.
    if (mp_obj_is_type(self->bitmap, &displayio_bitmap_type)) {
        displayio_area_t *refresh_area = displayio_bitmap_get_refresh_areas(self->bitmap, tail);
        if (refresh_area != tail) {
            // Special case a TileGrid that shows a full bitmap and use its
            // dirty area. Copy it to ours so we can transform it.
            // CIRCUITPY-CHANGE: "the bitmap is one tile" was treated as "the grid is
            // one cell". A tiled background -- one 16x16 bitmap shown across a 15x9
            // grid -- has tiles_in_bitmap == 1, so a change to the bitmap redrew only
            // the first cell and left the other 134 stale. The branch also copied over
            // dirty_area rather than merging, so a tile change accumulated in the same
            // frame was discarded. Take the shortcut only when the grid really is a
            // single cell.
            if (self->tiles_in_bitmap == 1 && self->width_in_tiles == 1 && self->height_in_tiles == 1) {
                displayio_area_copy(refresh_area, &self->dirty_area);
                self->partial_change = true;
            } else {
                self->full_change = true;
            }
        }
    }

    self->full_change = self->full_change ||
        (mp_obj_is_type(self->pixel_shader, &displayio_palette_type) &&
            displayio_palette_needs_refresh(self->pixel_shader)) ||
        (mp_obj_is_type(self->pixel_shader, &displayio_colorconverter_type) &&
            displayio_colorconverter_needs_refresh(self->pixel_shader));

    if (self->full_change || first_draw) {
        self->current_area.next = tail;
        return &self->current_area;
    }

    if (self->partial_change) {
        int16_t x = self->x;
        int16_t y = self->y;
        if (self->absolute_transform->transpose_xy) {
            int16_t temp = y;
            y = x;
            x = temp;
        }
        int16_t x1 = self->dirty_area.x1;
        int16_t x2 = self->dirty_area.x2;
        if (self->flip_x) {
            x1 = self->pixel_width - x1;
            x2 = self->pixel_width - x2;
        }
        int16_t y1 = self->dirty_area.y1;
        int16_t y2 = self->dirty_area.y2;
        if (self->flip_y) {
            y1 = self->pixel_height - y1;
            y2 = self->pixel_height - y2;
        }
        if (self->transpose_xy != self->absolute_transform->transpose_xy) {
            int16_t temp1 = y1, temp2 = y2;
            y1 = x1;
            x1 = temp1;
            y2 = x2;
            x2 = temp2;
        }
        self->dirty_area.x1 = self->absolute_transform->x + self->absolute_transform->dx * (x + x1);
        self->dirty_area.y1 = self->absolute_transform->y + self->absolute_transform->dy * (y + y1);
        self->dirty_area.x2 = self->absolute_transform->x + self->absolute_transform->dx * (x + x2);
        self->dirty_area.y2 = self->absolute_transform->y + self->absolute_transform->dy * (y + y2);
        if (self->dirty_area.y2 < self->dirty_area.y1) {
            int16_t temp = self->dirty_area.y2;
            self->dirty_area.y2 = self->dirty_area.y1;
            self->dirty_area.y1 = temp;
        }
        if (self->dirty_area.x2 < self->dirty_area.x1) {
            int16_t temp = self->dirty_area.x2;
            self->dirty_area.x2 = self->dirty_area.x1;
            self->dirty_area.x1 = temp;
        }

        self->dirty_area.next = tail;
        return &self->dirty_area;
    }
    return tail;
}
