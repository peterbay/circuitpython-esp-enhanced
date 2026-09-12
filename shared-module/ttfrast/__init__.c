// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: see shared-module/ttfrast/__init__.h.

#include <math.h>
#include <string.h>

#include "py/gc.h"
#include "py/objnamedtuple.h"
#include "py/runtime.h"

#include "shared-bindings/displayio/Bitmap.h"
#include "shared-bindings/fontio/Glyph.h"
#include "shared-bindings/time/__init__.h"
#include "shared-module/ttfrast/__init__.h"

#define TTFRAST_NO_CODEPOINT (0xFFFFFFFFu)

static uint32_t glyph_offset(ttfrast_font_obj_t *self, uint16_t glyph, uint32_t *end);

// --------------------------------------------------------------------+
// Big-endian readers over the font image
// --------------------------------------------------------------------+

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline int16_t rds16(const uint8_t *p) {
    return (int16_t)rd16(p);
}

static inline uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t find_table(const uint8_t *data, size_t len, const char tag[4]) {
    if (len < 12) {
        return 0;
    }
    uint16_t num_tables = rd16(data + 4);
    for (uint16_t i = 0; i < num_tables; i++) {
        const uint8_t *entry = data + 12 + 16 * (size_t)i;
        if ((size_t)(entry + 16 - data) > len) {
            return 0;
        }
        if (memcmp(entry, tag, 4) == 0) {
            uint32_t offset = rd32(entry + 8);
            return offset < len ? offset : 0;
        }
    }
    return 0;
}

void common_hal_ttfrast_font_construct(ttfrast_font_obj_t *self, mp_obj_t data_obj,
    float size, uint16_t max_glyphs, uint8_t bits_per_pixel) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
    self->data_obj = data_obj;
    self->data = bufinfo.buf;
    self->len = bufinfo.len;

    const uint8_t *d = self->data;
    size_t len = self->len;
    if (len < 12) {
        mp_raise_ValueError(MP_ERROR_TEXT("not a font"));
    }
    uint32_t version = rd32(d);
    // 0x00010000 is TrueType outlines, "true" is the same on Apple. An OpenType
    // file with PostScript outlines ("OTTO") has a CFF table instead of glyf and
    // is a different format altogether.
    if (version != 0x00010000 && memcmp(d, "true", 4) != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("not TrueType outlines"));
    }

    self->head = find_table(d, len, "head");
    self->maxp = find_table(d, len, "maxp");
    self->loca = find_table(d, len, "loca");
    self->glyf = find_table(d, len, "glyf");
    self->hhea = find_table(d, len, "hhea");
    self->hmtx = find_table(d, len, "hmtx");
    uint32_t cmap = find_table(d, len, "cmap");
    if (!self->head || !self->maxp || !self->loca || !self->glyf || !cmap) {
        mp_raise_ValueError(MP_ERROR_TEXT("font is missing a required table"));
    }

    self->units_per_em = rd16(d + self->head + 18);
    self->index_to_loc = rds16(d + self->head + 50);
    self->num_glyphs = rd16(d + self->maxp + 4);
    self->num_h_metrics = self->hhea ? rd16(d + self->hhea + 34) : 0;
    if (self->units_per_em == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("font has no units per em"));
    }

    // A format 4 Unicode subtable: (3,1) is what every Windows font carries, and
    // (0,3) is the Unicode platform's spelling of the same thing.
    self->cmap4 = 0;
    uint16_t n = rd16(d + cmap + 2);
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *rec = d + cmap + 4 + 8 * (size_t)i;
        uint16_t platform = rd16(rec);
        uint16_t encoding = rd16(rec + 2);
        uint32_t offset = cmap + rd32(rec + 4);
        if (offset + 4 > len) {
            continue;
        }
        if (rd16(d + offset) != 4) {
            continue;
        }
        if ((platform == 3 && (encoding == 1 || encoding == 0)) || platform == 0) {
            self->cmap4 = offset;
            break;
        }
    }
    if (self->cmap4 == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("font has no format 4 character map"));
    }

    self->acc = NULL;
    self->acc_len = 0;
    self->cov = NULL;
    self->cov_len = 0;
    self->segments = 0;

    self->size = 0.0f;
    self->atlas = NULL;
    self->slot_codepoints = NULL;
    self->max_glyphs = 0;
    self->next_slot = 0;
    self->bits_per_pixel = bits_per_pixel;
    if (size <= 0.0f) {
        return;
    }

    // The atlas: one row of uniform cells the size of the font's bounding box
    // at this size, so every glyph is addressed by a tile index alone. Cells
    // get the same spare column the renderer needs for its closing delta.
    //
    // The box comes from the glyphs themselves rather than the head table: a
    // subset font keeps the original head box, and Verdana's spans glyphs that
    // are not even Latin, which would triple the cell.
    int32_t fx0 = INT32_MAX, fy0 = INT32_MAX, fx1 = INT32_MIN, fy1 = INT32_MIN;
    for (uint16_t gi = 0; gi < self->num_glyphs; gi++) {
        uint32_t end = 0;
        uint32_t start = glyph_offset(self, gi, &end);
        if (end <= start || (size_t)self->glyf + start + 10 > len) {
            continue;
        }
        const uint8_t *g = d + self->glyf + start;
        int32_t gx0 = rds16(g + 2), gy0 = rds16(g + 4), gx1 = rds16(g + 6), gy1 = rds16(g + 8);
        fx0 = gx0 < fx0 ? gx0 : fx0;
        fy0 = gy0 < fy0 ? gy0 : fy0;
        fx1 = gx1 > fx1 ? gx1 : fx1;
        fy1 = gy1 > fy1 ? gy1 : fy1;
    }
    if (fx0 > fx1) {
        mp_raise_ValueError(MP_ERROR_TEXT("font has no outlines"));
    }
    float scale = size / (float)self->units_per_em;
    int xmin = (int)floorf(fx0 * scale);
    int ymin = (int)floorf(fy0 * scale);
    int xmax = (int)ceilf(fx1 * scale);
    int ymax = (int)ceilf(fy1 * scale);
    int cell_w = xmax - xmin + 2;
    int cell_h = ymax - ymin + 1;
    if (cell_w < 2 || cell_h < 1 || cell_w > 512 || cell_h > 512) {
        mp_raise_ValueError(MP_ERROR_TEXT("font bounding box is unusable at this size"));
    }
    if ((uint32_t)cell_w * max_glyphs > 65535) {
        mp_raise_ValueError(MP_ERROR_TEXT("max_glyphs too large for this size"));
    }

    self->size = size;
    self->max_glyphs = max_glyphs;
    self->cell_w = (int16_t)cell_w;
    self->cell_h = (int16_t)cell_h;
    // fontio places a glyph by its bottom left corner: dx to the right of the
    // pen, dy above the baseline, so a descender gives a negative dy. The cell
    // spans rows -ymax to -ymin inclusive below the baseline (y down), so its
    // exclusive bottom edge is one past -ymin.
    self->cell_dx = (int16_t)xmin;
    self->cell_dy = (int16_t)(ymin - 1);

    self->slot_codepoints = m_new(uint32_t, max_glyphs);
    self->slot_shift = m_new(int16_t, max_glyphs);
    for (uint16_t i = 0; i < max_glyphs; i++) {
        self->slot_codepoints[i] = TTFRAST_NO_CODEPOINT;
        self->slot_shift[i] = 0;
    }

    self->atlas = mp_obj_malloc(displayio_bitmap_t, &displayio_bitmap_type);
    common_hal_displayio_bitmap_construct(self->atlas, (uint32_t)cell_w * max_glyphs,
        cell_h, bits_per_pixel);
}

