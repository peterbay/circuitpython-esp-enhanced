// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2021 Kattni Rembor
//
// SPDX-License-Identifier: MIT

#include "shared-bindings/rainbowio/__init__.h"

int32_t colorwheel(mp_float_t pos) {
    // CIRCUITPY-CHANGE: the range reduction cast the quotient to uint32_t, which for
    // a negative argument does nothing useful -- colorwheel(-1) returned -768 rather
    // than a colour. A descending loop, colorwheel(base - i), reaches it the moment
    // the argument goes below zero. Truncate toward zero and then bring the result
    // back into 0..255, which is what the caller's colour lookup expects.
    pos = pos - (mp_float_t)((int32_t)(pos / 256) * 256);
    if (pos < 0) {
        pos += 256;
    }
    int shift1, shift2;
    if (pos < 85) {
        shift1 = 8;
        shift2 = 16;
    } else if (pos < 170) {
        pos -= 85;
        shift1 = 0;
        shift2 = 8;
    } else {
        pos -= 170;
        shift1 = 16;
        shift2 = 0;
    }
    int p = (int)(pos * 3);
    p = (p < 256) ? p : 255;
    return (p << shift1) | ((255 - p) << shift2);
}
