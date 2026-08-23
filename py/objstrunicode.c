/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013, 2014 Damien P. George
 * Copyright (c) 2014-2016 Paul Sokolovsky
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>
#include <assert.h>

#include "py/objstr.h"
#include "py/objlist.h"
#include "py/runtime.h"

#if MICROPY_PY_BUILTINS_STR_UNICODE

static mp_obj_t mp_obj_new_str_iterator(mp_obj_t str, mp_obj_iter_buf_t *iter_buf);

/******************************************************************************/
/* str                                                                        */

static void uni_print_quoted(const mp_print_t *print, const byte *str_data, uint str_len) {
    // this escapes characters, but it will be very slow to print (calling print many times)
    bool has_single_quote = false;
    bool has_double_quote = false;
    for (const byte *s = str_data, *top = str_data + str_len; !has_double_quote && s < top; s++) {
        if (*s == '\'') {
            has_single_quote = true;
        } else if (*s == '"') {
            has_double_quote = true;
        }
    }
    unichar quote_char = '\'';
    if (has_single_quote && !has_double_quote) {
        quote_char = '"';
    }
    mp_printf(print, "%c", quote_char);
    const byte *s = str_data, *top = str_data + str_len;
    while (s < top) {
        unichar ch;
        ch = utf8_get_char(s);
        // CIRCUITPY-CHANGE: print printable Unicode chars
        const byte *start = s;
        s = utf8_next_char(s);
        if (ch == quote_char) {
            mp_printf(print, "\\%c", quote_char);
        } else if (ch == '\\') {
            mp_print_str(print, "\\\\");
        } else if (32 <= ch && ch <= 126) {
            mp_printf(print, "%c", ch);
        } else if (ch == '\n') {
            mp_print_str(print, "\\n");
        } else if (ch == '\r') {
            mp_print_str(print, "\\r");
        } else if (ch == '\t') {
            mp_print_str(print, "\\t");
            // CIRCUITPY-CHANGE: print printable Unicode chars
        } else if (ch <= 0x1f || (0x7f <= ch && ch <= 0xa0) || ch == 0xad) {
            mp_printf(print, "\\x%02x", ch);
        } else if ((0x2000 <= ch && ch <= 0x200f) || ch == 0x2028 || ch == 0x2029 || ch == 0xffff) {
            mp_printf(print, "\\u%04x", ch);
        } else if (ch == 0x1ffff) {
            mp_printf(print, "\\U%08x", ch);
        } else {
            // Print the full character out.
            int width = s - start;
            mp_print_strn(print, (const char *)start, width, 0, ' ', width);
        }
    }
    mp_printf(print, "%c", quote_char);
}

static void uni_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    GET_STR_DATA_LEN(self_in, str_data, str_len);
    #if MICROPY_PY_JSON
    if (kind == PRINT_JSON) {
        mp_str_print_json(print, str_data, str_len);
        return;
    }
    #endif
    if (kind == PRINT_STR) {
        print->print_strn(print->data, (const char *)str_data, str_len);
    } else {
        uni_print_quoted(print, str_data, str_len);
    }
}

static mp_obj_t uni_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
    GET_STR_DATA_LEN(self_in, str_data, str_len);
    switch (op) {
        case MP_UNARY_OP_BOOL:
            return mp_obj_new_bool(str_len != 0);
        case MP_UNARY_OP_LEN:
            return MP_OBJ_NEW_SMALL_INT(utf8_charlen(str_data, str_len));
        default:
            return MP_OBJ_NULL; // op not supported
    }
}

