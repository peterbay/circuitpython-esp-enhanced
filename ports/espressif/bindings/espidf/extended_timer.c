// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.Timer; see extended.h.

#include "py/mpstate.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "supervisor/background_callback.h"

#include "esp_timer.h"

// esp_timer, exposed as a class.
//
// The ESP-IDF callback runs in the timer's own task, where nothing may touch the
// interpreter. So it only queues a background callback, which CircuitPython runs
// on the Python thread between two bytecodes. That means the callback is not an
// interrupt: it cannot pre-empt Python code, and it is delayed by anything that
// blocks without running background tasks.
//
// A running timer is held by a list that the collector traces, so it stays alive
// without the program keeping a reference. stop() and deinit() drop it again.

typedef struct {
    mp_obj_base_t base;
    esp_timer_handle_t handle;
    mp_obj_t callback;
    background_callback_t background;
    uint64_t period_us;
    bool repeat;
    bool running;
} espidf_timer_obj_t;

// A running timer must not be collected while the ESP-IDF still holds a pointer
// to it, so it is kept in a list the collector traces.
MP_REGISTER_ROOT_POINTER(mp_obj_t espidf_active_timers);

// The handles are kept a second time, outside the heap. Soft reset has to take
// the ESP-IDF timers down, and by then the list above may already be gone: the
// first version of this walked it during reset and kept firing into freed memory
// afterwards.
#define ESPIDF_MAX_TIMERS (8)
static esp_timer_handle_t espidf_live_handles[ESPIDF_MAX_TIMERS];

// Claims a slot and returns it, or raises. The handle is written into the slot
// once it exists; a reserved but unfilled slot is simply NULL again on the next
// call, so a raise between the two is harmless.
static size_t espidf_handle_reserve(void) {
    for (size_t i = 0; i < ESPIDF_MAX_TIMERS; i++) {
        if (espidf_live_handles[i] == NULL) {
            return i;
        }
    }
    mp_raise_RuntimeError(MP_ERROR_TEXT("too many timers"));
}

static void espidf_handle_untrack(esp_timer_handle_t handle) {
    for (size_t i = 0; i < ESPIDF_MAX_TIMERS; i++) {
        if (espidf_live_handles[i] == handle) {
            espidf_live_handles[i] = NULL;
            return;
        }
    }
}

static void espidf_timer_track(espidf_timer_obj_t *self) {
    if (MP_STATE_VM(espidf_active_timers) == MP_OBJ_NULL) {
        MP_STATE_VM(espidf_active_timers) = mp_obj_new_list(0, NULL);
    }
    mp_obj_list_append(MP_STATE_VM(espidf_active_timers), MP_OBJ_FROM_PTR(self));
}

static void espidf_timer_untrack(espidf_timer_obj_t *self) {
    mp_obj_t list_obj = MP_STATE_VM(espidf_active_timers);
    if (list_obj == MP_OBJ_NULL) {
        return;
    }
    size_t len;
    mp_obj_t *items;
    mp_obj_list_get(list_obj, &len, &items);
    for (size_t i = 0; i < len; i++) {
        if (items[i] == MP_OBJ_FROM_PTR(self)) {
            mp_obj_list_remove(list_obj, items[i]);
            return;
        }
    }
}

// Called on soft reset. Only the handles kept outside the heap are touched here,
// because the objects they belong to may already be gone.
void espidf_timer_reset(void) {
    for (size_t i = 0; i < ESPIDF_MAX_TIMERS; i++) {
        if (espidf_live_handles[i] != NULL) {
            esp_timer_stop(espidf_live_handles[i]);
            esp_timer_delete(espidf_live_handles[i]);
            espidf_live_handles[i] = NULL;
        }
    }
    MP_STATE_VM(espidf_active_timers) = MP_OBJ_NULL;
}

static void espidf_timer_run(void *data) {
    espidf_timer_obj_t *self = data;
    if (self->callback == mp_const_none) {
        return;
    }
    if (!self->repeat) {
        self->running = false;
        espidf_timer_untrack(self);
    }
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_call_function_0(self->callback);
        nlr_pop();
    } else {
        // Nothing above this can handle it, so report it the way an unhandled
        // exception in the main program is reported and keep the timer going.
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
    }
}

static void espidf_timer_isr(void *arg) {
    espidf_timer_obj_t *self = arg;
    background_callback_add(&self->background, espidf_timer_run, self);
}

