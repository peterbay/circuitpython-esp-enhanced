// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-module/usb_video/__init__.h"
#include "shared-module/usb_video/USBFramebuffer.h"
#include "shared-bindings/usb_video/USBFramebuffer.h"
#include "shared-bindings/usb_video/__init__.h"

usb_video_uvcframebuffer_obj_t usb_video_uvcframebuffer_singleton_obj = {
    .base = { &usb_video_USBFramebuffer_type },
};

void shared_module_usb_video_uvcframebuffer_get_bufinfo(usb_video_uvcframebuffer_obj_t *self, mp_buffer_info_t *bufinfo) {
    bufinfo->buf = usb_video_framebuffer_rgb565;
    // CIRCUITPY-CHANGE: nothing is buffered when the frame is compressed band
    // by band, so there is no length to report either.
    bufinfo->len = usb_video_framebuffer_rgb565 == NULL
        ? 0 : 2 * usb_video_frame_width * usb_video_frame_height;
}

void shared_module_usb_video_uvcframebuffer_refresh(usb_video_uvcframebuffer_obj_t *self) {
    shared_module_usb_video_swapbuffers();
}

int shared_module_usb_video_uvcframebuffer_get_width(usb_video_uvcframebuffer_obj_t *self) {
    return usb_video_frame_width;
}

int shared_module_usb_video_uvcframebuffer_get_height(usb_video_uvcframebuffer_obj_t *self) {
    return usb_video_frame_height;
}

// CIRCUITPY-CHANGE: with MJPEG the picture is compressed band by band as
// displayio draws it and never held whole, so the framebuffer asks for the
// encoder's band height. Uncompressed streaming still wants the whole frame,
// and zero rows is how that is asked for.
int shared_module_usb_video_uvcframebuffer_get_rows_per_buffer(usb_video_uvcframebuffer_obj_t *self) {
    return usb_video_streaming_rows();
}

void shared_module_usb_video_uvcframebuffer_write_rows(usb_video_uvcframebuffer_obj_t *self, uint16_t y, uint16_t rows, const void *data) {
    usb_video_streaming_write_rows(y, rows, data);
}

// CIRCUITPY-CHANGE: the display is being released, which happens when the
// running program stops or reloads, and can land in the middle of a frame.
void shared_module_usb_video_uvcframebuffer_deinit(usb_video_uvcframebuffer_obj_t *self) {
    usb_video_streaming_stop();
}