uint16_t common_hal_ttfrast_font_glyph_index(ttfrast_font_obj_t *self, uint32_t codepoint) {
    if (codepoint > 0xFFFF) {
        return 0;
    }
    const uint8_t *t = self->data + self->cmap4;
    uint16_t seg_count = rd16(t + 6) / 2;
    const uint8_t *end_code = t + 14;
    const uint8_t *start_code = end_code + 2 * (size_t)seg_count + 2;
    const uint8_t *id_delta = start_code + 2 * (size_t)seg_count;
    const uint8_t *id_range = id_delta + 2 * (size_t)seg_count;

    // The segments are sorted, so this is a binary search for the first whose
    // end is at or above the character.
    uint16_t lo = 0, hi = seg_count;
    while (lo < hi) {
        uint16_t mid = (uint16_t)((lo + hi) / 2);
        if (rd16(end_code + 2 * (size_t)mid) < codepoint) {
            lo = (uint16_t)(mid + 1);
        } else {
            hi = mid;
        }
    }
    if (lo >= seg_count || rd16(start_code + 2 * (size_t)lo) > codepoint) {
        return 0;
    }

    uint16_t range_offset = rd16(id_range + 2 * (size_t)lo);
    uint16_t glyph;
    if (range_offset == 0) {
        glyph = (uint16_t)(codepoint + rd16(id_delta + 2 * (size_t)lo));
    } else {
        const uint8_t *p = id_range + 2 * (size_t)lo + range_offset
            + 2 * (codepoint - rd16(start_code + 2 * (size_t)lo));
        if ((size_t)(p + 2 - self->data) > self->len) {
            return 0;
        }
        glyph = rd16(p);
        if (glyph != 0) {
            glyph = (uint16_t)(glyph + rd16(id_delta + 2 * (size_t)lo));
        }
    }
    return glyph < self->num_glyphs ? glyph : 0;
}

