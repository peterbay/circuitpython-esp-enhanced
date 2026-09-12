// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: see shared-module/ttfrast/__init__.h.

#include "py/objproperty.h"
#include "py/runtime.h"

#include "shared-bindings/displayio/Bitmap.h"
#include "shared-bindings/ttfrast/__init__.h"
#include "shared-bindings/ttfrast/Label.h"
#include "shared-module/ttfrast/__init__.h"

//| """Rasterize TrueType glyphs from an outline
//|
//| Renders glyphs straight from a ``.ttf`` at any size, with anti-aliased
//| edges, so one font file serves every text size without a bitmap font per
//| size. The algorithm is the signed area accumulation of font-rs: every line
//| segment writes coverage deltas into a dense grid, and one linear pass turns
//| the grid into 8-bit coverage. There is no edge sorting and no active edge
//| list.
//|
//| The font stays where it is -- a `bytes`, a `bytearray`, or the read-only
//| memoryview `espidf.Partition.mmap` returns -- and nothing of it is copied.
//|
//| A font created with a ``size`` follows the `fontio.FontProtocol`, so it
//| works with ``adafruit_display_text`` and `displayio.TileGrid` like a
//| `fontio.BuiltinFont` does:
//|
//| .. code-block:: py
//|
//|     import ttfrast
//|     from adafruit_display_text import label
//|
//|     font = ttfrast.Font(open("/font.ttf", "rb").read(), size=24)
//|     text = label.Label(font, text="Ahoj", color=0xFFFF00)
//|
//| For anti-aliasing, build the font with ``bits_per_pixel=4`` and give the
//| label's tile grids a 16-entry palette that ramps from the background to the
//| text colour; the label's own two-entry palette only knows on and off.
//|
//| Without a size, `render` gives raw coverage and `render_into` draws at any
//| size into any `displayio.Bitmap`:
//|
//| .. code-block:: py
//|
//|     font = ttfrast.Font(open("/font.ttf", "rb").read())
//|     x = 10
//|     for ch in "Ahoj":
//|         x += font.render_into(ord(ch), 48.0, bitmap, x, 100)
//| """

//| class Font:
//|     def __init__(
//|         self, data: ReadableBuffer, *, size: float = 0, max_glyphs: int = 64, bits_per_pixel: int = 1
//|     ) -> None:
//|         """Reads the tables of a TrueType font. The buffer is kept, not copied.
//|
//|         :param data: the font file, TrueType outlines only (no CFF)
//|         :param size: pixels per em for `get_glyph`; 0 leaves the font without
//|             a glyph cache and only `render` and `render_into` work
//|         :param max_glyphs: how many glyphs the cache holds; it must cover the
//|             distinct characters shown at once, as old slots are reused
//|         :param bits_per_pixel: depth of the cached glyphs: 1 for on/off,
//|             2, 4 or 8 for anti-aliasing with that many bits of coverage"""
//|         ...
static mp_obj_t ttfrast_font_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_data, ARG_size, ARG_max_glyphs, ARG_bits_per_pixel };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_size, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_max_glyphs, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 64} },
        { MP_QSTR_bits_per_pixel, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    float size = 0.0f;
    if (args[ARG_size].u_obj != MP_OBJ_NULL) {
        size = mp_obj_get_float(args[ARG_size].u_obj);
        if (size < 0.0f || size > 1024.0f) {
            mp_arg_error_invalid(MP_QSTR_size);
        }
    }
    mp_int_t max_glyphs = mp_arg_validate_int_range(args[ARG_max_glyphs].u_int, 1, 4096, MP_QSTR_max_glyphs);
    mp_int_t bpp = args[ARG_bits_per_pixel].u_int;
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8) {
        mp_arg_error_invalid(MP_QSTR_bits_per_pixel);
    }

    ttfrast_font_obj_t *self = mp_obj_malloc(ttfrast_font_obj_t, &ttfrast_font_type);
    common_hal_ttfrast_font_construct(self, args[ARG_data].u_obj, size, max_glyphs, bpp);
    return MP_OBJ_FROM_PTR(self);
}

