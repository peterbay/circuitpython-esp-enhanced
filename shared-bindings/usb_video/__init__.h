// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#pragma once

#include "shared-module/displayio/Bitmap.h"
// CIRCUITPY-CHANGE: jpeg_quality 0 streams uncompressed YUY2, 1-100 streams
// MJPEG at that quality.
bool shared_module_usb_video_enable(mp_int_t frame_width, mp_int_t frame_height, mp_int_t jpeg_quality);
bool shared_module_usb_video_disable(void);
void shared_module_usb_video_swapbuffers(void);
bool shared_module_usb_video_jpeg_available(void);