// --------------------------------------------------------------------+
// The rasterizer: coverage deltas into a dense grid
// --------------------------------------------------------------------+

typedef struct {
    ttfrast_font_obj_t *font;
    float *a;               // w * h + 4 accumulation cells
    int w, h;
    float scale;            // device pixels per font unit
    float ox, oy;           // device-space origin of the bitmap
    uint32_t segments;
} raster_t;

// floorf() and ceilf() are library calls on this toolchain, and the scanline
// loop below runs them twice per row, so they are worth doing by hand. Both
// arguments are known non-negative where these are used.
static inline int floor_nonneg(float v) {
    return (int)v;
}

static inline int ceil_nonneg(float v) {
    int i = (int)v;
    return (float)i < v ? i + 1 : i;
}

// One line segment, in device space. This is the whole of the draw phase: for
// every scanline the segment crosses, the exact area it covers in each pixel it
// touches is added, and the same amount is subtracted one pixel further right --
// which is what lets the accumulation phase recover the winding by summing.
static void draw_line(raster_t *r, float p0x, float p0y, float p1x, float p1y) {
    // A horizontal edge changes no winding. The epsilon also keeps the
    // reciprocal below from blowing up on a near-horizontal one.
    if (fabsf(p1y - p0y) < 1e-6f) {
        return;
    }
    r->segments++;

    float dir;
    if (p0y < p1y) {
        dir = 1.0f;
    } else {
        dir = -1.0f;
        float tx = p0x, ty = p0y;
        p0x = p1x;
        p0y = p1y;
        p1x = tx;
        p1y = ty;
    }

    if (p1y <= 0.0f) {
        return;                     // entirely above the bitmap
    }
    float dxdy = (p1x - p0x) / (p1y - p0y);
    float x = p0x;

    int y0;
    if (p0y < 0.0f) {
        x -= p0y * dxdy;
        y0 = 0;
        p0y = 0.0f;
    } else {
        y0 = (int)p0y;
    }
    int y_end = ceil_nonneg(p1y);
    if (y_end > r->h) {
        y_end = r->h;
    }

    float *acc = r->a;
    const int width = r->w;
    const float xmax = (float)(width - 1);
    int linestart = y0 * width;

    for (int y = y0; y < y_end; y++, linestart += width) {
        float ytop = (float)y;
        float ybot = ytop + 1.0f;
        float dy = (ybot < p1y ? ybot : p1y) - (ytop > p0y ? ytop : p0y);
        float xnext = x + dxdy * dy;
        float d = dy * dir;

        float x0, x1;
        if (x < xnext) {
            x0 = x;
            x1 = xnext;
        } else {
            x0 = xnext;
            x1 = x;
        }
        // Keep every write inside the row. A glyph is translated to its own
        // bounding box, so this only ever trims rounding.
        if (x0 < 0.0f) {
            x0 = 0.0f;
        }
        if (x1 > xmax) {
            x1 = xmax;
        }
        int x0i = floor_nonneg(x0);
        float x0floor = (float)x0i;
        int x1i = ceil_nonneg(x1);

        float *row = acc + linestart;
        if (x1i <= x0i + 1) {
            // The segment stays inside one pixel column: split the area between
            // that pixel and its right neighbour by the mean x.
            float xmf = 0.5f * (x + xnext) - x0floor;
            row[x0i] += d * (1.0f - xmf);
            row[x0i + 1] += d * xmf;
        } else {
            float s = 1.0f / (x1 - x0);
            float x0f = x0 - x0floor;
            float a0 = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
            float x1f = x1 - (float)x1i + 1.0f;
            float am = 0.5f * s * x1f * x1f;

            row[x0i] += d * a0;
            if (x1i == x0i + 2) {
                row[x0i + 1] += d * (1.0f - a0 - am);
            } else {
                float a1 = s * (1.5f - x0f);
                row[x0i + 1] += d * (a1 - a0);
                float ds = d * s;
                for (int xi = x0i + 2; xi < x1i - 1; xi++) {
                    row[xi] += ds;
                }
                float a2 = a1 + (float)(x1i - x0i - 3) * s;
                row[x1i - 1] += d * (1.0f - a2 - am);
            }
            row[x1i] += d * am;
        }
        x = xnext;
    }
}

