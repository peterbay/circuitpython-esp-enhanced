// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 by Jeff Epler for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "py/runtime.h"
#include <stdint.h>
#include "class/video/video_device.h"
#include "shared-bindings/displayio/Bitmap.h"
#include "shared-bindings/usb_video/__init__.h"
#include "shared-module/bitmapfilter/macros.h"
#include "shared-module/usb_video/__init__.h"
#include "shared-module/usb_video/uvc_usb_descriptors.h"
#include "supervisor/background_callback.h"
#include "supervisor/shared/reload.h"
#include "supervisor/shared/tick.h"
#include "shared/runtime/interrupt_char.h"
#include "device/usbd.h"

static bool do_convert = true;
static unsigned frame_num = 0;
// CIRCUITPY-CHANGE: written by the transfer completion callback, which runs in
// the USB task, and read by the frame path in the task running code.py. The two
// are pinned to the same core so there is no real concurrency, but nothing tells
// the compiler that, and it is free to keep a stale copy in a register.
static volatile unsigned tx_busy = 0;
static unsigned interval_ms = 1000 / DEFAULT_FRAME_RATE;

// TODO must dynamically allocate this, otherwise everyone pays for it
// CIRCUITPY-CHANGE: a word at a time, see convert_framebuffer_maybe().
static uint32_t *frame_buffer_yuyv;
uint16_t *usb_video_framebuffer_rgb565;
// CIRCUITPY-CHANGE: the allocation behind usb_video_framebuffer_rgb565, which
// is handed out aligned because the JPEG encoder asks for that.
static void *framebuffer_rgb565_allocation;

#if CIRCUITPY_USB_VIDEO_JPEG
// CIRCUITPY-CHANGE: the compressed frame, and how much of it the last encode
// produced. Frames vary in size, so the length goes to the host with each one.
//
// There are two of them when the frame is compressed band by band. On that
// path the encoder runs while displayio draws, outside the guard that keeps the
// whole-frame path from writing over a transfer in progress, and one buffer in
// three came out torn. The writer takes whichever buffer is not on the wire,
// and skips a frame outright rather than overwrite one that is.
static uint8_t *frame_buffer_jpeg[2];
static size_t frame_buffer_jpeg_size;
static size_t jpeg_frame_length;
static int jpeg_write_slot;
// Which buffer holds the frame to send, and which one the USB transfer is
// reading. Both cross tasks with tx_busy, so both are volatile for the same
// reason.
static volatile int jpeg_send_slot = -1;
static volatile int jpeg_inflight_slot = -1;
#endif

// CIRCUITPY-CHANGE: rows the encoder takes at a time when the frame is
// compressed as it is drawn, 0 when the whole picture is buffered first.
static uint16_t streaming_rows;
static uint16_t streaming_next_y;
static bool streaming_frame_ok;
#if CIRCUITPY_USB_VIDEO_JPEG
// CIRCUITPY-CHANGE: a frame is being written into the write slot. Sending that
// slot meanwhile would hand the host a half-written picture.
static bool streaming_encoding;
#endif
// CIRCUITPY-CHANGE: false once the display that was feeding us has been
// released. The encoder is then never called again until a new display is
// built, because what refreshes in the meantime is the supervisor's terminal,
// from a stack that has nothing like the room the encoder wants.
static bool streaming_active;
// CIRCUITPY-CHANGE: compressing a frame is spread over its bands, and the
// display runs background tasks between them, so usb_video_cb_fun re-enters
// this module half way through a picture. All it may not do then is send the
// buffer being written into, and streaming_encoding says when that is the case;
// holding every background callback off for the length of a frame was the first
// answer, but it stops the rest of the system for as long as a frame takes to
// compress -- 73 ms at 320x240 -- and it was never what the fault it was added
// for turned out to be.
static void streaming_frame_end(void) {
    #if CIRCUITPY_USB_VIDEO_JPEG
    streaming_encoding = false;
    #endif
}

