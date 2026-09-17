// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Peter Vavrin
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: the JPEG encoder behind usb_video's MJPEG stream, on top of
// Espressif's esp_new_jpeg. Uncompressed YUY2 needs two bytes a pixel, and the
// isochronous link on this chip carries about 200 kB/s however the endpoint is
// sized, so 160x120 is six frames a second and nothing larger is usable.
// Compressing the frame is the only way past that, and the encoder takes
// RGB565 big endian -- exactly what displayio has already put in the
// framebuffer -- so nothing converts the picture on the way.

#include <string.h>

#include "esp_jpeg_common.h"
#include "esp_jpeg_enc.h"

#include "shared-module/usb_video/__init__.h"

static jpeg_enc_handle_t jpeg_encoder = NULL;
// The band the encoder works in, one row of the source, and where the frame
// being built is going.
static size_t jpeg_block_size;
static size_t jpeg_row_bytes;
static uint8_t *jpeg_out_buf;
static size_t jpeg_out_size;
// The encoder reads its input sixteen bytes at a time and faults on anything
// less aligned. displayio composites each band into a uint32_t array on the
// stack, so the band is copied here on the way in. One band at 160x120 is
// 2560 bytes, against the 38400 the whole-frame buffer needed.
static uint8_t *jpeg_band;
// What the encoder was opened with, so that it can be put back to a known
// state, and how many bands of the current frame it has taken.
static jpeg_enc_config_t jpeg_config;
static int jpeg_bands_fed;
static size_t jpeg_band_fill;

bool usb_video_jpeg_init(uint16_t width, uint16_t height, uint8_t quality) {
    usb_video_jpeg_deinit();

    jpeg_enc_config_t config = DEFAULT_JPEG_ENC_CONFIG();
    config.width = width;
    config.height = height;
    config.src_type = JPEG_PIXEL_FORMAT_RGB565_BE;
    // 4:2:2 keeps the chroma resolution a drawn interface needs; 4:2:0 would be
    // smaller but soft on single-pixel coloured detail. It is also what the
    // MJPEG payload has always used, so every host decodes it.
    config.subsampling = JPEG_SUBSAMPLE_422;
    config.quality = quality;
    config.rotate = JPEG_ROTATE_0D;
    // A helper task would want a core and a priority of its own; one frame at a
    // time out of the USB background is enough for what the link can carry.
    config.task_enable = false;

    if (jpeg_enc_open(&config, &jpeg_encoder) != JPEG_ERR_OK) {
        jpeg_encoder = NULL;
        return false;
    }
    jpeg_config = config;
    jpeg_bands_fed = 0;
    // Both counters, not just the band count: a session that ended part way
    // through a band would otherwise leave the next one starting its first
    // frame at an offset into the block, and the picture comes out shifted.
    jpeg_band_fill = 0;
    jpeg_row_bytes = (size_t)width * 2;
    int block = jpeg_enc_get_block_size(jpeg_encoder);
    jpeg_block_size = block > 0 ? (size_t)block : 0;
    return true;
}

// How many rows one band is. The encoder reports the band in bytes and the
// source is two bytes a pixel, so the row length is what converts it.
int usb_video_jpeg_block_rows(void) {
    if (jpeg_encoder == NULL || jpeg_block_size == 0 || jpeg_row_bytes == 0) {
        return 0;
    }
    if (jpeg_block_size % jpeg_row_bytes != 0) {
        // Not a whole number of rows; this path cannot be used.
        return 0;
    }
    if (jpeg_band == NULL) {
        jpeg_band = jpeg_calloc_align(jpeg_block_size, 16);
        if (jpeg_band == NULL) {
            // Without somewhere aligned to copy each band into, the whole-frame
            // path is the only one left.
            return 0;
        }
    }
    return (int)(jpeg_block_size / jpeg_row_bytes);
}

