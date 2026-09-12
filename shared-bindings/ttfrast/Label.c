// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: see shared-module/ttfrast/Label.h.

#include "py/objproperty.h"
#include "py/runtime.h"

#include "shared-bindings/displayio/Group.h"
#include "shared-bindings/ttfrast/__init__.h"
#include "shared-bindings/ttfrast/Label.h"
#include "shared-module/ttfrast/Label.h"

//| class Label(displayio.Group):
//|     """Text drawn from a TrueType outline, laid out and rasterized in C.
//|
//|     A `Label` is a `displayio.Group` holding one tile grid with the whole
//|     string rendered into a bitmap, anti-aliased, so it goes wherever a
//|     Group goes and takes ``x``, ``y``, ``scale`` and ``hidden`` from there.
//|     The group's origin is the pen position at the start of the first
//|     baseline; `bounding_box` says where the ink is relative to it.
//|
//|     .. code-block:: py
//|
//|         import board, displayio, ttfrast
//|
//|         font = ttfrast.Font(open("/font.ttf", "rb").read())
//|         hello = ttfrast.Label(font, "Ahoj", size=32, color=0xFFE060, x=20, y=60)
//|         board.DISPLAY.root_group = hello
//|         hello.text = "Nazdar"
//|     """
//|
//|     def __init__(
//|         self,
//|         font: Font,
//|         text: str = "",
//|         *,
//|         size: Optional[float] = None,
//|         color: int = 0xFFFFFF,
//|         background_color: Optional[int] = None,
//|         line_spacing: float = 1.25,
//|         bits_per_pixel: int = 4,
//|         scale: int = 1,
//|         x: int = 0,
//|         y: int = 0,
//|     ) -> None:
//|         """Lays out and draws ``text``.
//|
//|         :param font: the outline to draw from
//|         :param text: the string; ``\\n`` starts a new line
//|         :param size: pixels per em; defaults to the font's own ``size``
//|         :param color: text colour
//|         :param background_color: fill behind the text, or `None` for
//|             transparent. The anti-aliased edge blends towards this colour,
//|             or towards black when transparent, so on a light background
//|             give it the background colour.
//|         :param line_spacing: baseline to baseline, in ems
//|         :param bits_per_pixel: 1, 2, 4 or 8 levels of edge coverage
//|         :param scale: integer magnification of the whole label
//|         :param x: pen position of the first baseline
//|         :param y: pen position of the first baseline"""
//|         ...
static mp_obj_t ttfrast_label_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_font, ARG_text, ARG_size, ARG_color, ARG_background_color, ARG_line_spacing,
           ARG_bits_per_pixel, ARG_scale, ARG_x, ARG_y };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_font, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_text, MP_ARG_OBJ, {.u_obj = MP_ROM_QSTR(MP_QSTR_)} },
        { MP_QSTR_size, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_color, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0xFFFFFF} },
        { MP_QSTR_background_color, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_line_spacing, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_bits_per_pixel, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 4} },
        { MP_QSTR_scale, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 1} },
        { MP_QSTR_x, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_y, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    ttfrast_font_obj_t *font = mp_arg_validate_type(args[ARG_font].u_obj, &ttfrast_font_type, MP_QSTR_font);
    mp_obj_t text = args[ARG_text].u_obj;
    if (!mp_obj_is_str(text)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q, not %q"), MP_QSTR_text, MP_QSTR_str, mp_obj_get_type(text)->name);
    }

    float size = font->size;
    if (args[ARG_size].u_obj != mp_const_none) {
        size = mp_obj_get_float(args[ARG_size].u_obj);
    }
    if (size <= 0.0f || size > 1024.0f) {
        mp_arg_error_invalid(MP_QSTR_size);
    }
    float line_spacing = 1.25f;
    if (args[ARG_line_spacing].u_obj != MP_OBJ_NULL) {
        line_spacing = mp_obj_get_float(args[ARG_line_spacing].u_obj);
    }
    mp_int_t bpp = args[ARG_bits_per_pixel].u_int;
    if (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8) {
        mp_arg_error_invalid(MP_QSTR_bits_per_pixel);
    }
    mp_int_t scale = mp_arg_validate_int_range(args[ARG_scale].u_int, 1, 32767, MP_QSTR_scale);
    bool has_background = args[ARG_background_color].u_obj != mp_const_none;
    uint32_t background = has_background ? mp_obj_get_int(args[ARG_background_color].u_obj) : 0;

    ttfrast_label_obj_t *self = mp_obj_malloc(ttfrast_label_obj_t, &ttfrast_label_type);
    common_hal_ttfrast_label_construct(self, font, text, size, args[ARG_color].u_int,
        has_background, background, line_spacing, bpp, scale, args[ARG_x].u_int, args[ARG_y].u_int);
    return MP_OBJ_FROM_PTR(self);
}