// CIRCUITPY-CHANGE: 0 streams YUY2, 1-100 streams MJPEG at that quality. It is
// fixed for as long as the framebuffer exists, because it decides both which
// buffers are allocated and which format the descriptor announces, and the
// descriptor is read once at enumeration.
static uint8_t usb_video_jpeg_quality;

static bool usb_video_is_enabled = false;
uint16_t usb_video_frame_width, usb_video_frame_height;

bool shared_module_usb_video_jpeg_available(void) {
    #if CIRCUITPY_USB_VIDEO_JPEG
    return true;
    #else
    return false;
    #endif
}

#if CIRCUITPY_USB_VIDEO_JPEG
static bool streaming_jpeg(void) {
    return usb_video_jpeg_quality > 0;
}
#endif

bool shared_module_usb_video_enable(mp_int_t frame_width, mp_int_t frame_height, mp_int_t jpeg_quality) {
    if (tud_connected()) {
        return false;
    }

    // this will free any previously allocated framebuffer as a side-effect
    shared_module_usb_video_disable();

    if (frame_width & 1) {
        // frame_width must be even, round it up
        frame_width++;
    }

    usb_video_frame_width = frame_width;
    usb_video_frame_height = frame_height;
    usb_video_jpeg_quality = shared_module_usb_video_jpeg_available() ? jpeg_quality : 0;

    size_t framebuffer_size = usb_video_frame_width * usb_video_frame_height * 2;

    size_t output_size = 0;
    bool output_ok;
    #if CIRCUITPY_USB_VIDEO_JPEG
    if (streaming_jpeg()) {
        // CIRCUITPY-CHANGE: the room a compressed frame gets does not follow the
        // size of the picture, because what it has to survive is the worst case
        // rather than the usual one. The encoder ignores the output size it is
        // given and writes past the end instead of reporting that the frame did
        // not fit, so a buffer that merely covers the frames actually seen is a
        // hard fault waiting for an unusual screen -- which is how this was
        // found: the console CircuitPython draws after ctrl-C needed 22743 bytes
        // where the drawn test pattern took 5000, and the buffer held 21120.
        frame_buffer_jpeg_size = USB_VIDEO_JPEG_FRAME_SIZE(usb_video_frame_width, usb_video_frame_height);
        output_size = frame_buffer_jpeg_size;
        frame_buffer_jpeg[0] = port_malloc(output_size, false);
        output_ok = frame_buffer_jpeg[0] != NULL;
        if (output_ok) {
            memset(frame_buffer_jpeg[0], 0, output_size);
            jpeg_frame_length = 0;
            jpeg_write_slot = 0;
            jpeg_send_slot = -1;
            jpeg_inflight_slot = -1;
            if (!usb_video_jpeg_init(usb_video_frame_width, usb_video_frame_height, usb_video_jpeg_quality)) {
                shared_module_usb_video_disable();
                mp_raise_RuntimeError(MP_ERROR_TEXT("Failed to init JPEG encoder"));
            }
            // CIRCUITPY-CHANGE: when the encoder takes the picture in bands,
            // displayio hands each band over as it is drawn and the frame is
            // never held whole. That is the whole point of this path: the
            // RGB565 buffer is two of the four bytes a pixel this module needs.
            // The second compressed buffer it costs is a quarter of what the
            // one it replaces did.
            // CIRCUITPY-CHANGE: the binding has already refused a height that
            // is not a whole number of eight-row bands, but an encoder may want
            // them in larger groups, so the real figure is checked here.
            int rows = usb_video_jpeg_block_rows();
            if (rows > 0 && usb_video_frame_height % rows != 0) {
                shared_module_usb_video_disable();
                mp_arg_error_invalid(MP_QSTR_height);
            }
            if (rows == 0) {
                // Compressing band by band is the only way this module streams
                // MJPEG. There used to be a whole-frame path to fall back on,
                // but nothing could reach it and it had quietly stopped working,
                // so saying so is better than a stream that carries no picture.
                shared_module_usb_video_disable();
                mp_raise_RuntimeError(MP_ERROR_TEXT("Failed to init JPEG encoder"));
            }
            streaming_rows = rows;
            streaming_active = true;
            // The second buffer is not an optimisation. With one, the encoder
            // writes the frame that USB is reading, and every other frame goes
            // out torn; guarding against that instead starves the stream. Better
            // to fail the allocation than to stream that, so this counts towards
            // output_ok.
            frame_buffer_jpeg[1] = port_malloc(output_size, false);
            output_ok = frame_buffer_jpeg[1] != NULL;
            if (output_ok) {
                memset(frame_buffer_jpeg[1], 0, output_size);
                output_size *= 2;
            }
        }
    } else
    #endif
    {
        output_size = framebuffer_size;
        frame_buffer_yuyv = port_malloc(output_size, false);
        output_ok = frame_buffer_yuyv != NULL;
        if (output_ok) {
            memset(frame_buffer_yuyv, 0, output_size);
        }
    }

    if (streaming_rows == 0) {
        // CIRCUITPY-CHANGE: the JPEG encoder on some ports reads the source in
        // sixteen byte pieces and wants it aligned to that, so the drawing
        // buffer is over-allocated and handed out aligned. The raw pointer is
        // what gets freed.
        framebuffer_rgb565_allocation = port_malloc(framebuffer_size + 15, false);
        if (framebuffer_rgb565_allocation) {
            usb_video_framebuffer_rgb565 =
                (uint16_t *)(((uintptr_t)framebuffer_rgb565_allocation + 15) & ~(uintptr_t)15);
            memset(usb_video_framebuffer_rgb565, 0, framebuffer_size);
        }
        if (!usb_video_framebuffer_rgb565) {
            shared_module_usb_video_disable();
            m_malloc_fail(framebuffer_size + output_size);
        }
    }

    if (!output_ok) {
        shared_module_usb_video_disable();
        m_malloc_fail(output_size);
    }

    usb_video_is_enabled = true;

    return true;
}