// A quadratic curve, flattened by the deviation of its control point: the
// number of segments follows the square root of the control polygon's
// deflection, which is the standard error bound for a quadratic.
static void draw_quad(raster_t *r, float p0x, float p0y, float cx, float cy, float p1x, float p1y) {
    float devx = p0x - 2.0f * cx + p1x;
    float devy = p0y - 2.0f * cy + p1y;
    float devsq = devx * devx + devy * devy;
    if (devsq < 0.333f) {
        draw_line(r, p0x, p0y, p1x, p1y);
        return;
    }
    const float tol = 3.0f;
    int n = 1 + (int)sqrtf(sqrtf(tol * devsq));
    if (n > 16) {
        n = 16;
    }
    // Forward differencing: with q(t) = p0 + t*B + t^2*A the second difference
    // over a fixed step h is the constant 2*h^2*A, so every further point is
    // two additions per axis and no multiplication at all.
    float h = 1.0f / (float)n;
    float hh = h * h;
    float ddx = 2.0f * hh * devx;
    float ddy = 2.0f * hh * devy;
    float dx = 2.0f * h * (cx - p0x) + hh * devx;
    float dy = 2.0f * h * (cy - p0y) + hh * devy;
    float px = p0x, py = p0y;
    for (int i = 0; i < n - 1; i++) {
        float qx = px + dx;
        float qy = py + dy;
        dx += ddx;
        dy += ddy;
        draw_line(r, px, py, qx, qy);
        px = qx;
        py = qy;
    }
    draw_line(r, px, py, p1x, p1y);
}

// The accumulation phase: one linear sweep, no branches worth speaking of.
static void accumulate(const float *a, uint8_t *out, size_t n) {
    float sum = 0.0f;
    for (size_t i = 0; i < n; i++) {
        sum += a[i];
        float y = sum < 0.0f ? -sum : sum;
        if (y > 1.0f) {
            y = 1.0f;
        }
        out[i] = (uint8_t)(255.0f * y + 0.5f);
    }
}

// --------------------------------------------------------------------+
// Outlines
// --------------------------------------------------------------------+

typedef struct {
    float a, b, c, d, e, f;     // font units in, font units out
} transform_t;

static void emit_point(raster_t *r, const transform_t *t, float fx, float fy, float *dx, float *dy) {
    float x = t->a * fx + t->c * fy + t->e;
    float y = t->b * fx + t->d * fy + t->f;
    *dx = x * r->scale - r->ox;
    // Font space has y up, the bitmap has y down.
    *dy = -y * r->scale - r->oy;
}

static uint32_t glyph_offset(ttfrast_font_obj_t *self, uint16_t glyph, uint32_t *end) {
    const uint8_t *d = self->data;
    if (glyph + 1 > self->num_glyphs) {
        return 0;
    }
    uint32_t start;
    if (self->index_to_loc == 0) {
        start = 2u * rd16(d + self->loca + 2 * (size_t)glyph);
        *end = 2u * rd16(d + self->loca + 2 * (size_t)glyph + 2);
    } else {
        start = rd32(d + self->loca + 4 * (size_t)glyph);
        *end = rd32(d + self->loca + 4 * (size_t)glyph + 4);
    }
    return start;
}

static void draw_glyph(raster_t *r, uint16_t glyph, const transform_t *t, int depth);