//|     text: str
//|     """The string shown. Setting it lays the label out again; the bitmap is
//|     kept when the new text has the same extent."""
static mp_obj_t ttfrast_label_get_text(mp_obj_t self_in) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return self->text;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_label_get_text_obj, ttfrast_label_get_text);

static mp_obj_t ttfrast_label_set_text(mp_obj_t self_in, mp_obj_t text) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!mp_obj_is_str(text)) {
        mp_raise_TypeError_varg(MP_ERROR_TEXT("%q must be of type %q, not %q"), MP_QSTR_text, MP_QSTR_str, mp_obj_get_type(text)->name);
    }
    common_hal_ttfrast_label_set_text(self, text);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ttfrast_label_set_text_obj, ttfrast_label_set_text);
MP_PROPERTY_GETSET(ttfrast_label_text_obj, (mp_obj_t)&ttfrast_label_get_text_obj, (mp_obj_t)&ttfrast_label_set_text_obj);

//|     color: int
//|     """Text colour. Changing it only rewrites the palette."""
static mp_obj_t ttfrast_label_get_color(mp_obj_t self_in) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->color);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_label_get_color_obj, ttfrast_label_get_color);

static mp_obj_t ttfrast_label_set_color(mp_obj_t self_in, mp_obj_t color) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    common_hal_ttfrast_label_set_color(self, mp_obj_get_int(color));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ttfrast_label_set_color_obj, ttfrast_label_set_color);
MP_PROPERTY_GETSET(ttfrast_label_color_obj, (mp_obj_t)&ttfrast_label_get_color_obj, (mp_obj_t)&ttfrast_label_set_color_obj);

//|     background_color: Optional[int]
//|     """Fill behind the text, `None` for transparent."""
static mp_obj_t ttfrast_label_get_background_color(mp_obj_t self_in) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return self->has_background ? MP_OBJ_NEW_SMALL_INT(self->background_color) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_label_get_background_color_obj, ttfrast_label_get_background_color);

static mp_obj_t ttfrast_label_set_background_color(mp_obj_t self_in, mp_obj_t color) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (color == mp_const_none) {
        common_hal_ttfrast_label_set_background_color(self, false, 0);
    } else {
        common_hal_ttfrast_label_set_background_color(self, true, mp_obj_get_int(color));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ttfrast_label_set_background_color_obj, ttfrast_label_set_background_color);
MP_PROPERTY_GETSET(ttfrast_label_background_color_obj, (mp_obj_t)&ttfrast_label_get_background_color_obj, (mp_obj_t)&ttfrast_label_set_background_color_obj);

//|     bounding_box: Tuple[int, int, int, int]
//|     """``(x, y, width, height)`` of the ink relative to the group's origin;
//|     ``y`` is negative for anything above the first baseline."""
static mp_obj_t ttfrast_label_get_bounding_box(mp_obj_t self_in) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t items[4] = {
        MP_OBJ_NEW_SMALL_INT(self->bb_x),
        MP_OBJ_NEW_SMALL_INT(self->bb_y),
        MP_OBJ_NEW_SMALL_INT(self->bb_w),
        MP_OBJ_NEW_SMALL_INT(self->bb_h),
    };
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_label_get_bounding_box_obj, ttfrast_label_get_bounding_box);
MP_PROPERTY_GETTER(ttfrast_label_bounding_box_obj, (mp_obj_t)&ttfrast_label_get_bounding_box_obj);

//|     anchor_point: Optional[Tuple[float, float]]
//|     """Which point of the bounding box `anchored_position` places, as
//|     fractions: ``(0, 0)`` the top left, ``(0.5, 0.5)`` the centre,
//|     ``(1, 1)`` the bottom right. Both must be set for it to take effect,
//|     and the group's ``x`` and ``y`` are then moved on every relayout."""
static mp_obj_t ttfrast_label_get_anchor_point(mp_obj_t self_in) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->has_anchor_point) {
        return mp_const_none;
    }
    mp_obj_t items[2] = { mp_obj_new_float(self->anchor_x), mp_obj_new_float(self->anchor_y) };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_label_get_anchor_point_obj, ttfrast_label_get_anchor_point);

static mp_obj_t ttfrast_label_set_anchor_point(mp_obj_t self_in, mp_obj_t value) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (value == mp_const_none) {
        common_hal_ttfrast_label_set_anchor(self, false, 0.0f, 0.0f,
            self->has_anchored_position, self->anchored_x, self->anchored_y);
    } else {
        mp_obj_t *items;
        mp_obj_get_array_fixed_n(value, 2, &items);
        common_hal_ttfrast_label_set_anchor(self, true, mp_obj_get_float(items[0]), mp_obj_get_float(items[1]),
            self->has_anchored_position, self->anchored_x, self->anchored_y);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ttfrast_label_set_anchor_point_obj, ttfrast_label_set_anchor_point);
