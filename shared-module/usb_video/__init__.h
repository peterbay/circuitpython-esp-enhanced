// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2023 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "shared-module/storage/__init__.h"
#include "shared-module/displayio/Bitmap.h"

bool usb_video_enabled(void);
size_t usb_video_descriptor_length(void);
size_t usb_video_add_descriptor(uint8_t *descriptor_buf, descriptor_counts_t *descriptor_counts, uint8_t *current_interface_string);
void usb_video_task(void);

extern uint16_t usb_video_frame_width, usb_video_frame_height;
extern uint16_t *usb_video_framebuffer_rgb565;

// CIRCUITPY-CHANGE: MJPEG. Uncompressed YUY2 at 160x120 fills the whole
// isochronous budget for six frames a second, so the only way to a larger
// picture or a higher rate is to send less. A port that can compress provides
// these; the default implementations in shared-module say it cannot, and
// usb_video then streams YUY2 as before.
//
// The frame is handed over exactly as displayio left it: RGB565, big endian,
// usb_video_frame_width by usb_video_frame_height, and band by band as it is
// drawn, so no copy of the whole picture is ever kept.
bool usb_video_jpeg_init(uint16_t width, uint16_t height, uint8_t quality);
void usb_video_jpeg_deinit(void);

// How many rows the encoder takes at a time, or 0 if it cannot take bands at
// all -- which usb_video treats as "this port cannot compress", because band by
// band is the only way it streams MJPEG.
int usb_video_jpeg_block_rows(void);
bool usb_video_jpeg_frame_begin(uint8_t *out_buf, size_t out_size);
// Throw away a frame that was started and will not be finished.
void usb_video_jpeg_frame_abandon(void);
// One band. *frame_length stays 0 until the last band completes the frame.
bool usb_video_jpeg_frame_rows(const void *rows, size_t length, size_t *frame_length);

// Used by USBFramebuffer to answer the framebuffer protocol.
int usb_video_streaming_rows(void);
void usb_video_streaming_write_rows(uint16_t y, uint16_t rows, const void *data);
void usb_video_streaming_stop(void);