//|     def get_bounding_box(self) -> Tuple[int, int, int, int]:
//|         """The largest glyph cell at the font's size, as
//|         ``(width, height, x_offset, y_offset)`` in the `fontio.FontProtocol`
//|         sense. Needs a font created with a ``size``."""
//|         ...
static mp_obj_t ttfrast_font_get_bounding_box(mp_obj_t self_in) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return common_hal_ttfrast_font_get_bounding_box(self);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_font_get_bounding_box_obj, ttfrast_font_get_bounding_box);

//|     def get_glyph(self, codepoint: int) -> Optional[fontio.Glyph]:
//|         """The glyph for a character at the font's size, rendered into the
//|         cache on first use, or `None` when the font has no such character.
//|         Needs a font created with a ``size``."""
//|         ...
static mp_obj_t ttfrast_font_get_glyph(mp_obj_t self_in, mp_obj_t codepoint_in) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t codepoint = mp_arg_validate_int_range(mp_obj_get_int(codepoint_in), 0, 0xFFFF, MP_QSTR_codepoint);
    return common_hal_ttfrast_font_get_glyph(self, codepoint);
}
static MP_DEFINE_CONST_FUN_OBJ_2(ttfrast_font_get_glyph_obj, ttfrast_font_get_glyph);

//|     def render_into(self, codepoint: int, size: float, bitmap: displayio.Bitmap, x: int, y: int) -> float:
//|         """Draws one glyph into ``bitmap`` with the pen at ``(x, y)`` on the
//|         baseline and returns the advance to the next pen position.
//|
//|         Coverage is quantized to the bitmap's depth: a 1-bit bitmap gets a
//|         threshold, a 4-bit one 16 levels. Pixels the glyph does not touch are
//|         left as they are, so text can go over existing content."""
//|         ...
static mp_obj_t ttfrast_font_render_into(size_t n_args, const mp_obj_t *args) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint32_t codepoint = mp_obj_get_int(args[1]);
    float size = mp_obj_get_float(args[2]);
    if (size <= 0.0f || size > 1024.0f) {
        mp_arg_error_invalid(MP_QSTR_size);
    }
    displayio_bitmap_t *bitmap = mp_arg_validate_type(args[3], &displayio_bitmap_type, MP_QSTR_bitmap);
    int x = mp_obj_get_int(args[4]);
    int y = mp_obj_get_int(args[5]);
    return mp_obj_new_float(common_hal_ttfrast_font_render_into(self, codepoint, size, bitmap, x, y));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ttfrast_font_render_into_obj, 6, 6, ttfrast_font_render_into);

//|     bitmap: Optional[displayio.Bitmap]
//|     """The glyph cache, one cell per tile, or `None` without a ``size``."""
static mp_obj_t ttfrast_font_get_bitmap(mp_obj_t self_in) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return self->atlas ? MP_OBJ_FROM_PTR(self->atlas) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_font_get_bitmap_obj, ttfrast_font_get_bitmap);
MP_PROPERTY_GETTER(ttfrast_font_bitmap_obj, (mp_obj_t)&ttfrast_font_get_bitmap_obj);

//|     size: float
//|     """Pixels per em of the cached glyphs, 0 without a cache."""
static mp_obj_t ttfrast_font_get_size(mp_obj_t self_in) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_float(self->size);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_font_get_size_obj, ttfrast_font_get_size);
MP_PROPERTY_GETTER(ttfrast_font_size_obj, (mp_obj_t)&ttfrast_font_get_size_obj);