bool shared_module_usb_video_disable(void) {
    if (tud_connected()) {
        return false;
    }
    usb_video_is_enabled = false;
    #if CIRCUITPY_USB_VIDEO_JPEG
    usb_video_jpeg_deinit();
    for (int i = 0; i < 2; i++) {
        port_free(frame_buffer_jpeg[i]);
        frame_buffer_jpeg[i] = NULL;
    }
    frame_buffer_jpeg_size = 0;
    jpeg_frame_length = 0;
    jpeg_write_slot = 0;
    jpeg_send_slot = -1;
    jpeg_inflight_slot = -1;
    #endif
    port_free(frame_buffer_yuyv);
    frame_buffer_yuyv = NULL;
    port_free(framebuffer_rgb565_allocation);
    framebuffer_rgb565_allocation = NULL;
    usb_video_framebuffer_rgb565 = NULL;
    streaming_rows = 0;
    streaming_next_y = 0;
    streaming_frame_ok = false;
    streaming_active = false;
    streaming_frame_end();
    return true;
}

bool usb_video_enabled(void) {
    return usb_video_is_enabled;
}

size_t usb_video_descriptor_length(void) {
    // CIRCUITPY-CHANGE: the two branches had the endpoint size arguments the
    // wrong way round -- the bulk length was measured with the isochronous
    // buffer size and the other way about. It made no difference, because
    // wMaxPacketSize is two bytes whatever it holds, but it was misleading.
    // The format is chosen in boot.py, so which of the two lengths applies is
    // known by the time the descriptor is assembled.
    #if CIRCUITPY_USB_VIDEO_JPEG
    if (streaming_jpeg()) {
        #if CFG_TUD_VIDEO_STREAMING_BULK
        return sizeof((char[]) {TUD_VIDEO_CAPTURE_DESCRIPTOR_MJPEG_BULK(0, 0, DEFAULT_FRAME_WIDTH, DEFAULT_FRAME_HEIGHT, DEFAULT_FRAME_RATE, 64, 0, 0)});
        #else
        return sizeof((char[]) {TUD_VIDEO_CAPTURE_DESCRIPTOR_MJPEG(0, 0, DEFAULT_FRAME_WIDTH, DEFAULT_FRAME_HEIGHT, DEFAULT_FRAME_RATE, CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE, 0, 0)});
        #endif
    }
    #endif
    #if CFG_TUD_VIDEO_STREAMING_BULK
    return sizeof((char[]) {TUD_VIDEO_CAPTURE_DESCRIPTOR_UNCOMPR_BULK(0, 0, DEFAULT_FRAME_WIDTH, DEFAULT_FRAME_HEIGHT, DEFAULT_FRAME_RATE, 64, 0, 0)});
    #else
    return sizeof((char[]) {TUD_VIDEO_CAPTURE_DESCRIPTOR_UNCOMPR(0, 0, DEFAULT_FRAME_WIDTH, DEFAULT_FRAME_HEIGHT, DEFAULT_FRAME_RATE, CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE, 0, 0)});
    #endif
}

