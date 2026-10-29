// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/espsdr/Receiver.h"

//| """Raw I/Q snapshots from the Wi-Fi receiver
//|
//| The ESP32-S3 has an undocumented debug path, used in factory RF testing,
//| that copies the receiver's baseband samples into on-chip SRAM before the
//| Wi-Fi modem sees them. That turns the radio into a low duty cycle software
//| defined receiver for the 2.4 GHz band: up to 16380 complex samples at 80,
//| 40 or 16 MS/s per snapshot, with gaps between snapshots.
//|
//| The register layout comes from the ESP-SDR project's description of the
//| hardware (https://github.com/ESPARGOS/esp-sdr, docs/how-it-works.md). None
//| of it is documented by Espressif, and a different PHY library may break it.
//|
//| Wi-Fi must be on (`wifi.radio.enabled`) and idle: not connected, no access
//| point. The receiver retunes the radio, so Wi-Fi cannot be used while one
//| is open.
//| """

static const mp_rom_map_elem_t espsdr_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_espsdr) },
    { MP_ROM_QSTR(MP_QSTR_Receiver), MP_ROM_PTR(&espsdr_receiver_type) },
    { MP_ROM_QSTR(MP_QSTR_MAX_SAMPLES), MP_ROM_INT(ESPSDR_MAX_SAMPLES) },
};
static MP_DEFINE_CONST_DICT(espsdr_module_globals, espsdr_module_globals_table);

const mp_obj_module_t espsdr_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&espsdr_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_espsdr, espsdr_module);
