// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/runtime.h"

#include "shared-bindings/usb_video/__init__.h"
#include "shared-bindings/usb_video/USBFramebuffer.h"
#include "shared-module/usb_video/__init__.h"

//| """Allows streaming bitmaps to a host computer via USB
//|
//| This makes your CircuitPython device identify to the host computer as a video camera.
//| This mode is also known as "USB UVC".
//|
//| This mode requires 1 IN endpoint. Generally, microcontrollers have a limit on
//| the number of endpoints. If you exceed the number of endpoints, CircuitPython
//| will automatically enter Safe Mode. Even in this case, you may be able to
//| enable USB video by also disabling other USB functions, such as `usb_hid` or
//| `usb_midi`.
//|
//| To enable this mode, you must configure the framebuffer size in ``boot.py`` and then
//| create a display in ``code.py``.
//|
//| .. code-block:: py
//|
//|     # boot.py
//|     import usb_video
//|     usb_video.enable_framebuffer(128, 96)
//|
//| .. code-block:: py
//|
//|     # code.py
//|     import usb_video
//|     import framebufferio
//|     import displayio
//|
//|     displayio.release_displays()
//|     display = framebufferio.FramebufferDisplay(usb_video.USBFramebuffer())
//|
//|     # ... use the display object with displayio Group and TileGrid objects
//|
//| This interface is experimental and may change without notice even in stable
//| versions of CircuitPython."""
//|
//|

//| def enable_framebuffer(
//|     width: int,
//|     height: int,
//|     *,
//|     jpeg_quality: Optional[int] = None,
//| ) -> None:
//|     """Enable a USB video framebuffer, setting the given width & height
//|
//|     This function may only be used from ``boot.py``.
//|
//|     Width is rounded up to a multiple of 2.
//|
//|     With ``jpeg_quality`` left at `None` the frame is sent uncompressed, as
//|     YUY2. Two bytes a pixel is more than a full speed USB link can carry
//|     many times a second, so this limits both the frame rate and how large a
//|     picture is possible.
//|
//|     Giving ``jpeg_quality`` a value from 1 to 100 streams MJPEG instead,
//|     which is several times smaller for the same picture. Higher is better
//|     and larger; around 90 the ringing that JPEG puts along the sharp edges
//|     of drawn graphics stops being measurable. Not every board can compress:
//|     asking for it where nothing can raises `NotImplementedError`.
//|
//|     Compressing takes the picture in bands of eight rows, so ``height`` must
//|     be a multiple of 8 when ``jpeg_quality`` is given.
//|
//|     Two compressed frames are kept, so that one can go out while the next is
//|     being made. There is no single-buffered option: sharing one buffer means
//|     the encoder writes it while USB is reading it, and every other frame
//|     goes out torn.
//|
//|     After boot.py completes, the framebuffer will be allocated. Total storage
//|     of 4×``width``×``height`` bytes is required, reducing the amount available
//|     for Python objects. If the allocation fails, a MemoryError is raised.
//|     This message can be seen in ``boot_out.txt``."""
//|
//|

static mp_obj_t usb_video_enable_framebuffer(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_width, ARG_height, ARG_jpeg_quality };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_width, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_height, MP_ARG_REQUIRED | MP_ARG_INT, { .u_int = 0 } },
        { MP_QSTR_jpeg_quality, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // (but note that most devices will not be able to allocate this much memory.
    uint32_t width = mp_arg_validate_int_range(args[ARG_width].u_int, 0, 32767, MP_QSTR_width);
    uint32_t height = mp_arg_validate_int_range(args[ARG_height].u_int, 0, 32767, MP_QSTR_height);

    // CIRCUITPY-CHANGE: zero means uncompressed, which is what the module did
    // before this argument existed.
    mp_int_t jpeg_quality = 0;
    if (args[ARG_jpeg_quality].u_obj != mp_const_none) {
        jpeg_quality = mp_arg_validate_int_range(
            mp_obj_get_int(args[ARG_jpeg_quality].u_obj), 1, 100, MP_QSTR_jpeg_quality);
        if (!shared_module_usb_video_jpeg_available()) {
            mp_raise_NotImplementedError(MP_ERROR_TEXT("JPEG encoding not available"));
        }
        // CIRCUITPY-CHANGE: the encoder takes the picture in bands of eight
        // rows, so a height that is not a whole number of them cannot be
        // compressed as it is drawn. Said here, because the alternative was a
        // MemoryError from the uncompressed path further in, which names a size
        // and not the reason.
        if (height % 8 != 0) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be multiple of 8."), MP_QSTR_height);
        }
        // CIRCUITPY-CHANGE: the encoder ignores the output size it is handed and
        // writes past the end rather than reporting that the frame did not fit,
        // so a picture that could outgrow the buffer has to be refused before
        // anything is allocated. Measured against white noise, which is the
        // worst case for JPEG, 4:2:2 costs 2.7 bytes a pixel at quality 100;
        // three leaves a margin, and the kilobyte covers the headers.
        if ((size_t)width * height * 3 + 1024 > CIRCUITPY_USB_VIDEO_JPEG_FRAME_BYTES) {
            mp_raise_ValueError(MP_ERROR_TEXT("Frame too large to compress"));
        }
    }

    if (!shared_module_usb_video_enable(width, height, jpeg_quality)) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("Cannot change USB devices now"));
    }

    return mp_const_none;
};
static MP_DEFINE_CONST_FUN_OBJ_KW(usb_video_enable_framebuffer_obj, 0, usb_video_enable_framebuffer);

static const mp_rom_map_elem_t usb_video_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_usb_video) },
    { MP_ROM_QSTR(MP_QSTR_USBFramebuffer), MP_ROM_PTR(&usb_video_USBFramebuffer_type) },
    { MP_ROM_QSTR(MP_QSTR_enable_framebuffer), MP_ROM_PTR(&usb_video_enable_framebuffer_obj) },
};

static MP_DEFINE_CONST_DICT(usb_video_module_globals, usb_video_module_globals_table);

const mp_obj_module_t usb_video_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&usb_video_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_usb_video, usb_video_module);