// CIRCUITPY-CHANGE: the bitmapfilter macros encode full-range Y'CbCr, but YUY2
// in the uncompressed UVC payload is BT.601 studio range -- Y in 16..235,
// chroma in 16..240 -- and that is what a host decodes it as. Every colour
// arrived stretched: over all 65536 RGB565 values, encoded here and decoded the
// way a host does, the mean error was 8/255 and the worst 19/255; with the
// coefficients below it is 1/255 and 2/255. Mid grey 51 came back as 41 and 204
// as 219, which is the contrast the picture was showing.
#define RGB888_TO_Y_BT601(r, g, b) (((((r) * 66) + ((g) * 129) + ((b) * 25)) >> 8) + 16)
#define RGB888_TO_U_BT601(r, g, b) (((((r) * -38) - ((g) * 74) + ((b) * 112)) >> 8) + 128)
#define RGB888_TO_V_BT601(r, g, b) (((((r) * 112) - ((g) * 94) - ((b) * 18)) >> 8) + 128)

static void convert_framebuffer_maybe(void) {
    if (!do_convert) {
        return; // new data not ready yet
    }
    do_convert = false; // assumes this happens via background, not interrupt

    #if CIRCUITPY_USB_VIDEO_JPEG
    if (streaming_rows != 0) {
        // The bands were compressed as they were drawn; there is nothing left
        // to do here. A compressed stream is only ever set up this way -- enable
        // refuses the format outright if the encoder cannot take bands -- so
        // there is no whole-frame compression to fall back on.
        return;
    }
    #endif

    // Both buffers come from port_malloc and the width is even, so the
    // destination is word aligned and stays that way. The four bytes go out as
    // one word, which is why the order below is the little-endian one.
    MP_STATIC_ASSERT(MP_ENDIANNESS_LITTLE);
    uint32_t *dest = frame_buffer_yuyv;
    uint16_t *src = usb_video_framebuffer_rgb565;

    for (int i = 0; i < usb_video_frame_width * usb_video_frame_height / 2; i++) {
        uint16_t p1 = IMAGE_GET_RGB565_PIXEL_FAST(src, 0);
        uint16_t p2 = IMAGE_GET_RGB565_PIXEL_FAST(src, 1);
        src += 2;

        // Each pixel is unpacked once. Going through the COLOR_RGB565_TO_Y/U/V
        // macros unpacked the one the chroma was taken from three times over.
        int r1 = COLOR_RGB565_TO_R8(p1), g1 = COLOR_RGB565_TO_G8(p1), b1 = COLOR_RGB565_TO_B8(p1);
        int r2 = COLOR_RGB565_TO_R8(p2), g2 = COLOR_RGB565_TO_G8(p2), b2 = COLOR_RGB565_TO_B8(p2);

        int y1 = RGB888_TO_Y_BT601(r1, g1, b1);
        int y2 = RGB888_TO_Y_BT601(r2, g2, b2);

        // The chroma the pair shares is the average of the two, the usual 4:2:2
        // filter. It used to be whichever pixel was brighter, which on a
        // one-pixel-wide vertical edge is a colour neither pixel had.
        int r = (r1 + r2) >> 1, g = (g1 + g2) >> 1, b = (b1 + b2) >> 1;
        int u = RGB888_TO_U_BT601(r, g, b);
        int v = RGB888_TO_V_BT601(r, g, b);

        *dest++ = (uint32_t)y1 | ((uint32_t)u << 8) | ((uint32_t)y2 << 16) | ((uint32_t)v << 24);
    }
}

