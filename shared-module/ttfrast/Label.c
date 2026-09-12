// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: see shared-module/ttfrast/Label.h.

#include <math.h>
#include <stddef.h>

#include "py/objstr.h"
#include "py/objtype.h"
#include "py/runtime.h"

#include "shared-bindings/displayio/Bitmap.h"
#include "shared-bindings/displayio/Group.h"
#include "shared-bindings/displayio/Palette.h"
#include "shared-bindings/displayio/TileGrid.h"
#include "shared-module/ttfrast/Label.h"

MP_STATIC_ASSERT(offsetof(ttfrast_label_obj_t, subobj) == offsetof(mp_obj_instance_t, subobj));

// --------------------------------------------------------------------+
// Text walking
// --------------------------------------------------------------------+

// One code point from UTF-8; malformed bytes come back as U+FFFD one byte at
// a time rather than stopping the walk.
static uint32_t next_codepoint(const byte **p, const byte *end) {
    uint32_t c = *(*p)++;
    int extra;
    if (c < 0x80) {
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        c &= 0x1F;
        extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0F;
        extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        c &= 0x07;
        extra = 3;
    } else {
        return 0xFFFD;
    }
    for (int i = 0; i < extra; i++) {
        if (*p >= end || (**p & 0xC0) != 0x80) {
            return 0xFFFD;
        }
        c = (c << 6) | (*(*p)++ & 0x3F);
    }
    return c;
}

typedef struct {
    int x0, y0, x1, y1;         // pixel bounds, exclusive on the far side
    bool any;
} bounds_t;

static void bounds_add(bounds_t *b, int x0, int y0, int x1, int y1) {
    if (!b->any) {
        b->x0 = x0;
        b->y0 = y0;
        b->x1 = x1;
        b->y1 = y1;
        b->any = true;
        return;
    }
    b->x0 = x0 < b->x0 ? x0 : b->x0;
    b->y0 = y0 < b->y0 ? y0 : b->y0;
    b->x1 = x1 > b->x1 ? x1 : b->x1;
    b->y1 = y1 > b->y1 ? y1 : b->y1;
}

// Walks the text once. With `bitmap` NULL it only collects the bounds; with a
// bitmap it draws, offsetting by (dx, dy) so the bounds land at the origin.
static void walk_text(ttfrast_label_obj_t *self, bounds_t *bounds, displayio_bitmap_t *bitmap, int dx, int dy) {
    size_t len;
    const char *str = mp_obj_str_get_data(self->text, &len);
    const byte *p = (const byte *)str;
    const byte *end = p + len;

    float line_height = self->size * self->line_spacing;
    float pen = 0.0f;
    int baseline = 0;
    int line = 0;

    while (p < end) {
        uint32_t cp = next_codepoint(&p, end);
        if (cp == '\n') {
            line++;
            baseline = (int)(line * line_height + 0.5f);
            pen = 0.0f;
            continue;
        }
        if (cp == '\r') {
            continue;
        }
        int x = (int)floorf(pen + 0.5f);
        if (bitmap == NULL) {
            int w, h, ox, oy;
            float advance;
            uint16_t glyph;
            if (common_hal_ttfrast_font_metrics(self->font, cp, self->size,
                &w, &h, &ox, &oy, &advance, &glyph)) {
                bounds_add(bounds, x + ox, baseline + oy, x + ox + w, baseline + oy + h);
            }
            pen += advance;
        } else {
            pen += common_hal_ttfrast_font_render_into(self->font, cp, self->size,
                bitmap, x + dx, baseline + dy);
        }
    }
}

// --------------------------------------------------------------------+
// Building the displayio objects
// --------------------------------------------------------------------+

static void fill_palette(ttfrast_label_obj_t *self) {
    uint32_t n = 1u << self->bits_per_pixel;
    uint32_t from = self->has_background ? self->background_color : 0x000000;
    uint32_t to = self->color;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = 0;
        for (int shift = 0; shift < 24; shift += 8) {
            int a = (from >> shift) & 0xFF;
            int b = (to >> shift) & 0xFF;
            int v = a + (b - a) * (int)i / (int)(n - 1);
            c |= (uint32_t)v << shift;
        }
        common_hal_displayio_palette_set_color(self->palette, i, c);
    }
    if (self->has_background) {
        common_hal_displayio_palette_make_opaque(self->palette, 0);
    } else {
        common_hal_displayio_palette_make_transparent(self->palette, 0);
    }
}

static void place(ttfrast_label_obj_t *self) {
    displayio_group_t *group = ttfrast_label_group(self);
    if (self->has_anchor_point && self->has_anchored_position) {
        int gx = self->anchored_x - (int)floorf(self->bb_x + self->anchor_x * self->bb_w + 0.5f);
        int gy = self->anchored_y - (int)floorf(self->bb_y + self->anchor_y * self->bb_h + 0.5f);
        common_hal_displayio_group_set_x(group, gx);
        common_hal_displayio_group_set_y(group, gy);
    }
}

