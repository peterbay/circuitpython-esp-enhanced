// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: experimental TrueType rasterizer, to find out what glyph
// rendering from an outline costs on this hardware. The algorithm is the signed
// area accumulation of Raph Levien's font-rs (and fontdue, which forked it):
// each line segment writes coverage *deltas* into a dense w*h grid, and one
// linear pass with a running sum turns the grid into coverage. There is no edge
// sorting and no active edge list, which is what makes it fast and branch-light.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "py/obj.h"
#include "shared-module/displayio/Bitmap.h"

typedef struct {
    mp_obj_base_t base;
    mp_obj_t data_obj;          // kept so the font bytes outlive the object
    const uint8_t *data;
    size_t len;

    // Table offsets into data, 0 when the table is absent.
    uint32_t head, maxp, loca, glyf, hhea, hmtx, cmap4;

    uint16_t units_per_em;
    uint16_t num_glyphs;
    uint16_t num_h_metrics;
    int16_t index_to_loc;       // 0: uint16 offsets (halved), 1: uint32

    float *acc;                 // accumulation grid, reused between renders
    size_t acc_len;
    uint8_t *cov;               // coverage scratch for the bitmap paths
    size_t cov_len;

    uint32_t segments;          // line segments the last render emitted

    // The fontio side: glyphs rendered at one fixed size into an atlas bitmap
    // of uniform cells, one glyph per tile, which is what displayio.TileGrid
    // and adafruit_display_text address by tile index. Unused when size is 0.
    float size;
    displayio_bitmap_t *atlas;
    uint32_t *slot_codepoints;
    int16_t *slot_shift;        // the advance each slot was rendered with
    uint16_t max_glyphs;
    uint16_t next_slot;
    uint8_t bits_per_pixel;
    int16_t cell_w, cell_h;     // the font's bounding box at this size
    int16_t cell_dx, cell_dy;   // where a cell sits relative to the pen, fontio style
} ttfrast_font_obj_t;

void common_hal_ttfrast_font_construct(ttfrast_font_obj_t *self, mp_obj_t data_obj,
    float size, uint16_t max_glyphs, uint8_t bits_per_pixel);

uint16_t common_hal_ttfrast_font_glyph_index(ttfrast_font_obj_t *self, uint32_t codepoint);

// The glyph's geometry at `size` from the tables alone, without reading the
// outline: the same w, h, ox, oy and advance render() reports. Returns false
// when the glyph has no outline; *glyph_out gets the glyph index either way.
bool common_hal_ttfrast_font_metrics(ttfrast_font_obj_t *self, uint32_t codepoint, float size,
    int *w, int *h, int *ox, int *oy, float *advance, uint16_t *glyph_out);

// Renders one glyph at `size` pixels per em. The outline is always parsed and
// drawn into the accumulation grid; `out` may be NULL to skip only the final
// accumulation into coverage (which is how that phase is timed apart),
// otherwise it takes w*h bytes of coverage, 0 to 255. Returns false when the
// glyph has no outline (a space, say), with *w and *h set to 0.
bool common_hal_ttfrast_font_render(ttfrast_font_obj_t *self, uint32_t codepoint, float size,
    uint8_t *out, size_t out_len, int *w, int *h, int *ox, int *oy, float *advance);

// Renders the same glyph `count` times and returns the nanoseconds one render
// took. Timing from Python would fold in the interpreter's own cost and the
// allocation of the result tuple, which at small sizes is most of the number.
uint64_t common_hal_ttfrast_font_time_render(ttfrast_font_obj_t *self, uint32_t codepoint,
    float size, uint8_t *out, size_t out_len, uint32_t count);

// Draws one glyph into a displayio bitmap with the pen at (x, y) on the
// baseline. Coverage is quantized to the bitmap's depth, so a 1-bit bitmap
// gets a threshold and a 4-bit one gets 16 levels; pixels the glyph does not
// touch are left alone. Returns the advance.
float common_hal_ttfrast_font_render_into(ttfrast_font_obj_t *self, uint32_t codepoint,
    float size, displayio_bitmap_t *bitmap, int x, int y);

// The fontio protocol, for fonts constructed with a size.
mp_obj_t common_hal_ttfrast_font_get_glyph(ttfrast_font_obj_t *self, uint32_t codepoint);
mp_obj_t common_hal_ttfrast_font_get_bounding_box(ttfrast_font_obj_t *self);
