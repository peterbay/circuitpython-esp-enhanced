// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: a text label drawn from a TrueType outline, laid out and
// rasterized in C. See shared-module/ttfrast/__init__.h for the rasterizer.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/displayio/Bitmap.h"
#include "shared-module/displayio/Group.h"
#include "shared-module/displayio/Palette.h"
#include "shared-module/displayio/TileGrid.h"
#include "shared-module/ttfrast/__init__.h"

typedef struct {
    // displayio accepts subclasses of Group by casting to the native base the
    // way it does for Python subclasses: it reads subobj[0] of an
    // mp_obj_instance_t. These three fields keep that layout, so a Label can
    // go wherever a Group can, and Group's own methods and properties
    // resolve through the type's parent and act on the Group in subobj[0].
    mp_obj_base_t base;
    mp_map_t members;
    mp_obj_t subobj[1];

    ttfrast_font_obj_t *font;
    mp_obj_t text;
    float size;
    float line_spacing;
    uint32_t color;
    uint32_t background_color;
    bool has_background;
    uint8_t bits_per_pixel;

    displayio_bitmap_t *bitmap;
    displayio_palette_t *palette;
    displayio_tilegrid_t *tilegrid;

    // The text's bounding box relative to the group's origin, which is the
    // pen position at the start of the first baseline.
    int16_t bb_x, bb_y, bb_w, bb_h;

    float anchor_x, anchor_y;
    int16_t anchored_x, anchored_y;
    bool has_anchor_point;
    bool has_anchored_position;
} ttfrast_label_obj_t;

void common_hal_ttfrast_label_construct(ttfrast_label_obj_t *self, ttfrast_font_obj_t *font,
    mp_obj_t text, float size, uint32_t color, bool has_background, uint32_t background_color,
    float line_spacing, uint8_t bits_per_pixel, uint32_t scale, mp_int_t x, mp_int_t y);

void common_hal_ttfrast_label_set_text(ttfrast_label_obj_t *self, mp_obj_t text);
void common_hal_ttfrast_label_set_color(ttfrast_label_obj_t *self, uint32_t color);
void common_hal_ttfrast_label_set_background_color(ttfrast_label_obj_t *self, bool has_background, uint32_t color);
void common_hal_ttfrast_label_set_anchor(ttfrast_label_obj_t *self, bool has_anchor_point,
    float anchor_x, float anchor_y, bool has_anchored_position, mp_int_t anchored_x, mp_int_t anchored_y);

static inline displayio_group_t *ttfrast_label_group(ttfrast_label_obj_t *self) {
    return MP_OBJ_TO_PTR(self->subobj[0]);
}