static void draw_simple_glyph(raster_t *r, const uint8_t *g, int n_contours, const transform_t *t) {
    const uint8_t *end_pts = g + 10;
    int n_points = rd16(end_pts + 2 * (size_t)(n_contours - 1)) + 1;
    const uint8_t *p = end_pts + 2 * (size_t)n_contours;
    p += 2 + rd16(p);                       // skip the hinting instructions

    // Flags, run-length encoded by the repeat bit.
    const uint8_t *flags_start = p;
    int flag_bytes = 0;
    for (int i = 0; i < n_points;) {
        uint8_t flag = p[flag_bytes++];
        i++;
        if (flag & 0x08) {
            i += p[flag_bytes++];
        }
    }
    p += flag_bytes;

    // Expanding the flags and the coordinates into stack arrays keeps the
    // contour walk below readable; a glyph past this many points is rejected
    // rather than risking the 24 kB stack.
    #define TTFRAST_MAX_POINTS (256)
    if (n_points > TTFRAST_MAX_POINTS) {
        return;
    }
    uint8_t flags[TTFRAST_MAX_POINTS];
    int16_t xs[TTFRAST_MAX_POINTS];
    int16_t ys[TTFRAST_MAX_POINTS];

    const uint8_t *fp = flags_start;
    for (int i = 0; i < n_points;) {
        uint8_t flag = *fp++;
        flags[i++] = flag;
        if (flag & 0x08) {
            uint8_t repeat = *fp++;
            while (repeat-- && i < n_points) {
                flags[i++] = flag;
            }
        }
    }

    int16_t v = 0;
    for (int i = 0; i < n_points; i++) {
        uint8_t flag = flags[i];
        if (flag & 0x02) {                  // one byte, sign in bit 4
            uint8_t dx = *p++;
            v = (int16_t)(v + ((flag & 0x10) ? dx : -dx));
        } else if (!(flag & 0x10)) {        // two bytes
            v = (int16_t)(v + rds16(p));
            p += 2;
        }                                    // else: same as the previous point
        xs[i] = v;
    }
    v = 0;
    for (int i = 0; i < n_points; i++) {
        uint8_t flag = flags[i];
        if (flag & 0x04) {
            uint8_t dy = *p++;
            v = (int16_t)(v + ((flag & 0x20) ? dy : -dy));
        } else if (!(flag & 0x20)) {
            v = (int16_t)(v + rds16(p));
            p += 2;
        }
        ys[i] = v;
    }

    int first = 0;
    for (int c = 0; c < n_contours; c++) {
        int last = rd16(end_pts + 2 * (size_t)c);
        if (last < first || last >= n_points) {
            break;
        }
        int count = last - first + 1;
        if (count < 2) {
            first = last + 1;
            continue;
        }

        // The contour starts at the first on-curve point; a contour made only of
        // off-curve points starts at the midpoint between the last and the first.
        float sx, sy;
        int start_i = -1;
        for (int i = 0; i < count; i++) {
            if (flags[first + i] & 0x01) {
                start_i = i;
                break;
            }
        }
        if (start_i < 0) {
            float ax, ay, bx, by;
            emit_point(r, t, xs[first], ys[first], &ax, &ay);
            emit_point(r, t, xs[last], ys[last], &bx, &by);
            sx = 0.5f * (ax + bx);
            sy = 0.5f * (ay + by);
            start_i = 0;
        } else {
            emit_point(r, t, xs[first + start_i], ys[first + start_i], &sx, &sy);
        }

        float cursor_x = sx, cursor_y = sy;
        bool have_control = false;
        float ctrl_x = 0.0f, ctrl_y = 0.0f;

        for (int k = 1; k <= count; k++) {
            int i = first + (start_i + k) % count;
            float px, py;
            emit_point(r, t, xs[i], ys[i], &px, &py);
            if (flags[i] & 0x01) {
                if (have_control) {
                    draw_quad(r, cursor_x, cursor_y, ctrl_x, ctrl_y, px, py);
                    have_control = false;
                } else {
                    draw_line(r, cursor_x, cursor_y, px, py);
                }
                cursor_x = px;
                cursor_y = py;
            } else {
                if (have_control) {
                    // Two control points in a row imply an on-curve point
                    // halfway between them.
                    float mx = 0.5f * (ctrl_x + px);
                    float my = 0.5f * (ctrl_y + py);
                    draw_quad(r, cursor_x, cursor_y, ctrl_x, ctrl_y, mx, my);
                    cursor_x = mx;
                    cursor_y = my;
                }
                ctrl_x = px;
                ctrl_y = py;
                have_control = true;
            }
        }
        // Close the contour back onto its start.
        if (have_control) {
            draw_quad(r, cursor_x, cursor_y, ctrl_x, ctrl_y, sx, sy);
        } else {
            draw_line(r, cursor_x, cursor_y, sx, sy);
        }

        first = last + 1;
    }
    #undef TTFRAST_MAX_POINTS
}

static void draw_glyph(raster_t *r, uint16_t glyph, const transform_t *t, int depth) {
    if (depth > 3) {
        return;
    }
    uint32_t end = 0;
    uint32_t start = glyph_offset(r->font, glyph, &end);
    if (end <= start) {
        return;                              // an empty glyph, such as a space
    }
    const uint8_t *g = r->font->data + r->font->glyf + start;
    int n_contours = rds16(g);
    if (n_contours >= 0) {
        draw_simple_glyph(r, g, n_contours, t);
        return;
    }

    // A composite glyph: components placed by a translation and, sometimes, a
    // 2x2 matrix. Accented Latin letters are built this way.
    const uint8_t *p = g + 10;
    while (true) {
        uint16_t flags = rd16(p);
        uint16_t index = rd16(p + 2);
        p += 4;
        float dx, dy;
        if (flags & 0x0001) {                // ARG_1_AND_2_ARE_WORDS
            dx = (float)rds16(p);
            dy = (float)rds16(p + 2);
            p += 4;
        } else {
            dx = (float)(int8_t)p[0];
            dy = (float)(int8_t)p[1];
            p += 2;
        }
        if (!(flags & 0x0002)) {             // ARGS_ARE_XY_VALUES
            dx = 0.0f;
            dy = 0.0f;                        // point matching is not supported
        }

        float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f;
        if (flags & 0x0008) {                // WE_HAVE_A_SCALE
            a = d = rds16(p) / 16384.0f;
            p += 2;
        } else if (flags & 0x0040) {         // X_AND_Y_SCALE
            a = rds16(p) / 16384.0f;
            d = rds16(p + 2) / 16384.0f;
            p += 4;
        } else if (flags & 0x0080) {         // TWO_BY_TWO
            a = rds16(p) / 16384.0f;
            b = rds16(p + 2) / 16384.0f;
            c = rds16(p + 4) / 16384.0f;
            d = rds16(p + 6) / 16384.0f;
            p += 8;
        }

        transform_t sub = {
            .a = a * t->a + b * t->c,
            .b = a * t->b + b * t->d,
            .c = c * t->a + d * t->c,
            .d = c * t->b + d * t->d,
            .e = dx * t->a + dy * t->c + t->e,
            .f = dx * t->b + dy * t->d + t->f,
        };
        draw_glyph(r, index, &sub, depth + 1);

        if (!(flags & 0x0020)) {             // MORE_COMPONENTS
            break;
        }
    }
}

