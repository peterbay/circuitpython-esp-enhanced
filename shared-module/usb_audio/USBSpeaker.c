// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Tim Cocks Adafruit Industries LLC
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/misc.h"

#include "shared-bindings/usb_audio/USBSpeaker.h"
#include "shared-bindings/audiocore/__init__.h"
#include "shared-bindings/microcontroller/__init__.h"
#include "shared-module/usb_audio/__init__.h"

#include "tusb.h"

// The ring is sized independently of the TinyUSB headers (see USBSpeaker.h);
// check it still matches the OUT endpoint's software FIFO so the push side can be
// reasoned about against the USB plumbing.
MP_STATIC_ASSERT(USB_AUDIO_SPEAKER_RING_SIZE == CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ);

// Only one speaker can be fed by the single USB OUT endpoint at a time. This
// points at the most recently constructed USBSpeaker, or NULL when none exists,
// mirroring active_microphone in USBMicrophone.c. The USB background task pushes
// received bytes into it via usb_audio_usbspeaker_background_drain().
static usb_audio_usbspeaker_obj_t *active_speaker = NULL;

void common_hal_usb_audio_usbspeaker_construct(usb_audio_usbspeaker_obj_t *self) {
    // The pipeline treats the speaker as an ordinary audiosample source, so
    // populate base from the format negotiated by usb_audio.enable(). The UAC2
    // format we present is 16-bit signed LE PCM, which is exactly what the
    // CircuitPython audio pipeline carries, so no conversion is needed.
    self->base.sample_rate = usb_audio_sample_rate;
    self->base.bits_per_sample = USB_AUDIO_BITS_PER_SAMPLE;
    self->base.channel_count = usb_audio_channel_count;
    self->base.samples_signed = true;
    self->base.single_buffer = false;
    self->base.max_buffer_length = USB_AUDIO_SPEAKER_OUTPUT_BUFFER_SIZE;

    self->ring_head = 0;
    self->ring_tail = 0;
    self->ring_count = 0;
    self->output_index = 0;
    self->ring_level_avg = USB_AUDIO_SPEAKER_RING_TARGET;

    // The most recently created speaker receives the host's OUT stream.
    active_speaker = self;
}

void common_hal_usb_audio_usbspeaker_deinit(usb_audio_usbspeaker_obj_t *self) {
    // Stop directing USB OUT data at this object. The producer (drain) and this
    // deinit both run in non-interrupt context, so the pointer swap needs no
    // interrupt guard.
    if (active_speaker == self) {
        active_speaker = NULL;
    }
    audiosample_mark_deinit(&self->base);
}

bool common_hal_usb_audio_usbspeaker_deinited(usb_audio_usbspeaker_obj_t *self) {
    return audiosample_deinited(&self->base);
}

bool common_hal_usb_audio_usbspeaker_get_connected(usb_audio_usbspeaker_obj_t *self) {
    (void)self;
    // True while the host has opened the OUT streaming interface, i.e. it is
    // actively sending audio. Speaker-specific so it stays correct when a mic
    // shares the same headset function.
    return usb_audio_speaker_streaming();
}

// --------------------------------------------------------------------+
// Receive ring (push side, producer = USB background task)
// --------------------------------------------------------------------+
//
// The ring decouples the two independently clocked stages from
// usb_audio_output_plan.md: USB push (paced by the host's SOF clock, in the
// background task) and the audiosample pull (paced by the output backend's
// sample clock, in its refill interrupt). It is a single-producer/
// single-consumer ring across an interrupt boundary:
//
//   * Producer: usb_audio_usbspeaker_background_drain(), task context.
//   * Consumer: usb_audio_usbspeaker_get_buffer(), output DMA/refill ISR.
//
// The consumer is an interrupt, so it can never be preempted by the producer and
// needs no guard of its own. The producer can be preempted by the consumer, and
// its drop-oldest overrun handling touches both cursors, so it does its whole
// read-modify-write with interrupts disabled.

