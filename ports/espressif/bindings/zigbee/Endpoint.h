// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

#include "ezbee/af.h"

extern const mp_obj_type_t zigbee_endpoint_type;

typedef struct {
    mp_obj_base_t base;
    uint8_t endpoint_id;
    // ZHA device ids run to 0x0302, so this is not a byte.
    uint16_t device_type;
    // Set once the stack has built the endpoint on its own task. Until then
    // there is nothing to read or write: the descriptors do not exist yet.
    bool registered;
} zigbee_endpoint_obj_t;

// A ZCL attribute value, typed by its ZCL type. Shared with Stack.c, which
// turns the values that arrive from other devices into the same Python objects
// a local attribute read gives back.
mp_obj_t zigbee_value_to_python(uint8_t type, const void *raw, uint16_t size);

// The other direction, laying a Python object out in the width and byte order
// the ZCL type says and returning how many bytes that took. Raises rather than
// truncating. Shared with Stack.c, which writes attributes on other devices and
// has no local descriptor to take a size from.
uint16_t zigbee_value_from_python(uint8_t type, mp_obj_t value,
    uint8_t *raw, uint16_t capacity);

// Builds the endpoint through the library's own ZHA helper and hands back the
// descriptor for the caller to add to the device. Runs on the stack's task,
// between esp_zigbee_init() and esp_zigbee_start(), which is the only window in
// which endpoints may be registered.
ezb_af_ep_desc_t zigbee_endpoint_build(zigbee_endpoint_obj_t *self);