// Put the encoder back to the start of a picture.
//
// A frame gets abandoned when the band loop stops part way -- the program is on
// its way out, or a band arrived out of order. The encoder is then counting
// bands of a picture that will never arrive, and feeding it the next frame's
// first band would run it past the end of the image, so the frame is finished
// off instead: the remaining bands go in blank and the result is thrown away,
// which costs a few milliseconds and leaves the encoder where it expects to be.
void usb_video_jpeg_frame_abandon(void) {
    if (jpeg_encoder == NULL || (jpeg_bands_fed == 0 && jpeg_band_fill == 0)) {
        jpeg_band_fill = 0;
        jpeg_out_buf = NULL;
        jpeg_out_size = 0;
        return;
    }

    bool finished = false;
    if (jpeg_out_buf != NULL && jpeg_band != NULL && jpeg_block_size != 0) {
        memset(jpeg_band, 0, jpeg_block_size);
        int bands = (int)(jpeg_config.height * jpeg_row_bytes / jpeg_block_size) + 1;
        for (int i = 0; i < bands; i++) {
            int out_length = 0;
            int err = jpeg_enc_process_with_block(jpeg_encoder, jpeg_band, (int)jpeg_block_size,
                jpeg_out_buf, (int)jpeg_out_size, &out_length);
            if (err == JPEG_ERR_OK) {
                finished = true;
                break;
            }
            if (err < JPEG_ERR_OK) {
                break;
            }
        }
    }

    if (!finished) {
        // The encoder is still somewhere in the middle of a picture and there is
        // no way to tell it otherwise. Leaving it there and saying no bands are
        // outstanding is worse than starting again: the next frame_begin would
        // see nothing to abandon and feed a fresh picture into the old one.
        jpeg_enc_close(jpeg_encoder);
        jpeg_encoder = NULL;
        if (jpeg_enc_open(&jpeg_config, &jpeg_encoder) != JPEG_ERR_OK) {
            jpeg_encoder = NULL;
        }
    }
    jpeg_bands_fed = 0;
    jpeg_band_fill = 0;
    jpeg_out_buf = NULL;
    jpeg_out_size = 0;
}

bool usb_video_jpeg_frame_begin(uint8_t *out_buf, size_t out_size) {
    if (jpeg_encoder == NULL) {
        return false;
    }
    // A frame that was started and not finished -- displayio stopped part way,
    // or a band was refused -- leaves the encoder counting bands of the old
    // picture. Feeding it the next frame's first band instead runs it past the
    // end of the image, so the old one is finished off first.
    if (jpeg_bands_fed != 0 || jpeg_band_fill != 0) {
        usb_video_jpeg_frame_abandon();
        if (jpeg_encoder == NULL) {
            return false;
        }
    }
    // out_size is passed on, but do not mistake it for a limit: this encoder
    // ignores it. Told it had 8 kB it still produced a complete 22 kB frame and
    // wrote every byte, so the buffer behind this has to be larger than any
    // frame the encoder can make -- see CIRCUITPY_USB_VIDEO_JPEG_FRAME_BYTES.
    jpeg_out_buf = out_buf;
    jpeg_out_size = out_size;
    return true;
}

bool usb_video_jpeg_frame_rows(const void *rows, size_t length, size_t *frame_length) {
    *frame_length = 0;
    if (jpeg_encoder == NULL || jpeg_out_buf == NULL) {
        return false;
    }
    if (jpeg_band == NULL) {
        return false;
    }

    // The display composites in bands of its own choosing; the encoder wants
    // exactly jpeg_block_size at a time, so they are gathered here. The last
    // band of a frame whose height is not a whole number of blocks is padded
    // with what the previous frame left, which the encoder never reaches.
    const uint8_t *src = rows;
    while (length > 0) {
        size_t take = jpeg_block_size - jpeg_band_fill;
        if (take > length) {
            take = length;
        }
        memcpy(jpeg_band + jpeg_band_fill, src, take);
        jpeg_band_fill += take;
        src += take;
        length -= take;

        if (jpeg_band_fill < jpeg_block_size) {
            break;
        }
        jpeg_band_fill = 0;

        int out_length = 0;
        // A positive return is the band size, meaning more bands are wanted;
        // JPEG_ERR_OK means the frame is complete and out_length is its size.
        int err = jpeg_enc_process_with_block(jpeg_encoder, jpeg_band, (int)jpeg_block_size,
            jpeg_out_buf, (int)jpeg_out_size, &out_length);
        jpeg_bands_fed++;
        if (err < JPEG_ERR_OK) {
            return false;
        }
        if (err == JPEG_ERR_OK && out_length > 0) {
            // The picture is complete. Anything still in `rows` belongs to the
            // next one and must not be fed into this call, or a second frame
            // starts inside it and is lost.
            *frame_length = (size_t)out_length;
            jpeg_bands_fed = 0;
            return true;
        }
    }
    return true;
}

void usb_video_jpeg_deinit(void) {
    if (jpeg_encoder != NULL) {
        jpeg_enc_close(jpeg_encoder);
        jpeg_encoder = NULL;
    }
    if (jpeg_band != NULL) {
        jpeg_free_align(jpeg_band);
        jpeg_band = NULL;
    }
    jpeg_row_bytes = 0;
    jpeg_block_size = 0;
    jpeg_out_buf = NULL;
    jpeg_out_size = 0;
    jpeg_bands_fed = 0;
    jpeg_band_fill = 0;
}
