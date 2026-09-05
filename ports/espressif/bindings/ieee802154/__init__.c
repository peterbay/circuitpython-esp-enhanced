// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Peter Vavrin
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/ieee802154/Radio.h"

//| """Raw IEEE 802.15.4 radio
//|
//| The radio underneath Zigbee, Thread and Matter, exposed as bare frames. It
//| carries no protocol of its own: what goes out is what you hand it, and what
//| comes in is whatever the channel had on it.
//|
//| Available on parts with an 802.15.4 radio, which on this fork means the
//| ESP32-C5.
//| """

//| PENDING_DISABLE: int
//| """Always set the frame pending bit in acknowledgements."""
//|
//| PENDING_ENABLE: int
//| """Set it only when the sender is in the pending address table."""
//|
//| PENDING_ENHANCED: int
//| """As `PENDING_ENABLE`, but for every acknowledgement rather than only those
//| answering a data request."""
//|
//| PENDING_ZIGBEE: int
//| """Clear it only for a short address that is in the table, which is what
//| Zigbee expects."""

static const mp_rom_map_elem_t ieee802154_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ieee802154) },
    { MP_ROM_QSTR(MP_QSTR_Radio), MP_ROM_PTR(&ieee802154_radio_type) },

    { MP_ROM_QSTR(MP_QSTR_PENDING_DISABLE), MP_ROM_INT(0) },
    { MP_ROM_QSTR(MP_QSTR_PENDING_ENABLE), MP_ROM_INT(1) },
    { MP_ROM_QSTR(MP_QSTR_PENDING_ENHANCED), MP_ROM_INT(2) },
    { MP_ROM_QSTR(MP_QSTR_PENDING_ZIGBEE), MP_ROM_INT(3) },
};
static MP_DEFINE_CONST_DICT(ieee802154_module_globals, ieee802154_module_globals_table);

const mp_obj_module_t ieee802154_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ieee802154_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ieee802154, ieee802154_module);