void usb_audio_usbspeaker_streaming_reset(void) {
    usb_audio_usbspeaker_obj_t *self = active_speaker;
    if (self == NULL) {
        return;
    }
    common_hal_mcu_disable_interrupts();
    self->ring_head = 0;
    self->ring_tail = 0;
    self->ring_count = 0;
    common_hal_mcu_enable_interrupts();
}

void usb_audio_usbspeaker_background_drain(const uint8_t *in, size_t n) {
    usb_audio_usbspeaker_obj_t *self = active_speaker;
    if (self == NULL || n == 0) {
        return;
    }
    if (n >= USB_AUDIO_SPEAKER_RING_SIZE) {
        // A single chunk larger than the whole ring can only contribute its
        // newest tail. (Cannot happen with USB packets << ring size, but keep
        // the copies provably in-bounds.)
        in += n - USB_AUDIO_SPEAKER_RING_SIZE;
        n = USB_AUDIO_SPEAKER_RING_SIZE;
    }

    common_hal_mcu_disable_interrupts();

    size_t free_space = USB_AUDIO_SPEAKER_RING_SIZE - self->ring_count;
    if (n > free_space) {
        // Overrun: advance the read cursor past the oldest bytes we're about to
        // overwrite, keeping latency bounded and following the newest host audio.
        // With the rate adaptation in get_buffer() this is a backstop rather than
        // the normal way the surplus is absorbed. Round up to a whole frame so
        // the consumer, which reads the ring a frame at a time, stays aligned.
        size_t drop = n - free_space;
        size_t ragged = drop % USB_AUDIO_BYTES_PER_FRAME;
        if (ragged != 0) {
            drop += USB_AUDIO_BYTES_PER_FRAME - ragged;
        }
        if (drop > self->ring_count) {
            drop = self->ring_count;
        }
        self->ring_tail = (self->ring_tail + drop) % USB_AUDIO_SPEAKER_RING_SIZE;
        self->ring_count -= drop;
    }

    // Copy in one or two segments, wrapping at the end of the ring.
    size_t first = MIN(n, USB_AUDIO_SPEAKER_RING_SIZE - self->ring_head);
    memcpy(&self->ring[self->ring_head], in, first);
    if (n > first) {
        memcpy(&self->ring[0], in + first, n - first);
    }
    self->ring_head = (self->ring_head + n) % USB_AUDIO_SPEAKER_RING_SIZE;
    self->ring_count += n;

    common_hal_mcu_enable_interrupts();
}