//|     def glyph_index(self, codepoint: int) -> int:
//|         """The glyph this character maps to, or 0 when the font has none."""
//|         ...
static mp_obj_t ttfrast_font_glyph_index(mp_obj_t self_in, mp_obj_t codepoint_in) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(common_hal_ttfrast_font_glyph_index(self, mp_obj_get_int(codepoint_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(ttfrast_font_glyph_index_obj, ttfrast_font_glyph_index);

//|     def render(self, codepoint: int, size: float, out: Optional[WriteableBuffer]) -> Tuple[int, int, int, int, float]:
//|         """Renders one glyph at ``size`` pixels per em.
//|
//|         Returns ``(width, height, x_offset, y_offset, advance)``: the size of
//|         the coverage bitmap, where its top left corner sits relative to the
//|         pen position and the baseline, and how far the pen moves.
//|
//|         ``out`` takes ``width * height`` bytes of coverage, 0 to 255. With
//|         `None` the outline is still parsed and drawn into the internal grid
//|         and only the final accumulation into coverage is skipped; the
//|         geometry and advance come back either way."""
//|         ...
static mp_obj_t ttfrast_font_render(size_t n_args, const mp_obj_t *args) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint32_t codepoint = mp_obj_get_int(args[1]);
    float size = mp_obj_get_float(args[2]);
    if (size <= 0.0f || size > 1024.0f) {
        mp_arg_error_invalid(MP_QSTR_size);
    }

    uint8_t *out = NULL;
    size_t out_len = 0;
    if (args[3] != mp_const_none) {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(args[3], &bufinfo, MP_BUFFER_WRITE);
        out = bufinfo.buf;
        out_len = bufinfo.len;
    }

    int w, h, ox, oy;
    float advance;
    common_hal_ttfrast_font_render(self, codepoint, size, out, out_len, &w, &h, &ox, &oy, &advance);

    mp_obj_t items[5] = {
        MP_OBJ_NEW_SMALL_INT(w),
        MP_OBJ_NEW_SMALL_INT(h),
        MP_OBJ_NEW_SMALL_INT(ox),
        MP_OBJ_NEW_SMALL_INT(oy),
        mp_obj_new_float(advance),
    };
    return mp_obj_new_tuple(5, items);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ttfrast_font_render_obj, 4, 4, ttfrast_font_render);

//|     def metrics(self, codepoint: int, size: float) -> Tuple[int, int, int, int, float]:
//|         """The same ``(width, height, x_offset, y_offset, advance)`` that
//|         `render` returns, read from the font tables without touching the
//|         outline. This is what a layout pass should call."""
//|         ...
static mp_obj_t ttfrast_font_metrics(mp_obj_t self_in, mp_obj_t codepoint_in, mp_obj_t size_in) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t codepoint = mp_obj_get_int(codepoint_in);
    float size = mp_obj_get_float(size_in);
    if (size <= 0.0f || size > 1024.0f) {
        mp_arg_error_invalid(MP_QSTR_size);
    }
    int w, h, ox, oy;
    float advance;
    uint16_t glyph;
    common_hal_ttfrast_font_metrics(self, codepoint, size, &w, &h, &ox, &oy, &advance, &glyph);
    mp_obj_t items[5] = {
        MP_OBJ_NEW_SMALL_INT(w),
        MP_OBJ_NEW_SMALL_INT(h),
        MP_OBJ_NEW_SMALL_INT(ox),
        MP_OBJ_NEW_SMALL_INT(oy),
        mp_obj_new_float(advance),
    };
    return mp_obj_new_tuple(5, items);
}
static MP_DEFINE_CONST_FUN_OBJ_3(ttfrast_font_metrics_obj, ttfrast_font_metrics);

//|     def time_render(self, codepoint: int, size: float, out: Optional[WriteableBuffer], count: int) -> int:
//|         """Renders the same glyph ``count`` times and returns the nanoseconds
//|         one render took.
//|
//|         Timing the loop from Python would fold in the interpreter's own cost
//|         and the allocation of the result tuple, which at small sizes is most
//|         of the number. With ``out`` `None` the number leaves out only the
//|         accumulation pass, not the drawing of the outline."""
//|         ...
static mp_obj_t ttfrast_font_time_render(size_t n_args, const mp_obj_t *args) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    uint32_t codepoint = mp_obj_get_int(args[1]);
    float size = mp_obj_get_float(args[2]);
    if (size <= 0.0f || size > 1024.0f) {
        mp_arg_error_invalid(MP_QSTR_size);
    }
    uint8_t *out = NULL;
    size_t out_len = 0;
    if (args[3] != mp_const_none) {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(args[3], &bufinfo, MP_BUFFER_WRITE);
        out = bufinfo.buf;
        out_len = bufinfo.len;
    }
    mp_int_t count = mp_obj_get_int(args[4]);
    if (count < 1) {
        mp_arg_error_invalid(MP_QSTR_count);
    }
    return mp_obj_new_int_from_ull(
        common_hal_ttfrast_font_time_render(self, codepoint, size, out, out_len, count));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(ttfrast_font_time_render_obj, 5, 5, ttfrast_font_time_render);

//|     segments: int
//|     """Line segments the last render emitted, after the curves were flattened."""
static mp_obj_t ttfrast_font_get_segments(mp_obj_t self_in) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->segments);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_font_get_segments_obj, ttfrast_font_get_segments);
MP_PROPERTY_GETTER(ttfrast_font_segments_obj, (mp_obj_t)&ttfrast_font_get_segments_obj);