// CIRCUITPY-CHANGE: what to hand to tud_video_n_frame_xfer. Uncompressed is
// always the whole buffer; a JPEG frame is as long as it came out.
static void *current_frame_buffer(void) {
    #if CIRCUITPY_USB_VIDEO_JPEG
    if (streaming_jpeg()) {
        return jpeg_send_slot < 0 ? NULL : frame_buffer_jpeg[jpeg_send_slot];
    }
    #endif
    return frame_buffer_yuyv;
}

static size_t current_frame_length(void) {
    #if CIRCUITPY_USB_VIDEO_JPEG
    if (streaming_jpeg()) {
        return jpeg_send_slot < 0 ? 0 : jpeg_frame_length;
    }
    #endif
    return usb_video_frame_width * usb_video_frame_height * 2;
}

void shared_module_usb_video_swapbuffers(void) {
    do_convert = true;
}

// CIRCUITPY-CHANGE: the framebuffer protocol's streaming side. Bands arrive in
// order, the first one starting a frame and the last one finishing it. A band
// out of order means a refresh was interrupted, so the frame is abandoned and
// the previous one keeps going out rather than half a picture.
int usb_video_streaming_rows(void) {
    // Asked once, when a display is built on this framebuffer. That is also
    // what says the encoder may be called again after a display was released.
    #if CIRCUITPY_USB_VIDEO_JPEG
    if (streaming_rows != 0) {
        streaming_active = true;
        streaming_next_y = 0;
        streaming_frame_ok = false;
        streaming_frame_end();
    }
    #endif
    return streaming_rows;
}

// CIRCUITPY-CHANGE: the display is going away -- the program stopped, or it is
// being reloaded -- and it may be doing so half way through a frame. The
// encoder is counting bands of a picture that will never be finished, so it is
// put back to a known state here rather than at the start of the next frame,
// which is too late if anything touches it in between.
void usb_video_streaming_stop(void) {
    #if CIRCUITPY_USB_VIDEO_JPEG
    streaming_active = false;
    streaming_frame_ok = false;
    streaming_next_y = 0;
    streaming_frame_end();
    // Deliberately not touching the encoder here: this runs while the display
    // is being torn down, and that is exactly the context the encoder must not
    // be called from. The next frame, on a display that has been built again,
    // sorts it out.
    #endif
}

void usb_video_streaming_write_rows(uint16_t y, uint16_t rows, const void *data) {
    #if CIRCUITPY_USB_VIDEO_JPEG
    if (streaming_rows == 0 || !streaming_active) {
        return;
    }
    // CIRCUITPY-CHANGE: the display refreshes a band at a time and runs
    // background tasks in between, and on this port that means the USB task --
    // a FreeRTOS task of its own -- gets the processor. By the time the next
    // band arrives the program may be on its way out: a keyboard interrupt is
    // pending, or a file was written and a reload is coming. Carrying on
    // feeding the encoder through a teardown is what faulted the board, so the
    // frame is dropped here, at a band boundary, while there is still a
    // CircuitPython stack to drop it from.
    if (mp_hal_is_interrupted() || autoreload_pending()) {
        streaming_frame_ok = false;
        streaming_frame_end();
        return;
    }
    if (y == 0) {
        streaming_frame_end();          // whatever was in progress is gone
        if (jpeg_write_slot == jpeg_inflight_slot) {
            // The only free buffer is the one going out; let this frame go and
            // compress the next one.
            streaming_frame_ok = false;
            return;
        }
        streaming_frame_ok = usb_video_jpeg_frame_begin(frame_buffer_jpeg[jpeg_write_slot],
            frame_buffer_jpeg_size);
        streaming_next_y = 0;
        streaming_encoding = streaming_frame_ok;
        if (!streaming_frame_ok) {
            streaming_frame_end();
            return;
        }
    }
    if (!streaming_frame_ok || y != streaming_next_y) {
        streaming_frame_ok = false;
        streaming_frame_end();
        return;
    }
    streaming_next_y = y + rows;

    // However many rows the display composited at once, the encoder gets them
    // in the bands it asked for; the port puts them together.
    size_t length = (size_t)rows * usb_video_frame_width * 2;
    size_t frame_length = 0;
    if (!usb_video_jpeg_frame_rows(data, length, &frame_length)) {
        streaming_frame_ok = false;
        streaming_frame_end();
        return;
    }
    if (frame_length > 0) {
        streaming_frame_end();
        jpeg_frame_length = frame_length;
        jpeg_send_slot = jpeg_write_slot;
        // The next frame goes in the other buffer, so this one can be sent
        // while it is being made.
        jpeg_write_slot ^= 1;
    }
    #else
    (void)y;
    (void)rows;
    (void)data;
    #endif
}

