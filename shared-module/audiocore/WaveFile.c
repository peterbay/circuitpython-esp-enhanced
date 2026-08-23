// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/audiocore/WaveFile.h"

#include <stdint.h>
#include <string.h>

#include "py/mperrno.h"
#include "py/runtime.h"

#include "shared-module/audiocore/WaveFile.h"
#include "shared-bindings/audiocore/__init__.h"

struct wave_format_chunk {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint16_t extra_params;
    uint16_t valid_bits_per_sample;
    uint32_t channel_mask;
    uint16_t extended_audio_format;
    uint8_t extended_guid[14];
};

void common_hal_audioio_wavefile_construct(audioio_wavefile_obj_t *self,
    pyb_file_obj_t *file,
    uint8_t *buffer,
    size_t buffer_size) {
    // Load the wave
    self->file = file;
    uint8_t chunk_header[12];
    f_rewind(&self->file->fp);
    UINT bytes_read;
    if (f_read(&self->file->fp, chunk_header, 12, &bytes_read) != FR_OK) {
        mp_raise_OSError(MP_EIO);
    }
    if (bytes_read != 12 ||
        memcmp(chunk_header, "RIFF", 4) != 0 ||
        memcmp(chunk_header + 8, "WAVE", 4) != 0) {
        mp_arg_error_invalid(MP_QSTR_file);
    }

    // CIRCUITPY-CHANGE: this used to require the literal bytes "WAVEfmt " at offset
    // 8, i.e. "fmt " had to be the very first subchunk. RIFF only requires it before
    // "data"; a JUNK or LIST chunk ahead of it is perfectly valid and such a file
    // was rejected outright. Walk the subchunks instead.
    uint32_t format_size = 0;
    {
        uint8_t tag[4];
        while (true) {
            if (f_read(&self->file->fp, tag, 4, &bytes_read) != FR_OK || bytes_read != 4) {
                mp_arg_error_invalid(MP_QSTR_file);
            }
            if (f_read(&self->file->fp, &format_size, 4, &bytes_read) != FR_OK || bytes_read != 4) {
                mp_arg_error_invalid(MP_QSTR_file);
            }
            if (memcmp(tag, "fmt ", 4) == 0) {
                break;
            }
            if (memcmp(tag, "data", 4) == 0) {
                // "data" before "fmt " leaves nothing to describe the samples.
                mp_arg_error_invalid(MP_QSTR_file);
            }
            if (f_lseek(&self->file->fp,
                f_tell(&self->file->fp) + format_size + (format_size & 1)) != FR_OK) {
                mp_raise_OSError(MP_EIO);
            }
        }
    }
    // CIRCUITPY-CHANGE: only an oversized chunk was rejected, so a short or zero
    // length one was accepted and the unwritten fields of the struct below were then
    // validated and copied out. The only sizes this parser understands are 16, 18
    // and 40.
    if (format_size != 16 && format_size != 18 && format_size != 40) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid format chunk size"));
    }
    // Zeroed because a 16 or 18 byte chunk leaves the rest of it unwritten.
    struct wave_format_chunk format = {0};
    if (f_read(&self->file->fp, &format, format_size, &bytes_read) != FR_OK) {
        mp_raise_OSError(MP_EIO);
    }
    // CIRCUITPY-CHANGE: this test had an empty body, so a short read fell straight
    // through into the validation below with the struct only partly filled.
    if (bytes_read != format_size) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid format chunk size"));
    }

    // CIRCUITPY-CHANGE: zero channels and zero bits per sample both passed these
    // "not too large" tests, and either one divides by zero further along.
    if ((format_size != 40 && format.audio_format != 1) ||
        format.num_channels < 1 || format.num_channels > 2 ||
        (format.bits_per_sample != 8 && format.bits_per_sample != 16) ||
        format.sample_rate < 1 ||
        (format_size == 18 && format.extra_params != 0) ||
        (format_size == 40 &&
         (format.audio_format != 0xfffe ||
          format.extended_audio_format != 1 ||
          format.valid_bits_per_sample != format.bits_per_sample))) {
        mp_raise_ValueError(MP_ERROR_TEXT("Format not supported"));
    }
    // Get the sample_rate
    self->base.sample_rate = format.sample_rate;
    self->base.channel_count = format.num_channels;
    self->base.bits_per_sample = format.bits_per_sample;
    self->base.samples_signed = format.bits_per_sample > 8;
    self->base.max_buffer_length = 512;
    self->base.single_buffer = false;

    uint8_t chunk_tag[4];
    uint32_t chunk_length;
    bool found_data_chunk = false;

    while (!found_data_chunk) {
        if (f_read(&self->file->fp, &chunk_tag, 4, &bytes_read) != FR_OK) {
            mp_raise_OSError(MP_EIO);
        }
        if (bytes_read != 4) {
            mp_raise_OSError(MP_EIO);
        }
        if (memcmp((uint8_t *)chunk_tag, "data", 4) == 0) {
            found_data_chunk = true;
        }

        if (f_read(&self->file->fp, &chunk_length, 4, &bytes_read) != FR_OK) {
            mp_raise_OSError(MP_EIO);
        }
        if (bytes_read != 4) {
            mp_raise_OSError(MP_EIO);
        }

        if (!found_data_chunk) {
            // CIRCUITPY-CHANGE: a RIFF chunk of odd length is followed by one pad
            // byte that is not counted in the length. Skipping only chunk_length
            // left the read pointer one byte short and the next tag was read
            // straddling the boundary.
            if (f_lseek(&self->file->fp,
                f_tell(&self->file->fp) + chunk_length + (chunk_length & 1)) != FR_OK) {
                mp_raise_OSError(MP_EIO);
            }
        }
    }

    self->file_length = chunk_length;
    self->data_start = self->file->fp.fptr;

    // Try to allocate two buffers, one will be loaded from file and the other
    // DMAed to DAC.
    if (buffer_size) {
        // CIRCUITPY-CHANGE: each half has to be a whole number of words, because the
        // final short read is rounded up to a word boundary in place. Without this a
        // caller-supplied odd size left no room for that rounding.
        self->len = (buffer_size / 2) & ~(size_t)(sizeof(uint32_t) - 1);
        if (self->len == 0) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("%q must be >= %d"), MP_QSTR_buffer_size, 8);
        }
        self->buffer = buffer;
        self->second_buffer = buffer + self->len;
    } else {
        self->len = 256;
        self->buffer = m_malloc_without_collect(self->len);
        if (self->buffer == NULL) {
            common_hal_audioio_wavefile_deinit(self);
            m_malloc_fail(self->len);
        }

        self->second_buffer = m_malloc_without_collect(self->len);
        if (self->second_buffer == NULL) {
            common_hal_audioio_wavefile_deinit(self);
            m_malloc_fail(self->len);
        }
    }
}

