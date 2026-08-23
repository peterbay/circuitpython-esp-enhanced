// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 Scott Shawcroft
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "shared-module/audiocore/__init__.h"

typedef struct {
    audiosample_base_t base;
    // CIRCUITPY-CHANGE: only the raw pointer was kept. A sliced memoryview hands
    // over an interior pointer that is neither block aligned nor at the head of a
    // GC block, so once the view and its source were dropped the collector was free
    // to reclaim the samples while playback was still reading them. Holding the
    // object itself keeps it traced. (A deliberate resize of the source can still
    // move the storage; that is a separate hazard this does not close.)
    mp_obj_t buffer_obj;
    uint8_t *buffer;
    uint8_t buffer_index;
} audioio_rawsample_obj_t;


// These are not available from Python because it may be called in an interrupt.
void audioio_rawsample_reset_buffer(audioio_rawsample_obj_t *self,
    bool single_channel_output,
    uint8_t channel);
audioio_get_buffer_result_t audioio_rawsample_get_buffer(audioio_rawsample_obj_t *self,
    bool single_channel_output,
    uint8_t channel,
    uint8_t **buffer,
    uint32_t *buffer_length);                                                      // length in bytes
