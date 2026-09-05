// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

extern const mp_obj_type_t zigbee_stack_type;

// Called on soft reset, so a script that raised before its deinit() does not
// leave the stack holding the radio until the next power cycle.
void zigbee_reset(void);

// The library requires its lock around every call made from outside one of its
// own callbacks. Shared with the Endpoint type, which reaches into the same
// descriptors.
void zigbee_lock(void);
void zigbee_unlock(void);

// True while the Zigbee stack owns the radio. The raw ieee802154 module checks
// this: the two cannot both drive it.
bool zigbee_holds_radio(void);
