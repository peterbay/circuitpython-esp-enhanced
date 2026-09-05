// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Peter Vavrin
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// What the radio wants in its buffer: the PHY length byte plus the largest PSDU
// the standard allows. Used for the transmit buffer and for one queue slot.
#define IEEE802154_FRAME_SLOT (128)

// What Python sees: MAC header plus payload, with the length byte and the
// two-byte checksum taken off. Both are the radio's business, not the caller's.
#define IEEE802154_MAX_FRAME (125)

// Shortest frame that can carry a MAC header at all: frame control and sequence
// number. The radio itself refuses anything shorter.
#define IEEE802154_MIN_FRAME (3)

extern const mp_obj_type_t ieee802154_radio_type;

// Drop any radio left claimed by a script that ended badly.
void ieee802154_reset(void);
