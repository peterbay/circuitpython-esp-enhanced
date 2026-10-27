// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#include "py/obj.h"
#include "py/runtime.h"

#include "bindings/espidf/extended.h"

#include "esp_heap_caps.h"

extern const mp_obj_module_t espidf_module;

static const mp_rom_map_elem_t espidf_extended_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_cycles), MP_ROM_PTR(&espidf_cycles_obj) },
    // The same counter under the name it had first.
    { MP_ROM_QSTR(MP_QSTR_get_cycle_count), MP_ROM_PTR(&espidf_cycles_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_time_us), MP_ROM_PTR(&espidf_get_time_us_obj) },
    { MP_ROM_QSTR(MP_QSTR_task_stats), MP_ROM_PTR(&espidf_task_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_check_heap), MP_ROM_PTR(&espidf_check_heap_obj) },
    { MP_ROM_QSTR(MP_QSTR_pin_status), MP_ROM_PTR(&espidf_pin_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_power_management), MP_ROM_PTR(&espidf_power_management_obj) },

    { MP_ROM_QSTR(MP_QSTR_heap_info), MP_ROM_PTR(&espidf_heap_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_8BIT), MP_ROM_INT(MALLOC_CAP_8BIT) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_32BIT), MP_ROM_INT(MALLOC_CAP_32BIT) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_DMA), MP_ROM_INT(MALLOC_CAP_DMA) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_EXEC), MP_ROM_INT(MALLOC_CAP_EXEC) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_INTERNAL), MP_ROM_INT(MALLOC_CAP_INTERNAL) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_SPIRAM), MP_ROM_INT(MALLOC_CAP_SPIRAM) },
    { MP_ROM_QSTR(MP_QSTR_MALLOC_CAP_DEFAULT), MP_ROM_INT(MALLOC_CAP_DEFAULT) },
    { MP_ROM_QSTR(MP_QSTR_reset_reason), MP_ROM_PTR(&espidf_reset_reason_obj) },
    { MP_ROM_QSTR(MP_QSTR_chip_info), MP_ROM_PTR(&espidf_chip_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_app_description), MP_ROM_PTR(&espidf_app_description_obj) },
    { MP_ROM_QSTR(MP_QSTR_efuse_user_data), MP_ROM_PTR(&espidf_efuse_user_data_obj) },
};
static MP_DEFINE_CONST_MAP(espidf_extended_globals, espidf_extended_globals_table);

// Called for every name espidf's own table lacks. Only loads are answered:
// these are constants, and a store or delete falls through to the usual
// AttributeError.
void espidf_extended_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    (void)self_in;
    if (dest[0] != MP_OBJ_NULL) {
        return;
    }
    mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)&espidf_extended_globals, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
    if (elem != NULL) {
        dest[0] = elem->value;
    }
}

MP_REGISTER_MODULE_DELEGATION(espidf_module, espidf_extended_attr);