uint32_t common_hal_usb_audio_usbspeaker_read(usb_audio_usbspeaker_obj_t *self,
    void *buffer, uint32_t length) {
    // Hand the most recent host audio to Python so it can be analysed (e.g. an
    // audio-reactive effect or VU meter). This drains the ring just like the
    // output backend's get_buffer() does, so it is an alternative *consumer*:
    // a USBSpeaker being read this way must not also be play()ed to an output
    // backend at the same time, or the two consumers would race on the single
    // SPSC ring.
    //
    // Unlike get_buffer() (which runs in the output refill ISR and so needs no
    // guard) this runs in VM/task context and can be preempted by the USB
    // producer, so it brackets its read-modify-write with interrupts disabled,
    // mirroring usb_audio_usbspeaker_background_drain().
    // The ring holds wire-format frames (always USB_AUDIO_N_CHANNELS channels).
    // `length` and the return value count entries in `buffer`, i.e. 16-bit
    // samples: a stereo speaker writes one per channel of each frame, a mono one
    // keeps only each frame's left channel.
    bool mono = usb_audio_channel_count != USB_AUDIO_N_CHANNELS;
    size_t samples_per_frame = mono ? 1 : USB_AUDIO_N_CHANNELS;
    size_t want = (size_t)(length / samples_per_frame) * USB_AUDIO_BYTES_PER_FRAME;

    common_hal_mcu_disable_interrupts();
    size_t to_copy = MIN(self->ring_count, want);
    // Never split a frame across calls (the ring always holds whole UAC2 frames,
    // but stay defensive so the returned count is always a whole number).
    to_copy -= to_copy % USB_AUDIO_BYTES_PER_FRAME;

    size_t first = MIN(to_copy, USB_AUDIO_SPEAKER_RING_SIZE - self->ring_tail);
    if (MP_LIKELY(!mono)) {
        memcpy(buffer, &self->ring[self->ring_tail], first);
        if (to_copy > first) {
            memcpy((uint8_t *)buffer + first, &self->ring[0], to_copy - first);
        }
    } else {
        int16_t *word_out = (int16_t *)buffer;
        const int16_t *word_ring = (const int16_t *)(const void *)self->ring;
        size_t base = self->ring_tail / USB_AUDIO_N_BYTES_PER_SAMPLE;
        size_t written = 0;
        for (size_t i = 0; i < first / USB_AUDIO_N_BYTES_PER_SAMPLE; i += USB_AUDIO_N_CHANNELS) {
            word_out[written++] = word_ring[base + i];
        }
        for (size_t i = 0; i < (to_copy - first) / USB_AUDIO_N_BYTES_PER_SAMPLE; i += USB_AUDIO_N_CHANNELS) {
            word_out[written++] = word_ring[i];
        }
    }
    self->ring_tail = (self->ring_tail + to_copy) % USB_AUDIO_SPEAKER_RING_SIZE;
    self->ring_count -= to_copy;
    common_hal_mcu_enable_interrupts();

    return to_copy / USB_AUDIO_BYTES_PER_FRAME * samples_per_frame;
}

// --------------------------------------------------------------------+
// audiosample protocol (pull side, consumer = output backend)
// --------------------------------------------------------------------+

void usb_audio_usbspeaker_reset_buffer(usb_audio_usbspeaker_obj_t *self,
    bool single_channel_output, uint8_t channel) {
    (void)single_channel_output;
    (void)channel;
    // Begin playback from live audio rather than whatever was buffered before:
    // drop the ring and restart the double-buffer. Guarded because some ports may
    // call reset_buffer outside the initial setup.
    common_hal_mcu_disable_interrupts();
    self->ring_head = 0;
    self->ring_tail = 0;
    self->ring_count = 0;
    common_hal_mcu_enable_interrupts();
    self->output_index = 0;
    self->ring_level_avg = USB_AUDIO_SPEAKER_RING_TARGET;
}

// Rate adaptation between the host's clock and the output's.
//
// The two are independent, and on some ports the output cannot even produce the
// rate that was asked for: the nRF52 derives its I2S clock as 32 MHz / ratio /
// divisor, so a request for 16 kHz actually runs at 15873 Hz and the host sends
// 127 frames a second more than the output takes. That surplus has to go
// somewhere. Letting the ring fill and then dropping whatever is oldest loses a
// long contiguous stretch in one go, which is plainly audible; the level is
// steered towards a target instead, by taking one frame more or less out of the
// ring than is handed on, at the quietest frame of the block, where a missing or
// repeated frame is not.
//
// With the mismatch above that is one frame in every 128, i.e. a correction in
// nearly every block, so the quiet frame is found inside the block rather than
// waited for, and a block may carry more than one (see MAX_ADJUST). How far off
// the level is is judged from ring_level_avg, not the raw count: the raw count
// falls by a whole block whenever one is pulled and is refilled in USB-packet
// bursts in between, so its sign says nothing about the drift.