// --------------------------------------------------------------------+
// Entry point
// --------------------------------------------------------------------+

// The glyph's geometry at `size` from the tables alone: the advance from
// hmtx, the bitmap box from the bounding box in the glyf header, which covers
// composites as well. Nothing of the outline is read, so a layout pass over a
// string costs a few table lookups per character. Returns the glyph index
// through *glyph_out and false when there is no outline.
bool common_hal_ttfrast_font_metrics(ttfrast_font_obj_t *self, uint32_t codepoint, float size,
    int *w, int *h, int *ox, int *oy, float *advance, uint16_t *glyph_out) {

    *w = 0;
    *h = 0;
    *ox = 0;
    *oy = 0;
    *advance = 0.0f;

    uint16_t glyph = common_hal_ttfrast_font_glyph_index(self, codepoint);
    *glyph_out = glyph;
    float scale = size / (float)self->units_per_em;

    if (self->hmtx && self->num_h_metrics) {
        uint16_t i = glyph < self->num_h_metrics ? glyph : (uint16_t)(self->num_h_metrics - 1);
        *advance = rd16(self->data + self->hmtx + 4 * (size_t)i) * scale;
    }

    uint32_t end = 0;
    uint32_t start = glyph_offset(self, glyph, &end);
    if (end <= start) {
        return false;
    }
    const uint8_t *g = self->data + self->glyf + start;

    float xmin = rds16(g + 2) * scale;
    float ymin = rds16(g + 4) * scale;
    float xmax = rds16(g + 6) * scale;
    float ymax = rds16(g + 8) * scale;

    int x0 = (int)floorf(xmin);
    int y0 = (int)floorf(-ymax);
    // One spare column and row: the draw phase writes the closing delta one
    // pixel to the right of the last one it covers.
    int bw = (int)ceilf(xmax) - x0 + 2;
    int bh = (int)ceilf(-ymin) - y0 + 1;
    if (bw < 2 || bh < 1 || bw > 1024 || bh > 1024) {
        return false;
    }

    *w = bw;
    *h = bh;
    *ox = x0;
    *oy = y0;
    return true;
}

bool common_hal_ttfrast_font_render(ttfrast_font_obj_t *self, uint32_t codepoint, float size,
    uint8_t *out, size_t out_len, int *w, int *h, int *ox, int *oy, float *advance) {

    self->segments = 0;
    uint16_t glyph;
    if (!common_hal_ttfrast_font_metrics(self, codepoint, size, w, h, ox, oy, advance, &glyph)) {
        return false;
    }
    float scale = size / (float)self->units_per_em;
    int bw = *w;
    int bh = *h;
    int x0 = *ox;
    int y0 = *oy;

    size_t cells = (size_t)bw * bh;
    if (out != NULL && out_len < cells) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer too small"));
    }

    if (self->acc_len < cells + 4) {
        self->acc = m_renew(float, self->acc, self->acc_len, cells + 4);
        self->acc_len = cells + 4;
    }
    memset(self->acc, 0, (cells + 4) * sizeof(float));

    raster_t r = {
        .font = self,
        .a = self->acc,
        .w = bw,
        .h = bh,
        .scale = scale,
        .ox = (float)x0,
        .oy = (float)y0,
        .segments = 0,
    };
    transform_t identity = { .a = 1.0f, .b = 0.0f, .c = 0.0f, .d = 1.0f, .e = 0.0f, .f = 0.0f };
    draw_glyph(&r, glyph, &identity, 0);
    self->segments = r.segments;

    if (out != NULL) {
        accumulate(self->acc, out, cells);
    }
    return true;
}