void common_hal_audioio_wavefile_deinit(audioio_wavefile_obj_t *self) {
    self->buffer = NULL;
    self->second_buffer = NULL;
    audiosample_mark_deinit(&self->base);
}

void audioio_wavefile_reset_buffer(audioio_wavefile_obj_t *self,
    bool single_channel_output,
    uint8_t channel) {
    if (single_channel_output && channel == 1) {
        return;
    }
    // We don't reset the buffer index in case we're looping and we have an odd number of buffer
    // loads
    self->bytes_remaining = self->file_length;
    f_lseek(&self->file->fp, self->data_start);
    self->read_count = 0;
    self->left_read_count = 0;
    self->right_read_count = 0;
}

audioio_get_buffer_result_t audioio_wavefile_get_buffer(audioio_wavefile_obj_t *self,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer,
    uint32_t *buffer_length) {
    if (!single_channel_output) {
        channel = 0;
    }

    uint32_t channel_read_count = self->left_read_count;
    if (channel == 1) {
        channel_read_count = self->right_read_count;
    }

    bool need_more_data = self->read_count == channel_read_count;

    if (self->bytes_remaining == 0 && need_more_data) {
        *buffer = NULL;
        *buffer_length = 0;
        return GET_BUFFER_DONE;
    }

    if (need_more_data) {
        uint32_t num_bytes_to_load = self->len;
        if (num_bytes_to_load > self->bytes_remaining) {
            num_bytes_to_load = self->bytes_remaining;
        }
        UINT length_read;
        if (self->buffer_index % 2 == 1) {
            *buffer = self->second_buffer;
        } else {
            *buffer = self->buffer;
        }
        if (f_read(&self->file->fp, *buffer, num_bytes_to_load, &length_read) != FR_OK || length_read != num_bytes_to_load) {
            return GET_BUFFER_ERROR;
        }
        self->bytes_remaining -= length_read;
        // Pad the last buffer to word align it.
        // CIRCUITPY-CHANGE: pad was the remainder rather than the number of bytes
        // needed to reach the next word. A final block of length 3 mod 4 therefore
        // grew by three instead of one and wrote two bytes past the allocation,
        // while 1 mod 4 grew by one and stayed misaligned. self->len is now kept a
        // multiple of four, so rounding a short read up can never leave the buffer.
        if (self->bytes_remaining == 0 && length_read % sizeof(uint32_t) != 0) {
            uint32_t pad = sizeof(uint32_t) - (length_read % sizeof(uint32_t));
            length_read += pad;
            if (self->base.bits_per_sample == 8) {
                for (uint32_t i = 0; i < pad; i++) {
                    ((uint8_t *)(*buffer))[length_read - i - 1] = 0x80;
                }
            } else if (self->base.bits_per_sample == 16) {
                // A caller-supplied buffer can be a sliced memoryview, which is not
                // necessarily halfword aligned, so write the silent sample bytewise.
                for (uint32_t i = 0; i < pad; i++) {
                    ((uint8_t *)(*buffer))[length_read - i - 1] = 0;
                }
            }
        }
        *buffer_length = length_read;
        if (self->buffer_index % 2 == 1) {
            self->second_buffer_length = length_read;
        } else {
            self->buffer_length = length_read;
        }
        self->buffer_index += 1;
        self->read_count += 1;
    }

    uint32_t buffers_back = self->read_count - 1 - channel_read_count;
    if ((self->buffer_index - buffers_back) % 2 == 0) {
        *buffer = self->second_buffer;
        *buffer_length = self->second_buffer_length;
    } else {
        *buffer = self->buffer;
        *buffer_length = self->buffer_length;
    }

    if (channel == 0) {
        self->left_read_count += 1;
    } else if (channel == 1) {
        self->right_read_count += 1;
        *buffer = *buffer + self->base.bits_per_sample / 8;
    }

    return self->bytes_remaining == 0 ? GET_BUFFER_DONE : GET_BUFFER_MORE_DATA;
}