// Convert an index into a pointer to its lead byte. Out of bounds indexing will raise IndexError or
// be capped to the first/last character of the string, depending on is_slice.
const byte *str_index_to_ptr(const mp_obj_type_t *type, const byte *self_data, size_t self_len,
    mp_obj_t index, bool is_slice) {
    // All str functions also handle bytes objects, and they call str_index_to_ptr(),
    // so it must handle bytes.
    if (type == &mp_type_bytes
        #if MICROPY_PY_BUILTINS_BYTEARRAY
        || type == &mp_type_bytearray
        #endif
        ) {
        // Taken from objstr.c:str_index_to_ptr()
        size_t index_val = mp_get_index(type, self_len, index, is_slice);
        return self_data + index_val;
    }

    mp_int_t i;
    // Copied from mp_get_index; I don't want bounds checking, just give me
    // the integer as-is. (I can't bounds-check without scanning the whole
    // string; an out-of-bounds index will be caught in the loops below.)
    if (mp_obj_is_small_int(index)) {
        i = MP_OBJ_SMALL_INT_VALUE(index);
    } else if (!mp_obj_get_int_maybe(index, &i)) {
        mp_raise_msg_varg(&mp_type_TypeError, MP_ERROR_TEXT("string indices must be integers, not %s"), mp_obj_get_type_str(index));
    }
    const byte *s, *top = self_data + self_len;
    if (i < 0) {
        // Negative indexing is performed by counting from the end of the string.
        for (s = top - 1; i; --s) {
            if (s < self_data) {
                if (is_slice) {
                    return self_data;
                }
                mp_raise_msg(&mp_type_IndexError, MP_ERROR_TEXT("string index out of range"));
            }
            if (!UTF8_IS_CONT(*s)) {
                ++i;
            }
        }
        ++s;
    } else {
        // Positive indexing, correspondingly, counts from the start of the string.
        // It's assumed that negative indexing will generally be used with small
        // absolute values (eg str[-1], not str[-1000000]), which means it'll be
        // more efficient this way.
        s = self_data;
        while (1) {
            // First check out-of-bounds
            if (s >= top) {
                if (is_slice) {
                    return top;
                }
                mp_raise_msg(&mp_type_IndexError, MP_ERROR_TEXT("string index out of range"));
            }
            // Then check completion
            if (i-- == 0) {
                break;
            }
            // Then skip UTF-8 char
            ++s;
            while (UTF8_IS_CONT(*s)) {
                ++s;
            }
        }
    }
    return s;
}

static mp_obj_t str_subscr(mp_obj_t self_in, mp_obj_t index, mp_obj_t value) {
    const mp_obj_type_t *type = mp_obj_get_type(self_in);
    assert(type == &mp_type_str);
    GET_STR_DATA_LEN(self_in, self_data, self_len);
    if (value == MP_OBJ_SENTINEL) {
        // load
        #if MICROPY_PY_BUILTINS_SLICE
        if (mp_obj_is_type(index, &mp_type_slice)) {
            mp_obj_t ostart, ostop, ostep;
            mp_obj_slice_t *slice = MP_OBJ_TO_PTR(index);
            ostart = slice->start;
            ostop = slice->stop;
            ostep = slice->step;

            if (ostep != mp_const_none && ostep != MP_OBJ_NEW_SMALL_INT(1)) {
                mp_raise_NotImplementedError(MP_ERROR_TEXT("only slices with step=1 (aka None) are supported"));
            }

            const byte *pstart, *pstop;
            if (ostart != mp_const_none) {
                pstart = str_index_to_ptr(type, self_data, self_len, ostart, true);
            } else {
                pstart = self_data;
            }
            if (ostop != mp_const_none) {
                // pstop will point just after the stop character. This depends on
                // the \0 at the end of the string.
                pstop = str_index_to_ptr(type, self_data, self_len, ostop, true);
            } else {
                pstop = self_data + self_len;
            }
            if (pstop < pstart) {
                return MP_OBJ_NEW_QSTR(MP_QSTR_);
            }
            return mp_obj_new_str_of_type(type, (const byte *)pstart, pstop - pstart);
        }
        #endif
        const byte *s = str_index_to_ptr(type, self_data, self_len, index, false);
        int len = 1;
        if (UTF8_IS_NONASCII(*s)) {
            // Count the number of 1 bits (after the first)
            for (char mask = 0x40; *s & mask; mask >>= 1) {
                ++len;
            }
        }
        #if MICROPY_OPT_SINGLE_CHAR_QSTR_CACHE
        // CIRCUITPY-CHANGE: len == 1 is NOT proof of ASCII, which is what this used
        // to assume. A lone continuation byte, 0x80 to 0xBF, has the high bit set but
        // no further one bits, so the counting loop above adds nothing and len stays
        // 1 -- and qstr_from_char then indexes its 128 entry table out of bounds on a
        // release build, where the assert is compiled out. Malformed UTF-8 is
        // reachable from Python; test the byte, not the length.
        if (len == 1 && *s < 0x80) {
            return MP_OBJ_NEW_QSTR(qstr_from_char(*s));
        }
        #endif
        #if MICROPY_OPT_STR_NO_INTERN
        // CIRCUITPY-CHANGE: a character above ASCII would otherwise become a qstr,
        // and qstrs live until the next reset. Two thousand different characters
        // cost about 18 kB that never comes back, and because the run-time pool is
        // not sorted it is scanned linearly, so every later lookup gets slower:
        // measured 125 us per character against 1 us for ASCII. A plain string
        // object costs one allocation and the collector can take it back.
        return mp_obj_new_str((const char *)s, len);
        #else
        return mp_obj_new_str_via_qstr((const char *)s, len); // This will create a one-character string
        #endif
    } else {
        return MP_OBJ_NULL; // op not supported
    }
}