size_t usb_video_add_descriptor(uint8_t *descriptor_buf, descriptor_counts_t *descriptor_counts, uint8_t *current_interface_string) {
    usb_add_interface_string(*current_interface_string, "CircuitPython UVC");
    #if CIRCUITPY_USB_VIDEO_JPEG
    const uint8_t usb_video_descriptor_mjpeg[] = {
        #if CFG_TUD_VIDEO_STREAMING_BULK
        TUD_VIDEO_CAPTURE_DESCRIPTOR_MJPEG_BULK(*current_interface_string, descriptor_counts->current_endpoint | 0x80, usb_video_frame_width, usb_video_frame_height, DEFAULT_FRAME_RATE, 64, descriptor_counts->current_interface, descriptor_counts->current_interface + 1)
        #else
        TUD_VIDEO_CAPTURE_DESCRIPTOR_MJPEG(*current_interface_string, descriptor_counts->current_endpoint | 0x80, usb_video_frame_width, usb_video_frame_height, DEFAULT_FRAME_RATE, CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE, descriptor_counts->current_interface, descriptor_counts->current_interface + 1)
        #endif
    };
    #endif
    const uint8_t usb_video_descriptor_uncompr[] = {
        #if CFG_TUD_VIDEO_STREAMING_BULK
        TUD_VIDEO_CAPTURE_DESCRIPTOR_UNCOMPR_BULK(*current_interface_string, descriptor_counts->current_endpoint | 0x80, usb_video_frame_width, usb_video_frame_height, DEFAULT_FRAME_RATE, 64, descriptor_counts->current_interface, descriptor_counts->current_interface + 1)
        #else
        TUD_VIDEO_CAPTURE_DESCRIPTOR_UNCOMPR(*current_interface_string, descriptor_counts->current_endpoint | 0x80, usb_video_frame_width, usb_video_frame_height, DEFAULT_FRAME_RATE, CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE, descriptor_counts->current_interface, descriptor_counts->current_interface + 1)
        #endif
    };

    const uint8_t *usb_video_descriptor = usb_video_descriptor_uncompr;
    size_t usb_video_descriptor_size = sizeof(usb_video_descriptor_uncompr);
    #if CIRCUITPY_USB_VIDEO_JPEG
    if (streaming_jpeg()) {
        usb_video_descriptor = usb_video_descriptor_mjpeg;
        usb_video_descriptor_size = sizeof(usb_video_descriptor_mjpeg);
    }
    #endif
    (*current_interface_string)++;
    // CIRCUITPY-CHANGE: the function is two interfaces -- VideoControl at
    // _itf_num_video_control and VideoStreaming at the next number, which the
    // IAD above also declares as 2. The count was advanced by three, so
    // bNumInterfaces came out one higher than the interfaces actually in the
    // configuration. Windows refuses a device that describes itself that way,
    // and refuses the whole composite device, taking the console and the drive
    // down with it.
    descriptor_counts->current_interface += 2;
    descriptor_counts->num_in_endpoints++;
    descriptor_counts->current_endpoint++;

    memcpy(descriptor_buf, usb_video_descriptor, usb_video_descriptor_size);

    return usb_video_descriptor_size;
}