// Lays the text out, sizes the bitmap to it and draws. The bitmap and tile
// grid are kept when the size did not change, which is the common case for a
// value that ticks.
static void relayout(ttfrast_label_obj_t *self) {
    bounds_t b = { 0, 0, 0, 0, false };
    walk_text(self, &b, NULL, 0, 0);
    if (!b.any) {
        b.x0 = 0;
        b.y0 = 0;
        b.x1 = 1;
        b.y1 = 1;
    }
    int w = b.x1 - b.x0;
    int h = b.y1 - b.y0;
    self->bb_x = (int16_t)b.x0;
    self->bb_y = (int16_t)b.y0;
    self->bb_w = (int16_t)w;
    self->bb_h = (int16_t)h;

    displayio_group_t *group = ttfrast_label_group(self);
    bool reuse = self->bitmap != NULL && self->bitmap->width == w && self->bitmap->height == h;
    if (reuse) {
        common_hal_displayio_bitmap_fill(self->bitmap, 0);
    } else {
        if (self->tilegrid != NULL) {
            common_hal_displayio_group_pop(group, 0);
        }
        self->bitmap = mp_obj_malloc(displayio_bitmap_t, &displayio_bitmap_type);
        common_hal_displayio_bitmap_construct(self->bitmap, w, h, self->bits_per_pixel);
        common_hal_displayio_bitmap_fill(self->bitmap, 0);
        self->tilegrid = mp_obj_malloc(displayio_tilegrid_t, &displayio_tilegrid_type);
        // One tile the size of the bitmap: width and height here count tiles.
        common_hal_displayio_tilegrid_construct(self->tilegrid, MP_OBJ_FROM_PTR(self->bitmap),
            1, 1, MP_OBJ_FROM_PTR(self->palette), 1, 1, w, h, 0, 0, 0);
        common_hal_displayio_group_insert(group, common_hal_displayio_group_get_len(group),
            MP_OBJ_FROM_PTR(self->tilegrid));
    }
    common_hal_displayio_tilegrid_set_x(self->tilegrid, b.x0);
    common_hal_displayio_tilegrid_set_y(self->tilegrid, b.y0);

    if (b.any) {
        walk_text(self, NULL, self->bitmap, -b.x0, -b.y0);
    }
    place(self);
}

// --------------------------------------------------------------------+
// The API
// --------------------------------------------------------------------+

void common_hal_ttfrast_label_construct(ttfrast_label_obj_t *self, ttfrast_font_obj_t *font,
    mp_obj_t text, float size, uint32_t color, bool has_background, uint32_t background_color,
    float line_spacing, uint8_t bits_per_pixel, uint32_t scale, mp_int_t x, mp_int_t y) {

    self->members.alloc = 0;
    self->members.used = 0;
    self->members.table = NULL;
    self->members.all_keys_are_qstrs = 1;
    self->members.is_fixed = 1;
    self->members.is_ordered = 1;

    displayio_group_t *group = mp_obj_malloc(displayio_group_t, &displayio_group_type);
    common_hal_displayio_group_construct(group, scale, x, y);
    self->subobj[0] = MP_OBJ_FROM_PTR(group);

    self->font = font;
    self->text = text;
    self->size = size;
    self->line_spacing = line_spacing;
    self->color = color;
    self->background_color = background_color;
    self->has_background = has_background;
    self->bits_per_pixel = bits_per_pixel;
    self->bitmap = NULL;
    self->tilegrid = NULL;
    self->has_anchor_point = false;
    self->has_anchored_position = false;

    self->palette = mp_obj_malloc(displayio_palette_t, &displayio_palette_type);
    common_hal_displayio_palette_construct(self->palette, 1u << bits_per_pixel, false);
    fill_palette(self);

    relayout(self);
}

void common_hal_ttfrast_label_set_text(ttfrast_label_obj_t *self, mp_obj_t text) {
    self->text = text;
    relayout(self);
}

void common_hal_ttfrast_label_set_color(ttfrast_label_obj_t *self, uint32_t color) {
    self->color = color;
    fill_palette(self);
}

void common_hal_ttfrast_label_set_background_color(ttfrast_label_obj_t *self, bool has_background, uint32_t color) {
    self->has_background = has_background;
    self->background_color = color;
    fill_palette(self);
}

void common_hal_ttfrast_label_set_anchor(ttfrast_label_obj_t *self, bool has_anchor_point,
    float anchor_x, float anchor_y, bool has_anchored_position, mp_int_t anchored_x, mp_int_t anchored_y) {
    self->has_anchor_point = has_anchor_point;
    self->anchor_x = anchor_x;
    self->anchor_y = anchor_y;
    self->has_anchored_position = has_anchored_position;
    self->anchored_x = (int16_t)anchored_x;
    self->anchored_y = (int16_t)anchored_y;
    place(self);
}
