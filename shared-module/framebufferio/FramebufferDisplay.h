// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2019 Scott Shawcroft for Adafruit Industries
// SPDX-FileCopyrightText: Copyright (c) 2020 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"
#include "py/proto.h"

#include "shared-bindings/displayio/Group.h"

#include "shared-module/displayio/area.h"
#include "shared-module/displayio/display_core.h"

typedef struct {
    mp_obj_base_t base;
    displayio_display_core_t core;
    mp_obj_t framebuffer;
    const struct _framebuffer_p_t *framebuffer_protocol;
    mp_buffer_info_t bufinfo;
    uint64_t last_refresh_call;
    uint16_t native_frames_per_second;
    uint16_t native_ms_per_frame;
    uint16_t first_pixel_offset;
    uint16_t row_stride;
    // CIRCUITPY-CHANGE: rows the framebuffer takes at a time, 0 when it keeps
    // the whole frame itself.
    uint16_t rows_per_buffer;
    bool auto_refresh;
    bool first_manual_refresh;
} framebufferio_framebufferdisplay_obj_t;

void framebufferio_framebufferdisplay_background(framebufferio_framebufferdisplay_obj_t *self);
void release_framebufferdisplay(framebufferio_framebufferdisplay_obj_t *self);
void framebufferio_framebufferdisplay_reset(framebufferio_framebufferdisplay_obj_t *self);

void framebufferio_framebufferdisplay_collect_ptrs(framebufferio_framebufferdisplay_obj_t *self);

mp_obj_t common_hal_framebufferio_framebufferdisplay_get_framebuffer(framebufferio_framebufferdisplay_obj_t *self);

typedef bool (*framebuffer_get_reverse_pixels_in_byte_fun)(mp_obj_t);
typedef bool (*framebuffer_get_reverse_pixels_in_word_fun)(mp_obj_t);
typedef bool (*framebuffer_set_brightness_fun)(mp_obj_t, mp_float_t);
typedef int (*framebuffer_get_bytes_per_cell_fun)(mp_obj_t);
typedef int (*framebuffer_get_color_depth_fun)(mp_obj_t);
typedef int (*framebuffer_get_first_pixel_offset_fun)(mp_obj_t);
typedef bool (*framebuffer_get_grayscale_fun)(mp_obj_t);
typedef int (*framebuffer_get_height_fun)(mp_obj_t);
typedef int (*framebuffer_get_native_frames_per_second_fun)(mp_obj_t);
typedef bool (*framebuffer_get_pixels_in_byte_share_row_fun)(mp_obj_t);
typedef int (*framebuffer_get_row_stride_fun)(mp_obj_t);
typedef int (*framebuffer_get_width_fun)(mp_obj_t);
typedef mp_float_t (*framebuffer_get_brightness_fun)(mp_obj_t);
typedef void (*framebuffer_deinit_fun)(mp_obj_t);
typedef void (*framebuffer_get_bufinfo_fun)(mp_obj_t, mp_buffer_info_t *bufinfo);
typedef void (*framebuffer_swapbuffers_fun)(mp_obj_t, uint8_t *dirty_row_bitmask);
// CIRCUITPY-CHANGE: see get_rows_per_buffer below.
typedef int (*framebuffer_get_rows_per_buffer_fun)(mp_obj_t);
typedef void (*framebuffer_write_rows_fun)(mp_obj_t, uint16_t y, uint16_t rows, const void *data);

typedef struct _framebuffer_p_t {
    MP_PROTOCOL_HEAD // MP_QSTR_protocol_framebuffer

    // Mandatory
    framebuffer_get_bufinfo_fun get_bufinfo;
    framebuffer_swapbuffers_fun swapbuffers;
    framebuffer_deinit_fun deinit;
    framebuffer_get_width_fun get_width;
    framebuffer_get_height_fun get_height;

    // Optional getters
    framebuffer_get_bytes_per_cell_fun get_bytes_per_cell; // default: 2
    framebuffer_get_color_depth_fun get_color_depth; // default: 16
    framebuffer_get_first_pixel_offset_fun get_first_pixel_offset; // default: 0
    framebuffer_get_grayscale_fun get_grayscale; // default: grayscale if depth < 8
    framebuffer_get_native_frames_per_second_fun get_native_frames_per_second; // default: 60
    framebuffer_get_pixels_in_byte_share_row_fun get_pixels_in_byte_share_row; // default: false
    framebuffer_get_reverse_pixels_in_byte_fun get_reverse_pixels_in_byte; // default: false
    framebuffer_get_reverse_pixels_in_word_fun get_reverse_pixels_in_word; // default: false
    framebuffer_get_row_stride_fun get_row_stride; // default: 0 (no extra row padding)

    // CIRCUITPY-CHANGE: a framebuffer that consumes the picture as it is drawn
    // rather than keeping all of it. get_rows_per_buffer returning non-zero
    // says so, and write_rows is then handed each band as it is composited;
    // get_bufinfo is never used for pixel data and may report a zero length.
    //
    // The display refreshes in bands already -- it composites into a small
    // buffer and copies that into the framebuffer -- so this only redirects
    // where the band goes. How many rows come at a time is the display's own
    // choice and varies, so a framebuffer that needs them in fixed groups puts
    // them together itself. What this costs is the dirty-area optimisation: a
    // framebuffer that keeps nothing has to be given every row of every frame,
    // so the display refreshes in full each time.
    //
    // Returning 0, or leaving either function out, keeps the whole-frame
    // behaviour.
    framebuffer_get_rows_per_buffer_fun get_rows_per_buffer; // default: 0 (whole frame)
    framebuffer_write_rows_fun write_rows;

    // Optional -- default is no brightness control
    framebuffer_get_brightness_fun get_brightness;
    framebuffer_set_brightness_fun set_brightness;

} framebuffer_p_t;
