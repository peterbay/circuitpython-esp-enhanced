// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2013, 2014 Damien P. George
// SPDX-FileCopyrightText: Copyright (c) 2015 Josef Gajdusek
// SPDX-FileCopyrightText: Copyright (c) 2017 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <string.h>
#include <stdarg.h>

#include "py/mpprint.h"
#include "py/mpstate.h"
#include "py/runtime.h"
#include "shared-bindings/warnings/__init__.h"
#include "shared-module/warnings/__init__.h"

void common_hal_warnings_warn(const char *message, const mp_obj_type_t *category) {
    warnings_action_t action = MP_STATE_THREAD(warnings_action);
    if (action == WARNINGS_IGNORE) {
        return;
    }
    if (action == WARNINGS_ERROR) {
        // CIRCUITPY-CHANGE: mp_raise_msg_str asserts that the type's make_new is
        // mp_obj_exception_make_new. A Python subclass of Warning has
        // mp_obj_instance_make_new instead, the assert is compiled out in a release
        // build, and the result is a native exception struct stamped with a Python
        // class -- every later mp_obj_exception_get_native() then reads subobj[0]
        // from a struct that has no subobj. Only Warning and FutureWarning exist
        // natively, so naming a category at all means defining a subclass.
        // Note that mp_raise_type_arg is no good here either: it goes through
        // mp_obj_new_exception_args, which carries the same assert and calls
        // mp_obj_exception_make_new directly rather than dispatching on the type.
        // Calling the category as an ordinary callable is what reaches the right
        // make_new for both a native type and a Python subclass.
        nlr_raise(mp_call_function_1(MP_OBJ_FROM_PTR(category),
            mp_obj_new_str(message, strlen(message))));
    }
    mp_printf(MICROPY_ERROR_PRINTER, "%q: %s\n", category->name, message);
}

void warnings_warn(const mp_obj_type_t *category, mp_rom_error_text_t message, ...) {
    warnings_action_t action = MP_STATE_THREAD(warnings_action);
    if (action == WARNINGS_IGNORE) {
        return;
    }
    va_list argptr;
    va_start(argptr, message);
    if (action == WARNINGS_ERROR) {
        mp_raise_msg_vlist(category, message, argptr);
        va_end(argptr);
        // Doesn't get here
        return;
    }

    mp_printf(MICROPY_ERROR_PRINTER, "%q: ", category->name);
    mp_vcprintf(MICROPY_ERROR_PRINTER, message, argptr);
    mp_print_str(MICROPY_ERROR_PRINTER, "\n");
    va_end(argptr);
}

void common_hal_warnings_simplefilter(warnings_action_t action) {
    MP_STATE_THREAD(warnings_action) = action;
}