MP_PROPERTY_GETSET(ttfrast_label_anchor_point_obj, (mp_obj_t)&ttfrast_label_get_anchor_point_obj, (mp_obj_t)&ttfrast_label_set_anchor_point_obj);

//|     anchored_position: Optional[Tuple[int, int]]
//|     """Where, in the parent's coordinates, the `anchor_point` sits."""
//|
static mp_obj_t ttfrast_label_get_anchored_position(mp_obj_t self_in) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->has_anchored_position) {
        return mp_const_none;
    }
    mp_obj_t items[2] = { MP_OBJ_NEW_SMALL_INT(self->anchored_x), MP_OBJ_NEW_SMALL_INT(self->anchored_y) };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(ttfrast_label_get_anchored_position_obj, ttfrast_label_get_anchored_position);

static mp_obj_t ttfrast_label_set_anchored_position(mp_obj_t self_in, mp_obj_t value) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (value == mp_const_none) {
        common_hal_ttfrast_label_set_anchor(self, self->has_anchor_point, self->anchor_x, self->anchor_y,
            false, 0, 0);
    } else {
        mp_obj_t *items;
        mp_obj_get_array_fixed_n(value, 2, &items);
        common_hal_ttfrast_label_set_anchor(self, self->has_anchor_point, self->anchor_x, self->anchor_y,
            true, mp_obj_get_int(items[0]), mp_obj_get_int(items[1]));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(ttfrast_label_set_anchored_position_obj, ttfrast_label_set_anchored_position);
MP_PROPERTY_GETSET(ttfrast_label_anchored_position_obj, (mp_obj_t)&ttfrast_label_get_anchored_position_obj, (mp_obj_t)&ttfrast_label_set_anchored_position_obj);

// Storing an attribute looks only in the type's own locals_dict, so Group's
// settable properties are listed here again; loads would find them through
// the parent on their own.
extern const mp_obj_property_getset_t displayio_group_hidden_obj;
extern const mp_obj_property_getset_t displayio_group_scale_obj;
extern const mp_obj_property_getset_t displayio_group_x_obj;
extern const mp_obj_property_getset_t displayio_group_y_obj;

static const mp_rom_map_elem_t ttfrast_label_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_hidden), MP_ROM_PTR(&displayio_group_hidden_obj) },
    { MP_ROM_QSTR(MP_QSTR_scale), MP_ROM_PTR(&displayio_group_scale_obj) },
    { MP_ROM_QSTR(MP_QSTR_x), MP_ROM_PTR(&displayio_group_x_obj) },
    { MP_ROM_QSTR(MP_QSTR_y), MP_ROM_PTR(&displayio_group_y_obj) },
    { MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&ttfrast_label_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_color), MP_ROM_PTR(&ttfrast_label_color_obj) },
    { MP_ROM_QSTR(MP_QSTR_background_color), MP_ROM_PTR(&ttfrast_label_background_color_obj) },
    { MP_ROM_QSTR(MP_QSTR_bounding_box), MP_ROM_PTR(&ttfrast_label_bounding_box_obj) },
    { MP_ROM_QSTR(MP_QSTR_anchor_point), MP_ROM_PTR(&ttfrast_label_anchor_point_obj) },
    { MP_ROM_QSTR(MP_QSTR_anchored_position), MP_ROM_PTR(&ttfrast_label_anchored_position_obj) },
};
static MP_DEFINE_CONST_DICT(ttfrast_label_locals_dict, ttfrast_label_locals_dict_table);

// Group's methods and properties come through `parent`; they cast self to the
// native Group the same way they do for a Python subclass, which the object
// layout in shared-module/ttfrast/Label.h is built to satisfy. Type slots are
// not inherited, so len(), indexing and iteration are forwarded by hand.
static mp_obj_t ttfrast_label_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_unary_op(op, self->subobj[0]);
}

static mp_obj_t ttfrast_label_subscr(mp_obj_t self_in, mp_obj_t index, mp_obj_t value) {
    ttfrast_label_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_subscr(self->subobj[0], index, value);
}

MP_DEFINE_CONST_OBJ_TYPE(
    ttfrast_label_type,
    MP_QSTR_Label,
    MP_TYPE_FLAG_ITER_IS_GETITER | MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, ttfrast_label_make_new,
    locals_dict, &ttfrast_label_locals_dict,
    subscr, ttfrast_label_subscr,
    unary_op, ttfrast_label_unary_op,
    iter, mp_obj_generic_subscript_getiter,
    parent, &displayio_group_type
    );