// CIRCUITPY-CHANGE: Diagnose json.dump on invalid types
MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_str,
    MP_QSTR_str,
    MP_TYPE_FLAG_ITER_IS_GETITER | MP_TYPE_FLAG_PRINT_JSON,
    make_new, mp_obj_str_make_new,
    print, uni_print,
    unary_op, uni_unary_op,
    binary_op, mp_obj_str_binary_op,
    subscr, str_subscr,
    iter, mp_obj_new_str_iterator,
    buffer, mp_obj_str_get_buffer,
    locals_dict, &mp_obj_str_locals_dict
    );

/******************************************************************************/
/* str iterator                                                               */

typedef struct _mp_obj_str_it_t {
    mp_obj_base_t base;
    mp_fun_1_t iternext;
    mp_obj_t str;
    size_t cur;
} mp_obj_str_it_t;

static mp_obj_t str_it_iternext(mp_obj_t self_in) {
    mp_obj_str_it_t *self = MP_OBJ_TO_PTR(self_in);
    GET_STR_DATA_LEN(self->str, str, len);
    if (self->cur < len) {
        const byte *cur = str + self->cur;
        const byte *end = utf8_next_char(str + self->cur);
        // CIRCUITPY-CHANGE: see str_subscr for why a character above ASCII is not
        // turned into a qstr here.
        mp_obj_t o_out;
        #if MICROPY_OPT_SINGLE_CHAR_QSTR_CACHE
        // CIRCUITPY-CHANGE: a one byte step is not proof of ASCII either --
        // utf8_next_char advances one byte over a lone continuation byte too. Same
        // out of bounds index into the 128 entry table as in str_subscr.
        if (end - cur == 1 && *cur < 0x80) {
            o_out = MP_OBJ_NEW_QSTR(qstr_from_char(*cur));
        } else
        #endif
        #if MICROPY_OPT_STR_NO_INTERN
        {
            o_out = mp_obj_new_str((const char *)cur, end - cur);
        }
        #else
        {
            o_out = mp_obj_new_str_via_qstr((const char *)cur, end - cur);
        }
        #endif
        self->cur += end - cur;
        return o_out;
    } else {
        return MP_OBJ_STOP_ITERATION;
    }
}

static mp_obj_t mp_obj_new_str_iterator(mp_obj_t str, mp_obj_iter_buf_t *iter_buf) {
    assert(sizeof(mp_obj_str_it_t) <= sizeof(mp_obj_iter_buf_t));
    mp_obj_str_it_t *o = (mp_obj_str_it_t *)iter_buf;
    o->base.type = &mp_type_polymorph_iter;
    o->iternext = str_it_iternext;
    o->str = str;
    o->cur = 0;
    return MP_OBJ_FROM_PTR(o);
}

#endif // MICROPY_PY_BUILTINS_STR_UNICODE