#if CIRCUITPY_USB_VIDEO_JPEG
// CIRCUITPY-CHANGE: a port that turns CIRCUITPY_USB_VIDEO_JPEG on provides
// these. Without them the module says so at usb_video.enable_framebuffer()
// rather than enumerating as a camera that never sends a frame.
MP_WEAK bool usb_video_jpeg_init(uint16_t width, uint16_t height, uint8_t quality) {
    (void)width;
    (void)height;
    (void)quality;
    return false;
}

MP_WEAK void usb_video_jpeg_deinit(void) {
}


// A port that cannot compress band by band says so with 0 and keeps the
// whole-frame path.
MP_WEAK int usb_video_jpeg_block_rows(void) {
    return 0;
}

MP_WEAK bool usb_video_jpeg_frame_begin(uint8_t *out_buf, size_t out_size) {
    (void)out_buf;
    (void)out_size;
    return false;
}

MP_WEAK void usb_video_jpeg_frame_abandon(void) {
}

MP_WEAK bool usb_video_jpeg_frame_rows(const void *rows, size_t length, size_t *frame_length) {
    (void)rows;
    (void)length;
    (void)frame_length;
    return false;
}
#endif

background_callback_t usb_video_cb;

static void usb_video_cb_fun(void *unused) {
    (void)unused;

    static unsigned start_ms = 0;
    static unsigned already_sent = 0;

    if (!tud_video_n_streaming(0, 0)) {
        already_sent = 0;
        frame_num = 0;
        return;
    }

    if (!already_sent) {
        already_sent = 1;
        start_ms = supervisor_ticks_ms32();
        convert_framebuffer_maybe();
        if (current_frame_length() > 0) {
            bool result = tud_video_n_frame_xfer(0, 0, current_frame_buffer(), current_frame_length());
            (void)result;
        }
    }

    unsigned cur = supervisor_ticks_ms32();
    if (cur - start_ms < interval_ms) {
        background_callback_add(&usb_video_cb, usb_video_cb_fun, NULL); // re-queue
        return;                             // not enough time
    }
    if (tx_busy) {
        background_callback_add(&usb_video_cb, usb_video_cb_fun, NULL); // re-queue
        return;
    }
    start_ms += interval_ms;

    convert_framebuffer_maybe();
    if (current_frame_length() == 0) {
        // Nothing has been drawn yet; there is no frame to send.
        background_callback_add(&usb_video_cb, usb_video_cb_fun, NULL);
        return;
    }
    #if CIRCUITPY_USB_VIDEO_JPEG
    if (streaming_encoding && jpeg_send_slot == jpeg_write_slot) {
        // The only complete frame is the one being written over. Wait.
        background_callback_add(&usb_video_cb, usb_video_cb_fun, NULL);
        return;
    }
    #endif
    bool result = tud_video_n_frame_xfer(0, 0, current_frame_buffer(), current_frame_length());
    // CIRCUITPY-CHANGE: nothing ever set this, so the guard above was dead and
    // the next frame could be converted into frame_buffer_yuyv while the
    // isochronous transfer was still reading it. The completion callback is
    // what clears it again.
    if (result) {
        tx_busy = 1;
        #if CIRCUITPY_USB_VIDEO_JPEG
        // Which buffer must not be written until the transfer is done.
        jpeg_inflight_slot = jpeg_send_slot;
        #endif
    }
}


void usb_video_task(void) {
    if (usb_video_is_enabled) {
        background_callback_add(&usb_video_cb, usb_video_cb_fun, NULL);
    }
}

void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx) {
    (void)ctl_idx;
    (void)stm_idx;
    usb_video_task();
    tx_busy = 0;
    #if CIRCUITPY_USB_VIDEO_JPEG
    jpeg_inflight_slot = -1;
    #endif
    /* flip buffer */
    ++frame_num;
}

int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx,
    video_probe_and_commit_control_t const *parameters) {
    (void)ctl_idx;
    (void)stm_idx;
    /* convert unit to ms from 100 ns */
    interval_ms = parameters->dwFrameInterval / 10000;
    return VIDEO_ERROR_NONE;
}
