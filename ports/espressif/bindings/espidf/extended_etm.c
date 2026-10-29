// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Petr Vavrin
//
// SPDX-License-Identifier: MIT

// espidf.PinLink, and the ETM drive Comparator(output=) uses; see extended.h.

#include "py/obj.h"
#include "py/objproperty.h"
#include "py/runtime.h"

#include "bindings/espidf/__init__.h"
#include "bindings/espidf/extended.h"
#include "common-hal/microcontroller/Pin.h"
#include "shared-bindings/digitalio/Pull.h"
#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/util.h"
#include "shared/runtime/context_manager_helpers.h"

#include "driver/gpio.h"
#include "driver/gpio_etm.h"
#include "esp_etm.h"

// The Event Task Matrix routes a peripheral's events straight to another
// peripheral's tasks, with no interrupt and no CPU: here an edge on a pin, or
// a comparator crossing, sets or clears another pin.

#if CIRCUITPY_ESPIDF_ETM

void espidf_etm_drive_stop(espidf_etm_drive_t *d) {
    for (int i = 0; i < 2; i++) {
        if (d->chan[i] != NULL) {
            esp_etm_channel_disable(d->chan[i]);
            esp_etm_del_channel(d->chan[i]);
            d->chan[i] = NULL;
        }
    }
    if (d->task_set != NULL) {
        gpio_etm_task_rm_gpio(d->task_set, d->target);
        esp_etm_del_task(d->task_set);
        d->task_set = NULL;
    }
    if (d->task_clr != NULL) {
        gpio_etm_task_rm_gpio(d->task_clr, d->target);
        esp_etm_del_task(d->task_clr);
        d->task_clr = NULL;
    }
}

esp_err_t espidf_etm_drive_start(espidf_etm_drive_t *d, esp_etm_event_handle_t up,
    esp_etm_event_handle_t down, int target, bool invert, int initial) {
    d->chan[0] = d->chan[1] = NULL;
    d->task_set = d->task_clr = NULL;
    d->target = target;

    // Input and output both, so that the level can be read back.
    esp_err_t err = gpio_set_direction(target, GPIO_MODE_INPUT_OUTPUT);
    if (err == ESP_OK) {
        err = gpio_set_level(target, initial);
    }
    gpio_etm_task_config_t task_config = { .actions = { GPIO_ETM_TASK_ACTION_SET, GPIO_ETM_TASK_ACTION_CLR } };
    if (err == ESP_OK) {
        err = gpio_new_etm_task(&task_config, &d->task_set, &d->task_clr);
    }
    if (err == ESP_OK) {
        err = gpio_etm_task_add_gpio(d->task_set, target);
    }
    if (err == ESP_OK) {
        err = gpio_etm_task_add_gpio(d->task_clr, target);
    }
    esp_etm_channel_config_t chan_config = { 0 };
    for (int i = 0; i < 2 && err == ESP_OK; i++) {
        err = esp_etm_new_channel(&chan_config, &d->chan[i]);
    }
    if (err == ESP_OK) {
        err = esp_etm_channel_connect(d->chan[0], up, invert ? d->task_clr : d->task_set);
    }
    if (err == ESP_OK) {
        err = esp_etm_channel_connect(d->chan[1], down, invert ? d->task_set : d->task_clr);
    }
    for (int i = 0; i < 2 && err == ESP_OK; i++) {
        err = esp_etm_channel_enable(d->chan[i]);
    }
    if (err != ESP_OK) {
        espidf_etm_drive_stop(d);
    }
    return err;
}

// GPIO ETM events and tasks are each eight to a chip; two of each per link.
#define PINLINK_SLOTS (4)

typedef struct {
    mp_obj_base_t base;
    espidf_etm_drive_t drive;
    esp_etm_event_handle_t rise;
    esp_etm_event_handle_t fall;
    const mcu_pin_obj_t *source;
    const mcu_pin_obj_t *target;
    bool invert;
    digitalio_pull_t pull;
} espidf_pinlink_obj_t;

static espidf_pinlink_obj_t *_open[PINLINK_SLOTS];

static void pinlink_close(espidf_pinlink_obj_t *self) {
    if (self->source == NULL) {
        return;
    }
    espidf_etm_drive_stop(&self->drive);
    if (self->rise != NULL) {
        esp_etm_del_event(self->rise);
        self->rise = NULL;
    }
    if (self->fall != NULL) {
        esp_etm_del_event(self->fall);
        self->fall = NULL;
    }
    reset_pin_number(self->source->number);
    reset_pin_number(self->target->number);
    self->source = NULL;
    for (size_t i = 0; i < PINLINK_SLOTS; i++) {
        if (_open[i] == self) {
            _open[i] = NULL;
        }
    }
}

void espidf_etm_reset(void) {
    for (size_t i = 0; i < PINLINK_SLOTS; i++) {
        if (_open[i] != NULL) {
            pinlink_close(_open[i]);
        }
    }
}

static void apply_pull(gpio_num_t pin, digitalio_pull_t pull) {
    CHECK_ESP_RESULT(gpio_set_pull_mode(pin, pull == PULL_UP ? GPIO_PULLUP_ONLY :
        pull == PULL_DOWN ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING));
}

