// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// The dump engine's sample counter is a 15-bit field, and the 64 KiB bank it
// writes holds 16384 words; ESP-SDR stays four short of that.
#define ESPSDR_MAX_SAMPLES (16380)

extern const mp_obj_type_t espsdr_receiver_type;

// Release a receiver left open by a script that ended badly.
void espsdr_reset(void);