// How far the samples move between two adjacent frames. This, not loudness, is
// what decides where a correction is least audible: removing or adding a frame
// shifts everything after it by one sample, so the step it leaves behind is the
// local slope. The quietest sample is the worst possible choice on a tone --
// that is the zero crossing, where the slope is greatest. Indices are in int16
// units from the start of the ring.
static uint32_t speaker_frame_slope(const usb_audio_usbspeaker_obj_t *self, size_t a, size_t b) {
    const int16_t *ring = (const int16_t *)(const void *)self->ring;
    const size_t words = USB_AUDIO_SPEAKER_RING_SIZE / USB_AUDIO_N_BYTES_PER_SAMPLE;
    uint32_t slope = 0;
    for (size_t c = 0; c < USB_AUDIO_N_CHANNELS; c++) {
        int32_t d = ring[(b + c) % words] - ring[(a + c) % words];
        slope += (uint32_t)(d < 0 ? -d : d);
    }
    return slope;
}

audioio_get_buffer_result_t usb_audio_usbspeaker_get_buffer(usb_audio_usbspeaker_obj_t *self,
    bool single_channel_output, uint8_t channel, uint8_t **buffer, uint32_t *buffer_length) {

    uint32_t half = self->base.max_buffer_length / 2;
    uint8_t *out = self->output_buffer + half * self->output_index;
    self->output_index = 1 - self->output_index;

    // The ring always holds wire-format (stereo) frames; a mono sample keeps
    // only the left channel of each, so it fills half as many bytes.
    bool mono = usb_audio_channel_count != USB_AUDIO_N_CHANNELS;
    uint32_t out_length = mono ? half / 2 : half;

    // Consumer side of the SPSC ring (runs in the output backend's refill ISR).
    // It is never preempted by the producer, so no interrupt guard is required.
    size_t want = half / USB_AUDIO_BYTES_PER_FRAME;
    size_t available = self->ring_count / USB_AUDIO_BYTES_PER_FRAME;
    size_t produce = MIN(want, available);

    // Follow the level at a consistent point in the cycle -- before this block is
    // taken -- and low pass it, so what is left is the drift and not the
    // block-by-block sawtooth. A thirty-second of the way each call gives a time
    // constant of about thirty blocks, a quarter of a second.
    // Divide rather than shift: an arithmetic shift of a negative value rounds
    // towards minus infinity, which would bias the average low and settle the
    // loop at the wrong level.
    self->ring_level_avg += ((int32_t)self->ring_count - self->ring_level_avg) / 32;
    int32_t error = self->ring_level_avg - (int32_t)USB_AUDIO_SPEAKER_RING_TARGET;

    // How many frames of correction this block carries, positive to give frames
    // back to the host's surplus and negative to make some up. Only on a block
    // that is otherwise complete: while underrunning there is nothing to give.
    int adjust = 0;
    if (produce == want) {
        const int32_t band = (int32_t)USB_AUDIO_SPEAKER_RING_HYSTERESIS;
        if (error > band) {
            adjust = (int)((error - band) / band) + 1;
            adjust = MIN(adjust, USB_AUDIO_SPEAKER_MAX_ADJUST);
            adjust = MIN(adjust, (int)(available - produce));
        } else if (error < -band) {
            adjust = -((int)((-error - band) / band) + 1);
            adjust = MAX(adjust, -USB_AUDIO_SPEAKER_MAX_ADJUST);
            adjust = MAX(adjust, -(int)(produce - 1));
        }
    }
    size_t consume = produce + adjust;
    size_t n_marks = (size_t)(adjust < 0 ? -adjust : adjust);

    const int16_t *ring = (const int16_t *)(const void *)self->ring;
    const size_t words = USB_AUDIO_SPEAKER_RING_SIZE / USB_AUDIO_N_BYTES_PER_SAMPLE;
    size_t base = self->ring_tail / USB_AUDIO_N_BYTES_PER_SAMPLE;

    // One mark per frame of correction, each the flattest place in its own slice
    // of the block, so several corrections in one block stay spread out. Every
    // mark needs the frame after it too, so none may be the last. Marks come out
    // in increasing order, which the copy below relies on.
    size_t marks[USB_AUDIO_SPEAKER_MAX_ADJUST];
    size_t last_usable = consume >= 2 ? consume - 2 : 0;
    size_t earliest = 0;
    for (size_t m = 0; m < n_marks; m++) {
        // Two marks must never end up adjacent. A correction that takes two
        // frames in also steps the loop past the second one, so a mark sitting
        // there would be missed and the block would come out one frame long --
        // which is one frame past the end of this half of output_buffer.
        size_t from = MIN(MAX(consume * m / n_marks, earliest), last_usable);
        size_t to = MIN(consume * (m + 1) / n_marks, last_usable + 1);
        if (to <= from) {
            n_marks = m;    // no room left for this one; carry it to a later block
            break;
        }
        uint32_t flattest = UINT32_MAX;
        marks[m] = from;
        for (size_t f = from; f < to; f++) {
            uint32_t slope = speaker_frame_slope(self,
                (base + f * USB_AUDIO_N_CHANNELS) % words,
                (base + (f + 1) * USB_AUDIO_N_CHANNELS) % words);
            if (slope < flattest) {
                flattest = slope;
                marks[m] = f;
                if (slope == 0) {
                    break;
                }
            }
        }
        earliest = marks[m] + 2;
    }

    // A correction is made by averaging rather than cutting: to give a frame
    // back, two frames leave as their mean; to make one up, a mean is inserted
    // between them. Either way the waveform keeps its first derivative, where a
    // plain drop or repeat would leave a step of the local slope.
    int16_t *word_out = (int16_t *)(void *)out;
    size_t written = 0;
    size_t next_mark = 0;
    size_t channels = mono ? 1 : USB_AUDIO_N_CHANNELS;
    // The arithmetic above says the block comes out exactly this long; the guard
    // is here because getting it wrong writes past this half of output_buffer
    // and into the object's own fields, which would be found as something else
    // entirely.
    size_t capacity = out_length / USB_AUDIO_N_BYTES_PER_SAMPLE;
    for (size_t f = 0; f < consume && written + channels <= capacity; f++) {
        size_t at = (base + f * USB_AUDIO_N_CHANNELS) % words;
        bool marked = next_mark < n_marks && f == marks[next_mark];
        if (!marked) {
            for (size_t c = 0; c < channels; c++) {
                word_out[written++] = ring[(at + c) % words];
            }
            continue;
        }
        next_mark++;
        size_t next = (base + (f + 1) * USB_AUDIO_N_CHANNELS) % words;
        if (adjust < 0) {
            for (size_t c = 0; c < channels; c++) {
                word_out[written++] = ring[(at + c) % words];
            }
        }
        if (written + channels <= capacity) {
            for (size_t c = 0; c < channels; c++) {
                int32_t mean = ((int32_t)ring[(at + c) % words] + ring[(next + c) % words]) / 2;
                word_out[written++] = (int16_t)mean;
            }
        }
        if (adjust > 0) {
            f++;    // both frames left as their mean
        }
    }

    size_t taken = consume * USB_AUDIO_BYTES_PER_FRAME;
    self->ring_tail = (self->ring_tail + taken) % USB_AUDIO_SPEAKER_RING_SIZE;
    self->ring_count -= taken;

    size_t produced = written * USB_AUDIO_N_BYTES_PER_SAMPLE;
    if (produced < out_length) {
        // Underrun: pad the remainder with silence. Samples are signed, so
        // silence is 0. This is the consume-side of the pacing failure mode
        // tracked in the usb-audio-artifact-pacing memory: we never spin.
        memset(out + produced, 0, out_length - produced);
    }

    // Computed the same way as audiocore.RawSample so stereo (interleaved ring)
    // can extend it.
    if (single_channel_output) {
        out += (channel % self->base.channel_count) * (self->base.bits_per_sample / 8);
    }

    *buffer = out;
    *buffer_length = out_length;
    // A live USB stream is infinite; never report DONE or the backend would stop.
    return GET_BUFFER_MORE_DATA;
}