//| class Timer:
//|     """A periodic or one-shot timer backed by the ESP-IDF esp_timer.
//|
//|     The callback does not run in interrupt context. It is queued and runs on the
//|     Python thread between two bytecodes, so it cannot interrupt running code and
//|     it is delayed while something blocks without servicing background tasks."""
//|
//|     def __init__(self, callback: Callable[[], None], period: float, *, repeat: bool = True) -> None:
//|         """Create a timer and start it. ``period`` is in seconds and is rounded to
//|         whole microseconds."""
//|         ...
static mp_obj_t espidf_timer_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_callback, ARG_period, ARG_repeat };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_callback, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_period, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_repeat, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = true } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (!mp_obj_is_callable(args[ARG_callback].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("callback must be callable"));
    }
    mp_float_t period = mp_obj_get_float(args[ARG_period].u_obj);
    if (!(period > 0)) {
        mp_raise_ValueError(MP_ERROR_TEXT("period must be positive"));
    }
    uint64_t period_us = (uint64_t)(period * 1000000 + (mp_float_t)0.5);
    if (period_us == 0) {
        period_us = 1;
    }

    espidf_timer_obj_t *self = mp_obj_malloc(espidf_timer_obj_t, &espidf_timer_type);
    self->callback = args[ARG_callback].u_obj;
    self->period_us = period_us;
    self->repeat = args[ARG_repeat].u_bool;
    self->running = false;
    self->handle = NULL;

    esp_timer_create_args_t timer_args = {
        .callback = espidf_timer_isr,
        .arg = self,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "CircuitPython espidf.Timer",
    };
    // Take the slot before creating anything, so that running out of slots does
    // not leave an ESP-IDF timer behind with nothing referring to it.
    size_t slot = espidf_handle_reserve();

    CHECK_ESP_RESULT(esp_timer_create(&timer_args, &self->handle));
    espidf_live_handles[slot] = self->handle;
    espidf_timer_track(self);
    self->running = true;
    esp_err_t err = self->repeat
        ? esp_timer_start_periodic(self->handle, period_us)
        : esp_timer_start_once(self->handle, period_us);
    if (err != ESP_OK) {
        self->running = false;
        espidf_timer_untrack(self);
        esp_timer_delete(self->handle);
        espidf_handle_untrack(self->handle);
        self->handle = NULL;
        CHECK_ESP_RESULT(err);
    }
    return MP_OBJ_FROM_PTR(self);
}

//|     def stop(self) -> None:
//|         """Stop the timer. It can be started again with ``start()``."""
//|         ...
static mp_obj_t espidf_timer_stop(mp_obj_t self_in) {
    espidf_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle != NULL && self->running) {
        esp_timer_stop(self->handle);
        self->running = false;
        espidf_timer_untrack(self);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_timer_stop_obj, espidf_timer_stop);

//|     def start(self) -> None:
//|         """Start the timer again with the period it was created with."""
//|         ...
static mp_obj_t espidf_timer_start(mp_obj_t self_in) {
    espidf_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("Timer is deinitialized"));
    }
    if (!self->running) {
        espidf_timer_track(self);
        self->running = true;
        esp_err_t err = self->repeat
            ? esp_timer_start_periodic(self->handle, self->period_us)
            : esp_timer_start_once(self->handle, self->period_us);
        if (err != ESP_OK) {
            self->running = false;
            espidf_timer_untrack(self);
            CHECK_ESP_RESULT(err);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_timer_start_obj, espidf_timer_start);

//|     def deinit(self) -> None:
//|         """Stop the timer and release it."""
//|         ...
static mp_obj_t espidf_timer_deinit(mp_obj_t self_in) {
    espidf_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle != NULL) {
        esp_timer_stop(self->handle);
        esp_timer_delete(self->handle);
        espidf_handle_untrack(self->handle);
        self->handle = NULL;
        self->running = false;
        self->callback = mp_const_none;
        espidf_timer_untrack(self);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_timer_deinit_obj, espidf_timer_deinit);

//|     active: bool
//|     """Whether the timer is currently running. Read-only."""
static mp_obj_t espidf_timer_get_active(mp_obj_t self_in) {
    espidf_timer_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->running);
}
MP_DEFINE_CONST_FUN_OBJ_1(espidf_timer_get_active_obj, espidf_timer_get_active);
MP_PROPERTY_GETTER(espidf_timer_active_obj, (mp_obj_t)&espidf_timer_get_active_obj);

static const mp_rom_map_elem_t espidf_timer_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&espidf_timer_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&espidf_timer_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_timer_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&espidf_timer_active_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_timer_locals_dict, espidf_timer_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_timer_type,
    MP_QSTR_Timer,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_timer_make_new,
    locals_dict, &espidf_timer_locals_dict
    );