//| class PinLink:
//|     """``target`` follows ``source`` (or its inverse) in hardware, through
//|     the Event Task Matrix: each edge on ``source`` sets or clears ``target``
//|     with no interrupt and no CPU. ESP32-C5 only (the S3 has no ETM)."""
//|
//|     def __init__(
//|         self,
//|         source: microcontroller.Pin,
//|         target: microcontroller.Pin,
//|         *,
//|         invert: bool = False,
//|         pull: Optional[digitalio.Pull] = None,
//|     ) -> None:
//|         """
//|         :param ~microcontroller.Pin source: input whose edges are followed.
//|         :param ~microcontroller.Pin target: output driven from them; its input
//|           stays on, so its level can be read back.
//|         :param bool invert: drive ``target`` to the opposite level.
//|         :param ~digitalio.Pull pull: a weak pull on ``source``."""
//|         ...
//|
static mp_obj_t espidf_pinlink_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_source, ARG_target, ARG_invert, ARG_pull };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_source, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_target, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_invert, MP_ARG_KW_ONLY | MP_ARG_BOOL, { .u_bool = false } },
        { MP_QSTR_pull, MP_ARG_KW_ONLY | MP_ARG_OBJ, { .u_obj = mp_const_none } },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    const mcu_pin_obj_t *source = validate_obj_is_free_pin(args[ARG_source].u_obj, MP_QSTR_source);
    const mcu_pin_obj_t *target = validate_obj_is_free_pin(args[ARG_target].u_obj, MP_QSTR_target);
    if (source == target) {
        mp_raise_ValueError(MP_ERROR_TEXT("source and target must differ"));
    }
    bool invert = args[ARG_invert].u_bool;
    digitalio_pull_t pull = validate_pull(args[ARG_pull].u_obj, MP_QSTR_pull);

    size_t slot = 0;
    while (slot < PINLINK_SLOTS && _open[slot] != NULL) {
        slot++;
    }
    if (slot == PINLINK_SLOTS) {
        mp_raise_RuntimeError(MP_ERROR_TEXT("All PinLinks in use"));
    }

    espidf_pinlink_obj_t *self = mp_obj_malloc_with_finaliser(espidf_pinlink_obj_t, &espidf_pinlink_type);
    self->rise = self->fall = NULL;
    self->source = NULL;
    self->target = target;
    self->invert = invert;
    self->pull = pull;

    CHECK_ESP_RESULT(gpio_set_direction(source->number, GPIO_MODE_INPUT));
    apply_pull(source->number, pull);
    gpio_etm_event_config_t event_config = { .edges = { GPIO_ETM_EVENT_EDGE_POS, GPIO_ETM_EVENT_EDGE_NEG } };
    esp_err_t err = gpio_new_etm_event(&event_config, &self->rise, &self->fall);
    if (err == ESP_OK) {
        err = gpio_etm_event_bind_gpio(self->rise, source->number);
    }
    if (err == ESP_OK) {
        err = gpio_etm_event_bind_gpio(self->fall, source->number);
    }
    if (err == ESP_OK) {
        // Start the target where the source already is; from then on only
        // edges move it.
        int level = gpio_get_level(source->number);
        err = espidf_etm_drive_start(&self->drive, self->rise, self->fall, target->number, invert, invert ? !level : level);
    }
    if (err != ESP_OK) {
        if (self->rise != NULL) {
            esp_etm_del_event(self->rise);
        }
        if (self->fall != NULL) {
            esp_etm_del_event(self->fall);
        }
        raise_esp_error(err);
    }
    self->source = source;
    claim_pin(source);
    claim_pin(target);
    _open[slot] = self;
    return MP_OBJ_FROM_PTR(self);
}

//|     def deinit(self) -> None:
//|         """Unlink and release both pins."""
//|         ...
//|
static mp_obj_t espidf_pinlink_deinit(mp_obj_t self_in) {
    pinlink_close(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pinlink_deinit_obj, espidf_pinlink_deinit);

//|     def __enter__(self) -> PinLink:
//|         """No-op used by Context Managers."""
//|         ...
//|
//|     def __exit__(self) -> None:
//|         """Automatically deinitializes when exiting a context."""
//|         ...
//|
static mp_obj_t espidf_pinlink___exit__(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return espidf_pinlink_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espidf_pinlink___exit___obj, 4, 4, espidf_pinlink___exit__);

//|     pull: Optional[digitalio.Pull]
//|     """The weak pull on ``source``."""
//|
//|
static mp_obj_t espidf_pinlink_get_pull(mp_obj_t self_in) {
    espidf_pinlink_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->source == NULL) {
        raise_deinited_error();
    }
    return self->pull == PULL_UP ? MP_OBJ_FROM_PTR(&digitalio_pull_up_obj) :
           self->pull == PULL_DOWN ? MP_OBJ_FROM_PTR(&digitalio_pull_down_obj) : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(espidf_pinlink_get_pull_obj, espidf_pinlink_get_pull);

static mp_obj_t espidf_pinlink_set_pull(mp_obj_t self_in, mp_obj_t value) {
    espidf_pinlink_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->source == NULL) {
        raise_deinited_error();
    }
    digitalio_pull_t pull = validate_pull(value, MP_QSTR_pull);
    apply_pull(self->source->number, pull);
    self->pull = pull;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(espidf_pinlink_set_pull_obj, espidf_pinlink_set_pull);

MP_PROPERTY_GETSET(espidf_pinlink_pull_obj,
    (mp_obj_t)&espidf_pinlink_get_pull_obj,
    (mp_obj_t)&espidf_pinlink_set_pull_obj);

static const mp_rom_map_elem_t espidf_pinlink_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espidf_pinlink_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&espidf_pinlink_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&default___enter___obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&espidf_pinlink___exit___obj) },
    { MP_ROM_QSTR(MP_QSTR_pull), MP_ROM_PTR(&espidf_pinlink_pull_obj) },
};
static MP_DEFINE_CONST_DICT(espidf_pinlink_locals_dict, espidf_pinlink_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    espidf_pinlink_type,
    MP_QSTR_PinLink,
    MP_TYPE_FLAG_HAS_SPECIAL_ACCESSORS,
    make_new, espidf_pinlink_make_new,
    locals_dict, &espidf_pinlink_locals_dict
    );

#endif