uint64_t common_hal_ttfrast_font_time_render(ttfrast_font_obj_t *self, uint32_t codepoint,
    float size, uint8_t *out, size_t out_len, uint32_t count) {
    int w, h, ox, oy;
    float advance;
    uint64_t t0 = common_hal_time_monotonic_ns();
    for (uint32_t i = 0; i < count; i++) {
        common_hal_ttfrast_font_render(self, codepoint, size, out, out_len, &w, &h, &ox, &oy, &advance);
    }
    uint64_t elapsed = common_hal_time_monotonic_ns() - t0;
    return elapsed / (count ? count : 1);
}

// --------------------------------------------------------------------+
// Into displayio bitmaps
// --------------------------------------------------------------------+

// Renders into the scratch coverage buffer and returns its geometry.
static bool render_to_scratch(ttfrast_font_obj_t *self, uint32_t codepoint, float size,
    int *w, int *h, int *ox, int *oy, float *advance) {
    if (!common_hal_ttfrast_font_render(self, codepoint, size, NULL, 0, w, h, ox, oy, advance)) {
        return false;
    }
    size_t cells = (size_t)*w * *h;
    if (self->cov_len < cells) {
        self->cov = m_renew(uint8_t, self->cov, self->cov_len, cells);
        self->cov_len = cells;
    }
    accumulate(self->acc, self->cov, cells);
    return true;
}

// Copies quantized coverage into a bitmap, leaving untouched pixels alone and
// staying inside the clip rectangle (exclusive on the far side).
//
// The bitmap's rows are written directly, packed to its depth, rather than
// through displayio_bitmap_write_pixel(): that call rechecks read-only, bounds
// and the format for every pixel, which was most of the blit's cost. `cov`
// may be NULL, meaning all zero, which is how an atlas cell is cleared.
// With `overwrite` false a pixel whose quantized value is 0 is left alone, so
// text can go over existing content; the atlas passes true.
static void blit_coverage(displayio_bitmap_t *bitmap, const uint8_t *cov, int w, int h,
    int x0, int y0, int clip_x0, int clip_y0, int clip_x1, int clip_y1, bool overwrite) {
    uint32_t bpp = bitmap->bits_per_value;
    if (bpp > 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("bitmap must have at most 8 bits per pixel"));
    }
    if (bitmap->read_only) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Read-only"));
    }
    if (clip_x0 < 0) {
        clip_x0 = 0;
    }
    if (clip_y0 < 0) {
        clip_y0 = 0;
    }
    if (clip_x1 > bitmap->width) {
        clip_x1 = bitmap->width;
    }
    if (clip_y1 > bitmap->height) {
        clip_y1 = bitmap->height;
    }
    int max_value = (1 << bpp) - 1;

    int ya = y0 > clip_y0 ? y0 : clip_y0;
    int yb = y0 + h < clip_y1 ? y0 + h : clip_y1;
    int xa = x0 > clip_x0 ? x0 : clip_x0;
    int xb = x0 + w < clip_x1 ? x0 + w : clip_x1;
    if (ya >= yb || xa >= xb) {
        return;
    }
    displayio_area_t dirty = { xa, ya, xb, yb, NULL };
    displayio_bitmap_set_dirty_area(bitmap, &dirty);

    for (int y = ya; y < yb; y++) {
        uint8_t *row8 = (uint8_t *)(bitmap->data + (size_t)y * bitmap->stride);
        const uint8_t *src = cov ? cov + (size_t)(y - y0) * w + (xa - x0) : NULL;

        if (bpp == 8) {
            for (int x = xa; x < xb; x++) {
                uint8_t q = src ? src[x - xa] : 0;
                if (q || overwrite) {
                    row8[x] = q;
                }
            }
            continue;
        }

        // Several pixels per byte, most significant first. The byte is kept in
        // a register across the pixels it holds and stored when x moves on.
        const uint32_t values_per_byte = 8 / bpp;
        const uint32_t x_shift = bitmap->x_shift;
        const uint32_t x_mask = bitmap->x_mask;
        const uint32_t bitmask = bitmap->bitmask;
        int idx = xa >> x_shift;
        uint8_t packed = row8[idx];
        for (int x = xa; x < xb; x++) {
            int next = x >> x_shift;
            if (next != idx) {
                row8[idx] = packed;
                idx = next;
                packed = row8[idx];
            }
            uint32_t q = src ? ((uint32_t)src[x - xa] * max_value + 127) / 255 : 0;
            if (q || overwrite) {
                uint32_t pos = (values_per_byte - (x & x_mask) - 1) * bpp;
                packed = (uint8_t)((packed & ~(bitmask << pos)) | (q << pos));
            }
        }
        row8[idx] = packed;
    }
}