//|     units_per_em: int
//|     """The font's design grid, which ``size`` is measured against."""
static mp_obj_t ttfrast_font_get_units_per_em(mp_obj_t self_in) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->units_per_em);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_font_get_units_per_em_obj, ttfrast_font_get_units_per_em);
MP_PROPERTY_GETTER(ttfrast_font_units_per_em_obj, (mp_obj_t)&ttfrast_font_get_units_per_em_obj);

//|     glyph_count: int
//|     """How many glyphs the font holds."""
//|
static mp_obj_t ttfrast_font_get_glyph_count(mp_obj_t self_in) {
    ttfrast_font_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->num_glyphs);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_font_get_glyph_count_obj, ttfrast_font_get_glyph_count);
MP_PROPERTY_GETTER(ttfrast_font_glyph_count_obj, (mp_obj_t)&ttfrast_font_get_glyph_count_obj);

static const mp_rom_map_elem_t ttfrast_font_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_glyph_index), MP_ROM_PTR(&ttfrast_font_glyph_index_obj) },
    { MP_ROM_QSTR(MP_QSTR_render), MP_ROM_PTR(&ttfrast_font_render_obj) },
    { MP_ROM_QSTR(MP_QSTR_metrics), MP_ROM_PTR(&ttfrast_font_metrics_obj) },
    { MP_ROM_QSTR(MP_QSTR_time_render), MP_ROM_PTR(&ttfrast_font_time_render_obj) },
    { MP_ROM_QSTR(MP_QSTR_render_into), MP_ROM_PTR(&ttfrast_font_render_into_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_glyph), MP_ROM_PTR(&ttfrast_font_get_glyph_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_bounding_box), MP_ROM_PTR(&ttfrast_font_get_bounding_box_obj) },
    { MP_ROM_QSTR(MP_QSTR_bitmap), MP_ROM_PTR(&ttfrast_font_bitmap_obj) },
    { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&ttfrast_font_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_segments), MP_ROM_PTR(&ttfrast_font_segments_obj) },
    { MP_ROM_QSTR(MP_QSTR_units_per_em), MP_ROM_PTR(&ttfrast_font_units_per_em_obj) },
    { MP_ROM_QSTR(MP_QSTR_glyph_count), MP_ROM_PTR(&ttfrast_font_glyph_count_obj) },
};
static MP_DEFINE_CONST_DICT(ttfrast_font_locals_dict, ttfrast_font_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    ttfrast_font_type,
    MP_QSTR_Font,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, ttfrast_font_make_new,
    locals_dict, &ttfrast_font_locals_dict
    );

static const mp_rom_map_elem_t ttfrast_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ttfrast) },
    { MP_ROM_QSTR(MP_QSTR_Font), MP_ROM_PTR(&ttfrast_font_type) },
    { MP_ROM_QSTR(MP_QSTR_Label), MP_ROM_PTR(&ttfrast_label_type) },
};
static MP_DEFINE_CONST_DICT(ttfrast_module_globals, ttfrast_module_globals_table);

const mp_obj_module_t ttfrast_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ttfrast_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ttfrast, ttfrast_module);
