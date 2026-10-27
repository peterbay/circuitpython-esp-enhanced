// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

#pragma once

#include "py/obj.h"

// What this firmware adds to the espidf module. None of it is listed in
// __init__.c: extended.c registers a delegation for the module, which the runtime
// asks for any name the module's own table does not have. That keeps the
// upstream file untouched.

// Not static: the generated moduledefs.h declares it extern.
void espidf_extended_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest);

// Called from reset_port() on every soft reset: takes down whatever the ESP-IDF
// still holds that points into the Python heap, before the heap goes.
void espidf_extended_reset(void);

// extended_system.c
extern const mp_obj_fun_builtin_fixed_t espidf_cycles_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_get_time_us_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_task_stats_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_check_heap_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_pin_status_obj;
extern const mp_obj_fun_builtin_var_t espidf_power_management_obj;

// extended_info.c
extern const mp_obj_fun_builtin_var_t espidf_heap_info_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_reset_reason_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_chip_info_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_app_description_obj;
extern const mp_obj_fun_builtin_fixed_t espidf_efuse_user_data_obj;

// extended_timer.c
extern const mp_obj_type_t espidf_timer_type;
void espidf_timer_reset(void);