float common_hal_ttfrast_font_render_into(ttfrast_font_obj_t *self, uint32_t codepoint,
    float size, displayio_bitmap_t *bitmap, int x, int y) {
    int w, h, ox, oy;
    float advance;
    if (render_to_scratch(self, codepoint, size, &w, &h, &ox, &oy, &advance)) {
        blit_coverage(bitmap, self->cov, w, h, x + ox, y + oy, 0, 0, bitmap->width, bitmap->height, false);
    }
    return advance;
}

// --------------------------------------------------------------------+
// The fontio protocol
// --------------------------------------------------------------------+

static void require_atlas(ttfrast_font_obj_t *self) {
    if (self->atlas == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("font was created without a size"));
    }
}

static uint16_t find_slot(ttfrast_font_obj_t *self, uint32_t codepoint) {
    for (uint16_t i = 0; i < self->max_glyphs; i++) {
        if (self->slot_codepoints[i] == codepoint) {
            return i;
        }
    }
    return UINT16_MAX;
}

mp_obj_t common_hal_ttfrast_font_get_glyph(ttfrast_font_obj_t *self, uint32_t codepoint) {
    require_atlas(self);

    // Only the format 4 character map is read, and 0xFFFFFFFF is the empty
    // slot marker, so anything past the BMP must be refused before the slot
    // search or -1 would come back as a hit on an empty cell.
    if (codepoint > 0xFFFF) {
        return mp_const_none;
    }

    // A hit costs the slot search and nothing else: the cell is already
    // drawn and the advance was kept with it.
    uint16_t slot = find_slot(self, codepoint);
    if (slot == UINT16_MAX) {
        if (common_hal_ttfrast_font_glyph_index(self, codepoint) == 0) {
            return mp_const_none;
        }
        // Render first: the scratch buffers may grow here and fail to, and a
        // slot claimed before that would survive the MemoryError as a hit on
        // a cell nobody drew.
        int w, h, ox, oy;
        float advance;
        bool has_outline = render_to_scratch(self, codepoint, self->size, &w, &h, &ox, &oy, &advance);

        // Slots are handed out round robin, so a glyph that is still on screen
        // gets overwritten once max_glyphs newer ones have come through; the
        // cache has to be at least as large as the distinct characters shown.
        slot = self->next_slot;
        self->next_slot = (uint16_t)((slot + 1) % self->max_glyphs);
        self->slot_codepoints[slot] = codepoint;
        self->slot_shift[slot] = (int16_t)(advance + 0.5f);

        // Two passes over the cell: clear it, then write the glyph rectangle
        // over it, zeros included.
        int cell_x = slot * self->cell_w;
        blit_coverage(self->atlas, NULL, self->cell_w, self->cell_h, cell_x, 0,
            cell_x, 0, cell_x + self->cell_w, self->cell_h, true);
        if (has_outline) {
            // The cell's top left is the font bounding box corner: cell_dx to
            // the right of the pen, and cell_dy + cell_h above the baseline.
            int px = cell_x + ox - self->cell_dx;
            int py = oy + self->cell_dy + self->cell_h;
            blit_coverage(self->atlas, self->cov, w, h, px, py,
                cell_x, 0, cell_x + self->cell_w, self->cell_h, true);
        }
    }

    mp_obj_t field_values[8] = {
        MP_OBJ_FROM_PTR(self->atlas),
        MP_OBJ_NEW_SMALL_INT(slot),
        MP_OBJ_NEW_SMALL_INT(self->cell_w),
        MP_OBJ_NEW_SMALL_INT(self->cell_h),
        MP_OBJ_NEW_SMALL_INT(self->cell_dx),
        MP_OBJ_NEW_SMALL_INT(self->cell_dy),
        MP_OBJ_NEW_SMALL_INT(self->slot_shift[slot]),
        MP_OBJ_NEW_SMALL_INT(0),
    };
    return namedtuple_make_new((const mp_obj_type_t *)&fontio_glyph_type, 8, 0, field_values);
}

mp_obj_t common_hal_ttfrast_font_get_bounding_box(ttfrast_font_obj_t *self) {
    require_atlas(self);
    mp_obj_t items[4] = {
        MP_OBJ_NEW_SMALL_INT(self->cell_w),
        MP_OBJ_NEW_SMALL_INT(self->cell_h),
        MP_OBJ_NEW_SMALL_INT(self->cell_dx),
        MP_OBJ_NEW_SMALL_INT(self->cell_dy),
    };
    return mp_obj_new_tuple(4, items);
}
